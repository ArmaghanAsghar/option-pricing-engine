#pragma once

#include <optional>
#include <string>
#include <vector>

namespace pricer {

enum class OptionType { Call, Put };
enum class ExerciseStyle { European, American };
enum class PayoffKind { Vanilla, Asian, Barrier };
enum class BarrierDirection { UpIn, UpOut, DownIn, DownOut };

/// Contract terms. Fields not relevant to a PayoffKind are ignored.
struct Instrument {
    PayoffKind kind = PayoffKind::Vanilla;
    OptionType option = OptionType::Call;
    ExerciseStyle exercise = ExerciseStyle::European;

    double strike = 100.0;
    double expiry = 1.0;  ///< years

    // Asian only: number of equally spaced averaging observations.
    int averaging_steps = 252;

    // Barrier only.
    BarrierDirection barrier_direction = BarrierDirection::UpOut;
    double barrier = 0.0;
    double rebate = 0.0;
    int monitoring_steps = 252;  ///< discrete proxy for continuous monitoring
};

/// Market state. Flat volatility for now; a surface can replace `vol` later.
struct Market {
    double spot = 100.0;
    double rate = 0.05;      ///< continuously compounded risk-free rate
    double dividend = 0.0;   ///< continuous dividend yield
    double vol = 0.20;       ///< annualized
};

/// Engine-specific knobs. Engines ignore what they do not use.
struct PricingParams {
    bool greeks = true;

    // Tree engines.
    int steps = 512;

    // Monte Carlo.
    long paths = 100000;
    unsigned long long seed = 42ULL;
    int threads = 0;              ///< 0 = hardware concurrency
    bool antithetic = true;
    int return_paths = 0;         ///< sample of paths to return for charting

    /// Simulation grid resolution, for charting only. A European vanilla needs
    /// just one terminal draw, which makes a returned path a straight line.
    /// Raising this subdivides the path without changing the price: geometric
    /// Brownian motion is exact at any step size, so the terminal distribution
    /// is identical. Ignored for Asian and barrier payoffs, where the
    /// observation count is part of the contract.
    int path_steps = 0;

    std::vector<long> convergence;  ///< path counts to report intermediate estimates at
};

struct ConvergencePoint {
    long paths = 0;
    double price = 0.0;
    double std_error = 0.0;
};

struct PricingResult {
    double price = 0.0;

    // Greeks. Present when PricingParams::greeks was set.
    std::optional<double> delta;
    std::optional<double> gamma;
    std::optional<double> vega;
    std::optional<double> theta;
    std::optional<double> rho;

    std::optional<double> std_error;  ///< stochastic engines only
    long paths_used = 0;
    int steps_used = 0;

    /// Sampled paths for charting: [path][step], including t=0.
    std::vector<std::vector<double>> sample_paths;
    std::vector<ConvergencePoint> convergence;

    std::string engine;
};

// Text conversions used by the JSON layer and the CLI.
std::string to_string(OptionType v);
std::string to_string(ExerciseStyle v);
std::string to_string(PayoffKind v);
std::string to_string(BarrierDirection v);

OptionType option_type_from_string(const std::string& s);
ExerciseStyle exercise_style_from_string(const std::string& s);
PayoffKind payoff_kind_from_string(const std::string& s);
BarrierDirection barrier_direction_from_string(const std::string& s);

/// Throws std::invalid_argument when the combination cannot be priced at all.
void validate(const Instrument& inst, const Market& mkt);

}  // namespace pricer
