#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>

#include "pricer/finnhub.hpp"
#include "pricer/market_data.hpp"

using Catch::Approx;
using namespace pricer::market;

namespace {

/// Deterministic stand-in for the network. Counts calls so the tests can prove
/// the cache and rate limiter are actually preventing fetches.
class FakeProvider : public Provider {
public:
    int quote_calls = 0;
    int history_calls = 0;
    int chain_calls = 0;
    bool fail = false;
    double price = 100.0;

    Quote quote(const std::string& symbol) override {
        ++quote_calls;
        if (fail) throw std::runtime_error("simulated provider outage");
        Quote q;
        q.symbol = symbol;
        q.price = price;
        q.previous_close = price - 1.0;
        q.change = 1.0;
        q.timestamp = 1700000000;
        return q;
    }

    History history(const std::string& symbol, int days) override {
        ++history_calls;
        if (fail) throw std::runtime_error("simulated provider outage");
        History h;
        h.symbol = symbol;
        double p = 100.0;
        for (int i = 0; i < days; ++i) {
            // Deterministic wiggle so the return series has non-zero variance.
            p *= std::exp(0.01 * ((i % 3) - 1));
            Candle c;
            c.timestamp = 1700000000 + static_cast<std::int64_t>(i) * 86400;
            c.close = p;
            c.open = p;
            c.high = p * 1.01;
            c.low = p * 0.99;
            c.volume = 1000.0;
            h.candles.push_back(c);
        }
        return h;
    }

    OptionChain chain(const std::string& symbol) override {
        ++chain_calls;
        if (fail) throw std::runtime_error("simulated provider outage");
        OptionChain c;
        c.symbol = symbol;
        c.spot = price;
        OptionExpiry e;
        e.expiry_date = "2026-12-18";
        e.years_to_expiry = 0.25;
        for (double k : {90.0, 100.0, 110.0}) {
            OptionQuote q;
            q.strike = k;
            q.option_type = "call";
            q.bid = 5.0;
            q.ask = 5.4;
            q.last = 5.2;
            e.quotes.push_back(q);
        }
        c.expiries.push_back(e);
        return c;
    }

    std::string name() const override { return "fake"; }
};

std::vector<Candle> constant_series(int n, double value) {
    std::vector<Candle> out;
    for (int i = 0; i < n; ++i) {
        Candle c;
        c.timestamp = i;
        c.close = value;
        out.push_back(c);
    }
    return out;
}

}  // namespace

TEST_CASE("symbol normalization trims, upper-cases, and rejects junk") {
    CHECK(normalize_symbol("  aapl ") == "AAPL");
    CHECK(normalize_symbol("brk.b") == "BRK.B");
    CHECK(normalize_symbol("RDS-A") == "RDS-A");

    CHECK_THROWS_AS(normalize_symbol(""), std::invalid_argument);
    CHECK_THROWS_AS(normalize_symbol("   "), std::invalid_argument);
    CHECK_THROWS_AS(normalize_symbol("WAY-TOO-LONG-SYMBOL"), std::invalid_argument);
    // These are the shapes that would otherwise reach the cache key or URL.
    CHECK_THROWS_AS(normalize_symbol("../../etc"), std::invalid_argument);
    CHECK_THROWS_AS(normalize_symbol("A';DROP"), std::invalid_argument);
    CHECK_THROWS_AS(normalize_symbol("A&b=1"), std::invalid_argument);
}

TEST_CASE("historical volatility matches a hand-computed series") {
    // Alternating +10% and -10% log returns: every return has magnitude 0.1,
    // so the sample mean is ~0 and the sample sd is ~0.1.
    std::vector<Candle> candles;
    double p = 100.0;
    for (int i = 0; i < 61; ++i) {
        Candle c;
        c.timestamp = i;
        c.close = p;
        candles.push_back(c);
        p *= (i % 2 == 0) ? std::exp(0.1) : std::exp(-0.1);
    }

    const auto vol = historical_volatility(candles, 60);
    REQUIRE(vol.has_value());
    // sd of the alternating series is very close to 0.1; annualized by sqrt(252).
    CHECK(*vol == Approx(0.1 * std::sqrt(252.0)).epsilon(0.02));
}

TEST_CASE("historical volatility needs enough observations") {
    CHECK_FALSE(historical_volatility(constant_series(5, 100.0), 60).has_value());
    // A flat series has zero variance, which is not a usable volatility.
    CHECK_FALSE(historical_volatility(constant_series(100, 100.0), 60).has_value());
}

