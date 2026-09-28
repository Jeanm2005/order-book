#!/usr/bin/env bash
# Produces the README's latency tables from a real run: Release build,
# wide feed (400k messages, seed 42), ladder book and the pre-Phase-4 map
# book, producer pinned to one core and the market-data consumer to the
# next. Prints Markdown tables to stdout.
#
# Run on the dev box, not in a container (AGENTS.md "Testing before any
# latency claim"). Ideally on an isolated core (isolcpus / nohz_full).
#
# Usage: ./scripts/bench_report.sh [cpu] [consumer_cpu]    (default 2 3)
set -euo pipefail

CPU="${1:-2}"
CONSUMER_CPU="${2:-3}"
BUILD_DIR="${BUILD_DIR:-build-bench}"
FEED="$(mktemp --suffix=.feed)"
trap 'rm -f "$FEED"' EXIT

cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="${CXX:-clang++}" >/dev/null
cmake --build "$BUILD_DIR" -j --target order_book_main order_book_bench >/dev/null
"$BUILD_DIR/order_book_main" generate "$FEED" 400000 42 wide >/dev/null

to_markdown() {
    # Bench rows look like: "<stage> <count> <min> <p50> <p99> <p99.9> <p99.99> <max>"
    echo "| stage | count | min | p50 | p99 | p99.9 | p99.99 | max |"
    echo "|---|---:|---:|---:|---:|---:|---:|---:|"
    awk '$1 ~ /^(add|cancel|signals|publish|total|overhead)$/ {
        bold = ($1 == "total")
        printf "| %s | %s | %s | %s%s%s | %s%s%s | %s%s%s | %s | %s |\n",
            (bold ? "**total**" : $1), $2, $3,
            (bold ? "**" : ""), $4, (bold ? "**" : ""),
            (bold ? "**" : ""), $5, (bold ? "**" : ""),
            (bold ? "**" : ""), $6, (bold ? "**" : ""), $7, $8
    }'
}

echo "Machine: $(uname -srm) | $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
echo "Compiler: $(${CXX:-clang++} --version | head -1) | commit $(git rev-parse --short HEAD)"
echo
for book in ladder map; do
    OUT="$("$BUILD_DIR/order_book_bench" "$FEED" --book "$book" --cpu "$CPU" --consumer-cpu "$CONSUMER_CPU")"
    echo "**$book** — $(echo "$OUT" | sed -n 's/^book: \(.*\)$/\1/p')"
    echo "$OUT" | grep '^md publish' || true
    echo
    echo "$OUT" | to_markdown
    echo
done
