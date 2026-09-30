#include "main_window.hpp"

#include <QAction>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <cmath>

#include "chart_builders.hpp"

namespace {

constexpr int kDebounceMs = 250;
constexpr int kCurvePoints = 61;
constexpr int kHistoryDays = 250;

QChartView* makeChartView() {
    auto* view = new QChartView;
    view->setRenderHint(QPainter::Antialiasing);
    view->setMinimumHeight(260);
    return view;
}

QString settingsPath() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/watchlist.db");
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      api_(new ApiClient(this)),
      watchlist_(new WatchlistModel(this)),
      debounce_(new QTimer(this)) {
    setWindowTitle(QStringLiteral("Option Pricer"));
    resize(1400, 900);

    debounce_->setSingleShot(true);
    debounce_->setInterval(kDebounceMs);
    connect(debounce_, &QTimer::timeout, this, &MainWindow::repriceAll);

    loadSettings();
    api_->setBaseUrl(base_url_);

    if (!watchlist_->open(settingsPath())) {
        // Not fatal: the session still works, it just will not remember symbols.
        setStatus(QStringLiteral("watchlist will not persist: %1").arg(watchlist_->lastError()),
                  true);
    }

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(buildLeftPane());
    splitter->addWidget(buildChartTabs());
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({420, 980});
    setCentralWidget(splitter);

    buildMenus();

    status_label_ = new QLabel;
    backend_label_ = new QLabel;
    statusBar()->addWidget(status_label_, 1);
    statusBar()->addPermanentWidget(backend_label_);

    connect(api_, &ApiClient::busyChanged, this, [this](bool busy) {
        if (busy) setStatus(QStringLiteral("working..."));
    });

    // Probe the backend so the user learns immediately if it is not running.
    api_->health(
        [this](const QJsonObject& health) {
            const QJsonObject md = health.value(QStringLiteral("market_data")).toObject();
            market_data_available_ = md.value(QStringLiteral("configured")).toBool();

            backend_label_->setText(
                QStringLiteral("api %1  %2")
                    .arg(health.value(QStringLiteral("version")).toString(),
                         market_data_available_ ? QStringLiteral("live data")
                                                : QStringLiteral("no market data")));
            setStatus(market_data_available_
                          ? QStringLiteral("connected")
                          : QStringLiteral("connected; market data is off because the "
                                           "backend has no Finnhub key"));
            if (market_data_available_) onRefreshWatchlist();
        },
        [this](const QString& error) {
            backend_label_->setText(QStringLiteral("offline"));
            setStatus(QStringLiteral("cannot reach %1: %2").arg(base_url_, error), true);
        });

    repriceAll();
}

QWidget* MainWindow::buildLeftPane() {
    auto* pane = new QWidget;
    auto* layout = new QVBoxLayout(pane);

    // ------------------------------------------------------------- watchlist
    auto* watch_box = new QGroupBox(QStringLiteral("Watchlist"));
    auto* watch_layout = new QVBoxLayout(watch_box);

    watchlist_view_ = new QTableView;
    watchlist_view_->setModel(watchlist_);
    watchlist_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    watchlist_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    watchlist_view_->verticalHeader()->setVisible(false);
    watchlist_view_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    watchlist_view_->setMaximumHeight(200);
    watchlist_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    watch_layout->addWidget(watchlist_view_);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(QStringLiteral("Add"));
    auto* remove = new QPushButton(QStringLiteral("Remove"));
    auto* refresh = new QPushButton(QStringLiteral("Refresh"));
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addWidget(refresh);
    watch_layout->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, &MainWindow::onAddSymbol);
    connect(remove, &QPushButton::clicked, this, &MainWindow::onRemoveSymbol);
    connect(refresh, &QPushButton::clicked, this, &MainWindow::onRefreshWatchlist);
    connect(watchlist_view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MainWindow::onWatchlistSelectionChanged);

