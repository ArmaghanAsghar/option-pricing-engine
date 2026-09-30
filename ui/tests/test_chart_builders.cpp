#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineSeries>
#include <QTest>
#include <QValueAxis>
#include <QtCharts/QChart>

#include "chart_builders.hpp"

namespace {

QJsonObject parse(const QString& json) {
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

/// A curve response shaped like the one /price/curve returns.
QJsonObject sampleCurve() {
    QJsonArray points;
    for (int i = 0; i < 11; ++i) {
        const double x = 50.0 + i * 10.0;
        points.append(QJsonObject{
            {"x", x},
            {"price", qMax(0.0, x - 100.0) + 5.0},
            {"payoff", qMax(0.0, x - 100.0)},
            {"greeks", QJsonObject{{"delta", i / 10.0}, {"gamma", 0.02}}}});
    }
    return QJsonObject{{"axis", "spot"}, {"engine", "black_scholes"}, {"points", points}};
}

int lineSeriesCount(QChart* chart) {
    int n = 0;
    for (QAbstractSeries* s : chart->series()) {
        if (qobject_cast<QLineSeries*>(s)) ++n;
    }
    return n;
}

}  // namespace

class ChartBuilderTests : public QObject {
    Q_OBJECT

private slots:
    void extractsATopLevelField() {
        const auto series = charts::extractSeries(sampleCurve(), "price", "Price");
        QCOMPARE(series.points.size(), 11);
        QCOMPARE(series.name, QStringLiteral("Price"));
        QCOMPARE(series.points.first().x(), 50.0);
        QCOMPARE(series.points.last().x(), 150.0);
    }

    void extractsANestedField() {
        const auto series = charts::extractSeries(sampleCurve(), "greeks.delta", "Delta");
        QCOMPARE(series.points.size(), 11);
        QCOMPARE(series.points.first().y(), 0.0);
        QCOMPARE(series.points.last().y(), 1.0);
    }

    void missingFieldsYieldAnEmptySeries() {
        QVERIFY(charts::extractSeries(sampleCurve(), "greeks.vomma", "Vomma").points.isEmpty());
        QVERIFY(charts::extractSeries(sampleCurve(), "nonsense", "None").points.isEmpty());
    }

    void pointsWithErrorsAreSkipped() {
        // One bad grid point must not truncate or poison the whole series.
        QJsonObject curve = sampleCurve();
        QJsonArray points = curve.value("points").toArray();
        points[3] = QJsonObject{{"x", 80.0}, {"error", "spot is already past the barrier"}};
        curve["points"] = points;

        const auto series = charts::extractSeries(curve, "price", "Price");
        QCOMPARE(series.points.size(), 10);
        for (const auto& p : series.points) QVERIFY(p.x() != 80.0);
    }

    void payoffChartHasValueAndPayoffSeries() {
        auto* chart = charts::payoffChart(sampleCurve(), "Test");
        QCOMPARE(lineSeriesCount(chart), 2);
        QCOMPARE(chart->axes(Qt::Horizontal).size(), 1);
        QCOMPARE(chart->axes(Qt::Vertical).size(), 1);
        delete chart;
    }

    void payoffChartSurvivesAnEmptyCurve() {
        const QJsonObject empty{{"axis", "spot"}, {"points", QJsonArray{}}};
        auto* chart = charts::payoffChart(empty, "Test");
        QVERIFY(chart != nullptr);
        QVERIFY(chart->title().contains("no priceable points"));
        delete chart;
    }

    void axisTitleFollowsTheCurveAxis() {
        QJsonObject curve = sampleCurve();
        curve["axis"] = "vol";
        auto* chart = charts::payoffChart(curve, "Test");
        auto* axis = qobject_cast<QValueAxis*>(chart->axes(Qt::Horizontal).first());
        QVERIFY(axis != nullptr);
        QCOMPARE(axis->titleText(), QStringLiteral("Volatility"));
        delete chart;
    }

