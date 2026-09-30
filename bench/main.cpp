#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "pricer/engines.hpp"

using Clock = std::chrono::steady_clock;

namespace {

template <typename F>
double time_ms(F&& f, int repeats) {
    const auto start = Clock::now();
    for (int i = 0; i < repeats; ++i) f();
    const auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count() / repeats;
}

}  // namespace

int main() {
    using namespace pricer;

    Market mkt{100.0, 0.05, 0.0, 0.2};
    Instrument euro;
    euro.strike = 100.0;
    euro.expiry = 1.0;

    std::printf("hardware threads: %u\n\n", std::thread::hardware_concurrency());
    std::printf("%-34s %12s %14s\n", "case", "ms/call", "price");
    std::printf("%-34s %12s %14s\n", "----", "-------", "-----");

    {
        PricingParams p;
        BlackScholesPricer bs;
        const double ms = time_ms([&] { bs.price(euro, mkt, p); }, 20000);
        std::printf("%-34s %12.6f %14.6f\n", "black_scholes + analytic greeks", ms,
                    bs.price(euro, mkt, p).price);
    }

    for (int steps : {256, 1024, 4096}) {
        PricingParams p;
        p.steps = steps;
        p.greeks = false;
        Instrument amer = euro;
        amer.exercise = ExerciseStyle::American;
        amer.option = OptionType::Put;
        BinomialPricer tree;
        const double ms = time_ms([&] { tree.price(amer, mkt, p); }, 20);
        std::printf("%-34s %12.3f %14.6f\n",
                    ("binomial american put, " + std::to_string(steps) + " steps").c_str(), ms,
                    tree.price(amer, mkt, p).price);
    }

    for (long paths : {100000L, 1000000L}) {
        PricingParams p;
        p.paths = paths;
        p.greeks = false;
        MonteCarloPricer mc;
        const double ms = time_ms([&] { mc.price(euro, mkt, p); }, 3);
        std::printf("%-34s %12.3f %14.6f\n",
                    ("monte carlo european, " + std::to_string(paths) + " paths").c_str(), ms,
                    mc.price(euro, mkt, p).price);
    }

    {
        Instrument asian = euro;
        asian.kind = PayoffKind::Asian;
        asian.averaging_steps = 252;
        PricingParams p;
        p.paths = 100000;
        p.greeks = false;
        MonteCarloPricer mc;
        const double ms = time_ms([&] { mc.price(asian, mkt, p); }, 3);
        std::printf("%-34s %12.3f %14.6f\n", "monte carlo asian, 252 steps", ms,
                    mc.price(asian, mkt, p).price);
    }

    {
        Instrument barrier = euro;
        barrier.kind = PayoffKind::Barrier;
        barrier.barrier_direction = BarrierDirection::UpOut;
        barrier.barrier = 130.0;
        barrier.monitoring_steps = 252;
        PricingParams p;
        p.paths = 100000;
        p.greeks = false;
        MonteCarloPricer mc;
        const double ms = time_ms([&] { mc.price(barrier, mkt, p); }, 3);
        std::printf("%-34s %12.3f %14.6f\n", "monte carlo up-and-out, 252 steps", ms,
                    mc.price(barrier, mkt, p).price);
    }

    std::printf("\nnote: monte carlo rows use all hardware threads\n");
    return 0;
}
