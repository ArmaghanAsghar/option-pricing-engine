#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QtCharts/QChart>
#include <QVector>

/// Pure builders: JSON in, a configured QChart out. Kept free of widget state
/// so they can be unit tested and reused across tabs.
namespace charts {

/// A single labelled series extracted from a curve response.
struct Series {
    QString name;
    QVector<QPointF> points;
};

/// Pulls one field out of a /price/curve response. `path` is either a
/// top-level key ("price") or "greeks.delta".
Series extractSeries(const QJsonObject& curve, const QString& path, const QString& name);

/// Option value against spot, with the payoff at expiry underneath it.
QChart* payoffChart(const QJsonObject& curve, const QString& title);

/// One or more Greeks against the curve's axis.
QChart* greeksChart(const QJsonObject& curve, const QStringList& greeks,
                    const QString& title);

/// Monte Carlo sample paths, with the strike and any barrier marked.
QChart* pathFanChart(const QJsonArray& sample_paths, double expiry_years, double strike,
                     double barrier, const QString& title);

/// Price estimate against path count, with a shaded two-standard-error band.
QChart* convergenceChart(const QJsonArray& convergence, double reference_price,
                         const QString& title);

/// Daily closes from a /market/history response.
QChart* historyChart(const QJsonObject& history, const QString& symbol);

/// Implied volatility against strike, one series per expiry. Points the solver
/// flagged as unreliable are drawn in a separate, de-emphasised series.
QChart* smileChart(const QJsonObject& implied, double spot, const QString& title);

/// A Greek over a spot-by-time grid, rendered as a heatmap. `rows` is a list of
/// curve responses, one per time slice, paired with its time to expiry.
struct HeatmapRow {
    double expiry = 0.0;
    QJsonObject curve;
};
QChart* heatmapChart(const QVector<HeatmapRow>& rows, const QString& greek,
                     const QString& title);

/// Applies the shared look: light grid, no chrome, legend only when useful.
void applyTheme(QChart* chart);

}  // namespace charts
