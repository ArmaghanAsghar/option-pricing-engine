#include "watchlist.hpp"

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

constexpr const char* kConnectionName = "watchlist";

/// Same rule the backend applies, so the UI rejects junk before a round trip.
bool validSymbol(const QString& symbol) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Z0-9.\\-]{1,12}$"));
    return pattern.match(symbol).hasMatch();
}

}  // namespace

WatchlistModel::WatchlistModel(QObject* parent) : QAbstractTableModel(parent) {}

bool WatchlistModel::open(const QString& path) {
    QSqlDatabase db = QSqlDatabase::contains(QLatin1String(kConnectionName))
                          ? QSqlDatabase::database(QLatin1String(kConnectionName))
                          : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                      QLatin1String(kConnectionName));
    db.setDatabaseName(path);

    if (!db.open()) {
        last_error_ = db.lastError().text();
        database_ready_ = false;
        return false;
    }

    QSqlQuery query(db);
    const bool ok = query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS watchlist ("
        "  symbol TEXT PRIMARY KEY,"
        "  added_at INTEGER NOT NULL)"));
    if (!ok) {
        last_error_ = query.lastError().text();
        database_ready_ = false;
        return false;
    }

    database_ready_ = true;
    last_error_.clear();
    loadFromDatabase();
    return true;
}

void WatchlistModel::loadFromDatabase() {
    if (!database_ready_) return;

    QSqlQuery query(QSqlDatabase::database(QLatin1String(kConnectionName)));
    if (!query.exec(QStringLiteral("SELECT symbol FROM watchlist ORDER BY added_at ASC"))) {
        last_error_ = query.lastError().text();
        return;
    }

    beginResetModel();
    rows_.clear();
    while (query.next()) {
        WatchlistRow row;
        row.symbol = query.value(0).toString();
        row.note = QStringLiteral("not yet fetched");
        rows_.push_back(row);
    }
    endResetModel();
}

int WatchlistModel::indexOf(const QString& symbol) const {
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_[i].symbol == symbol) return i;
    }
    return -1;
}

bool WatchlistModel::addSymbol(const QString& symbol, QString* why) {
    const QString clean = symbol.trimmed().toUpper();

    if (!validSymbol(clean)) {
        if (why) *why = QStringLiteral("A symbol is 1 to 12 letters, digits, dots or dashes.");
        return false;
    }
    if (indexOf(clean) >= 0) {
        if (why) *why = QStringLiteral("%1 is already on the watchlist.").arg(clean);
        return false;
    }

    if (database_ready_) {
        QSqlQuery query(QSqlDatabase::database(QLatin1String(kConnectionName)));
        query.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO watchlist (symbol, added_at) VALUES (?, ?)"));
        query.addBindValue(clean);
        query.addBindValue(QDateTime::currentSecsSinceEpoch());
        if (!query.exec()) {
            last_error_ = query.lastError().text();
            if (why) *why = last_error_;
            return false;
        }
    }

    beginInsertRows({}, rows_.size(), rows_.size());
    WatchlistRow row;
    row.symbol = clean;
    row.note = QStringLiteral("fetching");
    rows_.push_back(row);
    endInsertRows();
    return true;
}

void WatchlistModel::removeRow(int row) {
    if (row < 0 || row >= rows_.size()) return;
    const QString symbol = rows_[row].symbol;

    if (database_ready_) {
        QSqlQuery query(QSqlDatabase::database(QLatin1String(kConnectionName)));
        query.prepare(QStringLiteral("DELETE FROM watchlist WHERE symbol = ?"));
        query.addBindValue(symbol);
        if (!query.exec()) last_error_ = query.lastError().text();
    }

    beginRemoveRows({}, row, row);
    rows_.remove(row);
    endRemoveRows();
}

void WatchlistModel::updateRow(const QString& symbol, const WatchlistRow& row) {
    const int i = indexOf(symbol);
    if (i < 0) return;

    rows_[i] = row;
    rows_[i].symbol = symbol;  // never let an update rename the row
    emit dataChanged(index(i, 0), index(i, ColumnCount - 1));
}

QStringList WatchlistModel::symbols() const {
    QStringList out;
    out.reserve(rows_.size());
    for (const auto& row : rows_) out << row.symbol;
    return out;
}

std::optional<WatchlistRow> WatchlistModel::rowFor(const QString& symbol) const {
    const int i = indexOf(symbol);
    if (i < 0) return std::nullopt;
    return rows_[i];
}

const WatchlistRow* WatchlistModel::at(int row) const {
    if (row < 0 || row >= rows_.size()) return nullptr;
    return &rows_[row];
}

int WatchlistModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

int WatchlistModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant WatchlistModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const WatchlistRow& row = rows_[index.row()];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case Symbol:
                return row.stale ? row.symbol + QStringLiteral(" *") : row.symbol;
            case Spot:
                return row.spot > 0.0 ? QString::number(row.spot, 'f', 2) : QStringLiteral("--");
            case Change:
                if (row.spot <= 0.0) return QStringLiteral("--");
                return QStringLiteral("%1%2")
                    .arg(row.change >= 0 ? QStringLiteral("+") : QString())
                    .arg(row.change, 0, 'f', 2);
            case PercentChange:
                if (row.spot <= 0.0) return QStringLiteral("--");
                return QStringLiteral("%1%2%")
                    .arg(row.percent_change >= 0 ? QStringLiteral("+") : QString())
                    .arg(row.percent_change, 0, 'f', 2);
            case HistVol:
                return row.historical_vol
                           ? QStringLiteral("%1%").arg(*row.historical_vol * 100.0, 0, 'f', 1)
                           : QStringLiteral("--");
            case Updated:
                return row.updated.isEmpty() ? QStringLiteral("--") : row.updated;
            default:
                return {};
        }
    }

    if (role == Qt::ForegroundRole && (index.column() == Change || index.column() == PercentChange)) {
        if (row.spot <= 0.0) return {};
        // Red and green are conventional here; the sign is also in the text, so
        // colour is not the only cue.
        return row.change >= 0.0 ? QBrush(QColor(0, 140, 60)) : QBrush(QColor(200, 40, 40));
    }

    if (role == Qt::ToolTipRole) {
        if (row.stale) {
            return QStringLiteral("%1: showing cached data. %2").arg(row.symbol, row.note);
        }
        if (!row.note.isEmpty()) return QStringLiteral("%1: %2").arg(row.symbol, row.note);
        return row.symbol;
    }

    if (role == Qt::TextAlignmentRole && index.column() != Symbol) {
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }

    return {};
}

QVariant WatchlistModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
    switch (section) {
        case Symbol: return QStringLiteral("Symbol");
        case Spot: return QStringLiteral("Last");
        case Change: return QStringLiteral("Change");
        case PercentChange: return QStringLiteral("Change %");
        case HistVol: return QStringLiteral("Hist vol");
        case Updated: return QStringLiteral("Updated");
        default: return {};
    }
}
