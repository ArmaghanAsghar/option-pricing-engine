#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pricer/json_io.hpp"
#include "pricer/pricer.hpp"

namespace {

constexpr const char* kVersion = "0.1.0";

void print_usage() {
    std::cout << R"(pricer-cli )" << kVersion << R"( - option pricing engine

Usage:
  pricer-cli price   [file.json]   Price an instrument (reads stdin if no file)
  pricer-cli curve   [file.json]   Price across a spot/vol/expiry grid
  pricer-cli implied [file.json]   Solve implied volatility from market prices
  pricer-cli engines               List available engines
  pricer-cli version               Print the version

Options:
  --compact    Emit single-line JSON instead of indented

Example:
  echo '{"instrument":{"type":"vanilla","option":"call","strike":100,"expiry":1},
         "market":{"spot":100,"rate":0.05,"vol":0.2}}' | pricer-cli price
)";
}

std::string read_all(const std::string& path) {
    if (path.empty() || path == "-") {
        std::ostringstream buf;
        buf << std::cin.rdbuf();
        return buf.str();
    }
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open file: " + path);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

nlohmann::json run_implied(const nlohmann::json& j) {
    if (!j.contains("market")) {
        throw std::invalid_argument("missing required field: market");
    }
    const auto& mj = j.at("market");
    pricer::Market mkt;
    mkt.spot = mj.at("spot").get<double>();
    mkt.rate = mj.value("rate", 0.05);
    mkt.dividend = mj.value("dividend", 0.0);
    mkt.vol = 0.0;  // unused; the solver supplies it

    if (!j.contains("quotes") || !j.at("quotes").is_array()) {
        throw std::invalid_argument("implied requires a 'quotes' array");
    }

    nlohmann::json results = nlohmann::json::array();
    for (const auto& q : j.at("quotes")) {
        pricer::ImpliedVolQuote quote;
        quote.strike = q.at("strike").get<double>();
        quote.expiry = q.at("expiry").get<double>();
        quote.market_price = q.at("market_price").get<double>();
        quote.option = pricer::option_type_from_string(q.value("option", "call"));
        results.push_back(pricer::to_json(pricer::implied_volatility(quote, mkt)));
    }
    return {{"spot", mkt.spot}, {"rate", mkt.rate}, {"results", results}};
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    bool compact = false;
    for (auto it = args.begin(); it != args.end();) {
        if (*it == "--compact") {
            compact = true;
            it = args.erase(it);
        } else if (*it == "-h" || *it == "--help") {
            print_usage();
            return 0;
        } else {
            ++it;
        }
    }

    if (args.empty()) {
        print_usage();
        return 2;
    }

    const std::string command = args[0];
    const std::string path = args.size() > 1 ? args[1] : "";
    const int indent = compact ? -1 : 2;

    try {
        if (command == "version") {
            std::cout << kVersion << "\n";
            return 0;
        }

        if (command == "engines") {
            nlohmann::json out = nlohmann::json::array();
            for (const auto& info : pricer::EngineRegistry::instance().list()) {
                out.push_back(pricer::to_json(info));
            }
            std::cout << out.dump(indent) << "\n";
            return 0;
        }

        const nlohmann::json input = nlohmann::json::parse(read_all(path));

        if (command == "price") {
            std::cout << pricer::evaluate_price(pricer::parse_price_request(input)).dump(indent)
                      << "\n";
        } else if (command == "curve") {
            std::cout << pricer::evaluate_curve(pricer::parse_curve_request(input)).dump(indent)
                      << "\n";
        } else if (command == "implied") {
            std::cout << run_implied(input).dump(indent) << "\n";
        } else {
            std::cerr << "unknown command: " << command << "\n\n";
            print_usage();
            return 2;
        }
        return 0;

    } catch (const nlohmann::json::exception& e) {
        std::cerr << nlohmann::json{{"error", std::string("invalid JSON: ") + e.what()}}.dump()
                  << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << nlohmann::json{{"error", e.what()}}.dump() << "\n";
        return 1;
    }
}
