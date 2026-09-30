# Option Pricing Engine — Plan

Status: implemented 2026-10-01. See README.md for usage; this file records the
design decisions and why they were made.

Changes made during implementation, and why:

- **SQLite comes from the system, not a pinned download.** The original plan
  fetched the amalgamation by URL and hash. CMake ships `FindSQLite3`, which
  needs no hash to keep current and works on both Debian and macOS.
- **The implied volatility solver converges in volatility units, not price
  units.** An absolute price tolerance is larger than the entire value of a
  deep out-of-the-money option, which made an early version return confidently
  wrong answers. It now also reports an uncertainty estimate and refuses to
  claim a result it cannot justify.
- **A European vanilla simulates in one step** rather than over a grid, since
  only the terminal value matters. The consequence is that a vanilla and a
  barrier do not share paths under the same seed, which the barrier parity test
  accounts for.
- **`/market/snapshot/{symbol}`** was added so the UI can populate a watchlist
  row in one round trip instead of three.
- **A `path_steps` parameter** was added after the path-fan chart turned out to
  draw straight lines: a European vanilla is simulated in a single step, so a
  returned path had only two points. Subdividing is free for a vanilla and is
  refused where the observation count defines the contract.
- **The C++ runtime is linked statically in the container.** The GCC 13 builder
  image ships a newer libstdc++ than debian-slim provides, so the first working
  image would not start. glibc, OpenSSL and SQLite stay dynamic.
- **All executables build into the build root** via `CMAKE_RUNTIME_OUTPUT_DIRECTORY`,
  since targets defined in subdirectories otherwise land in matching subpaths
  and the Dockerfile could not find them.

## Decisions

| Topic        | Choice                                        |
|--------------|-----------------------------------------------|
| Interface    | CLI (JSON in/out) and HTTP REST API           |
| Scope v1     | European, American, Asian, barrier; Black-Scholes, binomial, Monte Carlo, implied vol |
| Purpose      | Learning and portfolio: readable code, thorough tests, correctness over raw speed |
| Compiler     | GCC on Debian (official `gcc` image), C++20   |
| Build        | CMake, dependencies via FetchContent (no Conan/vcpkg) |
| Deps (engine)| nlohmann/json, cpp-httplib (+OpenSSL for HTTPS client), Catch2, SQLite3 |
| Docker       | Two-stage: gcc builder (runs tests) -> debian-slim runtime; SQLite cache on a volume |
| UI           | Qt 6 Widgets + Qt Charts, native macOS app, talks to the container over HTTP |
| Market data  | Finnhub, fetched and cached by the container backend; key never reaches the UI |
| Smile data   | Live option chain from Finnhub, with a manual price-override table |
| Storage      | SQLite in both tiers: container cache on a volume, Qt watchlist in a local file (Qt SQL) |

## System overview

```
+------------------------- macOS host -------------------------+
|  Qt 6 app (pricer-ui)                                        |
|   watchlist (SQLite) | charts (Qt Charts) | HTTP client      |
+--------------------------|-----------------------------------+
                           | HTTP/JSON  localhost:8080
+--------------------------|------------- Docker --------------+
|  pricer-api                                                  |
|   /price  /implied_vol  /engines  /health                    |
|   /market/quote  /market/history  /market/chain  /watch...   |
|   core pricing lib  |  market data module  |  SQLite cache   |
|                          |  HTTPS -> Finnhub (FINNHUB_KEY)   |
+--------------------------------------------------------------+
```

## Instruments

- European call/put
- American call/put
- Arithmetic-average Asian call/put
- Barrier call/put: up/down, in/out, continuous monitoring approximated by discrete steps

## Engines

| Engine          | Instruments                | Greeks                    |
|-----------------|----------------------------|---------------------------|
| Black-Scholes   | European                   | Analytic                  |
| Binomial (CRR)  | European, American         | Bump-and-reprice          |
| Monte Carlo GBM | European, Asian, barrier   | Bump-and-reprice, common random numbers |
| Implied vol     | European (inverts BS)      | n/a                       |

