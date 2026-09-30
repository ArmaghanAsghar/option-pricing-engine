#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>

#include "pricer/engines.hpp"

using Catch::Approx;
using namespace pricer;

namespace {

ImpliedVolQuote quote(double strike, double expiry, OptionType type, double price) {
    return ImpliedVolQuote{strike, expiry, type, price};
}

}  // namespace

TEST_CASE("implied vol round-trips a Black-Scholes price") {
    Market mkt{100.0, 0.05, 0.02, 0.0};

    int recovered = 0;
    int unidentifiable = 0;

    for (double true_vol : {0.05, 0.10, 0.20, 0.40, 0.80, 1.50}) {
        for (double k : {70.0, 90.0, 100.0, 110.0, 140.0}) {
            for (double t : {0.05, 0.5, 2.0}) {
                for (auto type : {OptionType::Call, OptionType::Put}) {
                    Instrument inst;
                    inst.option = type;
                    inst.strike = k;
                    inst.expiry = t;

                    Market pricing = mkt;
                    pricing.vol = true_vol;
                    const double px = BlackScholesPricer::price_only(inst, pricing);

                    const auto r = implied_volatility(quote(k, t, type, px), mkt);

                    if (!r.converged) {
                        // The only acceptable failure is a quote whose value
                        // genuinely does not depend on volatility.
                        CHECK_FALSE(r.well_conditioned);
                        ++unidentifiable;
                        continue;
                    }

                    // Where the solve is trustworthy, it must be accurate to
                    // within its own stated error bar.
                    ++recovered;
                    CHECK(r.well_conditioned);
                    CHECK(r.implied_vol ==
                          Approx(true_vol).margin(std::max(1e-6, 10.0 * r.vol_uncertainty)));
                }
            }
        }
    }

    // The bulk of a normal strike and expiry grid must be recoverable;
    // only the degenerate corners should drop out.
    CHECK(recovered > 3 * unidentifiable);
}

TEST_CASE("at-the-money quotes invert to full precision") {
    Market mkt{100.0, 0.05, 0.0, 0.0};

    for (double true_vol : {0.08, 0.15, 0.25, 0.50, 1.00}) {
        for (double t : {0.1, 1.0, 3.0}) {
            Instrument inst;
            inst.option = OptionType::Call;
            inst.strike = 100.0;
            inst.expiry = t;

            Market pricing = mkt;
            pricing.vol = true_vol;
            const double px = BlackScholesPricer::price_only(inst, pricing);

            const auto r = implied_volatility(quote(100.0, t, OptionType::Call, px), mkt);
            REQUIRE(r.converged);
            CHECK(r.well_conditioned);
            CHECK(r.vol_uncertainty < 1e-6);
            CHECK(r.implied_vol == Approx(true_vol).epsilon(1e-9));
        }
    }
}

TEST_CASE("a quote with no volatility information is reported, not guessed") {
    // A deeply in-the-money short-dated call is almost pure intrinsic value, so
    // its price pins down no volatility at all.
    Market mkt{100.0, 0.05, 0.0, 0.0};
    Instrument inst;
    inst.option = OptionType::Call;
    inst.strike = 10.0;
    inst.expiry = 0.02;

    Market pricing = mkt;
    pricing.vol = 0.35;
    const double px = BlackScholesPricer::price_only(inst, pricing);

    const auto r = implied_volatility(quote(10.0, 0.02, OptionType::Call, px), mkt);
    CHECK_FALSE(r.converged);
    CHECK_FALSE(r.well_conditioned);
    CHECK(r.error.find("volatility") != std::string::npos);
}

TEST_CASE("implied vol handles deep out-of-the-money quotes") {
    Market mkt{100.0, 0.03, 0.0, 0.0};

    Instrument inst;
    inst.option = OptionType::Call;
    inst.strike = 250.0;
    inst.expiry = 0.25;

    Market pricing = mkt;
    pricing.vol = 0.60;
    const double px = BlackScholesPricer::price_only(inst, pricing);
    REQUIRE(px > 1e-10);

    const auto r = implied_volatility(quote(250.0, 0.25, OptionType::Call, px), mkt);
    REQUIRE(r.converged);
    CHECK(r.implied_vol == Approx(0.60).epsilon(1e-5));
}

