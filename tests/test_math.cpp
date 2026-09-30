#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "pricer/math.hpp"

using Catch::Approx;
using namespace pricer::math;

TEST_CASE("normal CDF matches known values") {
    CHECK(norm_cdf(0.0) == Approx(0.5).epsilon(1e-12));
    CHECK(norm_cdf(1.0) == Approx(0.8413447460685429).epsilon(1e-12));
    CHECK(norm_cdf(-1.0) == Approx(0.1586552539314571).epsilon(1e-12));
    CHECK(norm_cdf(1.96) == Approx(0.9750021048517795).epsilon(1e-12));
    CHECK(norm_cdf(-3.0) == Approx(0.001349898031630095).epsilon(1e-12));
}

TEST_CASE("normal CDF stays accurate deep in the left tail") {
    // This is where a 1 - cdf(-x) formulation would lose all its digits.
    CHECK(norm_cdf(-10.0) == Approx(7.619853024160526e-24).epsilon(1e-10));
    CHECK(norm_cdf(-20.0) > 0.0);
}

TEST_CASE("normal PDF matches known values") {
    CHECK(norm_pdf(0.0) == Approx(0.3989422804014327).epsilon(1e-12));
    CHECK(norm_pdf(1.0) == Approx(0.24197072451914337).epsilon(1e-12));
    CHECK(norm_pdf(-1.0) == Approx(norm_pdf(1.0)).epsilon(1e-15));
}

TEST_CASE("inverse normal CDF round-trips the CDF") {
    for (double p : {1e-8, 1e-4, 0.01, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99, 1.0 - 1e-8}) {
        const double x = norm_inv_cdf(p);
        CHECK(norm_cdf(x) == Approx(p).epsilon(1e-11));
    }
}

TEST_CASE("RNG is reproducible and distributionally sane") {
    Rng a(12345), b(12345);
    for (int i = 0; i < 100; ++i) CHECK(a.next_u64() == b.next_u64());

    Rng c(999);
    double sum = 0.0, sum_sq = 0.0;
    constexpr int n = 200000;
    for (int i = 0; i < n; ++i) {
        const double u = c.next_uniform();
        REQUIRE(u > 0.0);
        REQUIRE(u < 1.0);
        sum += u;
        sum_sq += u * u;
    }
    const double mean = sum / n;
    const double var = sum_sq / n - mean * mean;
    CHECK(mean == Approx(0.5).margin(0.005));
    CHECK(var == Approx(1.0 / 12.0).margin(0.005));
}

TEST_CASE("RNG normals have the right moments") {
    Rng r(2024);
    double sum = 0.0, sum_sq = 0.0;
    constexpr int n = 200000;
    for (int i = 0; i < n; ++i) {
        const double z = r.next_normal();
        sum += z;
        sum_sq += z * z;
    }
    CHECK(sum / n == Approx(0.0).margin(0.01));
    CHECK(sum_sq / n == Approx(1.0).margin(0.01));
}

TEST_CASE("different seeds give different streams") {
    Rng a(1), b(2);
    int same = 0;
    for (int i = 0; i < 50; ++i) {
        if (a.next_u64() == b.next_u64()) ++same;
    }
    CHECK(same == 0);
}

TEST_CASE("brent finds roots of well-behaved functions") {
    const auto f = [](double x) { return x * x - 2.0; };
    CHECK(brent(f, 0.0, 2.0) == Approx(std::sqrt(2.0)).epsilon(1e-10));

    const auto g = [](double x) { return std::cos(x) - x; };
    CHECK(brent(g, 0.0, 1.0) == Approx(0.7390851332151607).epsilon(1e-9));
}

TEST_CASE("brent rejects an interval that does not bracket a root") {
    const auto f = [](double x) { return x * x + 1.0; };
    CHECK_THROWS_AS(brent(f, -1.0, 1.0), std::runtime_error);
}