Monte Carlo: antithetic variates, per-thread RNG streams (seeded, reproducible
for fixed seed and thread count), std::thread over hardware concurrency.
The API can return a sample of paths (capped, e.g. 200) for the UI path-fan chart,
and a convergence series (price + std error at increasing path counts).

## Core types

- `Instrument` — type, strike, expiry (years), exercise style, barrier level/direction, averaging count
- `Market` — spot, risk-free rate, dividend yield, flat volatility (surface later)
- `PricingResult` — price, delta, gamma, vega, theta, rho, std_error (optional), paths/steps used
- `Pricer` — abstract: `PricingResult price(const Instrument&, const Market&) const`
- `EngineRegistry` — name -> factory, used by CLI and API to select engines
- Math: normal CDF/PDF (erfc-based), RNG wrapper, Brent/Newton root finder

## Market data module (in the container)

- `MarketDataProvider` interface; `FinnhubProvider` implementation using cpp-httplib's
  HTTPS client. Key read from `FINNHUB_KEY` env var (compose `.env`, never committed).
- Endpoints used: `/quote` (spot), `/stock/candle` (daily history), `/stock/option-chain`.
- `MarketDataCache` (SQLite): quotes with TTL (~60 s), daily candles keyed by symbol/date,
  option chains keyed by symbol/expiry with TTL (~15 min). Serves stale data with a flag
  when the provider is unreachable or rate-limited (60 req/min budget tracked in-process).
- `HistoricalVol` — annualized std dev of log returns over a chosen window (default 60 days).
  Used as the default `vol` for a stock when no implied vol is chosen.
- `DividendYield` — from Finnhub `/stock/metric` when available, else user-entered, default 0.
- Risk-free rate: not provided by Finnhub. Single user-editable setting in the UI
  (default 0.05), sent with every price request.
- Symbols v1: US equities only (Finnhub free tier).

## HTTP API (pricer-api)

| Method | Path                       | Purpose                                              |
|--------|----------------------------|------------------------------------------------------|
| POST   | /price                     | Price one instrument; optional Greeks, paths, convergence |
| POST   | /price/curve               | Price across a spot (or time) grid for chart data    |
| POST   | /implied_vol               | Implied vol for one or many (strike, market price)   |
| GET    | /engines                   | List registered engines and their capabilities       |
| GET    | /health                    | Liveness, provider reachability, cache stats         |
| GET    | /market/quote/{sym}        | Spot, change, timestamp                              |
| GET    | /market/history/{sym}      | Daily closes; query `days`; includes historical vol  |
| GET    | /market/chain/{sym}        | Option chain (expiries, strikes, bid/ask/last)       |

Draft `/price` request:

```json
{
  "engine": "monte_carlo",
  "instrument": { "type": "asian", "option": "call", "strike": 100, "expiry": 1.0,
                  "exercise": "european", "averaging_steps": 252 },
  "market": { "spot": 100, "rate": 0.05, "dividend": 0.0, "vol": 0.2 },
  "params": { "paths": 200000, "seed": 42, "greeks": true,
              "return_paths": 100, "convergence": [1000, 10000, 100000] }
}
```

## Qt UI (pricer-ui, native macOS)

Qt 6 Widgets + Qt Charts, installed via Homebrew (`qt`). Built with the same top-level
CMake (option `PRICER_BUILD_UI`, default OFF so the Docker build never needs Qt).

Screens:
1. **Watchlist** — QTableView over a SQLite-backed model (Qt SQL). Add symbol -> backend
   fetches quote + history; shows spot, change, historical vol, last updated.
2. **Pricing panel** — pick a stock, instrument type, strike, expiry, exercise, engine, params.
   Rate and dividend editable. Results table: price, Greeks, std error.
