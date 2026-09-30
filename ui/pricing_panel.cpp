#include "pricing_panel.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {

QDoubleSpinBox* makeSpin(double min, double max, double value, int decimals,
                         double step, const QString& suffix = {}) {
    auto* box = new QDoubleSpinBox;
    box->setRange(min, max);
    box->setDecimals(decimals);
    box->setSingleStep(step);
    box->setValue(value);
    if (!suffix.isEmpty()) box->setSuffix(suffix);
    box->setKeyboardTracking(false);  // do not reprice on every keystroke
    return box;
}

QSpinBox* makeInt(int min, int max, int value, const QString& suffix = {}) {
    auto* box = new QSpinBox;
    box->setRange(min, max);
    box->setValue(value);
    if (!suffix.isEmpty()) box->setSuffix(suffix);
    box->setKeyboardTracking(false);
    return box;
}

/// A labelled row that can be hidden as a unit.
QWidget* rowWith(const QString& label, QWidget* field, QWidget* extra = nullptr) {
    auto* wrapper = new QWidget;
    auto* layout = new QHBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* text = new QLabel(label);
    text->setMinimumWidth(110);
    layout->addWidget(text);
    layout->addWidget(field, 1);
    if (extra) layout->addWidget(extra, 1);
    return wrapper;
}

}  // namespace

PricingPanel::PricingPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(buildContractGroup());
    layout->addWidget(buildMarketGroup());
    layout->addWidget(buildEngineGroup());
    layout->addStretch(1);

    onPayoffKindChanged();
}

QGroupBox* PricingPanel::buildContractGroup() {
    auto* group = new QGroupBox(QStringLiteral("Contract"));
    auto* layout = new QVBoxLayout(group);

    payoff_kind_ = new QComboBox;
    payoff_kind_->addItem(QStringLiteral("Vanilla"), QStringLiteral("vanilla"));
    payoff_kind_->addItem(QStringLiteral("Asian (average price)"), QStringLiteral("asian"));
    payoff_kind_->addItem(QStringLiteral("Barrier"), QStringLiteral("barrier"));

    option_type_ = new QComboBox;
    option_type_->addItem(QStringLiteral("Call"), QStringLiteral("call"));
    option_type_->addItem(QStringLiteral("Put"), QStringLiteral("put"));

    exercise_ = new QComboBox;
    exercise_->addItem(QStringLiteral("European"), QStringLiteral("european"));
    exercise_->addItem(QStringLiteral("American"), QStringLiteral("american"));

    strike_ = makeSpin(0.01, 1e6, 100.0, 2, 1.0);
    expiry_ = makeSpin(0.003, 30.0, 1.0, 4, 0.25, QStringLiteral(" y"));

    layout->addWidget(rowWith(QStringLiteral("Payoff"), payoff_kind_));
    layout->addWidget(rowWith(QStringLiteral("Type"), option_type_, exercise_));
    layout->addWidget(rowWith(QStringLiteral("Strike"), strike_));
    layout->addWidget(rowWith(QStringLiteral("Expiry"), expiry_));

    averaging_steps_ = makeInt(1, 2000, 252);
    asian_row_ = rowWith(QStringLiteral("Averaging obs"), averaging_steps_);
    layout->addWidget(asian_row_);

    barrier_direction_ = new QComboBox;
    barrier_direction_->addItem(QStringLiteral("Up and out"), QStringLiteral("up_out"));
    barrier_direction_->addItem(QStringLiteral("Up and in"), QStringLiteral("up_in"));
    barrier_direction_->addItem(QStringLiteral("Down and out"), QStringLiteral("down_out"));
    barrier_direction_->addItem(QStringLiteral("Down and in"), QStringLiteral("down_in"));
    barrier_level_ = makeSpin(0.01, 1e6, 130.0, 2, 1.0);
    rebate_ = makeSpin(0.0, 1e6, 0.0, 2, 1.0);

    auto* barrier_wrapper = new QWidget;
    auto* barrier_layout = new QVBoxLayout(barrier_wrapper);
    barrier_layout->setContentsMargins(0, 0, 0, 0);
    barrier_layout->addWidget(rowWith(QStringLiteral("Barrier"), barrier_direction_));
    barrier_layout->addWidget(rowWith(QStringLiteral("Level"), barrier_level_, rebate_));
    barrier_row_ = barrier_wrapper;
    layout->addWidget(barrier_row_);

    connect(payoff_kind_, &QComboBox::currentIndexChanged, this,
            &PricingPanel::onPayoffKindChanged);
    for (auto* box : {payoff_kind_, option_type_, exercise_, barrier_direction_}) {
        connect(box, &QComboBox::currentIndexChanged, this, [this] { emit changed(); });
    }
    for (auto* box : {strike_, expiry_, barrier_level_, rebate_}) {
        connect(box, &QDoubleSpinBox::valueChanged, this, [this] { emit changed(); });
    }
    connect(averaging_steps_, &QSpinBox::valueChanged, this, [this] { emit changed(); });

    return group;
}