    void greeksChartDrawsOnlyGreeksThatExist() {
        auto* chart = charts::greeksChart(sampleCurve(), {"delta", "gamma", "vega"}, "Greeks");
        // vega is absent from the fixture, so two series, not three.
        QCOMPARE(lineSeriesCount(chart), 2);
        delete chart;
    }

    void greeksChartReportsWhenThereAreNoGreeks() {
        const QJsonObject bare{
            {"axis", "spot"},
            {"points", QJsonArray{QJsonObject{{"x", 100.0}, {"price", 10.0}}}}};
        auto* chart = charts::greeksChart(bare, {"delta"}, "Greeks");
        QVERIFY(chart->title().contains("no Greeks"));
        delete chart;
    }

    void pathFanScalesTimeAcrossTheExpiry() {
        QJsonArray paths;
        for (int p = 0; p < 3; ++p) {
            QJsonArray path;
            for (int i = 0; i <= 10; ++i) path.append(100.0 + i + p);
            paths.append(path);
        }

        auto* chart = charts::pathFanChart(paths, 2.0, 100.0, 130.0, "Paths");
        // Three paths plus a strike line and a barrier line.
        QCOMPARE(lineSeriesCount(chart), 5);

        auto* axis = qobject_cast<QValueAxis*>(chart->axes(Qt::Horizontal).first());
        QCOMPARE(axis->max(), 2.0);  // the expiry, in years
        delete chart;
    }

    void pathFanOmitsAnAbsentBarrier() {
        QJsonArray paths;
        QJsonArray path;
        for (int i = 0; i <= 5; ++i) path.append(100.0 + i);
        paths.append(path);

        auto* chart = charts::pathFanChart(paths, 1.0, 100.0, 0.0, "Paths");
        QCOMPARE(lineSeriesCount(chart), 2);  // one path plus the strike
        delete chart;
    }

    void pathFanExplainsItselfWhenEmpty() {
        auto* chart = charts::pathFanChart(QJsonArray{}, 1.0, 100.0, 0.0, "Paths");
        QVERIFY(chart->title().contains("sample paths"));
        delete chart;
    }

    void convergenceChartIncludesABandAndAReference() {
        QJsonArray convergence;
        for (int i = 1; i <= 4; ++i) {
            convergence.append(QJsonObject{{"paths", 1000.0 * i},
                                           {"price", 10.4 + 0.01 / i},
                                           {"std_error", 0.05 / i}});
        }
        auto* chart = charts::convergenceChart(convergence, 10.45, "Convergence");
        // Estimate line, reference line, and the area band.
        QCOMPARE(lineSeriesCount(chart), 2);
        QCOMPARE(chart->series().size(), 3);
        delete chart;
    }

    void convergenceChartWorksWithoutAReference() {
        QJsonArray convergence{
            QJsonObject{{"paths", 1000.0}, {"price", 10.4}, {"std_error", 0.05}},
            QJsonObject{{"paths", 2000.0}, {"price", 10.42}, {"std_error", 0.03}}};
        auto* chart = charts::convergenceChart(convergence, 0.0, "Convergence");
        QCOMPARE(lineSeriesCount(chart), 1);
        delete chart;
    }

    void historyChartUsesADateAxis() {
        QJsonArray candles;
        for (int i = 0; i < 30; ++i) {
            candles.append(QJsonObject{{"t", 1700000000.0 + i * 86400.0}, {"c", 400.0 + i}});
        }
        const QJsonObject history{
            {"candles", candles}, {"historical_vol", 0.23}, {"stale", false}};

        auto* chart = charts::historyChart(history, "SPY");
        QCOMPARE(chart->series().size(), 1);
        QVERIFY(chart->title().contains("SPY"));
        QVERIFY(chart->title().contains("23.0"));  // the volatility annotation
        delete chart;
    }

    void historyChartFlagsCachedData() {
        QJsonArray candles{QJsonObject{{"t", 1700000000.0}, {"c", 400.0}},
                           QJsonObject{{"t", 1700086400.0}, {"c", 401.0}}};
        auto* chart = charts::historyChart(QJsonObject{{"candles", candles}, {"stale", true}},
                                           "SPY");
        QVERIFY(chart->title().contains("cached"));
        delete chart;
    }