TEST_CASE("historical volatility ignores non-positive prices") {
    auto candles = constant_series(100, 100.0);
    for (std::size_t i = 0; i < candles.size(); ++i) {
        candles[i].close = 100.0 * std::exp(0.01 * static_cast<double>(i % 5));
    }
    candles[50].close = 0.0;  // a bad tick must not produce a NaN
    const auto vol = historical_volatility(candles, 90);
    REQUIRE(vol.has_value());
    CHECK(std::isfinite(*vol));
}

TEST_CASE("rate limiter enforces its budget within the window") {
    auto now = std::chrono::steady_clock::now();
    RateLimiter limiter(5, std::chrono::seconds(60), [&] { return now; });

    for (int i = 0; i < 5; ++i) CHECK(limiter.try_acquire());
    CHECK_FALSE(limiter.try_acquire());
    CHECK(limiter.remaining() == 0);

    // Still inside the window.
    now += std::chrono::seconds(59);
    CHECK_FALSE(limiter.try_acquire());

    // Past it, the budget comes back.
    now += std::chrono::seconds(2);
    CHECK(limiter.remaining() == 5);
    CHECK(limiter.try_acquire());
}

TEST_CASE("rate limiter never exceeds the budget under a burst") {
    auto now = std::chrono::steady_clock::now();
    RateLimiter limiter(60, std::chrono::seconds(60), [&] { return now; });

    int granted = 0;
    for (int i = 0; i < 500; ++i) {
        if (limiter.try_acquire()) ++granted;
    }
    CHECK(granted == 60);
}

TEST_CASE("cache stores and returns quotes with a freshness flag") {
    std::int64_t clock = 1000;
    Cache cache(":memory:", [&] { return clock; });

    Quote q;
    q.symbol = "AAPL";
    q.price = 187.5;
    q.timestamp = 900;
    cache.put_quote(q);

    auto fresh = cache.get_quote("AAPL", 60);
    REQUIRE(fresh.has_value());
    CHECK(fresh->price == Approx(187.5));
    CHECK_FALSE(fresh->stale);

    clock += 120;  // past the 60 second TTL
    auto stale = cache.get_quote("AAPL", 60);
    REQUIRE(stale.has_value());
    CHECK(stale->stale);

    CHECK_FALSE(cache.get_quote("MSFT", 60).has_value());
}

TEST_CASE("cache round-trips histories and chains") {
    std::int64_t clock = 1000;
    Cache cache(":memory:", [&] { return clock; });

    History h;
    h.symbol = "SPY";
    for (int i = 0; i < 30; ++i) {
        Candle c;
        c.timestamp = i;
        c.close = 400.0 + i;
        h.candles.push_back(c);
    }
    cache.put_history(h);

    auto got = cache.get_history("SPY", 30, 3600);
    REQUIRE(got.has_value());
    CHECK(got->candles.size() == 30);
    CHECK(got->candles.back().close == Approx(429.0));

    // A longer request than what was cached is a miss.
    CHECK_FALSE(cache.get_history("SPY", 90, 3600).has_value());

    OptionChain chain;
    chain.symbol = "SPY";
    chain.spot = 415.0;
    OptionExpiry e;
    e.expiry_date = "2026-06-19";
    e.years_to_expiry = 0.7;
    OptionQuote q;
    q.strike = 420.0;
    q.option_type = "call";
    q.bid = 12.0;
    q.ask = 12.4;
    q.provider_implied_vol = 0.19;
    e.quotes.push_back(q);
    chain.expiries.push_back(e);
    cache.put_chain(chain);

    auto back = cache.get_chain("SPY", 900);
    REQUIRE(back.has_value());
    REQUIRE(back->expiries.size() == 1);
    REQUIRE(back->expiries[0].quotes.size() == 1);
    CHECK(back->expiries[0].quotes[0].strike == Approx(420.0));
    REQUIRE(back->expiries[0].quotes[0].provider_implied_vol.has_value());
    CHECK(*back->expiries[0].quotes[0].provider_implied_vol == Approx(0.19));
}

TEST_CASE("cache stats count what is stored") {
    Cache cache(":memory:");
    CHECK(cache.stats().quotes == 0);

    Quote q;
    q.symbol = "A";
    q.price = 1.0;
    cache.put_quote(q);
    q.symbol = "B";
    cache.put_quote(q);
    CHECK(cache.stats().quotes == 2);

    // Re-inserting the same symbol updates rather than duplicates.
    cache.put_quote(q);
    CHECK(cache.stats().quotes == 2);
}

