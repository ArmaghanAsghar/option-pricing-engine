#pragma once

#include <string>
#include <vector>

#include "pricer/pricer.hpp"
#include "pricer/types.hpp"

namespace pricer {

/// Closed-form Black-Scholes-Merton for European vanillas, with analytic Greeks.
/// Doubles as the reference every other engine is tested against.
class BlackScholesPricer : public Pricer {
public:
    PricingResult price(const Instrument& inst, const Market& mkt,
                        const PricingParams& params) const override;
    std::string name() const override { return "black_scholes"; }
    bool supports(const Instrument& inst) const override;

    /// Price only, no Greeks. Used by the implied-vol solver's inner loop.
    static double price_only(const Instrument& inst, const Market& mkt);
};

/// Cox-Ross-Rubinstein binomial tree. Handles American exercise.
class BinomialPricer : public Pricer {
public:
    PricingResult price(const Instrument& inst, const Market& mkt,
                        const PricingParams& params) const override;
    std::string name() const override { return "binomial"; }
    bool supports(const Instrument& inst) const override;
};

/// Multithreaded Monte Carlo under geometric Brownian motion.
/// Antithetic variates; per-thread RNG streams keyed on (seed, thread) so a
/// given seed and thread count always reproduce the same estimate.
class MonteCarloPricer : public Pricer {
public:
    PricingResult price(const Instrument& inst, const Market& mkt,
                        const PricingParams& params) const override;
    std::string name() const override { return "monte_carlo"; }
    bool supports(const Instrument& inst) const override;
};

struct ImpliedVolQuote {
    double strike = 0.0;
    double expiry = 0.0;
    OptionType option = OptionType::Call;
    double market_price = 0.0;
};

struct ImpliedVolResult {
    double strike = 0.0;
    double expiry = 0.0;
    OptionType option = OptionType::Call;
    double market_price = 0.0;
    bool converged = false;
    double implied_vol = 0.0;
    std::string error;

    /// Roughly how far the implied volatility could be off given that the price
    /// is only known to floating-point precision. Large for deep in-the-money
    /// or very short-dated quotes, where almost none of the price is time
    /// value. A smile chart should drop or widen points with a large value.
    double vol_uncertainty = 0.0;

    /// False when `vol_uncertainty` is too large for the estimate to mean
    /// anything, even if a price-matching volatility was found.
    bool well_conditioned = false;
};

/// Inverts Black-Scholes for volatility. Newton from a Brenner-Subrahmanyam
/// seed, falling back to Brent on a bracketed interval when Newton stalls.
/// `tol` is in volatility units, not price: an absolute price tolerance would
/// be larger than the entire value of a deep out-of-the-money option.
ImpliedVolResult implied_volatility(const ImpliedVolQuote& quote, const Market& mkt,
                                    double tol = 1e-10, int max_iter = 100);

/// Registers every built-in engine. Called once by EngineRegistry.
void register_builtin_engines();

}  // namespace pricer
