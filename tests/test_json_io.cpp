#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "pricer/json_io.hpp"

using Catch::Approx;
using json = nlohmann::json;
using namespace pricer;

TEST_CASE("a minimal price request parses and prices") {
    const json in = {{"instrument", {{"type", "vanilla"}, {"option", "call"}, {"strike", 100}, {"expiry", 1.0}}},
                     {"market", {{"spot", 100}, {"rate", 0.05}, {"vol", 0.2}}}};

    const auto req = parse_price_request(in);
    CHECK(req.instrument.strike == 100.0);
    CHECK(req.market.vol == 0.2);
    CHECK(req.engine.empty());

    const json out = evaluate_price(req);
    CHECK(out["engine"] == "black_scholes");  // default for European vanilla
    CHECK(out["price"].get<double>() == Approx(10.450583572185565).epsilon(1e-9));
    CHECK(out.contains("greeks"));
    CHECK(out["greeks"].contains("delta"));
}

TEST_CASE("the default engine follows the instrument") {
    json in = {{"instrument", {{"type", "vanilla"}, {"option", "put"}, {"strike", 100}, {"expiry", 1.0},
                               {"exercise", "american"}}},
               {"market", {{"spot", 100}, {"rate", 0.05}, {"vol", 0.2}}},
               {"params", {{"steps", 200}, {"greeks", false}}}};
    CHECK(evaluate_price(parse_price_request(in))["engine"] == "binomial");

    in["instrument"]["type"] = "asian";
    in["instrument"]["exercise"] = "european";
    in["params"] = {{"paths", 2000}, {"greeks", false}};
    CHECK(evaluate_price(parse_price_request(in))["engine"] == "monte_carlo");
}

