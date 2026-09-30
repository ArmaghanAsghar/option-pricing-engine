#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "pricer/engines.hpp"

using Catch::Approx;
using namespace pricer;

namespace {

Instrument make(OptionType type, ExerciseStyle style, double strike, double expiry) {
    Instrument i;
    i.kind = PayoffKind::Vanilla;
    i.exercise = style;
    i.option = type;
    i.strike = strike;
    i.expiry = expiry;
    return i;
}

}  // namespace

TEST_CASE("binomial converges to Black-Scholes for European options") {
    const Market mkt{100.0, 0.05, 0.02, 0.25};
    PricingParams p;
    p.greeks = false;

    for (auto type : {OptionType::Call, OptionType::Put}) {
        const Instrument inst = make(type, ExerciseStyle::European, 100.0, 1.0);
        const double exact = BlackScholesPricer::price_only(inst, mkt);

        double prev_error = 1e9;
        for (int steps : {50, 200, 800, 3200}) {
            p.steps = steps;
            const double err = std::abs(BinomialPricer().price(inst, mkt, p).price - exact);
            // CRR error oscillates, so require the trend rather than monotonicity.
            CHECK(err < prev_error * 1.2);
            prev_error = err;
        }
        CHECK(prev_error < 2e-3);
    }
}

TEST_CASE("binomial reproduces a known American put value") {
    // Standard benchmark: S=K=100, r=5%, q=0, sig=20%, T=1. Published CRR value
    // converges near 6.0903.
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p;
    p.greeks = false;
    p.steps = 5000;

    const auto r = BinomialPricer().price(make(OptionType::Put, ExerciseStyle::American, 100.0, 1.0),
                                          mkt, p);
    CHECK(r.price == Approx(6.0903).epsilon(2e-3));
}

TEST_CASE("American options are worth at least their European twins") {
    const Market mkt{100.0, 0.06, 0.03, 0.30};
    PricingParams p;
    p.greeks = false;
    p.steps = 1000;

    for (auto type : {OptionType::Call, OptionType::Put}) {
        for (double k : {70.0, 100.0, 130.0}) {
            const double euro =
                BinomialPricer().price(make(type, ExerciseStyle::European, k, 1.0), mkt, p).price;
            const double amer =
                BinomialPricer().price(make(type, ExerciseStyle::American, k, 1.0), mkt, p).price;
            CHECK(amer >= euro - 1e-9);
        }
    }
}

TEST_CASE("American options never trade below intrinsic value") {
    PricingParams p;
    p.greeks = false;
    p.steps = 500;

    for (double spot : {60.0, 80.0, 100.0, 120.0, 140.0}) {
        const Market mkt{spot, 0.05, 0.0, 0.20};
        const double call =
            BinomialPricer().price(make(OptionType::Call, ExerciseStyle::American, 100.0, 1.0), mkt, p)
                .price;
        const double put =
            BinomialPricer().price(make(OptionType::Put, ExerciseStyle::American, 100.0, 1.0), mkt, p)
                .price;
        CHECK(call >= std::max(0.0, spot - 100.0) - 1e-9);
        CHECK(put >= std::max(0.0, 100.0 - spot) - 1e-9);
    }
}

TEST_CASE("American call without dividends equals the European call") {
    // With q = 0 early exercise of a call is never optimal, so the early
    // exercise premium must vanish.
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p;
    p.greeks = false;
    p.steps = 2000;

    const double euro =
        BinomialPricer().price(make(OptionType::Call, ExerciseStyle::European, 100.0, 1.0), mkt, p)
            .price;
    const double amer =
        BinomialPricer().price(make(OptionType::Call, ExerciseStyle::American, 100.0, 1.0), mkt, p)
            .price;
    CHECK(amer == Approx(euro).epsilon(1e-9));
}

TEST_CASE("deep-in-the-money American put approaches its exercise value") {
    const Market mkt{10.0, 0.05, 0.0, 0.20};
    PricingParams p;
    p.greeks = false;
    p.steps = 1000;

    const double put =
        BinomialPricer().price(make(OptionType::Put, ExerciseStyle::American, 100.0, 1.0), mkt, p)
            .price;
    CHECK(put == Approx(90.0).epsilon(1e-6));
}

TEST_CASE("binomial finite-difference Greeks track the analytic ones") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p;
    p.greeks = true;
    p.steps = 2000;

    const Instrument inst = make(OptionType::Call, ExerciseStyle::European, 100.0, 1.0);
    const auto tree = BinomialPricer().price(inst, mkt, p);
    const auto exact = BlackScholesPricer().price(inst, mkt, p);

    REQUIRE(tree.delta.has_value());
    CHECK(*tree.delta == Approx(*exact.delta).epsilon(2e-3));
    CHECK(*tree.vega == Approx(*exact.vega).epsilon(5e-2));
    CHECK(*tree.rho == Approx(*exact.rho).epsilon(5e-2));
    CHECK(*tree.theta == Approx(*exact.theta).epsilon(5e-2));
}

TEST_CASE("binomial rejects path-dependent payoffs and bad step counts") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p;

    Instrument asian = make(OptionType::Call, ExerciseStyle::European, 100.0, 1.0);
    asian.kind = PayoffKind::Asian;
    CHECK_FALSE(BinomialPricer().supports(asian));
    CHECK_THROWS_AS(BinomialPricer().price(asian, mkt, p), std::invalid_argument);

    p.steps = 0;
    CHECK_THROWS_AS(
        BinomialPricer().price(make(OptionType::Call, ExerciseStyle::European, 100.0, 1.0), mkt, p),
        std::invalid_argument);
}
