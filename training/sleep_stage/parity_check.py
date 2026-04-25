#!/usr/bin/env python3
"""Parity check: compare Python feature extractor output against C++ fixture.

Loads a fixture JSON (produced by C++ unit tests with known EpochSample values)
and a Python-extracted CSV, then compares feature values column-by-column.

Usage:
    python parity_check.py --fixture fixture.json --cpp-output cpp_features.csv --mode causal
    python parity_check.py --fixture fixture.json --cpp-output cpp_features.csv --mode bidirectional

Fixture JSON format (array of epoch objects):
[
  {
    "epoch_index": 0,
    "features": {
      "hr_mean": 72.5,
      "hr_std": 3.1,
      ...
    }
  },
  ...
]

The CSV must have the canonical column order from sleepStageFeatureNames().
Tolerance: 1e-6 absolute per feature value.
"""

import argparse
import json
import sys

import numpy as np
import pandas as pd

# Canonical feature names (must match SleepStageTypes.h)
BASE_FEATURES = [
    "hr_mean", "hr_std", "hr_min", "hr_max",
    "hrv_rmssd", "hrv_sdnn",
    "spo2_mean", "spo2_min", "spo2_std", "spo2_odi_proxy", "spo2_slope",
    "rr_mean", "rr_std", "tv_mean", "tv_std",
    "mv_mean", "mv_std", "ie_ratio_mean",
    "leak_mean", "flow_p95",
    "motion_fraction", "vibration_count",
    "time_of_night",
]

ROLLING_BASES = ["hr_mean", "spo2_mean", "rr_mean", "motion_fraction"]

TOLERANCE = 1e-6


def feature_columns(mode: str) -> list[str]:
    """Return the full ordered column list for the given mode."""
    cols = list(BASE_FEATURES)
    for w in [5, 10]:
        for b in ROLLING_BASES:
            cols.append(f"past_{w}_{b}")
    if mode == "bidirectional":
        for b in ROLLING_BASES:
            cols.append(f"future_5_{b}")
    return cols


def main():
    parser = argparse.ArgumentParser(description="Parity check: Python vs C++ features")
    parser.add_argument("--fixture", required=True, help="Path to fixture JSON from C++ tests")
    parser.add_argument("--cpp-output", required=True, help="Path to CSV with C++ extracted features")
    parser.add_argument("--mode", choices=["causal", "bidirectional"], default="causal")
    args = parser.parse_args()

    expected_cols = feature_columns(args.mode)
    expected_count = len(expected_cols)
    mode_label = f"{args.mode} ({expected_count} features)"

    # Load fixture
    with open(args.fixture) as f:
        fixture = json.load(f)

    # Load CSV
    csv_df = pd.read_csv(args.cpp_output)

    # Validate column count
    # CSV may have a 'label' column at the end -- strip it for comparison
    csv_feature_cols = [c for c in csv_df.columns if c != "label"]
    if len(csv_feature_cols) != expected_count:
        print(f"FAIL: CSV has {len(csv_feature_cols)} feature columns, expected {expected_count} for {mode_label}")
        print(f"  CSV columns: {csv_feature_cols}")
        print(f"  Expected:    {expected_cols}")
        sys.exit(1)

    # Check column names match
    mismatched_names = []
    for i, (csv_col, exp_col) in enumerate(zip(csv_feature_cols, expected_cols)):
        if csv_col != exp_col:
            mismatched_names.append((i, csv_col, exp_col))
    if mismatched_names:
        print(f"FAIL: Column name mismatches in {mode_label}:")
        for idx, csv_col, exp_col in mismatched_names:
            print(f"  [{idx}] CSV='{csv_col}' vs Expected='{exp_col}'")
        sys.exit(1)

    # Compare epoch by epoch
    n_fixture = len(fixture)
    n_csv = len(csv_df)
    if n_fixture != n_csv:
        print(f"WARN: Fixture has {n_fixture} epochs, CSV has {n_csv} rows. Comparing min({n_fixture}, {n_csv}).")

    n_compare = min(n_fixture, n_csv)
    total_checks = 0
    failures = []

    for epoch_idx in range(n_compare):
        fixture_epoch = fixture[epoch_idx]
        fixture_features = fixture_epoch.get("features", {})
        csv_row = csv_df.iloc[epoch_idx]

        for col in expected_cols:
            expected_val = fixture_features.get(col, 0.0)
            actual_val = float(csv_row.get(col, 0.0))

            if np.isnan(expected_val):
                expected_val = 0.0
            if np.isnan(actual_val):
                actual_val = 0.0

            total_checks += 1
            diff = abs(actual_val - expected_val)
            if diff > TOLERANCE:
                failures.append({
                    "epoch": epoch_idx,
                    "feature": col,
                    "expected": expected_val,
                    "actual": actual_val,
                    "diff": diff,
                })

    # Report
    print(f"Parity check: {mode_label}")
    print(f"  Epochs compared: {n_compare}")
    print(f"  Total checks:    {total_checks}")
    print(f"  Tolerance:       {TOLERANCE}")

    if failures:
        print(f"\n  FAILURES: {len(failures)}")
        # Show first 20
        for f in failures[:20]:
            print(f"    epoch={f['epoch']:>4d}  {f['feature']:<30s}  "
                  f"expected={f['expected']:.8f}  actual={f['actual']:.8f}  "
                  f"diff={f['diff']:.2e}")
        if len(failures) > 20:
            print(f"    ... and {len(failures) - 20} more")
        sys.exit(1)
    else:
        print(f"\n  PASS: all {total_checks} checks within tolerance")
        sys.exit(0)


if __name__ == "__main__":
    main()
