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

/**
 * Format a therapy night's date for display, in the reader's own language.
 *
 * THE YEAR IS DELIBERATE. This renders on the one line that tells someone how
 * current the data in front of them is, and "Sep 6" cannot distinguish this
 * year from last. A card that has sat in a drawer, a machine coming back after
 * a gap, a reader scrolling old nights — all of them read a bare month and day
 * as "recent" because that is what a bare month and day looks like.
 *
 * THE LOCALE IS NOT OPTIONAL EITHER. Both call sites used to hardcode 'en-US',
 * so a Spanish or Hungarian reader got an English date sitting underneath a
 * translated heading. `toLocaleDateString` also reorders the parts per locale
 * (day-before-month across most of Europe), which is the part a hand-rolled
 * format would get wrong even after translating the month name.
 *
 * Callers pass the active language from LanguageService rather than reading
 * navigator.language, so the date follows the switcher rather than the browser.
 *
 * @param dateStr  API date, 'YYYY-MM-DD'
 * @param locale   active language tag, e.g. 'es'
 */
export function formatNightDate(dateStr: string, locale: string): string {
  const d = parseNight(dateStr);
  if (!d) return dateStr || '';
  return d.toLocaleDateString(locale, {
    year: 'numeric',
    month: 'short',
    day: 'numeric',
  });
}

/**
 * The same date WITHOUT the year, for dense lists of recent nights.
 *
 * The year earns its place on a single headline date, where the reader has
 * nothing else to date the data by. In a table of a rolling 30-day window it is
 * the same four digits on every row, so it stops informing and starts crowding.
 * Still locale-aware: the ordering of day and month is not an English default.
 */
export function formatShortNightDate(dateStr: string, locale: string): string {
  const d = parseNight(dateStr);
  if (!d) return dateStr || '';
  return d.toLocaleDateString(locale, { month: 'short', day: 'numeric' });
}

/**
 * Parse an API night date to a local Date, or null when it is unusable.
 *
 * MIDDAY, DELIBERATELY. `new Date('2026-09-06')` is parsed as UTC midnight, and
 * rendering that anywhere west of Greenwich lands on the 5th — the date would be
 * a day early for every user in the Americas. Noon is far enough from both
 * midnights that no real timezone offset can move the calendar day.
 */
function parseNight(dateStr: string): Date | null {
  if (!dateStr) return null;
  const d = new Date(dateStr.substring(0, 10) + 'T12:00:00');
  return Number.isNaN(d.getTime()) ? null : d;
}

/**
 * Whole days between a night and today. Null when the date is unusable.
 *
 * The dashboard headline is whatever the NEWEST row happens to be — the query
 * has no date bound at all, unlike the 30-day trend beside it. So a machine that
 * has not synced in three weeks shows a three-week-old AHI that looks exactly
 * like last night's. This is what lets the view say so.
 *
 * Both sides are taken at midday for the same reason parseNight is, and the
 * difference is rounded rather than floored so a night stored across a DST
 * boundary (23 or 25 real hours) still counts as one day rather than zero.
 *
 * Negative results are possible and are returned as-is: a night dated in the
 * future means a machine clock is wrong, which is worth surfacing rather than
 * clamping away.
 */
export function nightAgeInDays(dateStr: string, now = new Date()): number | null {
  const d = parseNight(dateStr);
  if (!d) return null;
  const today = new Date(now.getFullYear(), now.getMonth(), now.getDate(), 12);
  return Math.round((today.getTime() - d.getTime()) / 86400000);
}
