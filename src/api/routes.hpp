#pragma once

#include <memory>
#include <string>

#include <httplib.h>

#include "pricer/market_data.hpp"

namespace pricer::api {

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
    std::size_t max_body_bytes = 4 * 1024 * 1024;
    std::string allowed_origin = "*";  ///< CORS for the Qt app and browsers
};

/// Wires every route onto the server. `market` may be null, in which case the
/// /market/* endpoints report that live data is unavailable rather than 404.
void install_routes(httplib::Server& server, std::shared_ptr<market::Service> market,
                    const ServerConfig& config);

/// Version string reported by /health.
const char* version();

}  // namespace pricer::api
