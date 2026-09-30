#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

#include "pricer/engines.hpp"
#include "pricer/math.hpp"

namespace pricer {

namespace {

/// Running mean and variance (Welford), merged across threads at the end.
struct Accumulator {
    long count = 0;
    double mean = 0.0;
    double m2 = 0.0;

    void add(double x) {
        ++count;
        const double delta = x - mean;
        mean += delta / static_cast<double>(count);
        m2 += delta * (x - mean);
    }

    void merge(const Accumulator& o) {
        if (o.count == 0) return;
        if (count == 0) {
            *this = o;
            return;
        }
        const double n_a = static_cast<double>(count);
        const double n_b = static_cast<double>(o.count);
        const double n = n_a + n_b;
        const double delta = o.mean - mean;
        mean = (n_a * mean + n_b * o.mean) / n;
        m2 += o.m2 + delta * delta * n_a * n_b / n;
        count += o.count;
    }

    double variance() const { return count > 1 ? m2 / static_cast<double>(count - 1) : 0.0; }
    double std_error() const {
        return count > 1 ? std::sqrt(variance() / static_cast<double>(count)) : 0.0;
    }
};

int steps_for(const Instrument& inst, const PricingParams& params) {
    switch (inst.kind) {
        case PayoffKind::Asian:
            // The observation count defines the contract, so it wins.
            return std::max(1, inst.averaging_steps);
        case PayoffKind::Barrier:
            return std::max(1, inst.monitoring_steps);
        case PayoffKind::Vanilla:
            // Only the terminal value matters, so one exact lognormal jump is
            // both cheaper and no less accurate. A caller that wants a
            // plottable path can subdivide it without affecting the price.
            return params.path_steps > 1 ? params.path_steps : 1;
    }
    return 1;
}

double vanilla_payoff(const Instrument& inst, double spot) {
    return inst.option == OptionType::Call ? std::max(0.0, spot - inst.strike)
                                           : std::max(0.0, inst.strike - spot);
}

/// Walks one GBM path and returns its discounted payoff. `normals` supplies the
/// per-step shocks; the caller negates them for the antithetic twin. When
/// `record` is non-null the full path (including t=0) is written there.
double path_payoff(const Instrument& inst, const Market& mkt,
                   const std::vector<double>& normals, double dt, double drift,
                   double vol_sqrt_dt, std::vector<double>* record) {
    const int steps = static_cast<int>(normals.size());
    double spot = mkt.spot;

    if (record) {
        record->clear();
        record->reserve(static_cast<std::size_t>(steps) + 1);
        record->push_back(spot);
    }

    double running_sum = 0.0;
    bool barrier_touched = false;

    const bool is_up = inst.barrier_direction == BarrierDirection::UpIn ||
                       inst.barrier_direction == BarrierDirection::UpOut;

    for (int i = 0; i < steps; ++i) {
        spot *= std::exp(drift * dt + vol_sqrt_dt * normals[static_cast<std::size_t>(i)]);
        if (record) record->push_back(spot);

        if (inst.kind == PayoffKind::Asian) {
            running_sum += spot;
        } else if (inst.kind == PayoffKind::Barrier && !barrier_touched) {
            // Discrete monitoring: only the sampled points can knock.
            barrier_touched = is_up ? spot >= inst.barrier : spot <= inst.barrier;
        }
    }

    const double disc = std::exp(-mkt.rate * inst.expiry);

    if (inst.kind == PayoffKind::Asian) {
        const double average = running_sum / static_cast<double>(steps);
        return disc * vanilla_payoff(inst, average);
    }

    if (inst.kind == PayoffKind::Barrier) {
        const bool is_in = inst.barrier_direction == BarrierDirection::UpIn ||
                           inst.barrier_direction == BarrierDirection::DownIn;
        const bool alive = is_in ? barrier_touched : !barrier_touched;
        if (!alive) return disc * inst.rebate;
        return disc * vanilla_payoff(inst, spot);
    }

    return disc * vanilla_payoff(inst, spot);
}

}  // namespace

bool MonteCarloPricer::supports(const Instrument& inst) const {
    // No early exercise: this is a forward-simulation engine.
    return inst.exercise == ExerciseStyle::European;
}

PricingResult MonteCarloPricer::price(const Instrument& inst, const Market& mkt,
                                      const PricingParams& params) const {
    validate(inst, mkt);
    if (!supports(inst)) {
        throw std::invalid_argument(
            "monte_carlo cannot handle American exercise; use binomial");
    }
    if (params.paths < 1) throw std::invalid_argument("paths must be at least 1");

    const int steps = steps_for(inst, params);
    const double dt = inst.expiry / steps;
    const double drift = mkt.rate - mkt.dividend - 0.5 * mkt.vol * mkt.vol;
    const double vol_sqrt_dt = mkt.vol * std::sqrt(dt);

    // Antithetic pairing halves the number of independent draws.
    const long batches = params.antithetic ? (params.paths + 1) / 2 : params.paths;

    int n_threads = params.threads > 0
                        ? params.threads
                        : static_cast<int>(std::thread::hardware_concurrency());
    if (n_threads < 1) n_threads = 1;
    n_threads = static_cast<int>(std::min<long>(n_threads, batches));

    // Convergence checkpoints are evaluated on thread 0's stream so the series
    // is reproducible regardless of how the other threads interleave.
    std::vector<long> checkpoints = params.convergence;
    std::sort(checkpoints.begin(), checkpoints.end());
    checkpoints.erase(std::unique(checkpoints.begin(), checkpoints.end()),
                      checkpoints.end());

    std::vector<Accumulator> thread_acc(static_cast<std::size_t>(n_threads));
    std::vector<std::vector<std::vector<double>>> thread_paths(
        static_cast<std::size_t>(n_threads));
    std::vector<ConvergencePoint> convergence;

    const int sample_target = std::max(0, params.return_paths);

    const auto worker = [&](int tid) {
        // Each thread gets its own deterministic stream derived from the seed.
        math::Rng rng(params.seed + 0x9E3779B97F4A7C15ULL * static_cast<unsigned long long>(tid + 1));
        Accumulator acc;

        const long begin = batches * tid / n_threads;
        const long end = batches * (tid + 1) / n_threads;

        // Thread 0 collects the chart sample; other threads skip the copying.
        const int my_sample = tid == 0 ? sample_target : 0;
        auto& paths_out = thread_paths[static_cast<std::size_t>(tid)];
        paths_out.reserve(static_cast<std::size_t>(my_sample));

        std::vector<double> normals(static_cast<std::size_t>(steps));
        std::vector<double> record;
        std::size_t next_checkpoint = 0;

        for (long b = begin; b < end; ++b) {
            for (auto& z : normals) z = rng.next_normal();

            const bool want_path = static_cast<int>(paths_out.size()) < my_sample;
            const double v1 = path_payoff(inst, mkt, normals, dt, drift, vol_sqrt_dt,
                                          want_path ? &record : nullptr);
            acc.add(v1);
            if (want_path) paths_out.push_back(record);

            if (params.antithetic) {
                for (auto& z : normals) z = -z;
                const bool want_anti = static_cast<int>(paths_out.size()) < my_sample;
                const double v2 = path_payoff(inst, mkt, normals, dt, drift, vol_sqrt_dt,
                                              want_anti ? &record : nullptr);
                acc.add(v2);
                if (want_anti) paths_out.push_back(record);
            }

            if (tid == 0 && next_checkpoint < checkpoints.size() &&
                acc.count >= checkpoints[next_checkpoint]) {
                convergence.push_back({acc.count, acc.mean, acc.std_error()});
                ++next_checkpoint;
            }
        }
        thread_acc[static_cast<std::size_t>(tid)] = acc;
    };

    if (n_threads == 1) {
        worker(0);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(n_threads));
        for (int t = 0; t < n_threads; ++t) pool.emplace_back(worker, t);
        for (auto& t : pool) t.join();
    }

    Accumulator total;
    for (const auto& a : thread_acc) total.merge(a);

    PricingResult r;
    r.engine = name();
    r.price = total.mean;
    r.std_error = total.std_error();
    r.paths_used = total.count;
    r.steps_used = steps;
    r.sample_paths = std::move(thread_paths[0]);
    r.convergence = std::move(convergence);

    if (!params.greeks) return r;
    return finite_difference_greeks(*this, inst, mkt, params, r);
}

}  // namespace pricer