TEST_CASE("option reference price prefers the mid, then the last trade") {
    OptionQuote q;
    q.bid = 4.0;
    q.ask = 4.4;
    q.last = 9.0;
    CHECK(q.reference_price() == Approx(4.2));

    q.bid = 0.0;
    q.ask = 0.0;
    CHECK(q.reference_price() == Approx(9.0));

    q.last = 0.0;
    q.ask = 3.0;
    CHECK(q.reference_price() == Approx(3.0));

    q.ask = 0.0;
    CHECK(q.reference_price() == Approx(0.0));

    // A crossed book falls back rather than returning a nonsense mid.
    q.bid = 5.0;
    q.ask = 4.0;
    q.last = 4.5;
    CHECK(q.reference_price() == Approx(4.5));
}

TEST_CASE("service serves fresh cache without touching the provider") {
    auto provider = std::make_shared<FakeProvider>();
    auto cache = std::make_shared<Cache>(":memory:");
    ServiceConfig cfg;
    cfg.quote_ttl_seconds = 3600;
    Service svc(provider, cache, cfg);

    const auto first = svc.quote("aapl");
    CHECK(first.symbol == "AAPL");
    CHECK(provider->quote_calls == 1);

    for (int i = 0; i < 5; ++i) svc.quote("AAPL");
    CHECK(provider->quote_calls == 1);  // all served from cache
}

TEST_CASE("service falls back to stale cache when the provider fails") {
    auto provider = std::make_shared<FakeProvider>();

    // The cache clock is injected so the entry can be aged deterministically
    // rather than depending on how fast the test runs.
    std::int64_t clock = 1'700'000'000;
    auto cache = std::make_shared<Cache>(":memory:", [&] { return clock; });

    ServiceConfig cfg;
    cfg.quote_ttl_seconds = 60;
    Service svc(provider, cache, cfg);

    const auto good = svc.quote("AAPL");
    CHECK(good.price == Approx(100.0));
    CHECK_FALSE(good.stale);
    CHECK(svc.provider_healthy());
    CHECK(provider->quote_calls == 1);

    clock += 3600;  // the cached quote is now well past its TTL
    provider->fail = true;

    const auto degraded = svc.quote("AAPL");
    CHECK(provider->quote_calls == 2);      // it did try to refresh
    CHECK(degraded.price == Approx(100.0)); // last known value
    CHECK(degraded.stale);
    CHECK_FALSE(svc.provider_healthy());
    CHECK(svc.last_error().find("outage") != std::string::npos);
}

TEST_CASE("service stops fetching once the rate limit is spent") {
    auto provider = std::make_shared<FakeProvider>();
    std::int64_t clock = 1'700'000'000;
    auto cache = std::make_shared<Cache>(":memory:", [&] { return clock; });

    ServiceConfig cfg;
    cfg.quote_ttl_seconds = 0;     // force a refresh attempt every call
    cfg.rate_limit_per_minute = 3;
    Service svc(provider, cache, cfg);

    for (int i = 0; i < 10; ++i) {
        ++clock;  // keep the cached entry stale
        try {
            svc.quote("AAPL");
        } catch (const std::exception&) {
            // Acceptable only before anything has been cached.
        }
    }
    CHECK(provider->quote_calls == 3);
    CHECK(svc.last_error().find("rate limit") != std::string::npos);
}

TEST_CASE("service surfaces an error when there is no cache to fall back on") {
    auto provider = std::make_shared<FakeProvider>();
    provider->fail = true;
    auto cache = std::make_shared<Cache>(":memory:");
    Service svc(provider, cache, ServiceConfig{});

    CHECK_THROWS_AS(svc.quote("NVDA"), std::runtime_error);
}

TEST_CASE("service computes historical volatility from candles") {
    auto provider = std::make_shared<FakeProvider>();
    auto cache = std::make_shared<Cache>(":memory:");
    ServiceConfig cfg;
    cfg.vol_window_days = 60;
    Service svc(provider, cache, cfg);

    const auto vol = svc.historical_vol("TSLA");
    REQUIRE(vol.has_value());
    CHECK(*vol > 0.0);
    CHECK(std::isfinite(*vol));
}

TEST_CASE("service rejects malformed symbols before any fetch") {
    auto provider = std::make_shared<FakeProvider>();
    auto cache = std::make_shared<Cache>(":memory:");
    Service svc(provider, cache, ServiceConfig{});

    CHECK_THROWS_AS(svc.quote("../secrets"), std::invalid_argument);
    CHECK(provider->quote_calls == 0);
}

// ---------------------------------------------------------- Finnhub parsing