QGroupBox* PricingPanel::buildMarketGroup() {
    auto* group = new QGroupBox(QStringLiteral("Market"));
    auto* layout = new QVBoxLayout(group);

    spot_ = makeSpin(0.01, 1e6, 100.0, 2, 1.0);
    vol_ = makeSpin(0.1, 500.0, 20.0, 2, 1.0, QStringLiteral(" %"));
    rate_ = makeSpin(-10.0, 50.0, 5.0, 3, 0.25, QStringLiteral(" %"));
    dividend_ = makeSpin(0.0, 50.0, 0.0, 3, 0.25, QStringLiteral(" %"));

    layout->addWidget(rowWith(QStringLiteral("Spot"), spot_));
    layout->addWidget(rowWith(QStringLiteral("Volatility"), vol_));
    layout->addWidget(rowWith(QStringLiteral("Risk-free rate"), rate_));
    layout->addWidget(rowWith(QStringLiteral("Dividend yield"), dividend_));

    for (auto* box : {spot_, vol_, rate_, dividend_}) {
        connect(box, &QDoubleSpinBox::valueChanged, this, [this] { emit changed(); });
    }
    return group;
}

QGroupBox* PricingPanel::buildEngineGroup() {
    auto* group = new QGroupBox(QStringLiteral("Engine"));
    auto* layout = new QVBoxLayout(group);

    engine_ = new QComboBox;
    engine_->addItem(QStringLiteral("Automatic"), QString());
    engine_->addItem(QStringLiteral("Black-Scholes"), QStringLiteral("black_scholes"));
    engine_->addItem(QStringLiteral("Binomial tree"), QStringLiteral("binomial"));
    engine_->addItem(QStringLiteral("Monte Carlo"), QStringLiteral("monte_carlo"));
    layout->addWidget(rowWith(QStringLiteral("Engine"), engine_));

    steps_ = makeInt(8, 20000, 512);
    tree_row_ = rowWith(QStringLiteral("Tree steps"), steps_);
    layout->addWidget(tree_row_);

    paths_thousands_ = makeInt(1, 20000, 200, QStringLiteral("k"));
    seed_ = makeInt(1, 1000000, 42);
    mc_row_ = rowWith(QStringLiteral("Paths"), paths_thousands_, seed_);
    layout->addWidget(mc_row_);

    connect(engine_, &QComboBox::currentIndexChanged, this, [this] {
        onPayoffKindChanged();
        emit changed();
    });
    connect(steps_, &QSpinBox::valueChanged, this, [this] { emit changed(); });
    connect(paths_thousands_, &QSpinBox::valueChanged, this, [this] { emit changed(); });
    connect(seed_, &QSpinBox::valueChanged, this, [this] { emit changed(); });

    return group;
}

void PricingPanel::onPayoffKindChanged() {
    const QString kind = payoffKind();
    const bool is_asian = kind == QStringLiteral("asian");
    const bool is_barrier = kind == QStringLiteral("barrier");

    asian_row_->setVisible(is_asian);
    barrier_row_->setVisible(is_barrier);

    // Only vanilla contracts can be exercised early.
    exercise_->setEnabled(!is_asian && !is_barrier);
    if (is_asian || is_barrier) exercise_->setCurrentIndex(0);

    const QString engine = engineName();
    const bool tree = engine == QStringLiteral("binomial");
    const bool mc = engine == QStringLiteral("monte_carlo") ||
                    (engine.isEmpty() && (is_asian || is_barrier));

    tree_row_->setVisible(tree || engine.isEmpty());
    mc_row_->setVisible(mc);
}

QString PricingPanel::payoffKind() const {
    return payoff_kind_->currentData().toString();
}

QString PricingPanel::engineName() const {
    return engine_->currentData().toString();
}

bool PricingPanel::isStochastic() const {
    const QString engine = engineName();
    if (engine == QStringLiteral("monte_carlo")) return true;
    if (!engine.isEmpty()) return false;
    // Automatic: path-dependent payoffs resolve to Monte Carlo.
    const QString kind = payoffKind();
    return kind == QStringLiteral("asian") || kind == QStringLiteral("barrier");
}

double PricingPanel::barrier() const {
    return payoffKind() == QStringLiteral("barrier") ? barrier_level_->value() : 0.0;
}

