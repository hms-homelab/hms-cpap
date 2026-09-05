/**
 * Shared display formatting.
 *
 * SDD-079: the index group — AHI, AI, HI, OAI, CAI, UAI, RIN — renders to TWO
 * decimals, everywhere, through this one helper. Every other metric (pressure,
 * leak, usage hours, SpO2, ODI, respiratory rate, event durations, MB sizes)
 * stays at one decimal and must NOT be routed through here.
 *
 * Why two decimals: ResMed's STR.edf AHI channel is a 0..2400 integer scaled by
 * 0.1, so the machine can only store one decimal and it floors into it. A true
 * AHI of 0.49 is stored as 0.4. The honest second digit is produced on the value
 * side (cpapdash-ingest SDD-001); this is the display half that shows it.
 *
 * RETURNS DIGITS ONLY. Never extend this to append a unit ("events/hr"), a label
 * or any other word. The moment it returns a word it stops being a value and
 * becomes copy, and copy has to come from the i18n dictionary or a Portuguese or
 * Spanish screen renders an English fragment. Units and labels belong in the
 * template, beside the translated string. Raised by the i18n owner during
 * SDD-080; the temptation is real, "events/hr" already sits next to the value in
 * key-metrics and is passed as the gauge unit in dashboard.component.ts.
 *
 * Chart Y-axis ticks stay at zero decimals deliberately — that is not an
 * oversight and must not be "fixed" by pointing them at this helper.
 */

/** Decimal places for every member of the index group. One edit changes them all. */
export const INDEX_DECIMALS = 2;

/**
 * Format an index-group value (AHI, AI, HI, OAI, CAI, UAI, RIN).
 *
 * Accepts the number the dashboard components hold and the string the API
 * serialises, so both display paths agree on precision.
 *
 * @param value    the index, as number or string; null/''/non-numeric yields the fallback
 * @param fallback what to render when there is no value at all (default '-')
 */
export function formatIndex(
  value: number | string | null | undefined,
  fallback = '-',
): string {
  if (value === null || value === undefined || value === '') return fallback;
  const n = typeof value === 'number' ? value : +value;
  if (!Number.isFinite(n)) return fallback;
  return n.toFixed(INDEX_DECIMALS);
}
