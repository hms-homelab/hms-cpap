#!/usr/bin/env python3
"""Extract sleep stage features from SHHS EDF + XML annotation files.

Produces a CSV with columns matching the canonical feature order defined in
include/ml/SleepStageTypes.h sleepStageFeatureNames().

Usage:
    python extract_shhs.py --shhs-dir /path/to/shhs --output features.csv --mode causal
    python extract_shhs.py --shhs-dir /path/to/shhs --output features.csv --mode bidirectional
    python extract_shhs.py --shhs-dir /path/to/shhs --output features.csv --mode causal --subject-split

SHHS structure:
    edfs/  shhs1-NNNNN.edf
    annotations-events-profusion/  shhs1-NNNNN-profusion.xml

R&K -> 4-class: S1+S2 -> Light(1), S3+S4 -> Deep(2), REM(5) -> REM(3), Wake(0) -> Wake(0).
Movement(6) and Unscored(9) epochs are skipped.
"""

import argparse
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
import pandas as pd
import pyedflib

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

EPOCH_SEC = 30

# R&K stage mapping to our 4-class system
RK_MAP = {
    0: 0,  # Wake -> Wake
    1: 1,  # S1 -> Light
    2: 1,  # S2 -> Light
    3: 2,  # S3 -> Deep
    4: 2,  # S4 -> Deep
    5: 3,  # REM -> REM
}
# 6 = Movement, 9 = Unscored -> skip

# Canonical base feature names (must match SleepStageTypes.h)
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


# ---------------------------------------------------------------------------
# XML annotation parsing
# ---------------------------------------------------------------------------

def parse_profusion_xml(xml_path: str) -> list[int]:
    """Parse SHHS Profusion XML and return list of R&K stage ints per 30s epoch."""
    tree = ET.parse(xml_path)
    root = tree.getroot()

    stages = []
    # Try multiple XML structures used in SHHS
    # Structure 1: <SleepStages><SleepStage>N</SleepStage>...</SleepStages>
    sleep_stages_elem = root.find(".//SleepStages")
    if sleep_stages_elem is not None:
        for stage_elem in sleep_stages_elem.findall("SleepStage"):
            try:
                stages.append(int(stage_elem.text.strip()))
            except (ValueError, AttributeError):
                stages.append(9)  # treat as unscored
        return stages

    # Structure 2: <ScoredEvents> with stage annotations
    for event in root.iter("ScoredEvent"):
        event_type = None
        start = None
        duration = None
        for child in event:
            tag = child.tag
            if tag == "EventType":
                event_type = child.text
            elif tag == "Start":
                start = float(child.text)
            elif tag == "Duration":
                duration = float(child.text)

        if event_type and "stage" in event_type.lower() and duration is not None:
            # Parse stage number from event concept
            concept = ""
            for child in event:
                if child.tag == "EventConcept":
                    concept = child.text or ""
            stage_match = re.search(r"(\d)", concept)
            if stage_match:
                stage_val = int(stage_match.group(1))
                n_epochs = max(1, int(round(duration / EPOCH_SEC)))
                stages.extend([stage_val] * n_epochs)

    return stages


# ---------------------------------------------------------------------------
# EDF channel reading helpers
# ---------------------------------------------------------------------------

def read_channel(edf: pyedflib.EdfReader, name_patterns: list[str]) -> tuple[np.ndarray | None, float]:
    """Read a channel matching any of the given name patterns (case-insensitive).

    Returns (signal_data, sample_rate) or (None, 0) if not found.
    """
    labels = edf.getSignalLabels()
    for i, label in enumerate(labels):
        label_lower = label.strip().lower()
        for pat in name_patterns:
            if pat.lower() in label_lower:
                data = edf.readSignal(i)
                sr = edf.getSampleFrequency(i)
                return data, sr
    return None, 0.0


# ---------------------------------------------------------------------------
# Per-epoch feature extraction
# ---------------------------------------------------------------------------

def extract_hr_features(hr_samples: np.ndarray) -> dict[str, float]:
    """Extract cardiac features from HR samples in one epoch."""
    valid = hr_samples[(hr_samples > 20) & (hr_samples < 250)]
    if len(valid) < 2:
        return {
            "hr_mean": 0.0, "hr_std": 0.0, "hr_min": 0.0, "hr_max": 0.0,
            "hrv_rmssd": 0.0, "hrv_sdnn": 0.0,
        }

    rr = 60.0 / valid  # RR intervals in seconds
    rr_diff = np.diff(rr)
    rmssd = np.sqrt(np.mean(rr_diff ** 2)) * 1000.0 if len(rr_diff) > 0 else 0.0
    sdnn = np.std(rr, ddof=0) * 1000.0

    return {
        "hr_mean": float(np.mean(valid)),
        "hr_std": float(np.std(valid, ddof=0)),
        "hr_min": float(np.min(valid)),
        "hr_max": float(np.max(valid)),
        "hrv_rmssd": float(rmssd),
        "hrv_sdnn": float(sdnn),
    }


