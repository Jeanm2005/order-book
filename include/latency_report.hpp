#pragma once
#include <cstdio>
#include "latency_histogram.hpp"
#include "tsc_clock.hpp"

// Reporting only (not hot path): prints LatencyHistogram rows in ns, in the
// same columns everywhere (order_book_bench, xdp-listen) so results from
// different tools line up.

inline void print_latency_header() {
    std::printf("%-9s %10s %8s %8s %8s %8s %8s %10s   (ns)\n",
                "stage", "count", "min", "p50", "p99", "p99.9", "p99.99", "max");
}

inline void print_latency_row(const char* name, const LatencyHistogram& h, const StampCalibration& cal) {
    auto ns = [&](std::uint64_t ticks) { return static_cast<unsigned long long>(cal.to_ns(ticks)); };
    std::printf("%-9s %10llu %8llu %8llu %8llu %8llu %8llu %10llu\n", name,
                static_cast<unsigned long long>(h.count()),
                ns(h.min()), ns(h.quantile_ppm(500'000)), ns(h.quantile_ppm(990'000)),
                ns(h.quantile_ppm(999'000)), ns(h.quantile_ppm(999'900)), ns(h.max()));
}
