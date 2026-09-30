#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "pricer/engines.hpp"

using Catch::Approx;
using namespace pricer;

namespace {

Instrument vanilla(OptionType type, double strike, double expiry) {
    Instrument i;
    i.kind = PayoffKind::Vanilla;
    i.exercise = ExerciseStyle::European;
    i.option = type;
    i.strike = strike;
    i.expiry = expiry;
    return i;
}

}  // namespace

TEST_CASE("Black-Scholes matches Hull's textbook example") {
    // Hull, Options Futures and Other Derivatives: S=42, K=40, r=10%, sig=20%,
    // T=0.5 gives call 4.76 and put 0.81.
    const Market mkt{42.0, 0.10, 0.0, 0.20};
    PricingParams p;

    const auto call = BlackScholesPricer().price(vanilla(OptionType::Call, 40.0, 0.5), mkt, p);
    const auto put = BlackScholesPricer().price(vanilla(OptionType::Put, 40.0, 0.5), mkt, p);

    CHECK(call.price == Approx(4.759422392871542).epsilon(1e-10));
    CHECK(put.price == Approx(0.8085993729000922).epsilon(1e-10));
}

TEST_CASE("put-call parity holds for Black-Scholes") {
    const Market mkt{100.0, 0.05, 0.03, 0.25};
    PricingParams p;
    p.greeks = false;

    for (double k : {70.0, 90.0, 100.0, 110.0, 150.0}) {
        for (double t : {0.08, 0.5, 1.0, 3.0}) {
            const double c = BlackScholesPricer().price(vanilla(OptionType::Call, k, t), mkt, p).price;
            const double put = BlackScholesPricer().price(vanilla(OptionType::Put, k, t), mkt, p).price;
            const double lhs = c - put;
            const double rhs = mkt.spot * std::exp(-mkt.dividend * t) - k * std::exp(-mkt.rate * t);
            CHECK(lhs == Approx(rhs).epsilon(1e-10));
        }
    }
}

TEST_CASE("analytic Greeks agree with central differences") {
    const Market mkt{100.0, 0.05, 0.02, 0.20};
    PricingParams p;

    for (auto type : {OptionType::Call, OptionType::Put}) {
        for (double k : {80.0, 100.0, 120.0}) {
            const Instrument inst = vanilla(type, k, 1.0);
            const auto r = BlackScholesPricer().price(inst, mkt, p);
            REQUIRE(r.delta.has_value());

            const double h = 1e-4;
            Market up = mkt, down = mkt;
            up.spot += h;
            down.spot -= h;
            const double pu = BlackScholesPricer::price_only(inst, up);
            const double pd = BlackScholesPricer::price_only(inst, down);

            CHECK(*r.delta == Approx((pu - pd) / (2 * h)).epsilon(1e-5));
            CHECK(*r.gamma == Approx((pu - 2 * r.price + pd) / (h * h)).epsilon(1e-3));

            Market vu = mkt, vd = mkt;
            vu.vol += h;
            vd.vol -= h;
            const double vega_fd =
                (BlackScholesPricer::price_only(inst, vu) -
                 BlackScholesPricer::price_only(inst, vd)) / (2 * h) / 100.0;
            CHECK(*r.vega == Approx(vega_fd).epsilon(1e-5));

            Market ru = mkt, rd = mkt;
            ru.rate += h;
            rd.rate -= h;
            const double rho_fd =
                (BlackScholesPricer::price_only(inst, ru) -
                 BlackScholesPricer::price_only(inst, rd)) / (2 * h) / 100.0;
            CHECK(*r.rho == Approx(rho_fd).epsilon(1e-5));

            // Theta is reported per calendar day, so the central difference
            // is taken over a small time step and rescaled. A one-sided
            // difference over a whole day carries enough truncation error to
            // look like a real disagreement.
            const double ht = 1e-5;
            Instrument t_up = inst, t_down = inst;
            t_up.expiry += ht;
            t_down.expiry -= ht;
            const double dP_dT = (BlackScholesPricer::price_only(t_up, mkt) -
                                  BlackScholesPricer::price_only(t_down, mkt)) /
                                 (2 * ht);
            CHECK(*r.theta == Approx(-dP_dT / 365.0).epsilon(1e-5));
        }
    }
}

