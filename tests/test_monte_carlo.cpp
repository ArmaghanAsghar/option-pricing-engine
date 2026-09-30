#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "pricer/engines.hpp"

using Catch::Approx;
using namespace pricer;

namespace {

Instrument euro(OptionType type, double strike = 100.0, double expiry = 1.0) {
    Instrument i;
    i.kind = PayoffKind::Vanilla;
    i.exercise = ExerciseStyle::European;
    i.option = type;
    i.strike = strike;
    i.expiry = expiry;
    return i;
}

Instrument barrier(BarrierDirection dir, double level, OptionType type = OptionType::Call) {
    Instrument i = euro(type);
    i.kind = PayoffKind::Barrier;
    i.barrier_direction = dir;
    i.barrier = level;
    i.monitoring_steps = 100;
    return i;
}

PricingParams mc_params(long paths) {
    PricingParams p;
    p.paths = paths;
    p.greeks = false;
    p.seed = 7777;
    return p;
}

}  // namespace

TEST_CASE("Monte Carlo lands within three standard errors of Black-Scholes") {
    const Market mkt{100.0, 0.05, 0.02, 0.20};
    const auto params = mc_params(400000);

    for (auto type : {OptionType::Call, OptionType::Put}) {
        for (double k : {85.0, 100.0, 115.0}) {
            const Instrument inst = euro(type, k);
            const double exact = BlackScholesPricer::price_only(inst, mkt);
            const auto r = MonteCarloPricer().price(inst, mkt, params);

            REQUIRE(r.std_error.has_value());
            REQUIRE(*r.std_error > 0.0);
            CHECK(std::abs(r.price - exact) < 3.0 * *r.std_error);
        }
    }
}

TEST_CASE("Monte Carlo is reproducible for a fixed seed and thread count") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p = mc_params(50000);
    p.threads = 4;

    const auto a = MonteCarloPricer().price(euro(OptionType::Call), mkt, p);
    const auto b = MonteCarloPricer().price(euro(OptionType::Call), mkt, p);
    CHECK(a.price == b.price);
    CHECK(*a.std_error == *b.std_error);
}

TEST_CASE("changing the seed changes the estimate but not the ballpark") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p = mc_params(100000);
    p.threads = 2;
    PricingParams q = p;
    q.seed = 1234;

    const auto a = MonteCarloPricer().price(euro(OptionType::Call), mkt, p);
    const auto b = MonteCarloPricer().price(euro(OptionType::Call), mkt, q);
    CHECK(a.price != b.price);
    CHECK(std::abs(a.price - b.price) < 5.0 * (*a.std_error + *b.std_error));
}

TEST_CASE("antithetic variates cut the standard error") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams with = mc_params(200000);
    PricingParams without = with;
    without.antithetic = false;

    const auto a = MonteCarloPricer().price(euro(OptionType::Call), mkt, with);
    const auto b = MonteCarloPricer().price(euro(OptionType::Call), mkt, without);
    CHECK(*a.std_error < *b.std_error);
}

TEST_CASE("thread count changes timing but not the answer materially") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams one = mc_params(200000);
    one.threads = 1;
    PricingParams many = one;
    many.threads = 8;

    const auto a = MonteCarloPricer().price(euro(OptionType::Call), mkt, one);
    const auto b = MonteCarloPricer().price(euro(OptionType::Call), mkt, many);
    CHECK(a.paths_used == b.paths_used);
    CHECK(std::abs(a.price - b.price) < 4.0 * (*a.std_error + *b.std_error));
}

TEST_CASE("put-call parity holds for Monte Carlo within noise") {
    const Market mkt{100.0, 0.05, 0.03, 0.22};
    const auto p = mc_params(400000);

    const auto c = MonteCarloPricer().price(euro(OptionType::Call), mkt, p);
    const auto put = MonteCarloPricer().price(euro(OptionType::Put), mkt, p);
    const double rhs = mkt.spot * std::exp(-mkt.dividend) - 100.0 * std::exp(-mkt.rate);

    CHECK(c.price - put.price == Approx(rhs).margin(4.0 * (*c.std_error + *put.std_error)));
}

