/**
 * Client-side signal analysis — shared across the dashboard and session detail
 * so SpO2 desaturation detection is defined once (mirrors the C++ parser's
 * DesatDetector: rolling-baseline, drop >= 3% opens, < 1% closes, min 8s).
 *
 * Used for the O2Ring SpO2 path (and any machine SAD SpO2 as a fallback) where
 * the backend has not pre-computed desaturation event spans.
 */

export interface DesatSpan {
  startIdx: number; // index into the supplied series (inclusive)
  endIdx: number;   // index into the supplied series (inclusive)
  nadir: number;    // lowest SpO2 reached (%)
  depth: number;    // baseline-at-open minus nadir (%)
}

export interface DesatOptions {
  sampleSec?: number;   // seconds between samples (O2Ring ~4s, machine SAD ~1s)
  dropPct?: number;     // drop below rolling baseline to open an event
  recoverPct?: number;  // drop must fall below this to close
  baselineSec?: number; // rolling-baseline lookback window
  minSec?: number;      // minimum sustained duration to record
}

function valid(v: unknown): number | null {
  const n = Number(v);
  return Number.isFinite(n) && n > 0 && n <= 100 ? n : null;
}

/** Median spacing (seconds) of an ISO/`YYYY-MM-DD HH:MM:SS` timestamp series. */
export function inferSampleSec(timestamps: string[], fallback = 1): number {
  if (!timestamps || timestamps.length < 2) return fallback;
  const ms = timestamps.map(t => new Date((t || '').replace(' ', 'T')).getTime());
  const diffs: number[] = [];
  for (let i = 1; i < ms.length; i++) {
    const d = (ms[i] - ms[i - 1]) / 1000;
    if (d > 0 && d < 600) diffs.push(d);
  }
  if (!diffs.length) return fallback;
  diffs.sort((a, b) => a - b);
  return diffs[Math.floor(diffs.length / 2)] || fallback;
}

/**
 * Detect SpO2 desaturation events. Returns index spans into `spo2`.
 * Indices align with whatever series you pass (slice it first if the chart is
 * zoomed, so the spans line up with the visible x-axis).
 */
export function detectDesaturations(
  spo2: (number | null | undefined)[],
  opts: DesatOptions = {}
): DesatSpan[] {
  const sampleSec = opts.sampleSec ?? 1;
  const dropPct = opts.dropPct ?? 3;
  const recoverPct = opts.recoverPct ?? 1;
  const baselineWin = Math.max(1, Math.round((opts.baselineSec ?? 120) / sampleSec));
  const minSamples = Math.max(1, Math.round((opts.minSec ?? 8) / sampleSec));

  const spans: DesatSpan[] = [];
  let inEvent = false, startIdx = 0, nadir = 0, baselineAtOpen = 0;

  for (let i = 0; i < spo2.length; i++) {
    const v = valid(spo2[i]);
    if (v === null) continue;

    let baseline = v;
    for (let j = Math.max(0, i - baselineWin); j < i; j++) {
      const pv = valid(spo2[j]);
      if (pv !== null && pv > baseline) baseline = pv;
    }
    const drop = baseline - v;

    if (!inEvent) {
      if (drop >= dropPct) { inEvent = true; startIdx = i; nadir = v; baselineAtOpen = baseline; }
    } else {
      if (v < nadir) nadir = v;
      if (drop < recoverPct) {
        if (i - startIdx >= minSamples) {
          spans.push({ startIdx, endIdx: i, nadir, depth: baselineAtOpen - nadir });
        }
        inEvent = false;
      }
    }
  }
  if (inEvent && (spo2.length - 1 - startIdx) >= minSamples) {
    spans.push({ startIdx, endIdx: spo2.length - 1, nadir, depth: baselineAtOpen - nadir });
  }
  return spans;
}

/** Oxygen Desaturation Index = desaturations per hour of recording. */
export function odiPerHour(desatCount: number, sampleCount: number, sampleSec: number): number {
  const hours = (sampleCount * sampleSec) / 3600;
  return hours > 0 ? desatCount / hours : 0;
}

/**
 * Chart.js box annotations (orange) for desat spans on an index-based x-axis.
 * MIN_W keeps very short events visible on a coarse axis.
 */
export function desatAnnotations(spans: DesatSpan[], color = '#fb923c'): any[] {
  const MIN_W = 0.35;
  return spans.map(s => ({
    type: 'box',
    xMin: s.startIdx,
    xMax: Math.max(s.endIdx, s.startIdx + MIN_W),
    backgroundColor: color + '33',
    borderColor: color + '88',
    borderWidth: 1,
  }));
}