    layout->addWidget(watch_box);

    // ------------------------------------------------------ inputs + results
    panel_ = new PricingPanel;
    connect(panel_, &PricingPanel::changed, this, &MainWindow::onInputsChanged);
    layout->addWidget(panel_);
    layout->addWidget(buildResultsBox());
    layout->addStretch(1);

    return pane;
}

QWidget* MainWindow::buildResultsBox() {
    auto* box = new QGroupBox(QStringLiteral("Result"));
    auto* layout = new QVBoxLayout(box);

    results_table_ = new QTableWidget(7, 2);
    results_table_->setHorizontalHeaderLabels({QStringLiteral("Measure"), QStringLiteral("Value")});
    results_table_->verticalHeader()->setVisible(false);
    results_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    results_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_table_->setMaximumHeight(230);

    const QStringList labels = {QStringLiteral("Price"),   QStringLiteral("Delta"),
                                QStringLiteral("Gamma"),   QStringLiteral("Vega"),
                                QStringLiteral("Theta"),   QStringLiteral("Rho"),
                                QStringLiteral("Std error")};
    for (int i = 0; i < labels.size(); ++i) {
        results_table_->setItem(i, 0, new QTableWidgetItem(labels[i]));
        results_table_->setItem(i, 1, new QTableWidgetItem(QStringLiteral("--")));
    }
    layout->addWidget(results_table_);
    return box;
}

QWidget* MainWindow::buildChartTabs() {
    tabs_ = new QTabWidget;

    payoff_view_ = makeChartView();
    greeks_view_ = makeChartView();
    heatmap_view_ = makeChartView();
    paths_view_ = makeChartView();
    convergence_view_ = makeChartView();
    history_view_ = makeChartView();

    tabs_->addTab(payoff_view_, QStringLiteral("Payoff and value"));
    tabs_->addTab(greeks_view_, QStringLiteral("Greeks"));
    tabs_->addTab(heatmap_view_, QStringLiteral("Greek surface"));

    auto* mc_tab = new QWidget;
    auto* mc_layout = new QVBoxLayout(mc_tab);
    mc_layout->addWidget(paths_view_);
    mc_layout->addWidget(convergence_view_);
    tabs_->addTab(mc_tab, QStringLiteral("Monte Carlo"));

    tabs_->addTab(history_view_, QStringLiteral("History"));
    tabs_->addTab(buildSmileTab(), QStringLiteral("Vol smile"));

    // Recompute the expensive tabs only when they become visible.
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) { onInputsChanged(); });
    return tabs_;
}

