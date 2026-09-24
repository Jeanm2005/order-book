// Phase 4: the latency histogram is a measurement instrument — if its
// bucketing or quantile walk is off, every reported p99.9 is off. Checks
// bucket geometry exhaustively over a wide range, and quantiles against
// exact order statistics.

#include "latency_histogram.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", expr, file, line);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

using H = LatencyHistogram;

// Exact order statistic matching quantile_ppm's rank definition.
std::uint64_t exact_quantile(std::vector<std::uint64_t> v, std::uint64_t ppm) {
    std::sort(v.begin(), v.end());
    std::uint64_t rank = (static_cast<std::uint64_t>(v.size()) * ppm + 999'999) / 1'000'000;
    if (rank == 0) rank = 1;
    return v[rank - 1];
}

} // namespace

// Buckets tile the value range with no gaps/overlaps, are monotone, every
// value lies within its bucket, and bucket width <= 1/128 of its values.
static void test_bucket_geometry() {
    // Exact region.
    for (std::uint64_t v = 0; v < H::kExact; ++v) {
        CHECK(H::bucket_of(v) == v);
        CHECK(H::bucket_upper(H::bucket_of(v)) == v);
    }
    // Contiguous tiling across every bucket.
    for (std::size_t i = 1; i < H::kNumBuckets; ++i) {
        std::uint64_t lo = H::bucket_upper(i - 1) + 1;
        CHECK(H::bucket_of(lo) == i);
        CHECK(H::bucket_of(H::bucket_upper(i)) == i);
        if (i >= H::kExact) {
            std::uint64_t width = H::bucket_upper(i) - lo + 1;
            CHECK(width * H::kSubBuckets <= lo); // relative error < 1/128
        }
    }
    CHECK(H::bucket_upper(H::kNumBuckets - 1) == H::kMaxValue);
}

static void test_quantiles_vs_exact(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    // Heavy-tailed, latency-shaped: mostly small, occasional huge outliers.
    std::lognormal_distribution<double> body(4.0, 0.6);
    std::uniform_int_distribution<int> pct(0, 9999);
    std::vector<std::uint64_t> vals;
    H h;
    for (int i = 0; i < 200'000; ++i) {
        std::uint64_t v = static_cast<std::uint64_t>(body(rng));
        if (pct(rng) == 0) v += 100'000 + static_cast<std::uint64_t>(pct(rng)) * 1000;
        vals.push_back(v);
        h.record(v);
    }
    CHECK(h.count() == vals.size());
    CHECK(h.min() == *std::min_element(vals.begin(), vals.end()));
    CHECK(h.max() == *std::max_element(vals.begin(), vals.end()));

    for (std::uint64_t ppm : {1ull, 500'000ull, 990'000ull, 999'000ull, 999'900ull, 1'000'000ull}) {
        std::uint64_t exact = exact_quantile(vals, ppm);
        std::uint64_t got = h.quantile_ppm(ppm);
        CHECK(got >= exact);                              // never under-reports
        CHECK(got <= exact + exact / 128 + 1);             // within one bucket
        CHECK(H::bucket_of(got) == H::bucket_of(exact) || got == h.max());
    }
}

static void test_edges() {
    H empty;
    CHECK(empty.quantile_ppm(500'000) == 0 && empty.min() == 0 && empty.max() == 0);

    H one;
    one.record(1234);
    CHECK(one.quantile_ppm(1) == 1234 && one.quantile_ppm(1'000'000) == 1234); // clamped to max

    H big;
    big.record(H::kMaxValue + 5);
    CHECK(big.overflow_count() == 1 && big.max() == H::kMaxValue);
}

int main() {
    test_bucket_geometry();
    test_edges();
    for (std::uint64_t seed : {1ull, 42ull, 20260923ull}) test_quantiles_vs_exact(seed);

    if (g_failures == 0) {
        std::puts("ALL TESTS PASSED");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
