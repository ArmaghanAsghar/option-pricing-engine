#pragma once

#include <QAbstractTableModel>
#include <QString>
#include <QVector>
#include <optional>

/// One row of the watchlist: the persisted symbol plus whatever the last
/// snapshot returned.
struct WatchlistRow {
    QString symbol;
    double spot = 0.0;
    double change = 0.0;
    double percent_change = 0.0;
    std::optional<double> historical_vol;
    QString updated;
    bool stale = false;
    QString note;  ///< why a field is blank, shown as a tooltip
};

/// Table model over a local SQLite file. Symbols persist between launches;
/// quotes do not, because they are always refetched on open.
class WatchlistModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { Symbol, Spot, Change, PercentChange, HistVol, Updated, ColumnCount };

    explicit WatchlistModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    /// Opens (or creates) the database. Returns false and sets lastError() when
    /// the file cannot be used; the model then works in memory only.
    bool open(const QString& path);
    QString lastError() const { return last_error_; }

    /// Adds a symbol if it is not already present. Returns false when it is a
    /// duplicate or fails validation.
    bool addSymbol(const QString& symbol, QString* why = nullptr);
    void removeRow(int row);

    void updateRow(const QString& symbol, const WatchlistRow& row);

    QStringList symbols() const;
    std::optional<WatchlistRow> rowFor(const QString& symbol) const;
    const WatchlistRow* at(int row) const;

private:
    void loadFromDatabase();
    int indexOf(const QString& symbol) const;

    QVector<WatchlistRow> rows_;
    QString last_error_;
    bool database_ready_ = false;
};
