#pragma once

#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QTableView>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QtCharts/QChartView>

#include "api_client.hpp"
#include "pricing_panel.hpp"
#include "watchlist.hpp"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onAddSymbol();
    void onRemoveSymbol();
    void onRefreshWatchlist();
    void onWatchlistSelectionChanged();
    void onInputsChanged();
    void onEditSettings();
    void onFetchSmile();

private:
    // Construction helpers.
    QWidget* buildLeftPane();
    QWidget* buildChartTabs();
    QWidget* buildResultsBox();
    QWidget* buildSmileTab();
    void buildMenus();
    void loadSettings();
    void saveSettings();

    // Work.
    void refreshSymbol(const QString& symbol);
    void repriceAll();
    void updatePayoffAndGreeks();
    void updateMonteCarloCharts();
    void updateHeatmap();
    void loadHistory(const QString& symbol);
    void showError(const QString& message);
    void setChart(QChartView* view, QChart* chart);
    void setStatus(const QString& message, bool is_error = false);

    QString selectedSymbol() const;

    ApiClient* api_;
    WatchlistModel* watchlist_;
    PricingPanel* panel_;

    QTableView* watchlist_view_;
    QTabWidget* tabs_;
    QChartView* payoff_view_;
    QChartView* greeks_view_;
    QChartView* heatmap_view_;
    QChartView* paths_view_;
    QChartView* convergence_view_;
    QChartView* history_view_;
    QChartView* smile_view_;
    QTableWidget* results_table_;
    QTableWidget* override_table_;

    QLabel* status_label_;
    QLabel* backend_label_;

    /// Coalesces the flurry of edits from a spin box into one round trip.
    QTimer* debounce_;

    QString base_url_;
    QString current_symbol_;
    double last_closed_form_price_ = 0.0;
    bool market_data_available_ = false;
};