std::pair<double, double> PricingPanel::spotRange() const {
    // Wide enough to show both wings of the payoff, anchored on strike and spot.
    const double centre = std::max(spot_->value(), strike_->value());
    double lo = std::max(0.01, centre * 0.4);
    double hi = centre * 1.6;

    if (payoffKind() == QStringLiteral("barrier")) {
        // Keep the plotted range on the live side of the barrier, since the
        // other side is not a valid contract.
        const QString direction = barrier_direction_->currentData().toString();
        const double level = barrier_level_->value();
        if (direction.startsWith(QStringLiteral("up"))) {
            hi = std::min(hi, level * 0.995);
        } else {
            lo = std::max(lo, level * 1.005);
        }
        if (!(hi > lo)) {
            lo = std::max(0.01, level * 0.5);
            hi = level * 0.995;
        }
    }
    return {lo, hi};
}

QJsonObject PricingPanel::priceRequest(bool want_paths_and_convergence) const {
    QJsonObject instrument{
        {QStringLiteral("type"), payoffKind()},
        {QStringLiteral("option"), option_type_->currentData().toString()},
        {QStringLiteral("exercise"), exercise_->currentData().toString()},
        {QStringLiteral("strike"), strike_->value()},
        {QStringLiteral("expiry"), expiry_->value()}};

    if (payoffKind() == QStringLiteral("asian")) {
        instrument[QStringLiteral("averaging_steps")] = averaging_steps_->value();
    }
    if (payoffKind() == QStringLiteral("barrier")) {
        instrument[QStringLiteral("barrier")] = barrier_level_->value();
        instrument[QStringLiteral("barrier_direction")] =
            barrier_direction_->currentData().toString();
        instrument[QStringLiteral("rebate")] = rebate_->value();
        instrument[QStringLiteral("monitoring_steps")] = 252;
    }

    const QJsonObject market{{QStringLiteral("spot"), spot_->value()},
                             {QStringLiteral("vol"), vol_->value() / 100.0},
                             {QStringLiteral("rate"), rate_->value() / 100.0},
                             {QStringLiteral("dividend"), dividend_->value() / 100.0}};

    QJsonObject params{{QStringLiteral("greeks"), true},
                       {QStringLiteral("steps"), steps_->value()},
                       {QStringLiteral("paths"), paths_thousands_->value() * 1000},
                       {QStringLiteral("seed"), seed_->value()}};

    if (want_paths_and_convergence && isStochastic()) {
        params[QStringLiteral("return_paths")] = 60;
        // A vanilla only needs one terminal draw, which would plot as a
        // straight line. This subdivides the path for the chart without
        // changing the price.
        params[QStringLiteral("path_steps")] = 100;
        // One thread keeps the convergence series deterministic.
        params[QStringLiteral("threads")] = 1;

        const long total = static_cast<long>(paths_thousands_->value()) * 1000;
        QJsonArray checkpoints;
        for (long n = 1000; n < total; n *= 2) checkpoints.append(static_cast<double>(n));
        checkpoints.append(static_cast<double>(total));
        params[QStringLiteral("convergence")] = checkpoints;
    }

    QJsonObject request{{QStringLiteral("instrument"), instrument},
                        {QStringLiteral("market"), market},
                        {QStringLiteral("params"), params}};
    if (!engineName().isEmpty()) request[QStringLiteral("engine")] = engineName();
    return request;
}

QJsonObject PricingPanel::curveRequest(const QString& axis, double from, double to,
                                       int points) const {
    QJsonObject request = priceRequest(false);
    request[QStringLiteral("axis")] = axis;
    request[QStringLiteral("from")] = from;
    request[QStringLiteral("to")] = to;
    request[QStringLiteral("points")] = points;

    // Curves are many repriced points, so a stochastic engine needs a smaller
    // path budget to stay responsive.
    if (isStochastic()) {
        QJsonObject params = request[QStringLiteral("params")].toObject();
        params[QStringLiteral("paths")] = 20000;
        params[QStringLiteral("return_paths")] = 0;
        params.remove(QStringLiteral("convergence"));
        request[QStringLiteral("params")] = params;
    }
    return request;
}

void PricingPanel::applySnapshot(const QJsonObject& suggested_market) {
    const double spot = suggested_market.value(QStringLiteral("spot")).toDouble();
    if (spot > 0.0) {
        spot_->setValue(spot);
        // Default the strike to at the money so the first plot is informative.
        strike_->setValue(spot);
    }
    const double vol = suggested_market.value(QStringLiteral("vol")).toDouble();
    if (vol > 0.0) vol_->setValue(std::clamp(vol * 100.0, 0.1, 500.0));

    const double dividend = suggested_market.value(QStringLiteral("dividend")).toDouble();
    if (dividend >= 0.0) dividend_->setValue(std::clamp(dividend * 100.0, 0.0, 50.0));

    if (payoffKind() == QStringLiteral("barrier") && spot > 0.0) {
        barrier_level_->setValue(spot * 1.3);
    }
}