def extract_spo2_features(spo2_samples: np.ndarray) -> dict[str, float]:
    """Extract SpO2 features from one epoch."""
    valid = spo2_samples[(spo2_samples > 50) & (spo2_samples <= 100)]
    if len(valid) < 2:
        return {
            "spo2_mean": 0.0, "spo2_min": 0.0, "spo2_std": 0.0,
            "spo2_odi_proxy": 0.0, "spo2_slope": 0.0,
        }

    mean_val = float(np.mean(valid))
    # ODI proxy: count samples >= 3% below epoch mean
    odi = int(np.sum(valid <= (mean_val - 3.0)))
    # Linear slope
    x = np.arange(len(valid), dtype=np.float64)
    if len(valid) > 1:
        slope = float(np.polyfit(x, valid, 1)[0])
    else:
        slope = 0.0

    return {
        "spo2_mean": mean_val,
        "spo2_min": float(np.min(valid)),
        "spo2_std": float(np.std(valid, ddof=0)),
        "spo2_odi_proxy": float(odi),
        "spo2_slope": slope,
    }


def extract_respiratory_features(airflow: np.ndarray, sr: float) -> dict[str, float]:
    """Extract respiratory features from airflow signal in one epoch.

    Derives RR from zero-crossings, approximate TV from amplitude, MV = TV * RR,
    and I:E ratio from inspiratory vs expiratory phase durations.
    """
    result = {
        "rr_mean": 0.0, "rr_std": 0.0, "tv_mean": 0.0, "tv_std": 0.0,
        "mv_mean": 0.0, "mv_std": 0.0, "ie_ratio_mean": 0.0,
        "leak_mean": 0.0, "flow_p95": 0.0,
    }

    if len(airflow) < 10 or sr <= 0:
        return result

    # Zero crossings for breath detection
    signs = np.sign(airflow)
    # Remove exact zeros to avoid ambiguity
    signs[signs == 0] = 1
    crossings = np.where(np.diff(signs) != 0)[0]

    if len(crossings) < 2:
        return result

    # Each pair of positive-going crossings is one breath cycle
    # Count total zero crossings -> approx breaths = crossings / 2
    n_breaths = len(crossings) / 2.0
    epoch_duration = len(airflow) / sr
    rr = n_breaths / epoch_duration * 60.0  # breaths per minute

    # Per-breath tidal volumes and I:E ratios
    tvs = []
    ie_ratios = []
    for i in range(0, len(crossings) - 1, 2):
        start = crossings[i]
        if i + 1 < len(crossings):
            mid = crossings[i + 1]
        else:
            break
        if i + 2 < len(crossings):
            end = crossings[i + 2]
        else:
            end = len(airflow) - 1

        # Inspiratory phase: start to mid, expiratory: mid to end
        insp = airflow[start:mid]
        exp = airflow[mid:end]

        if len(insp) > 0 and len(exp) > 0:
            tv = float(np.trapz(np.abs(insp)) / sr)  # approximate tidal volume
            tvs.append(tv)
            insp_time = len(insp) / sr
            exp_time = len(exp) / sr
            if exp_time > 0:
                ie_ratios.append(insp_time / exp_time)

    tv_arr = np.array(tvs) if tvs else np.array([0.0])
    ie_arr = np.array(ie_ratios) if ie_ratios else np.array([0.0])

    mv_values = tv_arr * rr  # minute ventilation per breath

    result["rr_mean"] = float(rr)
    result["rr_std"] = 0.0  # single RR estimate per epoch
    result["tv_mean"] = float(np.mean(tv_arr))
    result["tv_std"] = float(np.std(tv_arr, ddof=0))
    result["mv_mean"] = float(np.mean(mv_values))
    result["mv_std"] = float(np.std(mv_values, ddof=0))
    result["ie_ratio_mean"] = float(np.mean(ie_arr))
    # SHHS has no CPAP leak or flow_p95 -> 0.0
    result["leak_mean"] = 0.0
    result["flow_p95"] = 0.0

    return result


# ---------------------------------------------------------------------------
# Subject-level extraction
# ---------------------------------------------------------------------------

