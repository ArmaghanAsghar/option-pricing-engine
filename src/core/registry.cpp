#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "pricer/engines.hpp"
#include "pricer/pricer.hpp"

namespace pricer {

const EngineRegistry& EngineRegistry::instance() {
    static const EngineRegistry registry;
    return registry;
}

std::unique_ptr<Pricer> EngineRegistry::create(const std::string& name) const {
    if (name == "black_scholes" || name == "bs") return std::make_unique<BlackScholesPricer>();
    if (name == "binomial" || name == "crr") return std::make_unique<BinomialPricer>();
    if (name == "monte_carlo" || name == "mc") return std::make_unique<MonteCarloPricer>();
    throw std::invalid_argument("unknown engine: " + name);
}

bool EngineRegistry::has(const std::string& name) const {
    return name == "black_scholes" || name == "bs" || name == "binomial" ||
           name == "crr" || name == "monte_carlo" || name == "mc";
}

std::vector<EngineInfo> EngineRegistry::list() const {
    return {
        {"black_scholes",
         "Closed-form Black-Scholes-Merton with analytic Greeks",
         {"vanilla"},
         {"european"},
         false},
        {"binomial",
         "Cox-Ross-Rubinstein tree; handles early exercise",
         {"vanilla"},
         {"european", "american"},
         false},
        {"monte_carlo",
         "Multithreaded GBM simulation with antithetic variates",
         {"vanilla", "asian", "barrier"},
         {"european"},
         true},
    };
}

std::string EngineRegistry::default_engine_for(const Instrument& inst) const {
    if (inst.kind == PayoffKind::Vanilla) {
        return inst.exercise == ExerciseStyle::American ? "binomial" : "black_scholes";
    }
    return "monte_carlo";  // path-dependent payoffs
}

PricingResult finite_difference_greeks(const Pricer& engine, const Instrument& inst,
                                       const Market& mkt, const PricingParams& params,
                                       const PricingResult& base) {
    PricingResult r = base;

    // Reusing the seed makes the bumped runs share their random draws, so most
    // of the Monte Carlo noise cancels in the difference.
    PricingParams bumped = params;
    bumped.greeks = false;
    bumped.return_paths = 0;
    bumped.path_steps = 0;
    bumped.convergence.clear();

    const auto reprice = [&](const Market& m) {
        return engine.price(inst, m, bumped).price;
    };
    const auto reprice_inst = [&](const Instrument& i, const Market& m) {
        return engine.price(i, m, bumped).price;
    };

    // Relative bump on spot, absolute on the rest.
    const double h_s = std::max(1e-4, mkt.spot * 1e-3);
    const double h_v = 1e-4;
    const double h_r = 1e-4;
    const double h_t = std::min(1.0 / 365.0, inst.expiry / 2.0);

    Market up = mkt, down = mkt;
    up.spot += h_s;
    down.spot -= h_s;

    double p_up = 0.0, p_down = 0.0;
    bool spot_ok = true;
    try {
        p_up = reprice(up);
        p_down = reprice(down);
    } catch (const std::invalid_argument&) {
        // A bump can push spot across a barrier and make the contract invalid.
        spot_ok = false;
    }

    if (spot_ok) {
        r.delta = (p_up - p_down) / (2.0 * h_s);
        r.gamma = (p_up - 2.0 * base.price + p_down) / (h_s * h_s);
    }

    Market v_up = mkt, v_down = mkt;
    v_up.vol += h_v;
    v_down.vol = std::max(0.0, mkt.vol - h_v);
    if (v_down.vol < mkt.vol) {
        const double span = v_up.vol - v_down.vol;
        r.vega = (reprice(v_up) - reprice(v_down)) / span / 100.0;  // per vol point
    }

    Market r_up = mkt, r_down = mkt;
    r_up.rate += h_r;
    r_down.rate -= h_r;
    r.rho = (reprice(r_up) - reprice(r_down)) / (2.0 * h_r) / 100.0;  // per 1% rate

    // Theta is the change as the clock advances, hence the sign flip.
    if (inst.expiry > h_t) {
        Instrument shorter = inst;
        shorter.expiry = inst.expiry - h_t;
        r.theta = (reprice_inst(shorter, mkt) - base.price) / h_t / 365.0;  // per day
    }

    return r;
}

}  // namespace pricer
