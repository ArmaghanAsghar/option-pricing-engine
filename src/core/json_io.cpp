#include "pricer/json_io.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "pricer/pricer.hpp"

namespace pricer {

using nlohmann::json;

namespace {

const json& require_object(const json& j, const char* what) {
    if (!j.is_object()) {
        throw std::invalid_argument(std::string(what) + " must be a JSON object");
    }
    return j;
}

double get_number(const json& j, const char* key, double fallback) {
    if (!j.contains(key) || j.at(key).is_null()) return fallback;
    if (!j.at(key).is_number()) {
        throw std::invalid_argument(std::string(key) + " must be a number");
    }
    return j.at(key).get<double>();
}

double require_number(const json& j, const char* key) {
    if (!j.contains(key) || j.at(key).is_null()) {
        throw std::invalid_argument(std::string("missing required field: ") + key);
    }
    if (!j.at(key).is_number()) {
        throw std::invalid_argument(std::string(key) + " must be a number");
    }
    return j.at(key).get<double>();
}

int get_int(const json& j, const char* key, int fallback) {
    const double v = get_number(j, key, static_cast<double>(fallback));
    return static_cast<int>(std::llround(v));
}

long get_long(const json& j, const char* key, long fallback) {
    const double v = get_number(j, key, static_cast<double>(fallback));
    return static_cast<long>(std::llround(v));
}

bool get_bool(const json& j, const char* key, bool fallback) {
    if (!j.contains(key) || j.at(key).is_null()) return fallback;
    if (!j.at(key).is_boolean()) {
        throw std::invalid_argument(std::string(key) + " must be a boolean");
    }
    return j.at(key).get<bool>();
}

std::string get_string(const json& j, const char* key, const std::string& fallback) {
    if (!j.contains(key) || j.at(key).is_null()) return fallback;
    if (!j.at(key).is_string()) {
        throw std::invalid_argument(std::string(key) + " must be a string");
    }
    return j.at(key).get<std::string>();
}

Instrument parse_instrument(const json& j) {
    require_object(j, "instrument");
    Instrument inst;

    // "type" carries the payoff family; a bare "european"/"american" there is
    // also accepted and taken to mean a vanilla with that exercise.
    const std::string type = get_string(j, "type", "vanilla");
    inst.kind = payoff_kind_from_string(type);

    std::string exercise_default = "european";
    const std::string lowered_type = type;
    if (lowered_type == "american" || lowered_type == "American") {
        exercise_default = "american";
    }

    inst.option = option_type_from_string(get_string(j, "option", "call"));
    inst.exercise = exercise_style_from_string(get_string(j, "exercise", exercise_default));
    inst.strike = require_number(j, "strike");
    inst.expiry = require_number(j, "expiry");

    inst.averaging_steps = get_int(j, "averaging_steps", 252);
    inst.monitoring_steps = get_int(j, "monitoring_steps", 252);
    inst.rebate = get_number(j, "rebate", 0.0);

    if (inst.kind == PayoffKind::Barrier) {
        inst.barrier = require_number(j, "barrier");
        inst.barrier_direction =
            barrier_direction_from_string(get_string(j, "barrier_direction", "up_out"));
    }
    return inst;
}

Market parse_market(const json& j) {
    require_object(j, "market");
    Market m;
    m.spot = require_number(j, "spot");
    m.rate = get_number(j, "rate", 0.05);
    m.dividend = get_number(j, "dividend", 0.0);
    m.vol = require_number(j, "vol");
    return m;
}

PricingParams parse_params(const json& j) {
    PricingParams p;
    if (j.is_null()) return p;
    require_object(j, "params");

    p.greeks = get_bool(j, "greeks", true);
    p.steps = get_int(j, "steps", 512);
    p.paths = get_long(j, "paths", 100000);
    p.seed = static_cast<unsigned long long>(get_long(j, "seed", 42));
    p.threads = get_int(j, "threads", 0);
    p.antithetic = get_bool(j, "antithetic", true);
    p.return_paths = get_int(j, "return_paths", 0);
    p.path_steps = get_int(j, "path_steps", 0);

    // Guard rails: an unbounded request would let one caller pin every core.
    p.paths = std::clamp<long>(p.paths, 1, 50000000L);
    p.steps = std::clamp(p.steps, 1, 20000);
    p.return_paths = std::clamp(p.return_paths, 0, 500);
    p.path_steps = std::clamp(p.path_steps, 0, 2000);
    p.threads = std::clamp(p.threads, 0, 256);

    if (j.contains("convergence") && !j.at("convergence").is_null()) {
        if (!j.at("convergence").is_array()) {
            throw std::invalid_argument("convergence must be an array of path counts");
        }
        for (const auto& item : j.at("convergence")) {
            if (!item.is_number()) {
                throw std::invalid_argument("convergence entries must be numbers");
            }
            p.convergence.push_back(static_cast<long>(item.get<double>()));
        }
    }
    return p;
}

json greeks_to_json(const PricingResult& r) {
    json g = json::object();
    if (r.delta) g["delta"] = *r.delta;
    if (r.gamma) g["gamma"] = *r.gamma;
    if (r.vega) g["vega"] = *r.vega;
    if (r.theta) g["theta"] = *r.theta;
    if (r.rho) g["rho"] = *r.rho;
    return g;
}

double intrinsic_at(const Instrument& inst, double spot) {
    return inst.option == OptionType::Call ? std::max(0.0, spot - inst.strike)
                                           : std::max(0.0, inst.strike - spot);
}

}  // namespace

PriceRequest parse_price_request(const json& j) {
    require_object(j, "request");
    PriceRequest req;
    req.engine = get_string(j, "engine", "");
    if (!j.contains("instrument")) {
        throw std::invalid_argument("missing required field: instrument");
    }
    if (!j.contains("market")) {
        throw std::invalid_argument("missing required field: market");
    }
    req.instrument = parse_instrument(j.at("instrument"));
    req.market = parse_market(j.at("market"));
    req.params = parse_params(j.contains("params") ? j.at("params") : json());
    return req;
}

CurveRequest parse_curve_request(const json& j) {
    CurveRequest req;
    req.base = parse_price_request(j);

    const std::string axis = get_string(j, "axis", "spot");
    if (axis == "spot") {
        req.axis = CurveAxis::Spot;
    } else if (axis == "vol" || axis == "volatility") {
        req.axis = CurveAxis::Volatility;
    } else if (axis == "expiry" || axis == "time") {
        req.axis = CurveAxis::Expiry;
    } else {
        throw std::invalid_argument("axis must be spot, vol, or expiry");
    }

    // Default to a band around the current value of whichever axis was chosen.
    double centre = req.base.market.spot;
    if (req.axis == CurveAxis::Volatility) centre = req.base.market.vol;
    if (req.axis == CurveAxis::Expiry) centre = req.base.instrument.expiry;

    req.from = get_number(j, "from", centre * 0.5);
    req.to = get_number(j, "to", centre * 1.5);
    req.points = std::clamp(get_int(j, "points", 50), 2, 1000);
    req.include_payoff = get_bool(j, "include_payoff", true);

    if (!(req.to > req.from)) {
        throw std::invalid_argument("curve 'to' must be greater than 'from'");
    }
    return req;
}

json to_json(const PricingResult& r) {
    json out;
    out["price"] = r.price;
    out["engine"] = r.engine;

    const json g = greeks_to_json(r);
    if (!g.empty()) out["greeks"] = g;

    if (r.std_error) out["std_error"] = *r.std_error;
    if (r.paths_used > 0) out["paths_used"] = r.paths_used;
    if (r.steps_used > 0) out["steps_used"] = r.steps_used;

    if (!r.sample_paths.empty()) out["sample_paths"] = r.sample_paths;

    if (!r.convergence.empty()) {
        json conv = json::array();
        for (const auto& c : r.convergence) {
            conv.push_back({{"paths", c.paths}, {"price", c.price}, {"std_error", c.std_error}});
        }
        out["convergence"] = conv;
    }
    return out;
}

json to_json(const EngineInfo& info) {
    return {{"name", info.name},
            {"description", info.description},
            {"payoffs", info.payoffs},
            {"exercises", info.exercises},
            {"stochastic", info.stochastic}};
}

json to_json(const ImpliedVolResult& r) {
    json out;
    out["strike"] = r.strike;
    out["expiry"] = r.expiry;
    out["option"] = to_string(r.option);
    out["market_price"] = r.market_price;
    out["converged"] = r.converged;
    out["well_conditioned"] = r.well_conditioned;
    if (r.converged) {
        out["implied_vol"] = r.implied_vol;
        out["vol_uncertainty"] = r.vol_uncertainty;
    } else {
        out["error"] = r.error;
        // A solve that found a price-matching volatility but could not trust it
        // still reports the number, flagged, so a chart can widen the point.
        if (r.implied_vol > 0.0) out["unreliable_vol"] = r.implied_vol;
    }
    return out;
}

json evaluate_price(const PriceRequest& req) {
    const auto& registry = EngineRegistry::instance();
    const std::string engine_name =
        req.engine.empty() ? registry.default_engine_for(req.instrument) : req.engine;

    const auto engine = registry.create(engine_name);
    if (!engine->supports(req.instrument)) {
        throw std::invalid_argument("engine '" + engine_name +
                                    "' does not support this instrument");
    }
    return to_json(engine->price(req.instrument, req.market, req.params));
}

json evaluate_curve(const CurveRequest& req) {
    const auto& registry = EngineRegistry::instance();
    const std::string engine_name = req.base.engine.empty()
                                        ? registry.default_engine_for(req.base.instrument)
                                        : req.base.engine;
    const auto engine = registry.create(engine_name);
    if (!engine->supports(req.base.instrument)) {
        throw std::invalid_argument("engine '" + engine_name +
                                    "' does not support this instrument");
    }

    // Curves are for plotting, so drop the per-point extras that would balloon
    // the response.
    PricingParams params = req.base.params;
    params.return_paths = 0;
    params.path_steps = 0;
    params.convergence.clear();

    json points = json::array();
    const double step = (req.to - req.from) / (req.points - 1);

    for (int i = 0; i < req.points; ++i) {
        const double x = req.from + step * i;

        Instrument inst = req.base.instrument;
        Market mkt = req.base.market;
        switch (req.axis) {
            case CurveAxis::Spot: mkt.spot = x; break;
            case CurveAxis::Volatility: mkt.vol = x; break;
            case CurveAxis::Expiry: inst.expiry = x; break;
        }

        json point;
        point["x"] = x;
        try {
            const PricingResult r = engine->price(inst, mkt, params);
            point["price"] = r.price;
            const json g = greeks_to_json(r);
            if (!g.empty()) point["greeks"] = g;
            if (r.std_error) point["std_error"] = *r.std_error;
        } catch (const std::exception& e) {
            // One bad grid point (a barrier already breached, say) should not
            // sink the whole curve.
            point["error"] = e.what();
        }

        if (req.include_payoff && req.axis == CurveAxis::Spot) {
            point["payoff"] = intrinsic_at(req.base.instrument, x);
        }
        points.push_back(point);
    }

    const char* axis_name = req.axis == CurveAxis::Spot      ? "spot"
                            : req.axis == CurveAxis::Volatility ? "vol"
                                                                : "expiry";
    return {{"axis", axis_name}, {"engine", engine_name}, {"points", points}};
}

}  // namespace pricer
