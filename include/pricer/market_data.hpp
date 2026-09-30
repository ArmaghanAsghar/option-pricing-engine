#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pricer::market {

struct Quote {
    std::string symbol;
    double price = 0.0;
    double change = 0.0;
    double percent_change = 0.0;
    double high = 0.0;
    double low = 0.0;
    double open = 0.0;
    double previous_close = 0.0;
    std::int64_t timestamp = 0;  ///< Unix seconds
    bool stale = false;          ///< served from cache past its TTL
};

struct Candle {
    std::int64_t timestamp = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
};

struct History {
    std::string symbol;
    std::vector<Candle> candles;  ///< oldest first
    bool stale = false;
};

struct OptionQuote {
    double strike = 0.0;
    std::string option_type;  ///< "call" or "put"
    double bid = 0.0;
    double ask = 0.0;
    double last = 0.0;
    double volume = 0.0;
    double open_interest = 0.0;
    std::optional<double> provider_implied_vol;

    /// Mid where both sides are present, else last. Zero when unusable.
    double reference_price() const;
};

struct OptionExpiry {
    std::string expiry_date;  ///< YYYY-MM-DD
    double years_to_expiry = 0.0;
    std::vector<OptionQuote> quotes;
};

struct OptionChain {
    std::string symbol;
    double spot = 0.0;
    std::vector<OptionExpiry> expiries;
    bool stale = false;
};

/// Annualized standard deviation of log returns over the supplied candles.
/// Returns nullopt when fewer than `min_observations` returns are available.
std::optional<double> historical_volatility(const std::vector<Candle>& candles,
                                            int window_days = 60,
                                            int min_observations = 20);

/// Source of market data. Implementations may throw std::runtime_error on
/// network or parse failure; callers are expected to fall back to cache.
class Provider {
public:
    virtual ~Provider() = default;
    virtual Quote quote(const std::string& symbol) = 0;
    virtual History history(const std::string& symbol, int days) = 0;
    virtual OptionChain chain(const std::string& symbol) = 0;
    virtual std::string name() const = 0;
};

/// Token-bucket limiter so a burst of UI requests cannot blow the provider's
/// published rate limit.
class RateLimiter {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    RateLimiter(int max_calls, std::chrono::seconds window, Clock clock = nullptr);

    /// Records a call if there is budget. Returns false when the caller should
    /// serve from cache instead.
    bool try_acquire();
    int remaining();

private:
    void evict();

    int max_calls_;
    std::chrono::seconds window_;
    Clock clock_;
    std::vector<std::chrono::steady_clock::time_point> calls_;
};

/// SQLite-backed cache. Falls back to an in-memory database when the path
/// cannot be opened, so the API still runs on a read-only filesystem.
class Cache {
public:
    using Clock = std::function<std::int64_t()>;  ///< Unix seconds

    explicit Cache(const std::string& path, Clock clock = nullptr);
    ~Cache();

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    void put_quote(const Quote& q);
    /// Returns the cached quote when present. `stale` is set when older than ttl.
    std::optional<Quote> get_quote(const std::string& symbol, int ttl_seconds);

    void put_history(const History& h);
    std::optional<History> get_history(const std::string& symbol, int days, int ttl_seconds);

    void put_chain(const OptionChain& c);
    std::optional<OptionChain> get_chain(const std::string& symbol, int ttl_seconds);

    struct Stats {
        long quotes = 0;
        long candles = 0;
        long chains = 0;
    };
    Stats stats();

    bool in_memory() const { return in_memory_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool in_memory_ = false;
};

struct ServiceConfig {
    std::string cache_path = "/data/market.db";
    int quote_ttl_seconds = 60;
    int history_ttl_seconds = 6 * 3600;
    int chain_ttl_seconds = 15 * 60;
    int vol_window_days = 60;
    int rate_limit_per_minute = 55;  ///< Finnhub free tier is 60
};

/// Ties a provider to the cache and the rate limiter. Every read prefers fresh
/// cache, then the provider, then stale cache.
class Service {
public:
    Service(std::shared_ptr<Provider> provider, std::shared_ptr<Cache> cache,
            ServiceConfig config);

    Quote quote(const std::string& symbol);
    History history(const std::string& symbol, int days);
    OptionChain chain(const std::string& symbol);

    /// Historical volatility from cached or freshly fetched candles.
    std::optional<double> historical_vol(const std::string& symbol);

    /// True when the provider answered its last call without error.
    bool provider_healthy() const { return provider_healthy_; }
    std::string last_error() const { return last_error_; }
    Cache::Stats cache_stats() { return cache_->stats(); }
    const ServiceConfig& config() const { return config_; }

private:
    std::shared_ptr<Provider> provider_;
    std::shared_ptr<Cache> cache_;
    ServiceConfig config_;
    RateLimiter limiter_;
    bool provider_healthy_ = true;
    std::string last_error_;
};

/// Normalizes a ticker: trims blanks and upper-cases. Throws
/// std::invalid_argument when the result is not a plausible symbol, which also
/// keeps path traversal and injection out of the cache keys.
std::string normalize_symbol(const std::string& raw);

}  // namespace pricer::market
