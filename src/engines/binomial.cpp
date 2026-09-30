#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "pricer/engines.hpp"

namespace pricer {

namespace {

double intrinsic(const Instrument& inst, double spot) {
    return inst.option == OptionType::Call ? std::max(0.0, spot - inst.strike)
                                           : std::max(0.0, inst.strike - spot);
}

/// One CRR pass. Kept separate from `price` so the Greeks can re-run it on
/// bumped inputs with an identical lattice.
double crr_price(const Instrument& inst, const Market& mkt, int steps) {
    const double dt = inst.expiry / steps;
    const double u = std::exp(mkt.vol * std::sqrt(dt));
    const double d = 1.0 / u;
    const double disc = std::exp(-mkt.rate * dt);
    const double growth = std::exp((mkt.rate - mkt.dividend) * dt);

    // With very low vol or very coarse steps the CRR probability can leave
    // [0,1], which makes the tree arbitrageable. Clamp and let the step count
    // handle accuracy.
    double p = (growth - d) / (u - d);
    p = std::clamp(p, 0.0, 1.0);

    // Terminal payoffs, indexed by number of up-moves.
    std::vector<double> values(static_cast<std::size_t>(steps) + 1);
    const double lowest = mkt.spot * std::pow(d, steps);
    const double ratio = u / d;
    double spot = lowest;
    for (int i = 0; i <= steps; ++i) {
        values[static_cast<std::size_t>(i)] = intrinsic(inst, spot);
        spot *= ratio;
    }

    const bool american = inst.exercise == ExerciseStyle::American;
    for (int step = steps - 1; step >= 0; --step) {
        const double level_low = mkt.spot * std::pow(d, step);
        double node_spot = level_low;
        for (int i = 0; i <= step; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            const double cont = disc * (p * values[idx + 1] + (1.0 - p) * values[idx]);
            values[idx] = american ? std::max(cont, intrinsic(inst, node_spot)) : cont;
            node_spot *= ratio;
        }
    }
    return values[0];
}

}  // namespace

bool BinomialPricer::supports(const Instrument& inst) const {
    return inst.kind == PayoffKind::Vanilla;
}

PricingResult BinomialPricer::price(const Instrument& inst, const Market& mkt,
                                    const PricingParams& params) const {
    validate(inst, mkt);
    if (!supports(inst)) {
        throw std::invalid_argument("binomial handles vanilla options only");
    }
    if (params.steps < 1) {
        throw std::invalid_argument("steps must be at least 1");
    }

    PricingResult r;
    r.engine = name();
    r.steps_used = params.steps;
    r.price = crr_price(inst, mkt, params.steps);

    if (!params.greeks) return r;
    return finite_difference_greeks(*this, inst, mkt, params, r);
}

}  // namespace pricer