QWidget* MainWindow::buildSmileTab() {
    auto* tab = new QWidget;
    auto* layout = new QHBoxLayout(tab);

    smile_view_ = makeChartView();
    layout->addWidget(smile_view_, 2);

    auto* side = new QWidget;
    auto* side_layout = new QVBoxLayout(side);

    auto* fetch = new QPushButton(QStringLiteral("Fetch live chain"));
    connect(fetch, &QPushButton::clicked, this, &MainWindow::onFetchSmile);
    side_layout->addWidget(fetch);

    side_layout->addWidget(new QLabel(QStringLiteral(
        "Market prices. Edit a row to override the\nlive quote, or add your own strikes.")));

    override_table_ = new QTableWidget(12, 3);
    override_table_->setHorizontalHeaderLabels(
        {QStringLiteral("Strike"), QStringLiteral("Type"), QStringLiteral("Price")});
    override_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    override_table_->verticalHeader()->setVisible(false);
    side_layout->addWidget(override_table_);

    auto* solve = new QPushButton(QStringLiteral("Solve from table"));
    connect(solve, &QPushButton::clicked, this, [this] {
        QJsonArray quotes;
        for (int row = 0; row < override_table_->rowCount(); ++row) {
            const auto* strike_item = override_table_->item(row, 0);
            const auto* price_item = override_table_->item(row, 2);
            if (!strike_item || !price_item) continue;

            bool strike_ok = false;
            bool price_ok = false;
            const double strike = strike_item->text().toDouble(&strike_ok);
            const double price = price_item->text().toDouble(&price_ok);
            if (!strike_ok || !price_ok || strike <= 0.0 || price <= 0.0) continue;

            const auto* type_item = override_table_->item(row, 1);
            const QString type = type_item && type_item->text().startsWith(
                                                  QStringLiteral("p"), Qt::CaseInsensitive)
                                     ? QStringLiteral("put")
                                     : QStringLiteral("call");

            quotes.append(QJsonObject{{QStringLiteral("strike"), strike},
                                      {QStringLiteral("expiry"), panel_->expiry()},
                                      {QStringLiteral("option"), type},
                                      {QStringLiteral("market_price"), price}});
        }

        if (quotes.isEmpty()) {
            showError(QStringLiteral("Fill in at least one strike and price."));
            return;
        }

        const QJsonObject request = panel_->priceRequest(false);
        const QJsonObject body{
            {QStringLiteral("market"), request.value(QStringLiteral("market")).toObject()},
            {QStringLiteral("quotes"), quotes}};

        api_->impliedVol(
            body,
            [this](const QJsonObject& response) {
                setChart(smile_view_, charts::smileChart(response, panel_->spot(),
                                                         QStringLiteral("Implied volatility "
                                                                        "from entered prices")));
                setStatus(QStringLiteral("solved %1 of %2 quotes")
                              .arg(response.value(QStringLiteral("converged")).toInt())
                              .arg(response.value(QStringLiteral("total")).toInt()));
            },
            [this](const QString& error) { showError(error); });
    });
    side_layout->addWidget(solve);
    side_layout->addStretch(1);

    layout->addWidget(side, 1);
    return tab;
}