TEST_CASE("Finnhub quote payloads parse") {
    const std::string body =
        R"({"c":187.44,"d":1.23,"dp":0.66,"h":188.2,"l":185.1,"o":185.9,"pc":186.21,"t":1700000000})";
    const auto q = FinnhubProvider::parse_quote("AAPL", body);
    CHECK(q.symbol == "AAPL");
    CHECK(q.price == Approx(187.44));
    CHECK(q.percent_change == Approx(0.66));
    CHECK(q.timestamp == 1700000000);
}

TEST_CASE("an unknown ticker's all-zero payload is treated as an error") {
    // Finnhub answers a bad symbol with zeros and HTTP 200 rather than a 404.
    const std::string body = R"({"c":0,"d":null,"dp":null,"h":0,"l":0,"o":0,"pc":0,"t":0})";
    CHECK_THROWS_AS(FinnhubProvider::parse_quote("NOPE", body), std::runtime_error);
}

TEST_CASE("Finnhub candle payloads parse and sort oldest first") {
    const std::string body =
        R"({"s":"ok","t":[300,100,200],"c":[12.0,10.0,11.0],"o":[11.9,9.9,10.9],)"
        R"("h":[12.1,10.1,11.1],"l":[11.8,9.8,10.8],"v":[30,10,20]})";
    const auto h = FinnhubProvider::parse_history("MSFT", body);
    REQUIRE(h.candles.size() == 3);
    CHECK(h.candles[0].timestamp == 100);
    CHECK(h.candles[0].close == Approx(10.0));
    CHECK(h.candles[2].timestamp == 300);
    CHECK(h.candles[2].close == Approx(12.0));
}

TEST_CASE("a no_data candle response is an error") {
    CHECK_THROWS_AS(FinnhubProvider::parse_history("XYZ", R"({"s":"no_data"})"),
                    std::runtime_error);
    CHECK_THROWS_AS(FinnhubProvider::parse_history("XYZ", R"({"s":"ok"})"), std::runtime_error);
}

TEST_CASE("Finnhub option chains parse and drop expired entries") {
    const std::string body = R"({
      "code":"AAPL","lastTradePrice":190.0,
      "data":[
        {"expirationDate":"2099-01-15","options":{
          "CALL":[{"strike":190,"bid":5.0,"ask":5.4,"lastPrice":5.2,"volume":100,
                   "openInterest":500,"impliedVolatility":0.23}],
          "PUT":[{"strike":190,"bid":4.0,"ask":4.4,"lastPrice":4.2,"volume":80,
                  "openInterest":400}]}},
        {"expirationDate":"2001-01-19","options":{
          "CALL":[{"strike":100,"bid":1.0,"ask":1.2}]}}
      ]})";

    const auto chain = FinnhubProvider::parse_chain("AAPL", body);
    CHECK(chain.spot == Approx(190.0));
    REQUIRE(chain.expiries.size() == 1);  // the 2001 expiry is dropped
    CHECK(chain.expiries[0].expiry_date == "2099-01-15");
    CHECK(chain.expiries[0].years_to_expiry > 0.0);
    REQUIRE(chain.expiries[0].quotes.size() == 2);

    const auto& call = chain.expiries[0].quotes[0];
    CHECK(call.option_type == "call");
    REQUIRE(call.provider_implied_vol.has_value());
    CHECK(*call.provider_implied_vol == Approx(0.23));
    CHECK(call.reference_price() == Approx(5.2));
}

TEST_CASE("chain parsing tolerates numbers sent as strings") {
    const std::string body = R"({
      "lastTradePrice":"190.0",
      "data":[{"expirationDate":"2099-06-19","options":{
        "CALL":[{"strike":"200","bid":"2.5","ask":"2.9"}]}}]})";
    const auto chain = FinnhubProvider::parse_chain("AAPL", body);
    REQUIRE(chain.expiries.size() == 1);
    CHECK(chain.expiries[0].quotes[0].strike == Approx(200.0));
    CHECK(chain.expiries[0].quotes[0].reference_price() == Approx(2.7));
}

TEST_CASE("a chain with no live expiries is an error") {
    const std::string body = R"({"data":[{"expirationDate":"2001-01-19","options":{}}]})";
    CHECK_THROWS_AS(FinnhubProvider::parse_chain("AAPL", body), std::runtime_error);
    CHECK_THROWS_AS(FinnhubProvider::parse_chain("AAPL", R"({"code":"AAPL"})"),
                    std::runtime_error);
}

TEST_CASE("a provider with no key reports itself unconfigured") {
    FinnhubProvider provider("");
    CHECK_FALSE(provider.configured());
    CHECK_THROWS_AS(provider.quote("AAPL"), std::runtime_error);
}
