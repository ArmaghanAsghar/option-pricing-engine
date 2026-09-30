#pragma once

#include <cstdint>
#include <functional>

namespace pricer::math {

/// Standard normal probability density.
double norm_pdf(double x);

/// Standard normal cumulative distribution, via erfc for accuracy in the tails.
double norm_cdf(double x);

/// Inverse standard normal CDF (Acklam's rational approximation, refined once
/// by Halley's method). Accurate to roughly 1e-15 over (0,1).
double norm_inv_cdf(double p);

/// xoshiro256++ with splitmix64 seeding. Small, fast, and reproducible across
/// platforms, which std::mt19937 distributions are not.
class Rng {
public:
    explicit Rng(std::uint64_t seed);

    std::uint64_t next_u64();
    /// Uniform in (0,1), endpoints excluded.
    double next_uniform();
    /// Standard normal via inverse-CDF, so a path is a deterministic function
    /// of its uniforms (needed for common random numbers across bumps).
    double next_normal();

private:
    std::uint64_t s_[4];
};

/// Brent's method on [a,b] where f(a) and f(b) must straddle zero.
/// Throws std::runtime_error when they do not, or when the iteration cap is hit.
double brent(const std::function<double(double)>& f, double a, double b,
             double tol = 1e-12, int max_iter = 200);

}  // namespace pricer::math
