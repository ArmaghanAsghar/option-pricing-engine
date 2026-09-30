#include <algorithm>
#include <vector>

#include "pricer/market_data.hpp"

namespace pricer::market {

RateLimiter::RateLimiter(int max_calls, std::chrono::seconds window, Clock clock)
    : max_calls_(max_calls < 1 ? 1 : max_calls),
      window_(window),
      clock_(clock ? std::move(clock)
                   : [] { return std::chrono::steady_clock::now(); }) {
    calls_.reserve(static_cast<std::size_t>(max_calls_));
}

void RateLimiter::evict() {
    const auto cutoff = clock_() - window_;
    calls_.erase(std::remove_if(calls_.begin(), calls_.end(),
                                [cutoff](const auto& t) { return t <= cutoff; }),
                 calls_.end());
}

bool RateLimiter::try_acquire() {
    evict();
    if (static_cast<int>(calls_.size()) >= max_calls_) return false;
    calls_.push_back(clock_());
    return true;
}

int RateLimiter::remaining() {
    evict();
    return max_calls_ - static_cast<int>(calls_.size());
}

}  // namespace pricer::market
