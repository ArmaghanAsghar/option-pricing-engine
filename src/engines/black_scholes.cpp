#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "pricer/engines.hpp"
#include "pricer/math.hpp"

namespace pricer {

namespace {

struct BsTerms {
    double d1 = 0.0;
    double d2 = 0.0;
    double disc_r = 0.0;  ///< exp(-rT)
    double disc_q = 0.0;  ///< exp(-qT)
    double sqrt_t = 0.0;
};

BsTerms compute_terms(const Instrument& inst, const Market& mkt) {
    BsTerms t;
    t.sqrt_t = std::sqrt(inst.expiry);
    t.disc_r = std::exp(-mkt.rate * inst.expiry);
    t.disc_q = std::exp(-mkt.dividend * inst.expiry);

    const double sigma_sqrt_t = mkt.vol * t.sqrt_t;
    const double moneyness = std::log(mkt.spot / inst.strike);
    const double drift = (mkt.rate - mkt.dividend + 0.5 * mkt.vol * mkt.vol) * inst.expiry;

    t.d1 = (moneyness + drift) / sigma_sqrt_t;
    t.d2 = t.d1 - sigma_sqrt_t;
    return t;
}

/// Zero-vol (or zero-time) limit: the discounted forward intrinsic value.
double deterministic_price(const Instrument& inst, const Market& mkt) {
    const double fwd = mkt.spot * std::exp((mkt.rate - mkt.dividend) * inst.expiry);
    const double disc = std::exp(-mkt.rate * inst.expiry);
    const double intrinsic = inst.option == OptionType::Call ? fwd - inst.strike
                                                             : inst.strike - fwd;
    return disc * std::max(0.0, intrinsic);
}

}  // namespace

bool BlackScholesPricer::supports(const Instrument& inst) const {
    return inst.kind == PayoffKind::Vanilla && inst.exercise == ExerciseStyle::European;
}

double BlackScholesPricer::price_only(const Instrument& inst, const Market& mkt) {
    if (mkt.vol <= 0.0 || inst.expiry <= 0.0) return deterministic_price(inst, mkt);

    const BsTerms t = compute_terms(inst, mkt);
    if (inst.option == OptionType::Call) {
        return mkt.spot * t.disc_q * math::norm_cdf(t.d1) -
               inst.strike * t.disc_r * math::norm_cdf(t.d2);
    }
    return inst.strike * t.disc_r * math::norm_cdf(-t.d2) -
           mkt.spot * t.disc_q * math::norm_cdf(-t.d1);
}

PricingResult BlackScholesPricer::price(const Instrument& inst, const Market& mkt,
                                        const PricingParams& params) const {
    validate(inst, mkt);
    if (!supports(inst)) {
        throw std::invalid_argument(
            "black_scholes handles European vanilla options only");
    }

    PricingResult r;
    r.engine = name();
    r.price = price_only(inst, mkt);

    if (!params.greeks) return r;

    if (mkt.vol <= 0.0 || inst.expiry <= 0.0) {
        // Degenerate case: Greeks are step functions, so report the limits.
        const double fwd = mkt.spot * std::exp((mkt.rate - mkt.dividend) * inst.expiry);
        const bool itm = inst.option == OptionType::Call ? fwd > inst.strike
                                                         : fwd < inst.strike;
        const double sign = inst.option == OptionType::Call ? 1.0 : -1.0;
        r.delta = itm ? sign * std::exp(-mkt.dividend * inst.expiry) : 0.0;
        r.gamma = 0.0;
        r.vega = 0.0;
        r.theta = 0.0;
        r.rho = 0.0;
        return r;
    }

    const BsTerms t = compute_terms(inst, mkt);
    const double pdf_d1 = math::norm_pdf(t.d1);

    // Gamma and vega are exercise-symmetric; the rest flip with the option type.
    r.gamma = t.disc_q * pdf_d1 / (mkt.spot * mkt.vol * t.sqrt_t);
    r.vega = mkt.spot * t.disc_q * pdf_d1 * t.sqrt_t / 100.0;  // per 1 vol point

    const double theta_common =
        -(mkt.spot * t.disc_q * pdf_d1 * mkt.vol) / (2.0 * t.sqrt_t);

    if (inst.option == OptionType::Call) {
        r.delta = t.disc_q * math::norm_cdf(t.d1);
        r.theta = (theta_common - mkt.rate * inst.strike * t.disc_r * math::norm_cdf(t.d2) +
                   mkt.dividend * mkt.spot * t.disc_q * math::norm_cdf(t.d1)) /
                  365.0;  // per calendar day
        r.rho = inst.strike * inst.expiry * t.disc_r * math::norm_cdf(t.d2) / 100.0;
    } else {
        r.delta = -t.disc_q * math::norm_cdf(-t.d1);
        r.theta = (theta_common + mkt.rate * inst.strike * t.disc_r * math::norm_cdf(-t.d2) -
                   mkt.dividend * mkt.spot * t.disc_q * math::norm_cdf(-t.d1)) /
                  365.0;
        r.rho = -inst.strike * inst.expiry * t.disc_r * math::norm_cdf(-t.d2) / 100.0;
    }

    return r;
}

}  // namespace pricer
