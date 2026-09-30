#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "pricer/engines.hpp"
#include "pricer/types.hpp"

namespace pricer {

/// One decoded /price request.
struct PriceRequest {
    std::string engine;  ///< empty means "pick a sensible default"
    Instrument instrument;
    Market market;
    PricingParams params;
};

/// A /price/curve request: price the instrument across a grid.
enum class CurveAxis { Spot, Volatility, Expiry };

struct CurveRequest {
    PriceRequest base;
    CurveAxis axis = CurveAxis::Spot;
    double from = 0.0;
    double to = 0.0;
    int points = 50;
    bool include_payoff = true;  ///< also return intrinsic value at expiry
};

// Throw std::invalid_argument with a human-readable message on bad input.
PriceRequest parse_price_request(const nlohmann::json& j);
CurveRequest parse_curve_request(const nlohmann::json& j);

nlohmann::json to_json(const PricingResult& r);
nlohmann::json to_json(const EngineInfo& info);
nlohmann::json to_json(const ImpliedVolResult& r);

/// Runs a curve request and returns {axis, points:[{x, price, payoff, greeks...}]}.
nlohmann::json evaluate_curve(const CurveRequest& req);

/// Runs a price request end to end, choosing a default engine when unset.
nlohmann::json evaluate_price(const PriceRequest& req);

}  // namespace pricer