TEST_CASE("missing required fields produce clear errors") {
    CHECK_THROWS_AS(parse_price_request(json{{"market", {{"spot", 100}, {"vol", 0.2}}}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(parse_price_request(json{{"instrument", {{"strike", 100}, {"expiry", 1}}}}),
                    std::invalid_argument);

    const json no_strike = {{"instrument", {{"expiry", 1.0}}},
                            {"market", {{"spot", 100}, {"vol", 0.2}}}};
    CHECK_THROWS_AS(parse_price_request(no_strike), std::invalid_argument);
}

TEST_CASE("wrong JSON types are rejected rather than coerced") {
    const json bad = {{"instrument", {{"strike", "one hundred"}, {"expiry", 1.0}}},
                      {"market", {{"spot", 100}, {"vol", 0.2}}}};
    CHECK_THROWS_AS(parse_price_request(bad), std::invalid_argument);

    const json bad_bool = {{"instrument", {{"strike", 100}, {"expiry", 1.0}}},
                           {"market", {{"spot", 100}, {"vol", 0.2}}},
                           {"params", {{"greeks", "yes"}}}};
    CHECK_THROWS_AS(parse_price_request(bad_bool), std::invalid_argument);
}

TEST_CASE("runaway parameter values are clamped") {
    const json in = {{"instrument", {{"strike", 100}, {"expiry", 1.0}}},
                     {"market", {{"spot", 100}, {"vol", 0.2}}},
                     {"params", {{"paths", 999999999999.0}, {"steps", 1000000}, {"return_paths", 99999}}}};
    const auto req = parse_price_request(in);
    CHECK(req.params.paths <= 50000000L);
    CHECK(req.params.steps <= 20000);
    CHECK(req.params.return_paths <= 500);
}

TEST_CASE("a barrier request needs its barrier level") {
    json in = {{"instrument", {{"type", "barrier"}, {"strike", 100}, {"expiry", 1.0},
                               {"barrier_direction", "up_out"}}},
               {"market", {{"spot", 100}, {"vol", 0.2}}}};
    CHECK_THROWS_AS(parse_price_request(in), std::invalid_argument);

    in["instrument"]["barrier"] = 130.0;
    const auto req = parse_price_request(in);
    CHECK(req.instrument.barrier == 130.0);
    CHECK(req.instrument.barrier_direction == BarrierDirection::UpOut);
}

TEST_CASE("an engine that cannot price the instrument is refused") {
    const json in = {{"engine", "black_scholes"},
                     {"instrument", {{"type", "asian"}, {"strike", 100}, {"expiry", 1.0}}},
                     {"market", {{"spot", 100}, {"vol", 0.2}}}};
    CHECK_THROWS_AS(evaluate_price(parse_price_request(in)), std::invalid_argument);
}

TEST_CASE("an unknown engine name is refused") {
    const json in = {{"engine", "crystal_ball"},
                     {"instrument", {{"strike", 100}, {"expiry", 1.0}}},
                     {"market", {{"spot", 100}, {"vol", 0.2}}}};
    CHECK_THROWS_AS(evaluate_price(parse_price_request(in)), std::invalid_argument);
}

TEST_CASE("a spot curve returns monotone x values with payoffs") {
    const json in = {{"instrument", {{"strike", 100}, {"expiry", 1.0}}},
                     {"market", {{"spot", 100}, {"rate", 0.05}, {"vol", 0.2}}},
                     {"axis", "spot"},
                     {"from", 50},
                     {"to", 150},
                     {"points", 21}};

    const json out = evaluate_curve(parse_curve_request(in));
    CHECK(out["axis"] == "spot");
    REQUIRE(out["points"].size() == 21);

    double last_x = -1e9;
    double last_price = -1e9;
    for (const auto& pt : out["points"]) {
        const double x = pt["x"].get<double>();
        CHECK(x > last_x);
        last_x = x;

        // A call's value is increasing in spot.
        const double price = pt["price"].get<double>();
        CHECK(price >= last_price - 1e-9);
        last_price = price;

        REQUIRE(pt.contains("payoff"));
        CHECK(pt["payoff"].get<double>() == Approx(std::max(0.0, x - 100.0)).epsilon(1e-12));
        CHECK(price >= pt["payoff"].get<double>() - 1e-9);  // never below intrinsic
    }
}

TEST_CASE("curve axes other than spot are accepted") {
    json in = {{"instrument", {{"strike", 100}, {"expiry", 1.0}}},
               {"market", {{"spot", 100}, {"vol", 0.2}}},
               {"axis", "vol"},
               {"from", 0.05},
               {"to", 0.8},
               {"points", 10}};
    json out = evaluate_curve(parse_curve_request(in));
    CHECK(out["axis"] == "vol");
    // Vega is positive, so value rises with volatility.
    CHECK(out["points"].front()["price"].get<double>() <
          out["points"].back()["price"].get<double>());

    in["axis"] = "expiry";
    in["from"] = 0.05;
    in["to"] = 2.0;
    out = evaluate_curve(parse_curve_request(in));
    CHECK(out["axis"] == "expiry");
}

TEST_CASE("a curve with a bad range is refused") {
    const json in = {{"instrument", {{"strike", 100}, {"expiry", 1.0}}},
                     {"market", {{"spot", 100}, {"vol", 0.2}}},
                     {"from", 150},
                     {"to", 50}};
    CHECK_THROWS_AS(parse_curve_request(in), std::invalid_argument);
}

TEST_CASE("a curve survives individual grid points that cannot be priced") {
    // Sweeping spot across an up-and-out barrier invalidates the far end.
    const json in = {{"engine", "monte_carlo"},
                     {"instrument", {{"type", "barrier"}, {"strike", 100}, {"expiry", 1.0},
                                     {"barrier", 120.0}, {"barrier_direction", "up_out"},
                                     {"monitoring_steps", 20}}},
                     {"market", {{"spot", 100}, {"vol", 0.2}}},
                     {"params", {{"paths", 2000}, {"greeks", false}}},
                     {"from", 80},
                     {"to", 160},
                     {"points", 9}};

    const json out = evaluate_curve(parse_curve_request(in));
    int priced = 0, errored = 0;
    for (const auto& pt : out["points"]) {
        if (pt.contains("price")) ++priced;
        if (pt.contains("error")) ++errored;
    }
    CHECK(priced > 0);
    CHECK(errored > 0);  // points at or past the barrier
}

TEST_CASE("results serialize Monte Carlo extras") {
    const json in = {{"engine", "monte_carlo"},
                     {"instrument", {{"strike", 100}, {"expiry", 1.0}}},
                     {"market", {{"spot", 100}, {"vol", 0.2}}},
                     {"params", {{"paths", 20000}, {"greeks", false}, {"return_paths", 5},
                                 {"threads", 1}, {"convergence", {1000, 5000}}}}};

    const json out = evaluate_price(parse_price_request(in));
    CHECK(out.contains("std_error"));
    CHECK(out["paths_used"].get<long>() > 0);
    REQUIRE(out["sample_paths"].size() == 5);
    CHECK(out["convergence"].size() == 2);
    CHECK(out["convergence"][0].contains("std_error"));
}

TEST_CASE("engine listing serializes") {
    for (const auto& info : EngineRegistry::instance().list()) {
        const json j = to_json(info);
        CHECK(j["name"].get<std::string>().size() > 0);
        CHECK(j["payoffs"].is_array());
        CHECK(j.contains("stochastic"));
    }
}

TEST_CASE("implied vol results serialize both outcomes") {
    Market mkt{100.0, 0.05, 0.0, 0.0};

    const auto good = implied_volatility({100.0, 1.0, OptionType::Call, 10.45}, mkt);
    const json gj = to_json(good);
    CHECK(gj["converged"] == true);
    CHECK(gj.contains("implied_vol"));
    CHECK_FALSE(gj.contains("error"));

    const auto bad = implied_volatility({100.0, 1.0, OptionType::Call, 500.0}, mkt);
    const json bj = to_json(bad);
    CHECK(bj["converged"] == false);
    CHECK(bj.contains("error"));
    CHECK_FALSE(bj.contains("implied_vol"));
}
