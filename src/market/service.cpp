#include <stdexcept>
#include <string>
#include <utility>

#include "pricer/market_data.hpp"

namespace pricer::market {

Service::Service(std::shared_ptr<Provider> provider, std::shared_ptr<Cache> cache,
                 ServiceConfig config)
    : provider_(std::move(provider)),
      cache_(std::move(cache)),
      config_(std::move(config)),
      limiter_(config_.rate_limit_per_minute, std::chrono::seconds(60)) {}

namespace {

/// Fresh cache wins; then the provider; then stale cache. The template keeps
/// the three reads from drifting apart.
template <typename T, typename GetCached, typename Fetch, typename Store>
T resolve(GetCached get_cached, Fetch fetch, Store store, RateLimiter& limiter,
          bool& healthy, std::string& last_error) {
    auto cached = get_cached();
    if (cached && !cached->stale) return *cached;

    if (limiter.try_acquire()) {
        try {
            T fresh = fetch();
            store(fresh);
            healthy = true;
            last_error.clear();
            return fresh;
        } catch (const std::exception& e) {
            healthy = false;
            last_error = e.what();
        }
    } else {
        healthy = false;
        last_error = "local rate limit reached; serving cached data";
    }

    if (cached) {
        cached->stale = true;
        return *cached;
    }
    throw std::runtime_error(last_error.empty() ? "no data available" : last_error);
}

}  // namespace

Quote Service::quote(const std::string& symbol) {
    const std::string sym = normalize_symbol(symbol);
    return resolve<Quote>([&] { return cache_->get_quote(sym, config_.quote_ttl_seconds); },
                          [&] { return provider_->quote(sym); },
                          [&](const Quote& q) { cache_->put_quote(q); }, limiter_,
                          provider_healthy_, last_error_);
}

History Service::history(const std::string& symbol, int days) {
    const std::string sym = normalize_symbol(symbol);
    const int want = days > 0 ? days : config_.vol_window_days;
    return resolve<History>(
        [&] { return cache_->get_history(sym, want, config_.history_ttl_seconds); },
        [&] { return provider_->history(sym, want); },
        [&](const History& h) { cache_->put_history(h); }, limiter_, provider_healthy_,
        last_error_);
}

OptionChain Service::chain(const std::string& symbol) {
    const std::string sym = normalize_symbol(symbol);
    return resolve<OptionChain>([&] { return cache_->get_chain(sym, config_.chain_ttl_seconds); },
                                [&] { return provider_->chain(sym); },
                                [&](const OptionChain& c) { cache_->put_chain(c); }, limiter_,
                                provider_healthy_, last_error_);
}

std::optional<double> Service::historical_vol(const std::string& symbol) {
    try {
        const History h = history(symbol, config_.vol_window_days);
        return historical_volatility(h.candles, config_.vol_window_days);
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return std::nullopt;
    }
}

}  // namespace pricer::market
