// The figures OSCAR lists for each channel of a day (min, median, max, 95th and
// 99.5th percentile), for the session page's detail chart. Kept free of Angular
// (and of imports) so `npm test` can run it.
//
// They are taken from the values the chart draws: one a minute for the card's
// signals, the vitals bucket for the machine's SpO2 and heart rate, about 4 s
// for the ring, one a breath for breath flow limitation. OSCAR reads the card's
// full-resolution data (leak and pressure every 2 s), so every figure here can
// differ from OSCAR's, not only the top end.

export interface SignalStats {
  n: number;      // values counted
  min: number;
  med: number;
  max: number;
  p95: number;
  p995: number;
}

/**
 * The [p] (0..1) percentile of an ascending series, interpolated between the
 * two closest ranks. NaN for an empty series.
 */
export function percentile(sorted: number[], p: number): number {
  const n = sorted.length;
  if (!n) return NaN;
  const h = (n - 1) * Math.min(1, Math.max(0, p));
  const lo = Math.floor(h);
  const hi = Math.ceil(h);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * (h - lo);
}

/**
 * The figures for [values]: numbers, or numbers as text, as the rows carry them.
 * Null, empty and non-numbers are skipped, and so is anything [valid] rejects.
 * Null when nothing is left.
 */
export function signalStats(values: unknown[], valid?: (v: number) => boolean): SignalStats | null {
  const nums: number[] = [];
  for (const v of values || []) {
    if (v === null || v === undefined || v === '') continue;
    const n = Number(v);
    if (!Number.isFinite(n)) continue;
    if (valid && !valid(n)) continue;
    nums.push(n);
  }
  if (!nums.length) return null;
  nums.sort((a, b) => a - b);
  return {
    n: nums.length,
    min: nums[0],
    med: percentile(nums, 0.5),
    max: nums[nums.length - 1],
    p95: percentile(nums, 0.95),
    p995: percentile(nums, 0.995),
  };
}

/**
 * Which of a signal's values are readings. SpO2 and heart rate use 0 (no
 * reading) and 255 (the ring off the finger) as markers, not values; any other
 * signal's 0 is a reading (a night with no leak).
 */
export function validFor(key: string): ((v: number) => boolean) | undefined {
  if (key === 'spo2') return v => v > 0 && v <= 100;
  if (key === 'heart_rate') return v => v > 0 && v < 255;
  return undefined;
}

/** Decimals to show for a signal, by its unit. */
export function statsDigits(unit: string): number {
  if (unit === '0-1' || unit === 'ratio') return 2;
  if (unit === '%' || unit === 'bpm') return 0;
  return 1;
}
