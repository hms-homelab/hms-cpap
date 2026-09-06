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
#
# 'format' and 'count' were added for lcov 2.4, which promotes to a hard ERROR
# what earlier versions warned about: clang's gcov emits __cxx_global_var_init
# at line 0, and lcov 2.4 aborts the capture on the first one. That killed the
# gate outright -- the script exited before computing any total, so there was no
# number at all rather than a low one. A static initialiser reported at line 0
# is a gcov artefact, not a coverage fact, and it is the same class of benign
# mismatch every other flag on this line already tolerates.
#
# These suppress DIAGNOSTICS, never measurement: no flag here changes which
# lines are counted or the threshold they are judged against.
LCOV_FLAGS="--ignore-errors mismatch,unused,gcov,source,negative,empty,inconsistent,format,count"

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
# HARD TIMEOUT. The instrumented suite takes ~10 minutes; 25 is generous. A
# hung test used to sit here until the CI job's own limit killed it an hour
# later, with no log published (GitHub does not expose logs for an in-progress
# job), so the only symptom was a run that never finished and no way to see
# which test caused it. On a timeout the runner is killed and the partial gcov
# data is still captured below, which at least names how far it got.
#
# 124 is timeout(1)'s exit code for "the command timed out".
TEST_TIMEOUT="${COVERAGE_TEST_TIMEOUT:-1500}"
( cd "$BUILD_DIR/tests" && timeout --kill-after=30 "$TEST_TIMEOUT" \
    ./run_tests --gtest_filter="$GTEST_FILTER_ARG" ) || TEST_RC=$?
if [ "$TEST_RC" = "124" ]; then
    echo "!! The test run TIMED OUT after ${TEST_TIMEOUT}s. A test is hanging."
    echo "!! Coverage below is from a partial run and must not be trusted as a gate."
fi

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
     "*/services/MyAirClient.cpp" \
     --output-file "$OUT_DIR/coverage.info" $LCOV_FLAGS >/dev/null
# SleepHqClient/SleepHqExportService are pure SleepHQ network I/O (OAuth +
# multipart upload of a night's files); EzShareClient is the libcurl HTTP client
# for the ezShare WiFi SD card. Same exclusion rationale as the Fysetc transport
# above — exercised by integration/live paths, not unit tests.
#
# MyAirClient is the same shape again and is listed for the same reason: an
# OAuth client against ResMed's Okta tenant, where the only way to cover the
# flow is to stand up a fake Okta, which would prove that our fake matches our
# code and nothing else.
#
# NOTE WHAT IS *NOT* EXCLUDED. MyAirService stays in the denominator: its fetch
# is injected precisely so the storing, the window replace, the poll throttle,
# the sign-in bookkeeping and the disconnect are all unit-tested without a
# network. The pure halves of the client are tested too, in
# tests/services/test_MyAirClient.cpp, including PKCE against RFC 7636's own
# published vector; excluding the file removes those covered lines from the
# numerator as well, so this is not a way of buying coverage.

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
#
# sed, not `grep -oP`: -P is a GNU extension and BSD grep rejects it outright,
# so on macOS this line failed and took the whole script's exit status with it
# AFTER the summary had already printed. The number was on screen and the gate
# still reported failure, which is the worst shape a gate can have -- the
# STANDING RULE is that nothing is pushed before local coverage passes, and the
# local gate could not pass on the machine the rule is applied from.
LINE_PCT="$(echo "$SUMMARY" | sed -n 's/^ *lines\.*: *\([0-9.]*\)%.*/\1/p' | head -1)"
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