TEST_CASE("delta and gamma obey their theoretical bounds") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p;

    const auto call = BlackScholesPricer().price(vanilla(OptionType::Call, 100.0, 1.0), mkt, p);
    const auto put = BlackScholesPricer().price(vanilla(OptionType::Put, 100.0, 1.0), mkt, p);

    CHECK(*call.delta > 0.0);
    CHECK(*call.delta < 1.0);
    CHECK(*put.delta < 0.0);
    CHECK(*put.delta > -1.0);
    CHECK(*call.gamma > 0.0);
    CHECK(*call.gamma == Approx(*put.gamma).epsilon(1e-12));
    CHECK(*call.vega == Approx(*put.vega).epsilon(1e-12));
    // No dividends, so a European call is never worth exercising early and
    // loses value over time; the put's theta can go either way.
    CHECK(*call.theta < 0.0);
}

TEST_CASE("prices respect no-arbitrage bounds") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p;
    p.greeks = false;

    for (double k : {50.0, 100.0, 200.0}) {
        const Instrument c = vanilla(OptionType::Call, k, 1.0);
        const Instrument put = vanilla(OptionType::Put, k, 1.0);
        const double disc_k = k * std::exp(-mkt.rate * 1.0);

        const double cp = BlackScholesPricer::price_only(c, mkt);
        const double pp = BlackScholesPricer::price_only(put, mkt);

        CHECK(cp >= std::max(0.0, mkt.spot - disc_k) - 1e-12);
        CHECK(cp <= mkt.spot + 1e-12);
        CHECK(pp >= std::max(0.0, disc_k - mkt.spot) - 1e-12);
        CHECK(pp <= disc_k + 1e-12);
    }
}

TEST_CASE("zero volatility collapses to discounted forward intrinsic") {
    Market mkt{100.0, 0.05, 0.0, 0.0};
    PricingParams p;
    p.greeks = false;

    const double fwd = 100.0 * std::exp(0.05);
    const double disc = std::exp(-0.05);

    CHECK(BlackScholesPricer::price_only(vanilla(OptionType::Call, 100.0, 1.0), mkt) ==
          Approx(disc * (fwd - 100.0)).epsilon(1e-12));
    CHECK(BlackScholesPricer::price_only(vanilla(OptionType::Call, 200.0, 1.0), mkt) ==
          Approx(0.0).margin(1e-15));
    CHECK(BlackScholesPricer::price_only(vanilla(OptionType::Put, 100.0, 1.0), mkt) ==
          Approx(0.0).margin(1e-15));
}

TEST_CASE("Black-Scholes rejects contracts it cannot price") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p;

    Instrument american = vanilla(OptionType::Put, 100.0, 1.0);
    american.exercise = ExerciseStyle::American;
    CHECK_FALSE(BlackScholesPricer().supports(american));
    CHECK_THROWS_AS(BlackScholesPricer().price(american, mkt, p), std::invalid_argument);

    Instrument asian = vanilla(OptionType::Call, 100.0, 1.0);
    asian.kind = PayoffKind::Asian;
    CHECK_FALSE(BlackScholesPricer().supports(asian));
}

TEST_CASE("invalid inputs are rejected") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p;

    CHECK_THROWS_AS(BlackScholesPricer().price(vanilla(OptionType::Call, -1.0, 1.0), mkt, p),
                    std::invalid_argument);
    CHECK_THROWS_AS(BlackScholesPricer().price(vanilla(OptionType::Call, 100.0, -1.0), mkt, p),
                    std::invalid_argument);

    Market bad = mkt;
    bad.spot = 0.0;
    CHECK_THROWS_AS(BlackScholesPricer().price(vanilla(OptionType::Call, 100.0, 1.0), bad, p),
                    std::invalid_argument);
}
