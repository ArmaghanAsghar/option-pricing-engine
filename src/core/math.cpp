#include "pricer/math.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

// M_PI is a POSIX extension rather than standard C++, so do not rely on it.
namespace {
constexpr double kPi = 3.14159265358979323846;
}

namespace pricer::math {

namespace {
constexpr double kInvSqrt2Pi = 0.3989422804014326779399460599343819;
constexpr double kInvSqrt2 = 0.7071067811865475244008443621048490;

std::uint64_t rotl(std::uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

std::uint64_t splitmix64(std::uint64_t& x) {
    std::uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
}  // namespace

double norm_pdf(double x) {
    return kInvSqrt2Pi * std::exp(-0.5 * x * x);
}

double norm_cdf(double x) {
    // erfc is stable far into the left tail where 1 - 0.5*erfc(-x) would cancel.
    return 0.5 * std::erfc(-x * kInvSqrt2);
}

double norm_inv_cdf(double p) {
    if (p <= 0.0) return -std::numeric_limits<double>::infinity();
    if (p >= 1.0) return std::numeric_limits<double>::infinity();

    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,
                               -2.759285104469687e+02, 1.383577518672690e+02,
                               -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02,
                               -1.556989798598866e+02, 6.680131188771972e+01,
                               -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                               -2.400758277161838e+00, -2.549732539343734e+00,
                               4.374664141464968e+00,  2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01,
                               2.445134137142996e+00, 3.754408661907416e+00};

    constexpr double p_low = 0.02425;
    constexpr double p_high = 1.0 - p_low;
    double x = 0.0;

    if (p < p_low) {
        double q = std::sqrt(-2.0 * std::log(p));
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    } else if (p <= p_high) {
        double q = p - 0.5;
        double r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
    } else {
        double q = std::sqrt(-2.0 * std::log(1.0 - p));
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }

    // One Halley step lifts the rational approximation to near machine precision.
    double e = norm_cdf(x) - p;
    double u = e * std::sqrt(2.0 * kPi) * std::exp(x * x / 2.0);
    x = x - u / (1.0 + x * u / 2.0);
    return x;
}

Rng::Rng(std::uint64_t seed) {
    std::uint64_t z = seed ? seed : 0x9E3779B97F4A7C15ULL;
    for (auto& v : s_) v = splitmix64(z);
}

std::uint64_t Rng::next_u64() {
    const std::uint64_t result = rotl(s_[0] + s_[3], 23) + s_[0];
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

double Rng::next_uniform() {
    // The top 53 bits are exactly representable in a double, so this cast is
    // lossless. The half-unit offset keeps both 0 and 1 out of the range, which
    // matters because the inverse normal CDF is infinite at either endpoint.
    const double bits = static_cast<double>(next_u64() >> 11);
    return (bits + 0.5) * (1.0 / 9007199254740992.0);
}

double Rng::next_normal() {
    return norm_inv_cdf(next_uniform());
}

double brent(const std::function<double(double)>& f, double a, double b, double tol,
             int max_iter) {
    double fa = f(a);
    double fb = f(b);
    if (fa * fb > 0.0) {
        throw std::runtime_error("brent: root is not bracketed by the given interval");
    }
    if (std::abs(fa) < std::abs(fb)) {
        std::swap(a, b);
        std::swap(fa, fb);
    }

    double c = a, fc = fa, s = b, d = 0.0;
    bool bisect_last = true;

    for (int i = 0; i < max_iter; ++i) {
        if (std::abs(b - a) < tol || fb == 0.0) return b;

        if (fa != fc && fb != fc) {
            // Inverse quadratic interpolation.
            s = a * fb * fc / ((fa - fb) * (fa - fc)) +
                b * fa * fc / ((fb - fa) * (fb - fc)) +
                c * fa * fb / ((fc - fa) * (fc - fb));
        } else {
            s = b - fb * (b - a) / (fb - fa);  // secant
        }

        const double lo = (3.0 * a + b) / 4.0;
        const bool outside = !((s > lo && s < b) || (s < lo && s > b));
        const bool slow = bisect_last ? std::abs(s - b) >= std::abs(b - c) / 2.0
                                      : std::abs(s - b) >= std::abs(c - d) / 2.0;
        const bool tiny = bisect_last ? std::abs(b - c) < tol : std::abs(c - d) < tol;

        if (outside || slow || tiny) {
            s = (a + b) / 2.0;
            bisect_last = true;
        } else {
            bisect_last = false;
        }

        const double fs = f(s);
        d = c;
        c = b;
        fc = fb;

        if (fa * fs < 0.0) {
            b = s;
            fb = fs;
        } else {
            a = s;
            fa = fs;
        }
        if (std::abs(fa) < std::abs(fb)) {
            std::swap(a, b);
            std::swap(fa, fb);
        }
    }
    throw std::runtime_error("brent: exceeded iteration limit without converging");
}

}  // namespace pricer::math