void MainWindow::buildMenus() {
    auto* file = menuBar()->addMenu(QStringLiteral("&File"));

    auto* settings = file->addAction(QStringLiteral("Backend settings..."));
    connect(settings, &QAction::triggered, this, &MainWindow::onEditSettings);

    auto* refresh = file->addAction(QStringLiteral("Refresh watchlist"));
    refresh->setShortcut(QKeySequence::Refresh);
    connect(refresh, &QAction::triggered, this, &MainWindow::onRefreshWatchlist);

    file->addSeparator();
    auto* quit = file->addAction(QStringLiteral("Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);
}

void MainWindow::loadSettings() {
    QSettings settings;
    base_url_ =
        settings.value(QStringLiteral("backend_url"), QStringLiteral("http://localhost:8080"))
            .toString();
}

void MainWindow::saveSettings() {
    QSettings settings;
    settings.setValue(QStringLiteral("backend_url"), base_url_);
}

void MainWindow::onEditSettings() {
    bool ok = false;
    const QString url = QInputDialog::getText(
        this, QStringLiteral("Backend settings"), QStringLiteral("pricer-api base URL:"),
        QLineEdit::Normal, base_url_, &ok);
    if (!ok || url.trimmed().isEmpty()) return;

    base_url_ = url.trimmed();
    api_->setBaseUrl(base_url_);
    saveSettings();

    api_->health([this](const QJsonObject&) { setStatus(QStringLiteral("connected")); },
                 [this](const QString& error) { showError(error); });
}

void MainWindow::onAddSymbol() {
    bool ok = false;
    const QString symbol =
        QInputDialog::getText(this, QStringLiteral("Add stock"),
                              QStringLiteral("Ticker (US equities):"), QLineEdit::Normal,
                              QString(), &ok);
    if (!ok || symbol.trimmed().isEmpty()) return;

    QString why;
    if (!watchlist_->addSymbol(symbol, &why)) {
        showError(why);
        return;
    }
    refreshSymbol(symbol.trimmed().toUpper());
}

void MainWindow::onRemoveSymbol() {
    const QModelIndexList selected = watchlist_view_->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        setStatus(QStringLiteral("select a row to remove"));
        return;
    }
    watchlist_->removeRow(selected.first().row());
}

void MainWindow::onRefreshWatchlist() {
    const QStringList symbols = watchlist_->symbols();
    if (symbols.isEmpty()) {
        setStatus(QStringLiteral("watchlist is empty; add a ticker to begin"));
        return;
    }
    if (!market_data_available_) {
        setStatus(QStringLiteral("the backend has no market data key, so quotes cannot be "
                                 "fetched"),
                  true);
        return;
    }
    for (const QString& symbol : symbols) refreshSymbol(symbol);
}

void MainWindow::refreshSymbol(const QString& symbol) {
    api_->snapshot(
        symbol,
        [this, symbol](const QJsonObject& response) {
            const QJsonObject quote = response.value(QStringLiteral("quote")).toObject();

            WatchlistRow row;
            row.symbol = symbol;
            row.spot = quote.value(QStringLiteral("price")).toDouble();
            row.change = quote.value(QStringLiteral("change")).toDouble();
            row.percent_change = quote.value(QStringLiteral("percent_change")).toDouble();
            row.stale = quote.value(QStringLiteral("stale")).toBool();

            const QJsonValue vol = response.value(QStringLiteral("historical_vol"));
            if (vol.isDouble()) {
                row.historical_vol = vol.toDouble();
            } else {
                row.note = response.value(QStringLiteral("vol_note")).toString();
                if (row.note.isEmpty()) {
                    row.note = QStringLiteral("historical volatility unavailable");
                }
            }

            const qint64 t = static_cast<qint64>(quote.value(QStringLiteral("timestamp")).toDouble());
            if (t > 0) {
                row.updated = QDateTime::fromSecsSinceEpoch(t).toString(QStringLiteral("HH:mm"));
            }

            watchlist_->updateRow(symbol, row);

            // Feed the panel only when this is the symbol being looked at.
            if (symbol == current_symbol_) {
                panel_->applySnapshot(
                    response.value(QStringLiteral("suggested_market")).toObject());
            }
            setStatus(QStringLiteral("updated %1").arg(symbol));
        },
        [this, symbol](const QString& error) {
            WatchlistRow row;
            row.symbol = symbol;
            row.note = error;
            watchlist_->updateRow(symbol, row);
            setStatus(QStringLiteral("%1: %2").arg(symbol, error), true);
        });
}

QString MainWindow::selectedSymbol() const {
    const QModelIndexList selected = watchlist_view_->selectionModel()->selectedRows();
    if (selected.isEmpty()) return {};
    const WatchlistRow* row = watchlist_->at(selected.first().row());
    return row ? row->symbol : QString();
}

void MainWindow::onWatchlistSelectionChanged() {
    const QString symbol = selectedSymbol();
    if (symbol.isEmpty() || symbol == current_symbol_) return;
    current_symbol_ = symbol;

    const auto row = watchlist_->rowFor(symbol);
    if (row && row->spot > 0.0) {
        QJsonObject suggested{{QStringLiteral("spot"), row->spot}};
        if (row->historical_vol) {
            suggested[QStringLiteral("vol")] = *row->historical_vol;
        }
        panel_->applySnapshot(suggested);
    } else {
        refreshSymbol(symbol);
    }
    loadHistory(symbol);
}

void MainWindow::onInputsChanged() {
    debounce_->start();
}

void MainWindow::repriceAll() {
    // The headline price and Greeks.
    api_->price(
        panel_->priceRequest(true),
        [this](const QJsonObject& result) {
            const QJsonObject greeks = result.value(QStringLiteral("greeks")).toObject();

            const auto put = [this](int row, const QString& text) {
                results_table_->item(row, 1)->setText(text);
            };
            const auto put_number = [&](int row, const QJsonObject& source,
                                        const QString& key, int decimals) {
                const QJsonValue v = source.value(key);
                put(row, v.isDouble() ? QString::number(v.toDouble(), 'f', decimals)
                                      : QStringLiteral("--"));
            };

            put(0, QString::number(result.value(QStringLiteral("price")).toDouble(), 'f', 6));
            put_number(1, greeks, QStringLiteral("delta"), 6);
            put_number(2, greeks, QStringLiteral("gamma"), 6);
            put_number(3, greeks, QStringLiteral("vega"), 6);
            put_number(4, greeks, QStringLiteral("theta"), 6);
            put_number(5, greeks, QStringLiteral("rho"), 6);
            put_number(6, result, QStringLiteral("std_error"), 6);

            setStatus(QStringLiteral("priced with %1")
                          .arg(result.value(QStringLiteral("engine")).toString()));

            // Monte Carlo extras ride along on the same response.
            const QJsonArray paths = result.value(QStringLiteral("sample_paths")).toArray();
            const QJsonArray convergence = result.value(QStringLiteral("convergence")).toArray();
            if (!paths.isEmpty()) {
                setChart(paths_view_,
                         charts::pathFanChart(paths, panel_->expiry(), panel_->strike(),
                                              panel_->barrier(),
                                              QStringLiteral("Simulated paths")));
            }
            if (!convergence.isEmpty()) {
                setChart(convergence_view_,
                         charts::convergenceChart(convergence, last_closed_form_price_,
                                                  QStringLiteral("Convergence")));
            }
        },
        [this](const QString& error) {
            for (int row = 0; row < results_table_->rowCount(); ++row) {
                results_table_->item(row, 1)->setText(QStringLiteral("--"));
            }
            showError(error);
        });

    updatePayoffAndGreeks();

    // A closed-form reference for the convergence chart, where one exists.
    if (panel_->payoffKind() == QStringLiteral("vanilla") && panel_->isStochastic()) {
        QJsonObject reference = panel_->priceRequest(false);
        reference[QStringLiteral("engine")] = QStringLiteral("black_scholes");
        api_->price(
            reference,
            [this](const QJsonObject& result) {
                last_closed_form_price_ = result.value(QStringLiteral("price")).toDouble();
            },
            [](const QString&) { /* no reference is fine */ });
    } else {
        last_closed_form_price_ = 0.0;
    }

    if (tabs_->currentIndex() == 2) updateHeatmap();
}

void MainWindow::updatePayoffAndGreeks() {
    const auto [lo, hi] = panel_->spotRange();
    const QJsonObject request =
        panel_->curveRequest(QStringLiteral("spot"), lo, hi, kCurvePoints);

    api_->curve(
        request,
        [this](const QJsonObject& curve) {
            setChart(payoff_view_,
                     charts::payoffChart(curve, QStringLiteral("Value now against payoff at "
                                                               "expiry")));
            setChart(greeks_view_,
                     charts::greeksChart(curve,
                                         {QStringLiteral("delta"), QStringLiteral("gamma"),
                                          QStringLiteral("vega"), QStringLiteral("theta")},
                                         QStringLiteral("Greeks against spot")));
        },
        [this](const QString& error) { showError(error); });
}

void MainWindow::updateHeatmap() {
    // Several time slices, each a spot curve, drawn as a coloured family.
    const auto [lo, hi] = panel_->spotRange();
    const double expiry = panel_->expiry();

    auto slices = std::make_shared<QVector<charts::HeatmapRow>>();
    auto remaining = std::make_shared<int>(0);

    const QVector<double> fractions = {0.05, 0.2, 0.4, 0.6, 0.8, 1.0};
    for (double f : fractions) {
        const double t = std::max(0.004, expiry * f);
        QJsonObject request = panel_->curveRequest(QStringLiteral("spot"), lo, hi, 41);
        QJsonObject instrument = request.value(QStringLiteral("instrument")).toObject();
        instrument[QStringLiteral("expiry")] = t;
        request[QStringLiteral("instrument")] = instrument;

        ++(*remaining);
        api_->curve(
            request,
            [this, slices, remaining, t](const QJsonObject& curve) {
                slices->append({t, curve});
                if (--(*remaining) == 0) {
                    std::sort(slices->begin(), slices->end(),
                              [](const charts::HeatmapRow& a, const charts::HeatmapRow& b) {
                                  return a.expiry < b.expiry;
                              });
                    setChart(heatmap_view_,
                             charts::heatmapChart(*slices, QStringLiteral("delta"),
                                                  QStringLiteral("Delta across spot and time "
                                                                 "to expiry")));
                }
            },
            [this, remaining](const QString& error) {
                if (--(*remaining) == 0) showError(error);
            });
    }
}

void MainWindow::loadHistory(const QString& symbol) {
    if (!market_data_available_) return;
    api_->history(
        symbol, kHistoryDays,
        [this, symbol](const QJsonObject& history) {
            setChart(history_view_, charts::historyChart(history, symbol));
        },
        [this](const QString& error) { setStatus(error, true); });
}

void MainWindow::onFetchSmile() {
    const QString symbol = current_symbol_.isEmpty() ? selectedSymbol() : current_symbol_;
    if (symbol.isEmpty()) {
        showError(QStringLiteral("Select a stock in the watchlist first."));
        return;
    }
    if (!market_data_available_) {
        showError(QStringLiteral("The backend has no market data key, so option chains are "
                                 "unavailable. Enter prices in the table instead."));
        return;
    }

    const QJsonObject request = panel_->priceRequest(false);
    const QJsonObject body{
        {QStringLiteral("symbol"), symbol},
        {QStringLiteral("market"), request.value(QStringLiteral("market")).toObject()}};

    api_->impliedVol(
        body,
        [this, symbol](const QJsonObject& response) {
            const double spot = response.value(QStringLiteral("spot")).toDouble();
            setChart(smile_view_,
                     charts::smileChart(response, spot,
                                        QStringLiteral("%1 implied volatility").arg(symbol)));

            // Seed the override table with the live quotes so they can be edited.
            const QJsonArray results = response.value(QStringLiteral("results")).toArray();
            override_table_->setRowCount(std::max(12, static_cast<int>(results.size())));
            for (int i = 0; i < results.size(); ++i) {
                const QJsonObject r = results[i].toObject();
                override_table_->setItem(
                    i, 0,
                    new QTableWidgetItem(
                        QString::number(r.value(QStringLiteral("strike")).toDouble(), 'f', 2)));
                override_table_->setItem(
                    i, 1, new QTableWidgetItem(r.value(QStringLiteral("option")).toString()));
                override_table_->setItem(
                    i, 2,
                    new QTableWidgetItem(QString::number(
                        r.value(QStringLiteral("market_price")).toDouble(), 'f', 2)));
            }

            const int converged = response.value(QStringLiteral("converged")).toInt();
            const int total = response.value(QStringLiteral("total")).toInt();
            setStatus(QStringLiteral("%1: solved %2 of %3 strikes")
                          .arg(symbol)
                          .arg(converged)
                          .arg(total));
        },
        [this](const QString& error) { showError(error); });
}

void MainWindow::setChart(QChartView* view, QChart* chart) {
    QChart* previous = view->chart();
    view->setChart(chart);
    delete previous;  // QChartView does not take ownership of the old chart
}

void MainWindow::setStatus(const QString& message, bool is_error) {
    status_label_->setText(message);
    status_label_->setStyleSheet(is_error ? QStringLiteral("color: #c02020;") : QString());
}

void MainWindow::showError(const QString& message) {
    // Errors are frequent and mostly transient (a bad ticker, a stale barrier),
    // so they go to the status bar rather than a modal dialog.
    setStatus(message, true);
}
