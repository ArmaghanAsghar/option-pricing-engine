#include "routes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "pricer/engines.hpp"
#include "pricer/json_io.hpp"

namespace pricer::api {

using json = nlohmann::json;

namespace {

std::atomic<long> g_requests{0};
const auto g_started = std::chrono::steady_clock::now();

void send_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void send_error(httplib::Response& res, const std::string& message, int status) {
    send_json(res, json{{"error", message}}, status);
}

/// Wraps a handler so client mistakes become 400s and internal faults 500s,
/// and nothing escapes as an unhandled exception that would kill the worker.
template <typename Handler>
auto guard(Handler handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        ++g_requests;
        try {
            handler(req, res);
        } catch (const json::exception& e) {
            send_error(res, std::string("invalid JSON: ") + e.what(), 400);
        } catch (const std::invalid_argument& e) {
            send_error(res, e.what(), 400);
        } catch (const std::runtime_error& e) {
            // Upstream data problems, not caller mistakes.
            send_error(res, e.what(), 502);
        } catch (const std::exception& e) {
            send_error(res, std::string("internal error: ") + e.what(), 500);
        }
    };
}

json parse_body(const httplib::Request& req) {
    if (req.body.empty()) {
        throw std::invalid_argument("request body is empty; expected JSON");
    }
    return json::parse(req.body);
}

/// Path parameter as a clean ticker, or throws.
std::string symbol_param(const httplib::Request& req) {
    return market::normalize_symbol(req.matches[1].str());
}

int int_param(const httplib::Request& req, const char* key, int fallback, int lo, int hi) {
    if (!req.has_param(key)) return fallback;
    try {
        const int v = std::stoi(req.get_param_value(key));
        return std::clamp(v, lo, hi);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("query parameter '") + key +
                                    "' must be an integer");
    }
}

json quote_to_json(const market::Quote& q) {
    return {{"symbol", q.symbol},
            {"price", q.price},
            {"change", q.change},
            {"percent_change", q.percent_change},
            {"high", q.high},
            {"low", q.low},
            {"open", q.open},
            {"previous_close", q.previous_close},
            {"timestamp", q.timestamp},
            {"stale", q.stale}};
}

json chain_to_json(const market::OptionChain& chain) {
    json expiries = json::array();
    for (const auto& e : chain.expiries) {
        json quotes = json::array();
        for (const auto& q : e.quotes) {
            json qj = {{"strike", q.strike},
                       {"option", q.option_type},
                       {"bid", q.bid},
                       {"ask", q.ask},
                       {"last", q.last},
                       {"volume", q.volume},
                       {"open_interest", q.open_interest},
                       {"reference_price", q.reference_price()}};
            if (q.provider_implied_vol) qj["provider_implied_vol"] = *q.provider_implied_vol;
            quotes.push_back(qj);
        }
        expiries.push_back({{"expiry_date", e.expiry_date},
                            {"years_to_expiry", e.years_to_expiry},
                            {"quotes", quotes}});
    }
    return {{"symbol", chain.symbol},
            {"spot", chain.spot},
            {"stale", chain.stale},
            {"expiries", expiries}};
}

/// POST /implied_vol. Accepts either explicit quotes or a symbol whose live
/// chain is converted into quotes.
json handle_implied_vol(const json& body, const std::shared_ptr<market::Service>& svc) {
    Market mkt;
    if (body.contains("market")) {
        const auto& m = body.at("market");
        mkt.spot = m.value("spot", 0.0);
        mkt.rate = m.value("rate", 0.05);
        mkt.dividend = m.value("dividend", 0.0);
    }

    std::vector<ImpliedVolQuote> quotes;
    std::string symbol;

    if (body.contains("symbol") && body.at("symbol").is_string()) {
        if (!svc) throw std::runtime_error("live market data is not configured");
        symbol = market::normalize_symbol(body.at("symbol").get<std::string>());

        const auto chain = svc->chain(symbol);
        if (mkt.spot <= 0.0) mkt.spot = chain.spot;
        if (mkt.spot <= 0.0) mkt.spot = svc->quote(symbol).price;

        // Optionally narrow to one expiry so the response stays small.
        const std::string want_expiry = body.value("expiry_date", "");
        for (const auto& e : chain.expiries) {
            if (!want_expiry.empty() && e.expiry_date != want_expiry) continue;
            for (const auto& q : e.quotes) {
                const double px = q.reference_price();
                if (px <= 0.0) continue;
                quotes.push_back({q.strike, e.years_to_expiry,
                                  q.option_type == "put" ? OptionType::Put : OptionType::Call,
                                  px});
            }
        }
        if (quotes.empty()) {
            throw std::runtime_error("no usable option quotes for " + symbol);
        }
    } else if (body.contains("quotes") && body.at("quotes").is_array()) {
        for (const auto& q : body.at("quotes")) {
            ImpliedVolQuote quote;
            quote.strike = q.at("strike").get<double>();
            quote.expiry = q.at("expiry").get<double>();
            quote.market_price = q.at("market_price").get<double>();
            quote.option = option_type_from_string(q.value("option", "call"));
            quotes.push_back(quote);
        }
    } else {
        throw std::invalid_argument("provide either 'quotes' or 'symbol'");
    }

    if (!(mkt.spot > 0.0)) throw std::invalid_argument("market.spot is required");

    json results = json::array();
    int converged = 0;
    for (const auto& q : quotes) {
        const auto r = implied_volatility(q, mkt);
        if (r.converged) ++converged;
        results.push_back(to_json(r));
    }

    json out = {{"spot", mkt.spot},
                {"rate", mkt.rate},
                {"dividend", mkt.dividend},
                {"converged", converged},
                {"total", static_cast<int>(quotes.size())},
                {"results", results}};
    if (!symbol.empty()) out["symbol"] = symbol;
    return out;
}

}  // namespace

