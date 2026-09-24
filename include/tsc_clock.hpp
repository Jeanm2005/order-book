#pragma once
#include <cstdint>
#include <ctime>

// Timestamps for per-stage latency measurement.
//
// x86-64: RDTSC, fenced so the stamp can't drift across the code being
// measured (Intel's "How to Benchmark Code Execution Times" pattern:
// lfence;rdtsc;lfence to open, rdtscp;lfence to close). Requires an
// invariant TSC (constant_tsc + nonstop_tsc in /proc/cpuinfo), which is
// what makes ticks convertible to wall time at all.
//
// Elsewhere: falls back to clock_gettime(CLOCK_MONOTONIC) in ns, and the
// calibration below becomes the identity.
#if defined(__x86_64__)
#include <x86intrin.h>

inline std::uint64_t stamp_begin() {
    _mm_lfence();
    std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

inline std::uint64_t stamp_end() {
    unsigned aux;
    std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}
#else
inline std::uint64_t stamp_begin() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000u + static_cast<std::uint64_t>(ts.tv_nsec);
}
inline std::uint64_t stamp_end() { return stamp_begin(); }
#endif

inline std::uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000u + static_cast<std::uint64_t>(ts.tv_nsec);
}

// Stamp-units -> ns as an exact rational (ns_per / ticks_per), measured
// against CLOCK_MONOTONIC over a busy-wait window. Integer-only.
struct StampCalibration {
    std::uint64_t ticks = 1;
    std::uint64_t ns = 1;

    std::uint64_t to_ns(std::uint64_t t) const {
        return static_cast<std::uint64_t>(static_cast<unsigned __int128>(t) * ns / ticks);
    }
    std::uint64_t mhz() const { return ticks * 1000 / ns; }
};

inline StampCalibration calibrate_stamps(std::uint64_t window_ns = 200'000'000) {
    std::uint64_t n0 = monotonic_ns();
    std::uint64_t t0 = stamp_begin();
    std::uint64_t n1;
    do { n1 = monotonic_ns(); } while (n1 - n0 < window_ns);
    std::uint64_t t1 = stamp_end();
    return StampCalibration{t1 - t0, n1 - n0};
}
