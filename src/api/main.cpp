#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <httplib.h>

#include "pricer/finnhub.hpp"
#include "pricer/market_data.hpp"
#include "routes.hpp"

namespace {

httplib::Server* g_server = nullptr;

void handle_signal(int) {
    if (g_server) g_server->stop();
}

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

int env_int(const char* key, int fallback) {
    const char* v = std::getenv(key);
    if (!v || !*v) return fallback;
    try {
        return std::stoi(v);
    } catch (const std::exception&) {
        std::cerr << "warning: " << key << " is not an integer, using " << fallback << "\n";
        return fallback;
    }
}

}  // namespace

int main() {
    using namespace pricer;

    api::ServerConfig server_config;
    server_config.host = env_or("PRICER_HOST", "0.0.0.0");
    server_config.port = env_int("PRICER_PORT", 8080);
    server_config.allowed_origin = env_or("PRICER_CORS_ORIGIN", "*");

    // Market data is optional: without a key the pricing endpoints still work.
    std::shared_ptr<market::Service> market_service;
    const std::string key = env_or("FINNHUB_KEY", "");

    if (!key.empty()) {
        try {
            market::ServiceConfig cfg;
            cfg.cache_path = env_or("PRICER_CACHE_PATH", "/data/market.db");
            cfg.quote_ttl_seconds = env_int("PRICER_QUOTE_TTL", 60);
            cfg.history_ttl_seconds = env_int("PRICER_HISTORY_TTL", 6 * 3600);
            cfg.chain_ttl_seconds = env_int("PRICER_CHAIN_TTL", 15 * 60);
            cfg.vol_window_days = env_int("PRICER_VOL_WINDOW", 60);
            cfg.rate_limit_per_minute = env_int("PRICER_RATE_LIMIT", 55);

            auto provider = std::make_shared<market::FinnhubProvider>(
                key, env_or("FINNHUB_BASE_URL", ""));
            auto cache = std::make_shared<market::Cache>(cfg.cache_path);

            if (cache->in_memory()) {
                std::cerr << "warning: cache path " << cfg.cache_path
                          << " is not writable; using an in-memory cache\n";
            }
            market_service =
                std::make_shared<market::Service>(provider, cache, cfg);
            std::cout << "market data: finnhub, cache at " << cfg.cache_path << "\n";
        } catch (const std::exception& e) {
            std::cerr << "warning: market data disabled: " << e.what() << "\n";
        }
    } else {
        std::cout << "market data: disabled (FINNHUB_KEY not set)\n";
    }

    httplib::Server server;
    g_server = &server;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    api::install_routes(server, market_service, server_config);

    std::cout << "pricer-api " << api::version() << " listening on " << server_config.host
              << ":" << server_config.port << "\n"
              << "  POST /price  /price/curve  /implied_vol\n"
              << "  GET  /engines  /health  /market/{quote,history,chain,snapshot}/{symbol}\n"
              << std::flush;

    if (!server.listen(server_config.host, server_config.port)) {
        std::cerr << "error: cannot bind " << server_config.host << ":"
                  << server_config.port << "\n";
        return 1;
    }
    std::cout << "shutting down\n";
    return 0;
}
