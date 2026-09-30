# Option Pricing Engine

A C++20 option pricing engine with a containerized HTTP API, a JSON command-line
tool, and a Qt 6 desktop front end for adding stocks and plotting results.

Built as a learning and portfolio project: the priority is readable code and a
test suite that pins down real financial invariants, not raw throughput.

## What it prices

| Payoff                        | Exercise            | Engines                        |
|-------------------------------|---------------------|--------------------------------|
| Vanilla call and put          | European            | Black-Scholes, binomial, Monte Carlo |
| Vanilla call and put          | American            | Binomial                       |
| Arithmetic-average Asian      | European            | Monte Carlo                    |
| Barrier (up/down, in/out)     | European            | Monte Carlo                    |

Every result carries price, delta, gamma, vega, theta and rho. Black-Scholes
Greeks are analytic; the others come from bump-and-reprice with shared random
numbers so Monte Carlo noise largely cancels in the difference.

A separate solver inverts Black-Scholes for implied volatility.

## Architecture

```
+------------------------- macOS host -------------------------+
|  pricer-ui (Qt 6 Widgets + Qt Charts)                        |
|    watchlist (SQLite) | charts | async HTTP client           |
+--------------------------|-----------------------------------+
                           | HTTP/JSON  localhost:8080
+--------------------------|------------- Docker --------------+
|  pricer-api                                                  |
|    pricing routes | market data routes | SQLite cache        |
|    pricer_core (engines, no I/O)                             |
|                           |  HTTPS -> Finnhub (FINNHUB_KEY)  |
+--------------------------------------------------------------+
```

The engine library does no I/O. The UI does no pricing arithmetic: it posts JSON
and draws what comes back. The Finnhub key lives only in the container, so the
desktop app never handles a credential.

## Quick start

Run the API:

```bash
docker compose up --build
```

Price a European call, the worked example from Hull:

```bash
curl -s -X POST http://localhost:8080/price \
  -H 'Content-Type: application/json' \
  -d '{"instrument":{"type":"vanilla","option":"call","strike":40,"expiry":0.5},
       "market":{"spot":42,"rate":0.10,"vol":0.2}}'
```

That returns `4.759422392871542`, which matches the textbook's 4.76.

The same request through the command-line tool inside the container:

```bash
docker compose exec api sh -c 'echo "{\"instrument\":{\"strike\":40,\"expiry\":0.5},\"market\":{\"spot\":42,\"rate\":0.10,\"vol\":0.2}}" | pricer-cli price'
```

### Live market data

Market data is optional. Without a key the pricing endpoints work normally and
the market endpoints explain that live data is off.

```bash
cp .env.example .env
# Add a free key from https://finnhub.io/register
docker compose up --build
```

Quotes, daily history and option chains are cached in SQLite on a Docker volume.
A local token bucket keeps calls under Finnhub's 60-per-minute free tier, and
when the provider is unreachable or the budget is spent the service serves the
cached value with a `stale` flag rather than failing.

## HTTP API

| Method | Path                          | Purpose                                       |
|--------|-------------------------------|-----------------------------------------------|
| POST   | `/price`                      | Price one instrument, with Greeks             |
| POST   | `/price/curve`                | Reprice across a spot, volatility or expiry grid |
| POST   | `/implied_vol`                | Invert prices for volatility                  |
| GET    | `/engines`                    | Engines and what each supports                |
| GET    | `/health`                     | Liveness, provider health, cache counts       |
| GET    | `/market/quote/{symbol}`      | Latest quote                                  |
| GET    | `/market/history/{symbol}`    | Daily closes plus historical volatility       |
| GET    | `/market/chain/{symbol}`      | Option chain by expiry                        |
| GET    | `/market/snapshot/{symbol}`   | Quote, historical volatility and a ready-to-price market block |

### Request shape

```json
{
  "engine": "monte_carlo",
  "instrument": {
    "type": "asian", "option": "call", "strike": 100, "expiry": 1.0,
    "exercise": "european", "averaging_steps": 252
  },
  "market": { "spot": 100, "rate": 0.05, "dividend": 0.0, "vol": 0.2 },
  "params": {
    "paths": 200000, "seed": 42, "greeks": true,
    "return_paths": 100, "path_steps": 100,
    "convergence": [1000, 10000, 100000]
  }
}
```

`engine` is optional. Omit it and the registry picks the cheapest engine that
handles the contract: closed form for European vanillas, a tree for American
exercise, simulation for path-dependent payoffs.

