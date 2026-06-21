#!/bin/bash
# Code coverage for the C++ test suite (gcov + lcov).
#
# Usage:
#   scripts/coverage.sh [MIN_LINE_PCT]
#
# Builds run_tests with --coverage in a separate build dir, runs the suite,
# and produces:
#   - coverage/coverage.info   (lcov tracefile, src/ only)
#   - coverage/html/index.html (browsable report)
#   - a one-line total summary printed to stdout
#
# If MIN_LINE_PCT is given, exits non-zero when total line coverage is below it
# (used by CI as a regression gate). Without it, just reports.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build-coverage"
OUT_DIR="$PROJECT_DIR/coverage"
MIN_PCT="${1:-}"

# lcov 2.x is stricter; tolerate benign gcov/source mismatches without failing.
LCOV_FLAGS="--ignore-errors mismatch,unused,gcov,source,negative,empty,inconsistent"

echo "== Configuring instrumented build =="
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON >/dev/null

echo "== Building run_tests =="
cmake --build "$BUILD_DIR" --target run_tests -j"$(nproc)" >/dev/null

# Networking/threaded E2E suites are flaky under -O0 --coverage instrumentation
# (timing races; FysetcTcpServer can segfault, which skips gcov's exit handler
# and discards ALL .gcda data). Exclude them so the baseline is reproducible.
# They still run in the normal CI test step — this only affects the coverage run.
# Override with COVERAGE_GTEST_FILTER to measure a different slice.
DEFAULT_EXCLUDE='-FysetcTcpServerTest.*:FysetcLifecycle.*:FysetcCollectorTest.*:MqttClientTest.*:STRMqttIntegrationTest.*:SummaryRegenerationE2ETest.*'
GTEST_FILTER_ARG="${COVERAGE_GTEST_FILTER:-$DEFAULT_EXCLUDE}"
echo "== Resetting counters and running tests =="
echo "   (excluded from coverage run: ${GTEST_FILTER_ARG})"
lcov --directory "$BUILD_DIR" --zerocounters $LCOV_FLAGS >/dev/null 2>&1 || true
# Non-fatal: coverage is still meaningful if a test fails, and CI gates test
# pass/fail in its own step. Remember the status to surface at the end.
TEST_RC=0
( cd "$BUILD_DIR/tests" && ./run_tests --gtest_filter="$GTEST_FILTER_ARG" ) || TEST_RC=$?

mkdir -p "$OUT_DIR"
echo "== Capturing coverage =="
lcov --directory "$BUILD_DIR" --capture --output-file "$OUT_DIR/coverage.raw" \
     --rc branch_coverage=0 $LCOV_FLAGS >/dev/null

# Keep only this project's own source; drop tests, third-party, system headers,
# and FetchContent deps so the number reflects code we actually own.
lcov --extract "$OUT_DIR/coverage.raw" "$PROJECT_DIR/src/*" \
     --output-file "$OUT_DIR/coverage.info" $LCOV_FLAGS >/dev/null
# Also exclude the Fysetc raw-sector-over-TCP transport layer from the coverage
# DENOMINATOR. These are pure hardware/network I/O glue: a raw TCP server that
# segfaults under -O0 --coverage instrumentation, plus the sector collector +
# data-source that only function against a live socket. They are exercised by
# integration tests, not unit tests (FysetcTcpServerTest etc. are already
# filtered out of this run for the same reason). Documented exclusion, not a
# silent cap — the metric reflects unit-testable code we own.
lcov --remove "$OUT_DIR/coverage.info" \
     "$PROJECT_DIR/build*/*" "*/_deps/*" "*/tests/*" \
     "*/clients/FysetcTcpServer.cpp" \
     "*/services/FysetcSectorCollectorService.cpp" \
     "*/clients/FysetcDataSource.cpp" \
     "*/services/SleepHqClient.cpp" \
     "*/services/SleepHqExportService.cpp" \
     "*/clients/EzShareClient.cpp" \
     --output-file "$OUT_DIR/coverage.info" $LCOV_FLAGS >/dev/null
# SleepHqClient/SleepHqExportService are pure SleepHQ network I/O (OAuth +
# multipart upload of a night's files); EzShareClient is the libcurl HTTP client
# for the ezShare WiFi SD card. Same exclusion rationale as the Fysetc transport
# above — exercised by integration/live paths, not unit tests.

# genhtml is only the human-readable artifact; the gate uses lcov --summary
# below. genhtml's valid --ignore-errors categories differ from geninfo's
# (it rejects 'gcov'/'mismatch') and vary by version, so try a safe set, then
# bare, and never fail the run on report generation.
GENHTML_FLAGS="--ignore-errors source,unused,empty,inconsistent,category"
genhtml "$OUT_DIR/coverage.info" --output-directory "$OUT_DIR/html" $GENHTML_FLAGS >/dev/null 2>&1 \
  || genhtml "$OUT_DIR/coverage.info" --output-directory "$OUT_DIR/html" >/dev/null 2>&1 \
  || echo "WARNING: genhtml report failed (non-fatal); summary below is still valid"

echo ""
echo "== Coverage summary =="
SUMMARY="$(lcov --summary "$OUT_DIR/coverage.info" $LCOV_FLAGS 2>&1)"
echo "$SUMMARY"
echo "Report: $OUT_DIR/html/index.html"

# Extract total line coverage percentage (e.g. "lines......: 42.3% (...)").
LINE_PCT="$(echo "$SUMMARY" | grep -oP 'lines\.+:\s*\K[0-9.]+' | head -1)"
echo "TOTAL_LINE_COVERAGE=${LINE_PCT:-0}"
[[ "$TEST_RC" -ne 0 ]] && echo "WARNING: run_tests exited $TEST_RC (some tests failed; coverage still captured)"

if [[ -n "$MIN_PCT" ]]; then
    awk -v have="${LINE_PCT:-0}" -v min="$MIN_PCT" 'BEGIN {
        if (have+0 < min+0) {
            printf "FAIL: line coverage %.1f%% is below threshold %.1f%%\n", have, min;
            exit 1
        }
        printf "PASS: line coverage %.1f%% meets threshold %.1f%%\n", have, min;
    }'
fi
