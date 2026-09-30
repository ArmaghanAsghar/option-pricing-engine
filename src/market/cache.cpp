#include <chrono>
#include <cstdint>
#include <mutex>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "pricer/market_data.hpp"

namespace pricer::market {

namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS quotes (
  symbol TEXT PRIMARY KEY,
  fetched_at INTEGER NOT NULL,
  payload TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS histories (
  symbol TEXT NOT NULL,
  days INTEGER NOT NULL,
  fetched_at INTEGER NOT NULL,
  payload TEXT NOT NULL,
  PRIMARY KEY (symbol, days)
);
CREATE TABLE IF NOT EXISTS chains (
  symbol TEXT PRIMARY KEY,
  fetched_at INTEGER NOT NULL,
  payload TEXT NOT NULL
);
)sql";

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

nlohmann::json quote_to_json(const Quote& q) {
    return {{"symbol", q.symbol},         {"price", q.price},
            {"change", q.change},         {"percent_change", q.percent_change},
            {"high", q.high},             {"low", q.low},
            {"open", q.open},             {"previous_close", q.previous_close},
            {"timestamp", q.timestamp}};
}

Quote quote_from_json(const nlohmann::json& j) {
    Quote q;
    q.symbol = j.value("symbol", "");
    q.price = j.value("price", 0.0);
    q.change = j.value("change", 0.0);
    q.percent_change = j.value("percent_change", 0.0);
    q.high = j.value("high", 0.0);
    q.low = j.value("low", 0.0);
    q.open = j.value("open", 0.0);
    q.previous_close = j.value("previous_close", 0.0);
    q.timestamp = j.value("timestamp", std::int64_t{0});
    return q;
}

nlohmann::json candles_to_json(const std::vector<Candle>& candles) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& c : candles) {
        arr.push_back({{"t", c.timestamp},
                       {"o", c.open},
                       {"h", c.high},
                       {"l", c.low},
                       {"c", c.close},
                       {"v", c.volume}});
    }
    return arr;
}

std::vector<Candle> candles_from_json(const nlohmann::json& arr) {
    std::vector<Candle> out;
    out.reserve(arr.size());
    for (const auto& j : arr) {
        Candle c;
        c.timestamp = j.value("t", std::int64_t{0});
        c.open = j.value("o", 0.0);
        c.high = j.value("h", 0.0);
        c.low = j.value("l", 0.0);
        c.close = j.value("c", 0.0);
        c.volume = j.value("v", 0.0);
        out.push_back(c);
    }
    return out;
}

nlohmann::json chain_to_json(const OptionChain& c) {
    nlohmann::json expiries = nlohmann::json::array();
    for (const auto& e : c.expiries) {
        nlohmann::json quotes = nlohmann::json::array();
        for (const auto& q : e.quotes) {
            nlohmann::json qj = {{"strike", q.strike},
                                 {"option_type", q.option_type},
                                 {"bid", q.bid},
                                 {"ask", q.ask},
                                 {"last", q.last},
                                 {"volume", q.volume},
                                 {"open_interest", q.open_interest}};
            if (q.provider_implied_vol) qj["implied_vol"] = *q.provider_implied_vol;
            quotes.push_back(qj);
        }
        expiries.push_back({{"expiry_date", e.expiry_date},
                            {"years_to_expiry", e.years_to_expiry},
                            {"quotes", quotes}});
    }
    return {{"symbol", c.symbol}, {"spot", c.spot}, {"expiries", expiries}};
}

OptionChain chain_from_json(const nlohmann::json& j) {
    OptionChain c;
    c.symbol = j.value("symbol", "");
    c.spot = j.value("spot", 0.0);
    for (const auto& ej : j.value("expiries", nlohmann::json::array())) {
        OptionExpiry e;
        e.expiry_date = ej.value("expiry_date", "");
        e.years_to_expiry = ej.value("years_to_expiry", 0.0);
        for (const auto& qj : ej.value("quotes", nlohmann::json::array())) {
            OptionQuote q;
            q.strike = qj.value("strike", 0.0);
            q.option_type = qj.value("option_type", "call");
            q.bid = qj.value("bid", 0.0);
            q.ask = qj.value("ask", 0.0);
            q.last = qj.value("last", 0.0);
            q.volume = qj.value("volume", 0.0);
            q.open_interest = qj.value("open_interest", 0.0);
            if (qj.contains("implied_vol") && qj["implied_vol"].is_number()) {
                q.provider_implied_vol = qj["implied_vol"].get<double>();
            }
            e.quotes.push_back(q);
        }
        c.expiries.push_back(std::move(e));
    }
    return c;
}

}  // namespace

struct Cache::Impl {
    sqlite3* db = nullptr;
    std::mutex mutex;
    Clock clock;

    ~Impl() {
        if (db) sqlite3_close(db);
    }

