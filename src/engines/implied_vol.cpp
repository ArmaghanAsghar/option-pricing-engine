#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

#include "pricer/engines.hpp"
#include "pricer/math.hpp"

namespace pricer {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kVolFloor = 1e-9;
constexpr double kVolCeiling = 10.0;  // 1000% annualized

Instrument to_instrument(const ImpliedVolQuote& q) {
    Instrument inst;
    inst.kind = PayoffKind::Vanilla;
    inst.exercise = ExerciseStyle::European;
    inst.option = q.option;
    inst.strike = q.strike;
    inst.expiry = q.expiry;
    return inst;
}

/// Arbitrage bounds on a European price. Outside these no volatility exists.
std::pair<double, double> price_bounds(const Instrument& inst, const Market& mkt) {
    const double disc_r = std::exp(-mkt.rate * inst.expiry);
    const double disc_q = std::exp(-mkt.dividend * inst.expiry);
    if (inst.option == OptionType::Call) {
        const double lower = std::max(0.0, mkt.spot * disc_q - inst.strike * disc_r);
        return {lower, mkt.spot * disc_q};
    }
    const double lower = std::max(0.0, inst.strike * disc_r - mkt.spot * disc_q);
    return {lower, inst.strike * disc_r};
}

/// Vega per unit volatility at `vol`, in price units.
double vega_at(const Instrument& inst, const Market& mkt, double vol) {
    Market m = mkt;
    m.vol = vol;
    PricingParams p;
    p.greeks = true;
    const auto r = BlackScholesPricer().price(inst, m, p);
    return r.vega.value_or(0.0) * 100.0;
}

/// Fills in the conditioning fields and decides whether the estimate is
/// meaningful. A price is only known to about `eps * price`; dividing that by
/// vega converts it into a volatility error bar.
void finish(ImpliedVolResult& out, const Instrument& inst, const Market& mkt) {
    constexpr double kEps = 2.22e-16;
    constexpr double kUsableVolError = 1e-4;  // one basis point of volatility

    const double vega = vega_at(inst, mkt, std::max(out.implied_vol, kVolFloor));
    const double price_scale = std::max(std::abs(out.market_price), mkt.spot * kEps);
    const double noise = kEps * price_scale * 16.0;  // a few roundings deep

    if (!(vega > 0.0) || !std::isfinite(vega)) {
        out.vol_uncertainty = std::numeric_limits<double>::infinity();
    } else {
        out.vol_uncertainty = noise / vega;
    }

    out.well_conditioned = std::isfinite(out.vol_uncertainty) &&
                           out.vol_uncertainty < kUsableVolError;

    if (!out.well_conditioned) {
        out.converged = false;
        out.error =
            "the option's value barely depends on volatility here, so implied "
            "volatility cannot be recovered from this price";
    }
}

}  // namespace

ImpliedVolResult implied_volatility(const ImpliedVolQuote& quote, const Market& mkt,
                                    double tol, int max_iter) {
    ImpliedVolResult out;
    out.strike = quote.strike;
    out.expiry = quote.expiry;
    out.option = quote.option;
    out.market_price = quote.market_price;

    const Instrument inst = to_instrument(quote);

    if (!(quote.strike > 0.0) || !(quote.expiry > 0.0)) {
        out.error = "strike and expiry must be positive";
        return out;
    }
    if (!(mkt.spot > 0.0)) {
        out.error = "spot must be positive";
        return out;
    }

    const auto [lower, upper] = price_bounds(inst, mkt);
    // A tiny slack absorbs rounding in quoted prices sitting exactly on a bound.
    const double slack = 1e-10 * std::max(1.0, upper);
    if (quote.market_price < lower - slack) {
        out.error = "market price is below the no-arbitrage lower bound";
        return out;
    }
    if (quote.market_price > upper + slack) {
        out.error = "market price is above the no-arbitrage upper bound";
        return out;
    }

    const auto diff = [&](double vol) {
        Market m = mkt;
        m.vol = vol;
        return BlackScholesPricer::price_only(inst, m) - quote.market_price;
    };

    // `tol` is interpreted in volatility units: the answer is accurate to `tol`
    // vol points. An absolute price tolerance would be meaningless for deep
    // out-of-the-money quotes, whose entire value can be smaller than it.
    const double vol_tol = std::max(tol, 1e-12);

    // Brenner-Subrahmanyam: near the forward, price ~ 0.4 * S * sigma * sqrt(T).
    double vol = std::sqrt(2.0 * kPi / quote.expiry) * quote.market_price / mkt.spot;
    vol = std::clamp(vol, 0.01, 3.0);

    // Newton in volatility space, using analytic vega. Vega collapses far from
    // the money, so bail out to Brent rather than dividing by near-zero.
    for (int i = 0; i < max_iter; ++i) {
        Market m = mkt;
        m.vol = vol;
        PricingParams p;
        p.greeks = true;
        const PricingResult r = BlackScholesPricer().price(inst, m, p);

        const double err = r.price - quote.market_price;
        const double vega_raw = r.vega.value_or(0.0) * 100.0;  // back to per-unit vol
        if (!(vega_raw > 1e-12)) break;

        const double step = err / vega_raw;
        if (!std::isfinite(step)) break;

        // Converged once a further Newton step would not move the volatility.
        if (std::abs(step) < vol_tol) {
            out.converged = true;
            out.implied_vol = vol;
            finish(out, inst, mkt);
            return out;
        }

        const double next = vol - step;
        if (!std::isfinite(next) || next <= kVolFloor || next >= kVolCeiling) break;
        vol = next;
    }

    // Fallback: bracket the root, then bisect. `diff` is strictly increasing in
    // volatility, so expanding the upper end is enough to bracket it.
    try {
        double lo = kVolFloor;
        double hi = 1.0;

        // A price at or below the zero-vol value implies (numerically) zero vol.
        if (diff(lo) >= 0.0) {
            // The quote is at or below the zero-volatility value.
            out.converged = true;
            out.implied_vol = lo;
            finish(out, inst, mkt);
            return out;
        }

        double f_hi = diff(hi);
        int expansions = 0;
        while (f_hi < 0.0 && hi < kVolCeiling && expansions < 20) {
            lo = hi;
            hi = std::min(hi * 2.0, kVolCeiling);
            f_hi = diff(hi);
            ++expansions;
        }
        if (f_hi < 0.0) {
            out.error = "no volatility below 1000% reproduces this price";
            return out;
        }
        out.implied_vol = math::brent(diff, lo, hi, vol_tol, 300);
        out.converged = true;
        finish(out, inst, mkt);
    } catch (const std::exception& e) {
        out.error = std::string("solver failed: ") + e.what();
    }
    return out;
}

}  // namespace pricer