const char* version() { return "0.1.0"; }

void install_routes(httplib::Server& server, std::shared_ptr<market::Service> market_svc,
                    const ServerConfig& config) {
    server.set_payload_max_length(config.max_body_bytes);

    // CORS, so a browser-based client or the Qt app can call the API directly.
    server.set_post_routing_handler(
        [origin = config.allowed_origin](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", origin);
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        });

    server.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
            std::string message = "internal error";
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                message = e.what();
            }
            send_error(res, message, 500);
        });

    // ------------------------------------------------------------- pricing

    server.Post("/price", guard([](const httplib::Request& req, httplib::Response& res) {
                    send_json(res, evaluate_price(parse_price_request(parse_body(req))));
                }));

    server.Post("/price/curve", guard([](const httplib::Request& req, httplib::Response& res) {
                    send_json(res, evaluate_curve(parse_curve_request(parse_body(req))));
                }));

    server.Post("/implied_vol",
                guard([market_svc](const httplib::Request& req, httplib::Response& res) {
                    send_json(res, handle_implied_vol(parse_body(req), market_svc));
                }));

    server.Get("/engines", guard([](const httplib::Request&, httplib::Response& res) {
                   json out = json::array();
                   for (const auto& info : EngineRegistry::instance().list()) {
                       out.push_back(to_json(info));
                   }
                   send_json(res, out);
               }));

    server.Get("/health", guard([market_svc](const httplib::Request&, httplib::Response& res) {
                   const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                                           std::chrono::steady_clock::now() - g_started)
                                           .count();
                   json out = {{"status", "ok"},
                               {"version", version()},
                               {"uptime_seconds", uptime},
                               {"requests_served", g_requests.load()}};

                   if (market_svc) {
                       const auto stats = market_svc->cache_stats();
                       out["market_data"] = {
                           {"configured", true},
                           {"provider_healthy", market_svc->provider_healthy()},
                           {"last_error", market_svc->last_error()},
                           {"cache", {{"quotes", stats.quotes},
                                      {"histories", stats.candles},
                                      {"chains", stats.chains}}}};
                   } else {
                       out["market_data"] = {{"configured", false},
                                             {"reason", "FINNHUB_KEY not set"}};
                   }
                   send_json(res, out);
               }));

    // --------------------------------------------------------- market data

    const auto require_market = [market_svc]() {
        if (!market_svc) {
            throw std::runtime_error(
                "live market data is not configured; set FINNHUB_KEY and restart");
        }
        return market_svc;
    };

    server.Get(R"(/market/quote/([A-Za-z0-9.\-]+))",
               guard([require_market](const httplib::Request& req, httplib::Response& res) {
                   const auto svc = require_market();
                   send_json(res, quote_to_json(svc->quote(symbol_param(req))));
               }));

    server.Get(R"(/market/history/([A-Za-z0-9.\-]+))",
               guard([require_market](const httplib::Request& req, httplib::Response& res) {
                   const auto svc = require_market();
                   const std::string sym = symbol_param(req);
                   const int days = int_param(req, "days", svc->config().vol_window_days, 2, 2000);

                   const auto h = svc->history(sym, days);
                   json candles = json::array();
                   for (const auto& c : h.candles) {
                       candles.push_back({{"t", c.timestamp},
                                          {"o", c.open},
                                          {"h", c.high},
                                          {"l", c.low},
                                          {"c", c.close},
                                          {"v", c.volume}});
                   }

                   json out = {{"symbol", h.symbol},
                               {"stale", h.stale},
                               {"count", static_cast<int>(h.candles.size())},
                               {"candles", candles}};

                   const auto vol = pricer::market::historical_volatility(h.candles, days);
                   if (vol) {
                       out["historical_vol"] = *vol;
                       out["vol_window_days"] = days;
                   } else {
                       out["historical_vol"] = nullptr;
                       out["vol_note"] = "not enough observations in the window";
                   }
                   send_json(res, out);
               }));

    server.Get(R"(/market/chain/([A-Za-z0-9.\-]+))",
               guard([require_market](const httplib::Request& req, httplib::Response& res) {
                   const auto svc = require_market();
                   send_json(res, chain_to_json(svc->chain(symbol_param(req))));
               }));

    /// Everything the UI needs to populate a row in one round trip.
    server.Get(R"(/market/snapshot/([A-Za-z0-9.\-]+))",
               guard([require_market](const httplib::Request& req, httplib::Response& res) {
                   const auto svc = require_market();
                   const std::string sym = symbol_param(req);

                   json out = {{"symbol", sym}};
                   const auto q = svc->quote(sym);
                   out["quote"] = quote_to_json(q);

                   const auto vol = svc->historical_vol(sym);
                   out["historical_vol"] = vol ? json(*vol) : json(nullptr);
                   if (!vol) out["vol_note"] = svc->last_error();

                   // A sensible default market block the UI can price against
                   // straight away.
                   out["suggested_market"] = {{"spot", q.price},
                                              {"vol", vol.value_or(0.25)},
                                              {"dividend", 0.0}};
                   send_json(res, out);
               }));

    server.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (res.status == 404) {
            send_error(res, "no route for " + req.method + " " + req.path, 404);
        }
    });
}

}  // namespace pricer::api