    void historyChartDropsBadTicks() {
        QJsonArray candles{QJsonObject{{"t", 1700000000.0}, {"c", 400.0}},
                           QJsonObject{{"t", 1700086400.0}, {"c", 0.0}},  // bad
                           QJsonObject{{"t", 1700172800.0}, {"c", 402.0}}};
        auto* chart = charts::historyChart(QJsonObject{{"candles", candles}}, "SPY");
        auto* line = qobject_cast<QLineSeries*>(chart->series().first());
        QCOMPARE(line->count(), 2);
        delete chart;
    }

    void smileSeparatesReliableFromFlaggedPoints() {
        const QJsonObject implied = parse(R"({
          "spot": 100,
          "results": [
            {"strike": 90,  "expiry": 0.25, "converged": true,  "implied_vol": 0.26},
            {"strike": 100, "expiry": 0.25, "converged": true,  "implied_vol": 0.22},
            {"strike": 110, "expiry": 0.25, "converged": true,  "implied_vol": 0.24},
            {"strike": 40,  "expiry": 0.25, "converged": false, "unreliable_vol": 0.9},
            {"strike": 45,  "expiry": 0.25, "converged": false}
          ]})");

        auto* chart = charts::smileChart(implied, 100.0, "Smile");
        // A line and a scatter for the one expiry, a scatter for the flagged
        // point, and the spot marker. The result with no vol at all is dropped.
        QCOMPARE(chart->series().size(), 4);
        delete chart;
    }

    void smileGroupsByExpiry() {
        const QJsonObject implied = parse(R"({
          "results": [
            {"strike": 95,  "expiry": 0.25, "converged": true, "implied_vol": 0.25},
            {"strike": 105, "expiry": 0.25, "converged": true, "implied_vol": 0.23},
            {"strike": 95,  "expiry": 1.00, "converged": true, "implied_vol": 0.21},
            {"strike": 105, "expiry": 1.00, "converged": true, "implied_vol": 0.20}
          ]})");

        auto* chart = charts::smileChart(implied, 0.0, "Smile");
        QCOMPARE(lineSeriesCount(chart), 2);  // one per expiry, no spot marker
        delete chart;
    }

    void smileReportsWhenNothingSolved() {
        const QJsonObject implied = parse(R"({
          "results": [{"strike": 100, "expiry": 0.25, "converged": false}]})");
        auto* chart = charts::smileChart(implied, 100.0, "Smile");
        QVERIFY(chart->title().contains("no strike"));
        delete chart;
    }

    void heatmapDrawsOneSeriesPerTimeSlice() {
        QVector<charts::HeatmapRow> rows;
        for (double t : {0.1, 0.5, 1.0}) rows.append({t, sampleCurve()});

        auto* chart = charts::heatmapChart(rows, "delta", "Delta surface");
        QCOMPARE(lineSeriesCount(chart), 3);
        delete chart;
    }

    void heatmapHandlesAMissingGreek() {
        QVector<charts::HeatmapRow> rows{{0.5, sampleCurve()}};
        auto* chart = charts::heatmapChart(rows, "vanna", "Vanna surface");
        QVERIFY(chart->title().contains("no slice"));
        delete chart;
    }

    void aFlatSeriesStillGetsAVisibleRange() {
        // A constant series would otherwise collapse the axis to zero height.
        QJsonArray points;
        for (int i = 0; i < 5; ++i) {
            points.append(QJsonObject{{"x", 90.0 + i}, {"price", 7.0}, {"payoff", 7.0}});
        }
        auto* chart =
            charts::payoffChart(QJsonObject{{"axis", "spot"}, {"points", points}}, "Flat");
        auto* axis = qobject_cast<QValueAxis*>(chart->axes(Qt::Vertical).first());
        QVERIFY(axis->max() > axis->min());
        delete chart;
    }
};

QTEST_MAIN(ChartBuilderTests)
#include "test_chart_builders.moc"
