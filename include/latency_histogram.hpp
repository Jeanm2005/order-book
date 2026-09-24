#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

// Fixed-size HDR-style (log-linear) latency histogram. record() is a few
// integer ops and one increment into preallocated storage — no allocation,
// no floating point — so it can sit inside a measured pipeline without
// perturbing it much.
//
// Buckets: values < 256 are exact. Above that, each power-of-two octave is
// split into 128 equal sub-buckets, so any recorded value is known to within
// 1/128 (< 0.8%) of itself. Tracks values up to 2^40 - 1 (~18 minutes in ns,
// ~6 minutes in TSC ticks at 3 GHz); anything larger lands in the top bucket
// and is counted in overflow_count().
//
// Quantiles are reported as the *upper* bound of the bucket the quantile
// falls in (HdrHistogram's "highest equivalent value"): a reported p99.9 is
// never lower than the true one.
class LatencyHistogram {
public:
    static constexpr unsigned kExactBits = 8;                 // 0..255 exact
    static constexpr std::uint64_t kExact = 1u << kExactBits;
    static constexpr std::uint64_t kSubBuckets = kExact / 2;  // per octave above kExact
    static constexpr unsigned kMaxBits = 40;
    static constexpr std::uint64_t kMaxValue = (std::uint64_t{1} << kMaxBits) - 1;
    static constexpr std::size_t kNumBuckets = kExact + (kMaxBits - kExactBits) * kSubBuckets;

    void record(std::uint64_t v) {
        if (v > kMaxValue) { ++overflow_; v = kMaxValue; }
        ++counts_[bucket_of(v)];
        ++total_;
        if (v < min_) min_ = v;
        if (v > max_) max_ = v;
    }

    std::uint64_t count() const { return total_; }
    std::uint64_t overflow_count() const { return overflow_; }
    std::uint64_t min() const { return total_ ? min_ : 0; }
    std::uint64_t max() const { return max_; }

    // Quantile given in parts per million (p50 = 500'000, p99.99 = 999'900),
    // integer so the reporting side has no rounding ambiguity either. Returns
    // the smallest bucket upper bound such that at least ceil(ppm * count /
    // 1e6) samples are <= it. 0 if empty.
    std::uint64_t quantile_ppm(std::uint64_t ppm) const {
        if (total_ == 0) return 0;
        unsigned __int128 num = static_cast<unsigned __int128>(total_) * ppm;
        std::uint64_t rank = static_cast<std::uint64_t>((num + 999'999) / 1'000'000);
        if (rank == 0) rank = 1;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kNumBuckets; ++i) {
            seen += counts_[i];
            if (seen >= rank) {
                std::uint64_t hi = bucket_upper(i);
                return hi < max_ ? hi : max_; // never report above the true max
            }
        }
        return max_;
    }

    static constexpr std::size_t bucket_of(std::uint64_t v) {
        if (v < kExact) return static_cast<std::size_t>(v);
        unsigned msb = 63u - static_cast<unsigned>(std::countl_zero(v)); // >= kExactBits
        unsigned shift = msb - (kExactBits - 1);                        // >= 1
        std::uint64_t sub = v >> shift;                                  // [kSubBuckets, kExact)
        return static_cast<std::size_t>(kExact + (shift - 1) * kSubBuckets + (sub - kSubBuckets));
    }

    static constexpr std::uint64_t bucket_upper(std::size_t i) {
        if (i < kExact) return i;
        std::uint64_t k = i - kExact;
        unsigned shift = static_cast<unsigned>(k / kSubBuckets) + 1;
        std::uint64_t sub = kSubBuckets + k % kSubBuckets;
        return (sub << shift) + ((std::uint64_t{1} << shift) - 1);
    }

private:
    std::array<std::uint64_t, kNumBuckets> counts_{};
    std::uint64_t total_ = 0;
    std::uint64_t overflow_ = 0;
    std::uint64_t min_ = ~std::uint64_t{0};
    std::uint64_t max_ = 0;
};

static_assert(LatencyHistogram::bucket_of(LatencyHistogram::kMaxValue) == LatencyHistogram::kNumBuckets - 1);