def extract_subject(edf_path: str, xml_path: str, total_epochs_hint: int = 0) -> pd.DataFrame | None:
    """Extract features for all valid epochs of one subject.

    Returns DataFrame with base feature columns + 'label', or None on error.
    """
    try:
        stages = parse_profusion_xml(xml_path)
    except Exception as e:
        print(f"  WARN: cannot parse XML {xml_path}: {e}", file=sys.stderr)
        return None

    if not stages:
        print(f"  WARN: no stages in {xml_path}", file=sys.stderr)
        return None

    try:
        edf = pyedflib.EdfReader(edf_path)
    except Exception as e:
        print(f"  WARN: cannot open EDF {edf_path}: {e}", file=sys.stderr)
        return None

    try:
        hr_signal, hr_sr = read_channel(edf, ["hr", "pr", "pulse"])
        spo2_signal, spo2_sr = read_channel(edf, ["sao2", "spo2"])
        airflow_signal, airflow_sr = read_channel(edf, ["airflow", "nasal"])
    finally:
        edf.close()

    n_epochs = len(stages)
    rows = []

    for i in range(n_epochs):
        stage_rk = stages[i]
        if stage_rk not in RK_MAP:
            continue  # skip Movement(6), Unscored(9)

        label = RK_MAP[stage_rk]

        # HR epoch samples
        if hr_signal is not None and hr_sr > 0:
            samples_per_epoch = int(hr_sr * EPOCH_SEC)
            start_idx = i * samples_per_epoch
            end_idx = start_idx + samples_per_epoch
            hr_epoch = hr_signal[start_idx:end_idx] if end_idx <= len(hr_signal) else np.array([])
        else:
            hr_epoch = np.array([])
        hr_feats = extract_hr_features(hr_epoch)

        # SpO2 epoch samples
        if spo2_signal is not None and spo2_sr > 0:
            samples_per_epoch = int(spo2_sr * EPOCH_SEC)
            start_idx = i * samples_per_epoch
            end_idx = start_idx + samples_per_epoch
            spo2_epoch = spo2_signal[start_idx:end_idx] if end_idx <= len(spo2_signal) else np.array([])
        else:
            spo2_epoch = np.array([])
        spo2_feats = extract_spo2_features(spo2_epoch)

        # Respiratory features from airflow
        if airflow_signal is not None and airflow_sr > 0:
            samples_per_epoch = int(airflow_sr * EPOCH_SEC)
            start_idx = i * samples_per_epoch
            end_idx = start_idx + samples_per_epoch
            airflow_epoch = airflow_signal[start_idx:end_idx] if end_idx <= len(airflow_signal) else np.array([])
        else:
            airflow_epoch = np.array([])
        resp_feats = extract_respiratory_features(airflow_epoch, airflow_sr)

        # SHHS has no motion/vibration data
        row = {}
        row.update(hr_feats)
        row.update(spo2_feats)
        row.update(resp_feats)
        row["motion_fraction"] = 0.0
        row["vibration_count"] = 0.0
        row["time_of_night"] = i / max(n_epochs, 1)
        row["label"] = label
        row["_epoch_idx"] = i  # internal, removed later
        rows.append(row)

    if not rows:
        return None

    return pd.DataFrame(rows)


# ---------------------------------------------------------------------------
# Rolling context
# ---------------------------------------------------------------------------

def add_rolling_context(df: pd.DataFrame, mode: str) -> pd.DataFrame:
    """Add rolling mean context features. Operates on base feature columns."""
    for window in [5, 10]:
        for base in ROLLING_BASES:
            col_name = f"past_{window}_{base}"
            # Causal: rolling mean of previous `window` epochs (not including current)
            df[col_name] = df[base].shift(1).rolling(window=window, min_periods=1).mean().fillna(0.0)

    if mode == "bidirectional":
        for base in ROLLING_BASES:
            col_name = f"future_5_{base}"
            # Future: rolling mean of next 5 epochs (not including current)
            df[col_name] = df[base].shift(-1).rolling(window=5, min_periods=1).mean().iloc[::-1].values
            df[col_name] = df[col_name].fillna(0.0)

    return df


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def find_subject_files(shhs_dir: str) -> list[tuple[str, str, str]]:
    """Find (subject_id, edf_path, xml_path) tuples in SHHS directory.

    Searches common SHHS layouts:
      shhs_dir/edfs/shhs1-NNNNN.edf
      shhs_dir/annotations-events-profusion/shhs1-NNNNN-profusion.xml
    or flat layout where both files are in the same directory.
    """
    results = []
    shhs_path = Path(shhs_dir)

    # Try structured layout first
    edf_dir = shhs_path / "edfs"
    xml_dir = shhs_path / "annotations-events-profusion"

    if not edf_dir.is_dir():
        edf_dir = shhs_path  # flat layout

    if not xml_dir.is_dir():
        xml_dir = shhs_path  # flat layout

    edf_files = sorted(edf_dir.glob("shhs1-*.edf"))
    for edf_path in edf_files:
        match = re.search(r"shhs1-(\d+)\.edf$", edf_path.name)
        if not match:
            continue
        subject_id = match.group(1)
        xml_path = xml_dir / f"shhs1-{subject_id}-profusion.xml"
        if xml_path.exists():
            results.append((subject_id, str(edf_path), str(xml_path)))

    return results