    /// Single-row fetch of (fetched_at, payload) for a prepared statement that
    /// the caller has already bound.
    std::optional<std::pair<std::int64_t, std::string>> fetch(sqlite3_stmt* stmt) {
        if (sqlite3_step(stmt) != SQLITE_ROW) return std::nullopt;
        const auto fetched_at = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 0));
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        return std::make_pair(fetched_at, std::string(text ? text : ""));
    }
};

Cache::Cache(const std::string& path, Clock clock) : impl_(std::make_unique<Impl>()) {
    impl_->clock = clock ? std::move(clock) : [] { return now_seconds(); };

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(path.c_str(), &impl_->db, flags, nullptr);

    if (rc != SQLITE_OK) {
        // A read-only or missing volume must not take the whole service down.
        if (impl_->db) sqlite3_close(impl_->db);
        impl_->db = nullptr;
        rc = sqlite3_open_v2(":memory:", &impl_->db, flags, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("cannot open even an in-memory SQLite database");
        }
        in_memory_ = true;
    }

    char* err = nullptr;
    if (sqlite3_exec(impl_->db, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string message = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("cannot create cache schema: " + message);
    }
    sqlite3_exec(impl_->db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr,
                 nullptr, nullptr);
}

Cache::~Cache() = default;

void Cache::put_quote(const Quote& q) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO quotes (symbol, fetched_at, payload) VALUES (?,?,?) "
        "ON CONFLICT(symbol) DO UPDATE SET fetched_at=excluded.fetched_at, "
        "payload=excluded.payload";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    const std::string payload = quote_to_json(q).dump();
    sqlite3_bind_text(stmt, 1, q.symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, impl_->clock());
    sqlite3_bind_text(stmt, 3, payload.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::optional<Quote> Cache::get_quote(const std::string& symbol, int ttl_seconds) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT fetched_at, payload FROM quotes WHERE symbol = ?";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);

    const auto row = impl_->fetch(stmt);
    sqlite3_finalize(stmt);
    if (!row) return std::nullopt;

    try {
        Quote q = quote_from_json(nlohmann::json::parse(row->second));
        q.stale = impl_->clock() - row->first > ttl_seconds;
        return q;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;  // corrupt row; treat as a miss
    }
}

void Cache::put_history(const History& h) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO histories (symbol, days, fetched_at, payload) VALUES (?,?,?,?) "
        "ON CONFLICT(symbol, days) DO UPDATE SET fetched_at=excluded.fetched_at, "
        "payload=excluded.payload";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    const std::string payload = candles_to_json(h.candles).dump();
    sqlite3_bind_text(stmt, 1, h.symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, static_cast<int>(h.candles.size()));
    sqlite3_bind_int64(stmt, 3, impl_->clock());
    sqlite3_bind_text(stmt, 4, payload.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::optional<History> Cache::get_history(const std::string& symbol, int days,
                                          int ttl_seconds) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3_stmt* stmt = nullptr;
    // Any cached series at least as long as the request can serve it.
    const char* sql =
        "SELECT fetched_at, payload FROM histories WHERE symbol = ? AND days >= ? "
        "ORDER BY days ASC LIMIT 1";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, days);

    const auto row = impl_->fetch(stmt);
    sqlite3_finalize(stmt);
    if (!row) return std::nullopt;

    try {
        History h;
        h.symbol = symbol;
        h.candles = candles_from_json(nlohmann::json::parse(row->second));
        h.stale = impl_->clock() - row->first > ttl_seconds;
        return h;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

void Cache::put_chain(const OptionChain& c) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO chains (symbol, fetched_at, payload) VALUES (?,?,?) "
        "ON CONFLICT(symbol) DO UPDATE SET fetched_at=excluded.fetched_at, "
        "payload=excluded.payload";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    const std::string payload = chain_to_json(c).dump();
    sqlite3_bind_text(stmt, 1, c.symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, impl_->clock());
    sqlite3_bind_text(stmt, 3, payload.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::optional<OptionChain> Cache::get_chain(const std::string& symbol, int ttl_seconds) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT fetched_at, payload FROM chains WHERE symbol = ?";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);

    const auto row = impl_->fetch(stmt);
    sqlite3_finalize(stmt);
    if (!row) return std::nullopt;

    try {
        OptionChain c = chain_from_json(nlohmann::json::parse(row->second));
        c.stale = impl_->clock() - row->first > ttl_seconds;
        return c;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

Cache::Stats Cache::stats() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Stats s;
    const auto count = [this](const char* sql) -> long {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
        long n = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) n = static_cast<long>(sqlite3_column_int64(stmt, 0));
        sqlite3_finalize(stmt);
        return n;
    };
    s.quotes = count("SELECT COUNT(*) FROM quotes");
    s.candles = count("SELECT COUNT(*) FROM histories");
    s.chains = count("SELECT COUNT(*) FROM chains");
    return s;
}

}  // namespace pricer::market
