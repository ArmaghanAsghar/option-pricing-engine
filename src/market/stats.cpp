#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "pricer/market_data.hpp"

namespace pricer::market {

double OptionQuote::reference_price() const {
    // A two-sided quote is the most reliable mark; last trade is the fallback.
    if (bid > 0.0 && ask > 0.0 && ask >= bid) return 0.5 * (bid + ask);
    if (last > 0.0) return last;
    if (ask > 0.0) return ask;
    if (bid > 0.0) return bid;
    return 0.0;
}

std::optional<double> historical_volatility(const std::vector<Candle>& candles,
                                            int window_days, int min_observations) {
    if (window_days < 2) return std::nullopt;

    // Take the most recent window_days closes.
    const std::size_t want = static_cast<std::size_t>(window_days);
    const std::size_t begin = candles.size() > want ? candles.size() - want : 0;

    std::vector<double> returns;
    returns.reserve(candles.size());
    for (std::size_t i = begin + 1; i < candles.size(); ++i) {
        const double prev = candles[i - 1].close;
        const double curr = candles[i].close;
        if (prev > 0.0 && curr > 0.0) returns.push_back(std::log(curr / prev));
    }

    if (returns.size() < static_cast<std::size_t>(min_observations)) return std::nullopt;

    const double n = static_cast<double>(returns.size());
    const double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / n;

    double sum_sq = 0.0;
    for (double r : returns) {
        const double d = r - mean;
        sum_sq += d * d;
    }
    const double variance = sum_sq / (n - 1.0);  // sample variance

    // 252 trading days a year.
    const double annualized = std::sqrt(variance * 252.0);
    if (!std::isfinite(annualized) || annualized <= 0.0) return std::nullopt;
    return annualized;
}

}  // namespace pricer::market