def main():
    parser = argparse.ArgumentParser(description="Extract SHHS sleep stage features")
    parser.add_argument("--shhs-dir", required=True, help="Path to SHHS dataset root")
    parser.add_argument("--output", required=True, help="Output CSV path")
    parser.add_argument("--mode", choices=["causal", "bidirectional"], default="causal",
                        help="Feature mode: causal (31 features) or bidirectional (35 features)")
    parser.add_argument("--subject-split", action="store_true",
                        help="Output subject-disjoint train/val/test splits (70/15/15)")
    parser.add_argument("--max-subjects", type=int, default=0,
                        help="Limit number of subjects (0 = all)")
    args = parser.parse_args()

    subjects = find_subject_files(args.shhs_dir)
    if not subjects:
        print(f"ERROR: No SHHS subject files found in {args.shhs_dir}", file=sys.stderr)
        sys.exit(1)

    if args.max_subjects > 0:
        subjects = subjects[:args.max_subjects]

    print(f"Found {len(subjects)} subjects, mode={args.mode}")

    all_dfs = []
    for idx, (subject_id, edf_path, xml_path) in enumerate(subjects):
        print(f"  [{idx + 1}/{len(subjects)}] Subject {subject_id}...", end="", flush=True)
        df = extract_subject(edf_path, xml_path)
        if df is not None:
            df["subject_id"] = subject_id
            all_dfs.append(df)
            print(f" {len(df)} epochs")
        else:
            print(" SKIPPED")

    if not all_dfs:
        print("ERROR: No valid data extracted", file=sys.stderr)
        sys.exit(1)

    combined = pd.concat(all_dfs, ignore_index=True)
    print(f"Total epochs: {len(combined)}")

    # Add rolling context per subject
    result_dfs = []
    for sid, group in combined.groupby("subject_id"):
        group = group.sort_values("_epoch_idx").reset_index(drop=True)
        group = add_rolling_context(group, args.mode)
        result_dfs.append(group)
    combined = pd.concat(result_dfs, ignore_index=True)

    # Select final columns in canonical order
    cols = feature_columns(args.mode) + ["label"]
    # Fill any missing columns with 0.0
    for c in cols:
        if c not in combined.columns:
            combined[c] = 0.0
    combined = combined[cols]

    # Replace any NaN with 0.0
    combined = combined.fillna(0.0)

    if args.subject_split:
        # Subject-disjoint split: 70/15/15
        unique_subjects = sorted(combined_with_sid := pd.concat(result_dfs, ignore_index=True)["subject_id"].unique())
        np.random.seed(42)
        np.random.shuffle(unique_subjects)
        n = len(unique_subjects)
        n_train = int(n * 0.70)
        n_val = int(n * 0.15)

        train_sids = set(unique_subjects[:n_train])
        val_sids = set(unique_subjects[n_train:n_train + n_val])
        test_sids = set(unique_subjects[n_train + n_val:])

        # Re-build combined with subject_id for splitting
        full = pd.concat(result_dfs, ignore_index=True)
        for c in cols:
            if c not in full.columns:
                full[c] = 0.0
        full = full.fillna(0.0)

        base_path = Path(args.output)
        stem = base_path.stem
        suffix = base_path.suffix

        for split_name, sids in [("train", train_sids), ("val", val_sids), ("test", test_sids)]:
            split_df = full[full["subject_id"].isin(sids)][cols]
            out_path = base_path.parent / f"{stem}_{split_name}{suffix}"
            split_df.to_csv(out_path, index=False)
            print(f"  {split_name}: {len(split_df)} epochs, {len(sids)} subjects -> {out_path}")
    else:
        combined.to_csv(args.output, index=False)
        print(f"Saved {len(combined)} epochs to {args.output}")

    # Print class distribution
    print("\nClass distribution:")
    labels = combined["label"] if not args.subject_split else pd.concat(result_dfs)["label"]
    for stage, name in [(0, "Wake"), (1, "Light"), (2, "Deep"), (3, "REM")]:
        count = int((labels == stage).sum())
        pct = 100.0 * count / len(labels) if len(labels) > 0 else 0
        print(f"  {name} ({stage}): {count:>8d} ({pct:5.1f}%)")


if __name__ == "__main__":
    main()
