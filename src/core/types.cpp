#include "pricer/types.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace pricer {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
}  // namespace

std::string to_string(OptionType v) {
    return v == OptionType::Call ? "call" : "put";
}

std::string to_string(ExerciseStyle v) {
    return v == ExerciseStyle::European ? "european" : "american";
}

std::string to_string(PayoffKind v) {
    switch (v) {
        case PayoffKind::Vanilla: return "vanilla";
        case PayoffKind::Asian: return "asian";
        case PayoffKind::Barrier: return "barrier";
    }
    return "vanilla";
}

std::string to_string(BarrierDirection v) {
    switch (v) {
        case BarrierDirection::UpIn: return "up_in";
        case BarrierDirection::UpOut: return "up_out";
        case BarrierDirection::DownIn: return "down_in";
        case BarrierDirection::DownOut: return "down_out";
    }
    return "up_out";
}

OptionType option_type_from_string(const std::string& s) {
    const auto v = lower(s);
    if (v == "call" || v == "c") return OptionType::Call;
    if (v == "put" || v == "p") return OptionType::Put;
    throw std::invalid_argument("unknown option type: " + s);
}

ExerciseStyle exercise_style_from_string(const std::string& s) {
    const auto v = lower(s);
    if (v == "european" || v == "euro") return ExerciseStyle::European;
    if (v == "american") return ExerciseStyle::American;
    throw std::invalid_argument("unknown exercise style: " + s);
}

PayoffKind payoff_kind_from_string(const std::string& s) {
    const auto v = lower(s);
    if (v == "vanilla" || v == "european" || v == "american") return PayoffKind::Vanilla;
    if (v == "asian") return PayoffKind::Asian;
    if (v == "barrier") return PayoffKind::Barrier;
    throw std::invalid_argument("unknown payoff kind: " + s);
}

BarrierDirection barrier_direction_from_string(const std::string& s) {
    const auto v = lower(s);
    if (v == "up_in" || v == "up-and-in" || v == "ui") return BarrierDirection::UpIn;
    if (v == "up_out" || v == "up-and-out" || v == "uo") return BarrierDirection::UpOut;
    if (v == "down_in" || v == "down-and-in" || v == "di") return BarrierDirection::DownIn;
    if (v == "down_out" || v == "down-and-out" || v == "do") return BarrierDirection::DownOut;
    throw std::invalid_argument("unknown barrier direction: " + s);
}

void validate(const Instrument& inst, const Market& mkt) {
    if (!(inst.strike > 0.0)) throw std::invalid_argument("strike must be positive");
    if (!(inst.expiry > 0.0)) throw std::invalid_argument("expiry must be positive");
    if (!(mkt.spot > 0.0)) throw std::invalid_argument("spot must be positive");
    if (!(mkt.vol >= 0.0)) throw std::invalid_argument("volatility must be non-negative");

    if (inst.kind == PayoffKind::Asian) {
        if (inst.averaging_steps < 1)
            throw std::invalid_argument("averaging_steps must be at least 1");
        if (inst.exercise != ExerciseStyle::European)
            throw std::invalid_argument("Asian options support European exercise only");
    }

    if (inst.kind == PayoffKind::Barrier) {
        if (!(inst.barrier > 0.0))
            throw std::invalid_argument("barrier must be positive");
        if (inst.monitoring_steps < 1)
            throw std::invalid_argument("monitoring_steps must be at least 1");
        if (inst.exercise != ExerciseStyle::European)
            throw std::invalid_argument("barrier options support European exercise only");

        const bool up = inst.barrier_direction == BarrierDirection::UpIn ||
                        inst.barrier_direction == BarrierDirection::UpOut;
        if (up && mkt.spot >= inst.barrier)
            throw std::invalid_argument("spot is already at or above an up barrier");
        if (!up && mkt.spot <= inst.barrier)
            throw std::invalid_argument("spot is already at or below a down barrier");
    }
}

}  // namespace pricer