TEST_CASE("implied vol rejects prices outside no-arbitrage bounds") {
    Market mkt{100.0, 0.05, 0.0, 0.0};

    // Above the spot, a call cannot be worth this much.
    auto too_high = implied_volatility(quote(100.0, 1.0, OptionType::Call, 150.0), mkt);
    CHECK_FALSE(too_high.converged);
    CHECK(too_high.error.find("upper bound") != std::string::npos);

    // Below intrinsic on the forward.
    auto too_low = implied_volatility(quote(50.0, 1.0, OptionType::Call, 1.0), mkt);
    CHECK_FALSE(too_low.converged);
    CHECK(too_low.error.find("lower bound") != std::string::npos);
}

TEST_CASE("a price on the lower bound is either zero vol or flagged") {
    // At the no-arbitrage floor the option has no time value, so either answer
    // is defensible as long as the solver is honest about which it gave.
    Market mkt{100.0, 0.05, 0.0, 0.0};
    const double lower = 100.0 - 90.0 * std::exp(-0.05);
    const auto r = implied_volatility(quote(90.0, 1.0, OptionType::Call, lower), mkt);

    if (r.converged) {
        CHECK(r.implied_vol < 1e-4);
    } else {
        CHECK_FALSE(r.well_conditioned);
    }
}

TEST_CASE("the uncertainty estimate grows as a quote moves away from the money") {
    Market mkt{100.0, 0.05, 0.0, 0.0};
    double previous = 0.0;

    // Sweeping a call's strike downward turns time value into intrinsic value,
    // which must make the inversion progressively less certain.
    for (double k : {100.0, 70.0, 40.0, 20.0}) {
        Instrument inst;
        inst.option = OptionType::Call;
        inst.strike = k;
        inst.expiry = 0.25;

        Market pricing = mkt;
        pricing.vol = 0.25;
        const double px = BlackScholesPricer::price_only(inst, pricing);

        const auto r = implied_volatility(quote(k, 0.25, OptionType::Call, px), mkt);
        const double reported = r.converged ? r.vol_uncertainty : 1e9;
        CHECK(reported >= previous);
        previous = reported;
    }
}

TEST_CASE("implied vol rejects nonsensical contract terms") {
    Market mkt{100.0, 0.05, 0.0, 0.0};

    CHECK_FALSE(implied_volatility(quote(-10.0, 1.0, OptionType::Call, 5.0), mkt).converged);
    CHECK_FALSE(implied_volatility(quote(100.0, 0.0, OptionType::Call, 5.0), mkt).converged);

    Market bad = mkt;
    bad.spot = -1.0;
    CHECK_FALSE(implied_volatility(quote(100.0, 1.0, OptionType::Call, 5.0), bad).converged);
}

TEST_CASE("implied vol recovers a smile shape from synthetic prices") {
    // Build prices from a known smile, then check we get the smile back.
    Market mkt{100.0, 0.04, 0.01, 0.0};
    const double t = 0.5;
    const std::vector<std::pair<double, double>> smile = {
        {80.0, 0.32}, {90.0, 0.26}, {100.0, 0.22}, {110.0, 0.24}, {120.0, 0.29}};

    for (const auto& [k, vol] : smile) {
        Instrument inst;
        inst.option = k >= 100.0 ? OptionType::Call : OptionType::Put;
        inst.strike = k;
        inst.expiry = t;

        Market pricing = mkt;
        pricing.vol = vol;
        const double px = BlackScholesPricer::price_only(inst, pricing);

        const auto r = implied_volatility(quote(k, t, inst.option, px), mkt);
        REQUIRE(r.converged);
        CHECK(r.implied_vol == Approx(vol).epsilon(1e-7));
    }
}
