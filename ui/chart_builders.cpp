#include "chart_builders.hpp"

#include <QAreaSeries>
#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QLegendMarker>
#include <QLineSeries>
#include <QPen>
#include <QScatterSeries>
#include <QValueAxis>
#include <QtCharts/QDateTimeAxis>
#include <algorithm>
#include <cmath>

namespace charts {

namespace {

// A small categorical palette, distinguishable in both light and dark themes.
const QColor kPrimary(31, 119, 180);
const QColor kSecondary(255, 127, 14);
const QColor kTertiary(44, 160, 44);
const QColor kQuaternary(214, 39, 40);
const QColor kMuted(150, 150, 150);
const QColor kGrid(220, 220, 220);

QColor paletteColor(int index) {
    static const QVector<QColor> palette = {kPrimary, kSecondary, kTertiary, kQuaternary,
                                            QColor(148, 103, 189), QColor(140, 86, 75)};
    return palette[index % palette.size()];
}

/// Reads "greeks.delta" style paths out of a point object.
std::optional<double> valueAt(const QJsonObject& point, const QString& path) {
    const int dot = path.indexOf('.');
    if (dot < 0) {
        if (!point.contains(path)) return std::nullopt;
        const QJsonValue v = point.value(path);
        return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
    }
    const QString head = path.left(dot);
    const QString tail = path.mid(dot + 1);
    if (!point.contains(head) || !point.value(head).isObject()) return std::nullopt;
    const QJsonObject nested = point.value(head).toObject();
    if (!nested.contains(tail)) return std::nullopt;
    const QJsonValue v = nested.value(tail);
    return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
}

/// Hides a series' legend entry. `markers()` can come back empty, so calling
/// front() on it directly would be undefined behaviour.
void hideLegendEntry(QChart* chart, QAbstractSeries* series) {
    const auto markers = chart->legend()->markers(series);
    for (auto* marker : markers) marker->setVisible(false);
}

QLineSeries* lineFrom(const Series& series, const QColor& color, int width = 2) {
    auto* line = new QLineSeries;
    line->setName(series.name);
    for (const auto& p : series.points) line->append(p);
    QPen pen(color);
    pen.setWidth(width);
    line->setPen(pen);
    return line;
}

/// Axis titled and ranged from the data, with a little padding so the extremes
/// are not drawn on the frame.
QValueAxis* makeAxis(const QString& title, double lo, double hi, bool pad = true) {
    auto* axis = new QValueAxis;
    axis->setTitleText(title);
    if (!(hi > lo)) {
        // A flat series still needs a visible range.
        const double centre = std::isfinite(lo) ? lo : 0.0;
        lo = centre - 1.0;
        hi = centre + 1.0;
    } else if (pad) {
        const double margin = (hi - lo) * 0.05;
        lo -= margin;
        hi += margin;
    }
    axis->setRange(lo, hi);
    axis->setGridLineColor(kGrid);
    axis->setLabelFormat(QStringLiteral("%.4g"));
    return axis;
}

struct Bounds {
    double min_x = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double min_y = std::numeric_limits<double>::max();
    double max_y = std::numeric_limits<double>::lowest();
    bool empty = true;