TEST_CASE("barrier in plus out equals the vanilla") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    const auto p = mc_params(300000);

    // The exact identity only holds on identical paths. A plain vanilla is
    // simulated with a single terminal draw (cheaper and exact), so it consumes
    // a different number of normals than a 100-step barrier. An unreachable
    // barrier gives the same payoff on the same 100-step paths, which is the
    // right reference for the identity.
    const auto reference = MonteCarloPricer().price(barrier(BarrierDirection::UpOut, 1e9), mkt, p);

    const auto up_in = MonteCarloPricer().price(barrier(BarrierDirection::UpIn, 120.0), mkt, p);
    const auto up_out = MonteCarloPricer().price(barrier(BarrierDirection::UpOut, 120.0), mkt, p);
    CHECK(up_in.price + up_out.price == Approx(reference.price).epsilon(1e-9));

    const auto down_in = MonteCarloPricer().price(barrier(BarrierDirection::DownIn, 80.0), mkt, p);
    const auto down_out = MonteCarloPricer().price(barrier(BarrierDirection::DownOut, 80.0), mkt, p);
    CHECK(down_in.price + down_out.price == Approx(reference.price).epsilon(1e-9));

    // The sum must also agree with the closed-form vanilla within sampling noise.
    const double exact = BlackScholesPricer::price_only(euro(OptionType::Call), mkt);
    CHECK(up_in.price + up_out.price ==
          Approx(exact).margin(4.0 * *reference.std_error));
}

TEST_CASE("knock-out options are cheaper than the vanilla") {
    const Market mkt{100.0, 0.05, 0.0, 0.25};
    const auto p = mc_params(200000);

    const double plain = MonteCarloPricer().price(euro(OptionType::Call), mkt, p).price;
    const double knocked =
        MonteCarloPricer().price(barrier(BarrierDirection::UpOut, 115.0), mkt, p).price;
    CHECK(knocked < plain);
    CHECK(knocked >= 0.0);
}

TEST_CASE("a distant barrier is nearly irrelevant") {
    const Market mkt{100.0, 0.05, 0.0, 0.15};
    const auto p = mc_params(200000);

    // Compared against the closed form rather than a one-step vanilla
    // simulation, since the two path lengths do not share random draws.
    const double exact = BlackScholesPricer::price_only(euro(OptionType::Call), mkt);
    const auto far_out = MonteCarloPricer().price(barrier(BarrierDirection::UpOut, 1000.0), mkt, p);
    CHECK(far_out.price == Approx(exact).margin(4.0 * *far_out.std_error));
}

TEST_CASE("Asian options are cheaper than their vanilla equivalents") {
    // Averaging dampens the terminal distribution, so the option is worth less.
    const Market mkt{100.0, 0.05, 0.0, 0.30};
    const auto p = mc_params(200000);

    Instrument asian = euro(OptionType::Call);
    asian.kind = PayoffKind::Asian;
    asian.averaging_steps = 252;

    const double avg = MonteCarloPricer().price(asian, mkt, p).price;
    const double plain = MonteCarloPricer().price(euro(OptionType::Call), mkt, p).price;
    CHECK(avg < plain);
    CHECK(avg > 0.0);
}

TEST_CASE("a single-observation Asian option is just a vanilla") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    const auto p = mc_params(100000);

    Instrument asian = euro(OptionType::Call);
    asian.kind = PayoffKind::Asian;
    asian.averaging_steps = 1;

    const double avg = MonteCarloPricer().price(asian, mkt, p).price;
    const double plain = MonteCarloPricer().price(euro(OptionType::Call), mkt, p).price;
    CHECK(avg == Approx(plain).epsilon(1e-9));
}

TEST_CASE("subdividing a vanilla path does not change its price") {
    // The charting resolution must be free: geometric Brownian motion is exact
    // at any step size, so a subdivided path has the same terminal
    // distribution as a single jump. Both runs must agree to within noise.
    const Market mkt{100.0, 0.05, 0.0, 0.20};

    PricingParams one_step = mc_params(400000);
    PricingParams many_steps = one_step;
    many_steps.path_steps = 100;

    const auto coarse = MonteCarloPricer().price(euro(OptionType::Call), mkt, one_step);
    const auto fine = MonteCarloPricer().price(euro(OptionType::Call), mkt, many_steps);

    CHECK(coarse.steps_used == 1);
    CHECK(fine.steps_used == 100);

    const double exact = BlackScholesPricer::price_only(euro(OptionType::Call), mkt);
    CHECK(coarse.price == Approx(exact).margin(3.0 * *coarse.std_error));
    CHECK(fine.price == Approx(exact).margin(3.0 * *fine.std_error));
}

