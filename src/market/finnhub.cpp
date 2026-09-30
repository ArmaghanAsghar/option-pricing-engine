#include "pricer/finnhub.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#endif

namespace pricer::market {

namespace {

constexpr const char* kHost = "https://finnhub.io";

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// Days between a YYYY-MM-DD date and today, as a year fraction. Returns 0 for
/// anything unparseable or already expired.
double years_until(const std::string& date) {
    if (date.size() < 10) return 0.0;
    std::tm tm{};
    tm.tm_year = std::atoi(date.substr(0, 4).c_str()) - 1900;
    tm.tm_mon = std::atoi(date.substr(5, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(date.substr(8, 2).c_str());
    tm.tm_hour = 16;  // US close

    const std::time_t expiry = timegm(&tm);
    if (expiry <= 0) return 0.0;

    const double seconds = static_cast<double>(expiry - now_seconds());
    if (seconds <= 0.0) return 0.0;
    return seconds / (365.25 * 24.0 * 3600.0);
}

double json_number(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return 0.0;
    const auto& v = j.at(key);
    if (v.is_number()) return v.get<double>();
    // Finnhub occasionally returns numbers as strings in the chain payload.
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (const std::exception&) {
            return 0.0;
        }
    }
    return 0.0;
}

}  // namespace

std::string normalize_symbol(const std::string& raw) {
    std::string s;
    for (char c : raw) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (s.empty() || s.size() > 12) {
        throw std::invalid_argument("symbol must be 1 to 12 characters");
    }
    for (char c : s) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-';
        if (!ok) throw std::invalid_argument("symbol contains unsupported characters");
    }
    return s;
}

FinnhubProvider::FinnhubProvider(std::string api_key, std::string base_url)
    : api_key_(std::move(api_key)),
      base_url_(base_url.empty() ? kHost : std::move(base_url)) {}

bool FinnhubProvider::configured() const {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    return !api_key_.empty();
#else
    return false;
#endif
}

std::string FinnhubProvider::get(const std::string& path) {
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    (void)path;
    throw std::runtime_error(
        "this build has no TLS support, so live market data is unavailable");
#else
    if (api_key_.empty()) {
        throw std::runtime_error("FINNHUB_KEY is not set; live market data is unavailable");
    }

    httplib::Client client(base_url_);
    client.set_connection_timeout(5);
    client.set_read_timeout(10);
    client.set_follow_location(true);
    // The key goes in a header, never the query string, so it stays out of logs.
    client.set_default_headers({{"X-Finnhub-Token", api_key_}});

    const auto res = client.Get(path);
    if (!res) {
        throw std::runtime_error("network error reaching Finnhub: " +
                                 httplib::to_string(res.error()));
    }
    if (res->status == 401 || res->status == 403) {
        throw std::runtime_error("Finnhub rejected the API key");
    }
    if (res->status == 429) {
        throw std::runtime_error("Finnhub rate limit exceeded");
    }
    if (res->status != 200) {
        throw std::runtime_error("Finnhub returned HTTP " + std::to_string(res->status));
    }
    return res->body;
#endif
}

Quote FinnhubProvider::parse_quote(const std::string& symbol, const std::string& body) {
    const auto j = nlohmann::json::parse(body);

    Quote q;
    q.symbol = symbol;
    q.price = json_number(j, "c");
    q.change = json_number(j, "d");
    q.percent_change = json_number(j, "dp");
    q.high = json_number(j, "h");
    q.low = json_number(j, "l");
    q.open = json_number(j, "o");
    q.previous_close = json_number(j, "pc");
    q.timestamp = static_cast<std::int64_t>(json_number(j, "t"));

    // Finnhub answers an unknown ticker with an all-zero payload rather than a
    // 404, so an absent price is how a bad symbol surfaces.
    if (!(q.price > 0.0)) {
        throw std::runtime_error("no quote available for symbol " + symbol);
    }
    if (q.timestamp == 0) q.timestamp = now_seconds();
    return q;
}

History FinnhubProvider::parse_history(const std::string& symbol, const std::string& body) {
    const auto j = nlohmann::json::parse(body);

    History h;
    h.symbol = symbol;

    if (j.value("s", std::string("no_data")) != "ok") {
        throw std::runtime_error("no historical data available for " + symbol);
    }
    if (!j.contains("c") || !j.at("c").is_array()) {
        throw std::runtime_error("malformed candle payload for " + symbol);
    }

    const auto& closes = j.at("c");
    const auto& times = j.at("t");
    const auto n = closes.size();
    h.candles.reserve(n);

    const auto at = [&](const char* key, std::size_t i) -> double {
        if (!j.contains(key) || !j.at(key).is_array() || i >= j.at(key).size()) return 0.0;
        const auto& v = j.at(key)[i];
        return v.is_number() ? v.get<double>() : 0.0;
    };

    for (std::size_t i = 0; i < n; ++i) {
        Candle c;
        c.timestamp = i < times.size() && times[i].is_number()
                          ? times[i].get<std::int64_t>()
                          : 0;
        c.open = at("o", i);
        c.high = at("h", i);
        c.low = at("l", i);
        c.close = closes[i].is_number() ? closes[i].get<double>() : 0.0;
        c.volume = at("v", i);
        if (c.close > 0.0) h.candles.push_back(c);
    }

    if (h.candles.empty()) {
        throw std::runtime_error("no usable candles for " + symbol);
    }
    // Oldest first, which is what the volatility calculation expects.
    std::sort(h.candles.begin(), h.candles.end(),
              [](const Candle& a, const Candle& b) { return a.timestamp < b.timestamp; });
    return h;
}

OptionChain FinnhubProvider::parse_chain(const std::string& symbol, const std::string& body) {
    const auto j = nlohmann::json::parse(body);

    OptionChain chain;
    chain.symbol = symbol;
    chain.spot = json_number(j, "lastTradePrice");

    if (!j.contains("data") || !j.at("data").is_array()) {
        throw std::runtime_error("no option chain available for " + symbol);
    }

    for (const auto& entry : j.at("data")) {
        OptionExpiry expiry;
        expiry.expiry_date = entry.value("expirationDate", "");
        expiry.years_to_expiry = years_until(expiry.expiry_date);
        if (expiry.years_to_expiry <= 0.0) continue;  // already expired

        if (!entry.contains("options")) continue;
        const auto& options = entry.at("options");

        const auto collect = [&](const char* key, const char* type) {
            if (!options.contains(key) || !options.at(key).is_array()) return;
            for (const auto& o : options.at(key)) {
                OptionQuote q;
                q.strike = json_number(o, "strike");
                if (!(q.strike > 0.0)) continue;
                q.option_type = type;
                q.bid = json_number(o, "bid");
                q.ask = json_number(o, "ask");
                q.last = json_number(o, "lastPrice");
                q.volume = json_number(o, "volume");
                q.open_interest = json_number(o, "openInterest");

                const double iv = json_number(o, "impliedVolatility");
                if (iv > 0.0) q.provider_implied_vol = iv;

                expiry.quotes.push_back(q);
            }
        };
        collect("CALL", "call");
        collect("PUT", "put");

        if (!expiry.quotes.empty()) {
            std::sort(expiry.quotes.begin(), expiry.quotes.end(),
                      [](const OptionQuote& a, const OptionQuote& b) {
                          if (a.option_type != b.option_type) return a.option_type < b.option_type;
                          return a.strike < b.strike;
                      });
            chain.expiries.push_back(std::move(expiry));
        }
    }

    if (chain.expiries.empty()) {
        throw std::runtime_error("option chain for " + symbol + " has no live expiries");
    }
    std::sort(chain.expiries.begin(), chain.expiries.end(),
              [](const OptionExpiry& a, const OptionExpiry& b) {
                  return a.years_to_expiry < b.years_to_expiry;
              });
    return chain;
}

Quote FinnhubProvider::quote(const std::string& symbol) {
    return parse_quote(symbol, get("/api/v1/quote?symbol=" + symbol));
}

History FinnhubProvider::history(const std::string& symbol, int days) {
    const std::int64_t to = now_seconds();
    // Pad for weekends and holidays so a 60-day window really gets 60 sessions.
    const std::int64_t from = to - static_cast<std::int64_t>(days) * 86400 * 7 / 5 - 86400 * 10;

    std::ostringstream path;
    path << "/api/v1/stock/candle?symbol=" << symbol << "&resolution=D&from=" << from
         << "&to=" << to;
    return parse_history(symbol, get(path.str()));
}

OptionChain FinnhubProvider::chain(const std::string& symbol) {
    return parse_chain(symbol, get("/api/v1/stock/option-chain?symbol=" + symbol));
}

}  // namespace pricer::market