    void add(double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
        empty = false;
    }
};

QString axisLabel(const QJsonObject& curve) {
    const QString axis = curve.value(QStringLiteral("axis")).toString();
    if (axis == QStringLiteral("vol")) return QStringLiteral("Volatility");
    if (axis == QStringLiteral("expiry")) return QStringLiteral("Years to expiry");
    return QStringLiteral("Spot");
}

QChart* emptyChart(const QString& title, const QString& message) {
    auto* chart = new QChart;
    chart->setTitle(QStringLiteral("%1\n%2").arg(title, message));
    applyTheme(chart);
    return chart;
}

}  // namespace

void applyTheme(QChart* chart) {
    chart->setBackgroundRoundness(0);
    chart->setMargins(QMargins(8, 8, 8, 8));
    chart->setAnimationOptions(QChart::NoAnimation);  // plots update on every edit
    chart->legend()->setAlignment(Qt::AlignBottom);
    QFont title_font = chart->titleFont();
    title_font.setBold(true);
    chart->setTitleFont(title_font);
}

Series extractSeries(const QJsonObject& curve, const QString& path, const QString& name) {
    Series out;
    out.name = name;

    const QJsonArray points = curve.value(QStringLiteral("points")).toArray();
    out.points.reserve(points.size());

    for (const auto& item : points) {
        const QJsonObject point = item.toObject();
        // Grid points the engine could not price carry an "error" instead.
        if (point.contains(QStringLiteral("error"))) continue;

        const auto y = valueAt(point, path);
        if (!y || !std::isfinite(*y)) continue;
        out.points.append(QPointF(point.value(QStringLiteral("x")).toDouble(), *y));
    }
    return out;
}

QChart* payoffChart(const QJsonObject& curve, const QString& title) {
    const Series value = extractSeries(curve, QStringLiteral("price"), QStringLiteral("Value now"));
    if (value.points.isEmpty()) {
        return emptyChart(title, QStringLiteral("no priceable points in this range"));
    }
    const Series payoff =
        extractSeries(curve, QStringLiteral("payoff"), QStringLiteral("Payoff at expiry"));

    auto* chart = new QChart;
    chart->setTitle(title);

    Bounds bounds;
    for (const auto& p : value.points) bounds.add(p.x(), p.y());
    for (const auto& p : payoff.points) bounds.add(p.x(), p.y());

    auto* value_line = lineFrom(value, kPrimary, 2);
    chart->addSeries(value_line);

    QLineSeries* payoff_line = nullptr;
    if (!payoff.points.isEmpty()) {
        payoff_line = lineFrom(payoff, kMuted, 1);
        QPen pen = payoff_line->pen();
        pen.setStyle(Qt::DashLine);
        payoff_line->setPen(pen);
        chart->addSeries(payoff_line);
    }

    auto* x_axis = makeAxis(axisLabel(curve), bounds.min_x, bounds.max_x, false);
    auto* y_axis = makeAxis(QStringLiteral("Option value"), std::min(0.0, bounds.min_y),
                            bounds.max_y);
    chart->addAxis(x_axis, Qt::AlignBottom);
    chart->addAxis(y_axis, Qt::AlignLeft);

    value_line->attachAxis(x_axis);
    value_line->attachAxis(y_axis);
    if (payoff_line) {
        payoff_line->attachAxis(x_axis);
        payoff_line->attachAxis(y_axis);
    }

    applyTheme(chart);
    return chart;
}

QChart* greeksChart(const QJsonObject& curve, const QStringList& greeks, const QString& title) {
    auto* chart = new QChart;
    chart->setTitle(title);

    Bounds bounds;
    QVector<QLineSeries*> lines;
    int index = 0;

    for (const QString& greek : greeks) {
        const Series series =
            extractSeries(curve, QStringLiteral("greeks.") + greek, greek);
        if (series.points.isEmpty()) continue;

        for (const auto& p : series.points) bounds.add(p.x(), p.y());
        auto* line = lineFrom(series, paletteColor(index++));
        chart->addSeries(line);
        lines.append(line);
    }

    if (lines.isEmpty()) {
        delete chart;
        return emptyChart(title, QStringLiteral("this engine returned no Greeks"));
    }

    auto* x_axis = makeAxis(axisLabel(curve), bounds.min_x, bounds.max_x, false);
    auto* y_axis = makeAxis(QStringLiteral("Value"), bounds.min_y, bounds.max_y);
    chart->addAxis(x_axis, Qt::AlignBottom);
    chart->addAxis(y_axis, Qt::AlignLeft);
    for (auto* line : lines) {
        line->attachAxis(x_axis);
        line->attachAxis(y_axis);
    }

    applyTheme(chart);
    chart->legend()->setVisible(lines.size() > 1);
    return chart;
}

QChart* pathFanChart(const QJsonArray& sample_paths, double expiry_years, double strike,
                     double barrier, const QString& title) {
    if (sample_paths.isEmpty()) {
        return emptyChart(title, QStringLiteral("ask for sample paths to populate this"));
    }

    auto* chart = new QChart;
    chart->setTitle(title);

    Bounds bounds;
    QVector<QLineSeries*> lines;

    // Translucent strokes so overlapping paths build up density visually.
    QColor stroke = kPrimary;
    stroke.setAlpha(std::max(18, 160 / std::max(1, static_cast<int>(sample_paths.size()) / 4)));

    for (const auto& item : sample_paths) {
        const QJsonArray path = item.toArray();
        if (path.size() < 2) continue;

        auto* line = new QLineSeries;
        const double dt = expiry_years / (path.size() - 1);
        for (int i = 0; i < path.size(); ++i) {
            const double t = dt * i;
            const double s = path[i].toDouble();
            line->append(t, s);
            bounds.add(t, s);
        }
        QPen pen(stroke);
        pen.setWidth(1);
        line->setPen(pen);
        chart->addSeries(line);
        lines.append(line);
    }

    if (lines.isEmpty()) {
        delete chart;
        return emptyChart(title, QStringLiteral("sample paths were empty"));
    }

    // Reference levels, drawn on top of the fan.
    const auto add_level = [&](double level, const QColor& color, const QString& name) {
        if (!(level > 0.0)) return static_cast<QLineSeries*>(nullptr);
        auto* line = new QLineSeries;
        line->setName(name);
        line->append(bounds.min_x, level);
        line->append(bounds.max_x, level);
        QPen pen(color);
        pen.setWidth(2);
        pen.setStyle(Qt::DashLine);
        line->setPen(pen);
        chart->addSeries(line);
        bounds.add(bounds.min_x, level);
        return line;
    };
    auto* strike_line = add_level(strike, kSecondary, QStringLiteral("Strike"));
    auto* barrier_line = add_level(barrier, kQuaternary, QStringLiteral("Barrier"));

    auto* x_axis = makeAxis(QStringLiteral("Years"), 0.0, expiry_years, false);
    auto* y_axis = makeAxis(QStringLiteral("Underlying"), bounds.min_y, bounds.max_y);
    chart->addAxis(x_axis, Qt::AlignBottom);
    chart->addAxis(y_axis, Qt::AlignLeft);
    for (auto* line : lines) {
        line->attachAxis(x_axis);
        line->attachAxis(y_axis);
    }
    for (auto* line : {strike_line, barrier_line}) {
        if (!line) continue;
        line->attachAxis(x_axis);
        line->attachAxis(y_axis);
    }

    applyTheme(chart);
    // Individual paths would swamp the legend.
    for (auto* line : lines) hideLegendEntry(chart, line);
    chart->legend()->setVisible(strike_line || barrier_line);
    return chart;
}

QChart* convergenceChart(const QJsonArray& convergence, double reference_price,
                         const QString& title) {
    if (convergence.isEmpty()) {
        return emptyChart(title, QStringLiteral("ask for convergence points to populate this"));
    }

    auto* chart = new QChart;
    chart->setTitle(title);

    auto* estimate = new QLineSeries;
    estimate->setName(QStringLiteral("Estimate"));
    auto* upper = new QLineSeries;
    auto* lower = new QLineSeries;

    Bounds bounds;
    for (const auto& item : convergence) {
        const QJsonObject point = item.toObject();
        const double paths = point.value(QStringLiteral("paths")).toDouble();
        const double price = point.value(QStringLiteral("price")).toDouble();
        const double se = point.value(QStringLiteral("std_error")).toDouble();
        if (!(paths > 0.0) || !std::isfinite(price)) continue;

        estimate->append(paths, price);
        upper->append(paths, price + 2.0 * se);
        lower->append(paths, price - 2.0 * se);
        bounds.add(paths, price + 2.0 * se);
        bounds.add(paths, price - 2.0 * se);
    }

    if (estimate->count() == 0) {
        delete chart;
        delete estimate;
        delete upper;
        delete lower;
        return emptyChart(title, QStringLiteral("convergence points were unusable"));
    }

    // Two standard errors is a roughly 95% band.
    auto* band = new QAreaSeries(upper, lower);
    band->setName(QStringLiteral("±2 standard errors"));
    QColor fill = kPrimary;
    fill.setAlpha(45);
    band->setBrush(QBrush(fill));
    band->setPen(QPen(Qt::NoPen));
    chart->addSeries(band);

    QPen estimate_pen(kPrimary);
    estimate_pen.setWidth(2);
    estimate->setPen(estimate_pen);
    chart->addSeries(estimate);

    QLineSeries* reference = nullptr;
    if (reference_price > 0.0) {
        reference = new QLineSeries;
        reference->setName(QStringLiteral("Closed form"));
        reference->append(bounds.min_x, reference_price);
        reference->append(bounds.max_x, reference_price);
        QPen pen(kQuaternary);
        pen.setWidth(2);
        pen.setStyle(Qt::DashLine);
        reference->setPen(pen);
        chart->addSeries(reference);
        bounds.add(bounds.min_x, reference_price);
    }

    auto* x_axis = makeAxis(QStringLiteral("Paths"), bounds.min_x, bounds.max_x, false);
    auto* y_axis = makeAxis(QStringLiteral("Price"), bounds.min_y, bounds.max_y);
    chart->addAxis(x_axis, Qt::AlignBottom);
    chart->addAxis(y_axis, Qt::AlignLeft);

    for (QAbstractSeries* s : chart->series()) {
        s->attachAxis(x_axis);
        s->attachAxis(y_axis);
    }

    applyTheme(chart);
    return chart;
}

QChart* historyChart(const QJsonObject& history, const QString& symbol) {
    const QJsonArray candles = history.value(QStringLiteral("candles")).toArray();
    const QString title = QStringLiteral("%1 daily close").arg(symbol);
    if (candles.isEmpty()) {
        return emptyChart(title, QStringLiteral("no history returned"));
    }

    auto* chart = new QChart;
    auto* line = new QLineSeries;
    line->setName(symbol);

    double min_y = std::numeric_limits<double>::max();
    double max_y = std::numeric_limits<double>::lowest();
    qint64 min_t = std::numeric_limits<qint64>::max();
    qint64 max_t = std::numeric_limits<qint64>::lowest();

    for (const auto& item : candles) {
        const QJsonObject candle = item.toObject();
        const qint64 t = static_cast<qint64>(candle.value(QStringLiteral("t")).toDouble());
        const double close = candle.value(QStringLiteral("c")).toDouble();
        if (!(close > 0.0) || t <= 0) continue;

        line->append(static_cast<double>(t) * 1000.0, close);  // Qt wants milliseconds
        min_y = std::min(min_y, close);
        max_y = std::max(max_y, close);
        min_t = std::min(min_t, t);
        max_t = std::max(max_t, t);
    }

    if (line->count() == 0) {
        delete chart;
        delete line;
        return emptyChart(title, QStringLiteral("history contained no usable closes"));
    }

    QPen pen(kPrimary);
    pen.setWidth(2);
    line->setPen(pen);
    chart->addSeries(line);

    auto* x_axis = new QDateTimeAxis;
    x_axis->setFormat(QStringLiteral("MMM yyyy"));
    x_axis->setTitleText(QStringLiteral("Date"));
    x_axis->setGridLineColor(kGrid);
    x_axis->setRange(QDateTime::fromSecsSinceEpoch(min_t), QDateTime::fromSecsSinceEpoch(max_t));

    auto* y_axis = makeAxis(QStringLiteral("Close"), min_y, max_y);

    chart->addAxis(x_axis, Qt::AlignBottom);
    chart->addAxis(y_axis, Qt::AlignLeft);
    line->attachAxis(x_axis);
    line->attachAxis(y_axis);

    QString subtitle = title;
    if (history.value(QStringLiteral("historical_vol")).isDouble()) {
        subtitle += QStringLiteral("   historical vol %1%")
                        .arg(history.value(QStringLiteral("historical_vol")).toDouble() * 100.0,
                             0, 'f', 1);
    }
    if (history.value(QStringLiteral("stale")).toBool()) {
        subtitle += QStringLiteral("   (cached)");
    }
    chart->setTitle(subtitle);

    applyTheme(chart);
    chart->legend()->setVisible(false);
    return chart;
}

QChart* smileChart(const QJsonObject& implied, double spot, const QString& title) {
    const QJsonArray results = implied.value(QStringLiteral("results")).toArray();
    if (results.isEmpty()) {
        return emptyChart(title, QStringLiteral("no implied volatilities returned"));
    }

    // Group by expiry so each maturity gets its own series.
    QMap<double, QVector<QPointF>> reliable;
    QVector<QPointF> flagged;
    Bounds bounds;

    for (const auto& item : results) {
        const QJsonObject r = item.toObject();
        const double strike = r.value(QStringLiteral("strike")).toDouble();
        if (!(strike > 0.0)) continue;

        if (r.value(QStringLiteral("converged")).toBool()) {
            const double vol = r.value(QStringLiteral("implied_vol")).toDouble() * 100.0;
            const double expiry = r.value(QStringLiteral("expiry")).toDouble();
            reliable[expiry].append(QPointF(strike, vol));
            bounds.add(strike, vol);
        } else if (r.contains(QStringLiteral("unreliable_vol"))) {
            // Kept visible but visually separated, since the solver said the
            // price barely constrains volatility here.
            const double vol = r.value(QStringLiteral("unreliable_vol")).toDouble() * 100.0;
            flagged.append(QPointF(strike, vol));
            bounds.add(strike, vol);
        }
    }

    if (bounds.empty) {
        return emptyChart(title, QStringLiteral("no strike produced a usable implied volatility"));
    }

    auto* chart = new QChart;
    chart->setTitle(title);

    int index = 0;
    QVector<QAbstractSeries*> attach;

    for (auto it = reliable.constBegin(); it != reliable.constEnd(); ++it) {
        QVector<QPointF> points = it.value();
        std::sort(points.begin(), points.end(),
                  [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });

        auto* line = new QLineSeries;
        line->setName(QStringLiteral("%1 y").arg(it.key(), 0, 'f', 2));
        for (const auto& p : points) line->append(p);
        QPen pen(paletteColor(index));
        pen.setWidth(2);
        line->setPen(pen);
        chart->addSeries(line);
        attach.append(line);

        auto* dots = new QScatterSeries;
        dots->setMarkerSize(7.0);
        dots->setColor(paletteColor(index));
        dots->setBorderColor(Qt::white);
        for (const auto& p : points) dots->append(p);
        chart->addSeries(dots);
        attach.append(dots);
        hideLegendEntry(chart, dots);

        ++index;
    }

    if (!flagged.isEmpty()) {
        auto* dots = new QScatterSeries;
        dots->setName(QStringLiteral("low sensitivity"));
        dots->setMarkerSize(7.0);
        dots->setColor(kMuted);
        dots->setBorderColor(kMuted);
        for (const auto& p : flagged) dots->append(p);
        chart->addSeries(dots);
        attach.append(dots);
    }

    QLineSeries* spot_line = nullptr;
    if (spot > 0.0) {
        spot_line = new QLineSeries;
        spot_line->setName(QStringLiteral("Spot"));
        spot_line->append(spot, bounds.min_y);
        spot_line->append(spot, bounds.max_y);
        QPen pen(kMuted);
        pen.setStyle(Qt::DotLine);
        pen.setWidth(1);
        spot_line->setPen(pen);
        chart->addSeries(spot_line);
        attach.append(spot_line);
        bounds.add(spot, bounds.min_y);
    }

    auto* x_axis = makeAxis(QStringLiteral("Strike"), bounds.min_x, bounds.max_x);
    auto* y_axis = makeAxis(QStringLiteral("Implied volatility (%)"), bounds.min_y, bounds.max_y);
    chart->addAxis(x_axis, Qt::AlignBottom);
    chart->addAxis(y_axis, Qt::AlignLeft);
    for (auto* s : attach) {
        s->attachAxis(x_axis);
        s->attachAxis(y_axis);
    }

    applyTheme(chart);
    return chart;
}

QChart* heatmapChart(const QVector<HeatmapRow>& rows, const QString& greek,
                     const QString& title) {
    if (rows.isEmpty()) {
        return emptyChart(title, QStringLiteral("no time slices to plot"));
    }

    // Qt Charts has no native heatmap, so each time slice is drawn as its own
    // line, coloured along a sequential ramp. It reads as a surface without
    // pulling in Qt DataVisualization.
    auto* chart = new QChart;
    chart->setTitle(title);

    Bounds bounds;
    QVector<QLineSeries*> lines;

    const int n = rows.size();
    for (int i = 0; i < n; ++i) {
        const Series series = extractSeries(rows[i].curve, QStringLiteral("greeks.") + greek,
                                            QStringLiteral("%1 y").arg(rows[i].expiry, 0, 'f', 2));
        if (series.points.isEmpty()) continue;

        // Blue (near expiry) through to orange (far from expiry).
        const double f = n > 1 ? static_cast<double>(i) / (n - 1) : 0.0;
        const QColor color(static_cast<int>(31 + f * (255 - 31)),
                           static_cast<int>(119 + f * (127 - 119)),
                           static_cast<int>(180 + f * (14 - 180)));

        for (const auto& p : series.points) bounds.add(p.x(), p.y());
        auto* line = lineFrom(series, color, 2);
        chart->addSeries(line);
        lines.append(line);
    }

    if (lines.isEmpty()) {
        delete chart;
        return emptyChart(title, QStringLiteral("no slice returned this Greek"));
    }

    auto* x_axis = makeAxis(QStringLiteral("Spot"), bounds.min_x, bounds.max_x, false);
    auto* y_axis = makeAxis(greek, bounds.min_y, bounds.max_y);
    chart->addAxis(x_axis, Qt::AlignBottom);
    chart->addAxis(y_axis, Qt::AlignLeft);
    for (auto* line : lines) {
        line->attachAxis(x_axis);
        line->attachAxis(y_axis);
    }

    applyTheme(chart);
    chart->legend()->setVisible(lines.size() <= 8);
    return chart;
}

}  // namespace charts