3. **Charts** (tabs, all Qt Charts):
   - Payoff at expiry and option value vs spot (from `/price/curve`).
   - Greeks vs spot (line series) and Greek vs (spot, time) as a heatmap (QImage under a chart,
     or a grid of shaded QGraphicsRects; Qt DataVisualization is optional and deferred).
   - Monte Carlo path fan (sampled paths) and convergence (price with ±std error band vs paths).
   - Historical prices (line/candlestick) and implied vol smile per expiry (scatter + line),
     with a manual override table beside it.
4. **Settings** — backend URL (default `http://localhost:8080`), risk-free rate, vol window.

Architecture: `ApiClient` (QNetworkAccessManager, async, JSON to/from the same schema as the
CLI), `WatchlistModel` (QSqlTableModel), `ChartBuilders` (pure functions from JSON to
QChart), one `MainWindow` with a QTabWidget. Business logic stays in the container; the UI
does no pricing math. Blocking network calls are never made on the GUI thread.

## Layout

```
CMakeLists.txt
CMakePresets.json        debug (sanitizers) / release / ui
Dockerfile
docker-compose.yml       pricer-api on :8080, volume for SQLite cache, .env for FINNHUB_KEY
.env.example
include/pricer/          public headers
src/core/                instrument, market, result, math
src/engines/             black_scholes, binomial, monte_carlo, implied_vol
src/market/              provider interface, finnhub, cache, historical_vol
src/cli/                 pricer-cli
src/api/                 pricer-api (httplib server, routes, json mapping)
ui/                      pricer-ui (Qt 6): main_window, api_client, models, charts
tests/                   Catch2 unit + convergence tests; market tests use a fake provider
bench/                   timing harness
```

## Tests

Engine:
- Black-Scholes vs published values (Hull) to 1e-4
- Put-call parity on every European engine
- Binomial -> Black-Scholes convergence as steps grow
- Monte Carlo mean within 3 std errors of Black-Scholes
- American put >= European put and >= intrinsic
- Barrier in + out = vanilla (in-out parity)
- Implied vol round-trips a Black-Scholes price

Market data:
- Finnhub JSON parsing from recorded fixtures (no network in unit tests)
- Cache TTL and stale-serve behaviour with a fake clock
- Historical vol against a hand-computed series
- Rate-limit budget never exceeds 60/min under a burst

API: JSON requests to the running container, checked with curl or a small Catch2 client test.

UI: `ApiClient` tested against a local httplib stub; chart builders tested as pure functions
(series point counts, axis ranges). No pixel tests.

Docker build runs the engine, market, and API test suites; a failing test fails the image.

## Build order

1. Skeleton: CMake, Dockerfile, empty lib, CLI prints version, one test. Image builds.
2. Core types + Black-Scholes + analytic Greeks + tests.
3. Binomial tree, American exercise.
4. Monte Carlo: threading, antithetic, Asian and barrier payoffs, path sampling, convergence.
5. Implied vol solver.
6. HTTP API for pricing (`/price`, `/price/curve`, `/implied_vol`, `/engines`, `/health`), compose file.
7. Market data module: Finnhub provider, SQLite cache, historical vol, `/market/*` endpoints.
8. Qt UI: watchlist + pricing panel + payoff/price-vs-spot chart (first usable end-to-end).
9. Qt UI: Greeks charts and heatmap, Monte Carlo fan and convergence.
10. Qt UI: historical prices and implied vol smile with manual override.
11. Benchmarks, README, GitHub Actions (build image; UI build on a macOS runner without Qt Charts pixel tests).

Each container step ends with a green `docker build`; each UI step ends with the app
running against the container.

## Known constraints and risks

- Finnhub free tier: 60 req/min, US equities only, option chain fields vary by symbol; the
  cache and stale-serve path exist to absorb this.
- Qt Charts and Qt 6 are GPLv3/LGPL; fine for a portfolio project, note in README.
- Qt is not installed locally yet (`brew install qt` needed); the conda `qmake` is unrelated.
- HTTPS client in the container needs OpenSSL and CA certificates in the runtime image.
- No display in Docker on macOS: the UI intentionally runs natively.
