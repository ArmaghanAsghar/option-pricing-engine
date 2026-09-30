#pragma once

#include <string>

#include "pricer/market_data.hpp"

namespace pricer::market {

/// Finnhub REST client. The API key is sent as a header, never in a URL, so it
/// does not leak into proxy or server logs.
class FinnhubProvider : public Provider {
public:
    explicit FinnhubProvider(std::string api_key, std::string base_url = "");

    Quote quote(const std::string& symbol) override;
    History history(const std::string& symbol, int days) override;
    OptionChain chain(const std::string& symbol) override;
    std::string name() const override { return "finnhub"; }

    /// False when no key is set or the build lacks TLS, in which case every
    /// fetch throws and the service falls back to cache.
    bool configured() const;

    // Parsing is separated from transport so the tests can run on fixtures.
    static Quote parse_quote(const std::string& symbol, const std::string& body);
    static History parse_history(const std::string& symbol, const std::string& body);
    static OptionChain parse_chain(const std::string& symbol, const std::string& body);

private:
    std::string get(const std::string& path);

    std::string api_key_;
    std::string base_url_;
};

}  // namespace pricer::market