TEST_CASE("path resolution never overrides a contract's own observation count") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p = mc_params(20000);
    p.path_steps = 7;  // would corrupt the payoff if it were honoured

    Instrument asian = euro(OptionType::Call);
    asian.kind = PayoffKind::Asian;
    asian.averaging_steps = 50;
    CHECK(MonteCarloPricer().price(asian, mkt, p).steps_used == 50);

    Instrument knockout = barrier(BarrierDirection::UpOut, 130.0);
    knockout.monitoring_steps = 64;
    CHECK(MonteCarloPricer().price(knockout, mkt, p).steps_used == 64);
}

TEST_CASE("a subdivided vanilla path is plottable") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p = mc_params(5000);
    p.return_paths = 3;
    p.path_steps = 40;

    const auto r = MonteCarloPricer().price(euro(OptionType::Call), mkt, p);
    REQUIRE(r.sample_paths.size() == 3);
    for (const auto& path : r.sample_paths) {
        REQUIRE(path.size() == 41);  // 40 steps plus t=0
        CHECK(path.front() == Approx(100.0).epsilon(1e-12));
        // A genuine random walk, not a straight line.
        bool varies = false;
        for (std::size_t i = 2; i < path.size(); ++i) {
            const double d1 = path[i] - path[i - 1];
            const double d2 = path[i - 1] - path[i - 2];
            if (std::abs(d1 - d2) > 1e-9) varies = true;
        }
        CHECK(varies);
    }
}

TEST_CASE("path sampling returns the requested shape") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p = mc_params(20000);
    p.return_paths = 25;

    Instrument asian = euro(OptionType::Call);
    asian.kind = PayoffKind::Asian;
    asian.averaging_steps = 50;

    const auto r = MonteCarloPricer().price(asian, mkt, p);
    REQUIRE(r.sample_paths.size() == 25);
    for (const auto& path : r.sample_paths) {
        REQUIRE(path.size() == 51);  // includes t=0
        CHECK(path.front() == Approx(100.0).epsilon(1e-12));
        for (double v : path) CHECK(v > 0.0);
    }
}

TEST_CASE("convergence checkpoints are reported in order") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p = mc_params(100000);
    p.threads = 1;
    p.convergence = {1000, 10000, 50000};

    const auto r = MonteCarloPricer().price(euro(OptionType::Call), mkt, p);
    REQUIRE(r.convergence.size() == 3);
    CHECK(r.convergence[0].paths >= 1000);
    CHECK(r.convergence[1].paths >= 10000);
    CHECK(r.convergence[2].paths >= 50000);
    // More paths must mean a tighter error bar.
    CHECK(r.convergence[2].std_error < r.convergence[0].std_error);
}

TEST_CASE("Monte Carlo standard error shrinks as the square root of paths") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    const auto small = MonteCarloPricer().price(euro(OptionType::Call), mkt, mc_params(50000));
    const auto large = MonteCarloPricer().price(euro(OptionType::Call), mkt, mc_params(800000));

    const double ratio = *small.std_error / *large.std_error;
    CHECK(ratio == Approx(4.0).epsilon(0.25));  // sqrt(16)
}

TEST_CASE("Monte Carlo Greeks match Black-Scholes for a vanilla") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    PricingParams p;
    p.paths = 2000000;
    p.seed = 31337;
    p.greeks = true;

    const Instrument inst = euro(OptionType::Call);
    const auto mc = MonteCarloPricer().price(inst, mkt, p);
    const auto exact = BlackScholesPricer().price(inst, mkt, p);

    REQUIRE(mc.delta.has_value());
    // Common random numbers keep the bumped runs correlated, which is what
    // makes a pathwise delta usable at this path count.
    CHECK(*mc.delta == Approx(*exact.delta).margin(0.01));
    CHECK(*mc.vega == Approx(*exact.vega).margin(0.02));
}

TEST_CASE("Monte Carlo refuses American exercise") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    Instrument amer = euro(OptionType::Put);
    amer.exercise = ExerciseStyle::American;

    CHECK_FALSE(MonteCarloPricer().supports(amer));
    CHECK_THROWS_AS(MonteCarloPricer().price(amer, mkt, mc_params(1000)), std::invalid_argument);
}

TEST_CASE("a barrier already breached at inception is rejected") {
    const Market mkt{100.0, 0.05, 0.0, 0.20};
    CHECK_THROWS_AS(
        MonteCarloPricer().price(barrier(BarrierDirection::UpOut, 90.0), mkt, mc_params(1000)),
        std::invalid_argument);
    CHECK_THROWS_AS(
        MonteCarloPricer().price(barrier(BarrierDirection::DownOut, 110.0), mkt, mc_params(1000)),
        std::invalid_argument);
}