`return_paths`, `path_steps` and `convergence` exist for the charts.
`return_paths` returns a sample of simulated paths; `convergence` returns a
price estimate and standard error at increasing path counts.

`path_steps` raises the simulation resolution for a European vanilla, which
otherwise needs only one terminal draw and would plot as a straight line.
Subdividing costs no accuracy, since geometric Brownian motion is exact at any
step size, and a test asserts both resolutions agree with the closed form. For
Asian and barrier payoffs the observation count is part of the contract, so it
is never overridden.

## Desktop app

The Qt app is off by default so the Docker build never needs Qt.

```bash
brew install qt
cmake --preset ui -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build --preset ui
./build/ui/pricer-ui.app/Contents/MacOS/pricer-ui
```

It expects the API at `http://localhost:8080`, changeable under File then
Backend settings.

Tabs:

- **Payoff and value.** Option value against spot with the expiry payoff underneath.
- **Greeks.** Delta, gamma, vega and theta against spot.
- **Greek surface.** Delta across spot and time to expiry, drawn as a coloured family of time slices.
- **Monte Carlo.** A fan of simulated paths with the strike and barrier marked, plus convergence with a two-standard-error band.
- **History.** Daily closes and the historical volatility estimate.
- **Vol smile.** Implied volatility against strike from the live chain, with an editable price table for overrides.

The watchlist persists in a local SQLite file. Quotes are always refetched, so
nothing stale survives a restart silently.

## Building without Docker

Needs CMake 3.20, a C++20 compiler, OpenSSL and SQLite. On macOS:

```bash
brew install cmake openssl sqlite
cmake --preset release
cmake --build --preset release
ctest --preset release
```

A sanitizer build is available as the `debug` preset.

## Tests

90 test cases. The ones that matter are the financial invariants, not line
coverage:

- Black-Scholes against published values to 1e-10.
- Put-call parity on every European engine.
- Binomial converging to Black-Scholes as steps grow, and reproducing the standard American put benchmark of 6.0903.
- An American option never below its European twin, and never below intrinsic value.
- An American call without dividends exactly equal to the European call, since early exercise is never optimal there.
- Monte Carlo within three standard errors of closed form, with the standard error shrinking as the square root of path count.
- Barrier knock-in plus knock-out equal to the vanilla on identical paths.
- A single-observation Asian option identical to a vanilla.
- Implied volatility round-tripping a Black-Scholes price.
- Symbol normalization rejecting path traversal and injection shapes before they reach a cache key or URL.
- The rate limiter never exceeding its budget under a burst.
- The cache serving stale data when the provider fails, with the clock injected so the test does not depend on wall time.

The Docker build runs the whole suite, so a built image is always a tested one.

## Notes on the numerics

**Why the implied volatility solver reports uncertainty.** For a deeply
in-the-money or very short-dated option, almost the entire price is intrinsic
value. The volatility-dependent part can fall below double precision, at which
point no solver can recover volatility from the price no matter how it iterates.
The solver estimates its own error bar by dividing the price's rounding noise by
vega, and reports `well_conditioned: false` instead of returning a confident
wrong number. The smile chart draws those strikes in grey.

Convergence is also measured in volatility units rather than price units. An
absolute price tolerance of 1e-8 is meaningless for an option whose entire value
is 1e-8, and an early version of this solver silently returned wrong answers
because of exactly that.

**Why a European vanilla simulates in one step.** Only the terminal value
matters, so a single exact lognormal jump is both faster and more accurate than
252 small ones. A consequence worth knowing: a vanilla and a 252-step barrier
consume different numbers of random draws, so they do not share paths even under
the same seed. The barrier parity test uses an unreachable barrier as its
reference for that reason.

**Reproducibility.** Monte Carlo results are deterministic for a given seed and
thread count. Each thread draws from its own stream derived from the seed, so
results do not depend on how threads interleave, but changing the thread count
does repartition the streams and therefore changes the estimate within its
standard error.

## Licence and dependencies

| Dependency     | Purpose                    | Licence        |
|----------------|----------------------------|----------------|
| nlohmann/json  | JSON parsing               | MIT            |
| cpp-httplib    | HTTP server and client     | MIT            |
| Catch2         | Tests                      | BSL-1.0        |
| SQLite         | Cache and watchlist        | Public domain  |
| Qt 6 + Qt Charts | Desktop UI               | LGPL-3.0 / GPL-3.0 |

Qt Charts is GPL-3.0 under the open-source licence, which is why the UI is a
separate optional target and not part of the container image.
