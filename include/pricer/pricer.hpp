#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pricer/types.hpp"

namespace pricer {

/// Stateless pricing engine. Implementations must be safe to call concurrently.
class Pricer {
public:
    virtual ~Pricer() = default;

    virtual PricingResult price(const Instrument& inst, const Market& mkt,
                                const PricingParams& params) const = 0;

    virtual std::string name() const = 0;

    /// Whether this engine can handle the contract at all.
    virtual bool supports(const Instrument& inst) const = 0;
};

struct EngineInfo {
    std::string name;
    std::string description;
    std::vector<std::string> payoffs;
    std::vector<std::string> exercises;
    bool stochastic = false;
};

/// Name -> factory lookup shared by the CLI and the HTTP API.
class EngineRegistry {
public:
    static const EngineRegistry& instance();

    /// Throws std::invalid_argument for an unknown name.
    std::unique_ptr<Pricer> create(const std::string& name) const;

    bool has(const std::string& name) const;
    std::vector<EngineInfo> list() const;

    /// Picks the cheapest engine that supports the contract.
    std::string default_engine_for(const Instrument& inst) const;

private:
    EngineRegistry() = default;
};

/// Bump-and-reprice Greeks, shared by every engine without analytic formulas.
/// Reuses the same seed across bumps so Monte Carlo noise largely cancels.
PricingResult finite_difference_greeks(
    const Pricer& engine, const Instrument& inst, const Market& mkt,
    const PricingParams& params, const PricingResult& base);

}  // namespace pricer
