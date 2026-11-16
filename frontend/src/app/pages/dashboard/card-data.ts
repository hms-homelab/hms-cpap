// Whether a dashboard card has anything to show. Kept free of Angular so
// `npm test` can run it.
//
// The rule (Albin): an empty card is better not shown. A card with no data for
// the night or the source being viewed is left out, never drawn as a frame of
// zeros, dashes, empty axes or a "not enough data" note. A myAir-only night
// was the worst case: key metrics read "AHI 0.00 Excellent" for a night with
// no CPAP data at all, under five empty chart frames.
//
// Every card decides with hasCardData, so the rule is the same everywhere.

/** One value: absent, empty, a placeholder dash or NaN is not data. */
function present(v: unknown, zeroIsEmpty: boolean): boolean {
  if (v === null || v === undefined) return false;
  if (typeof v === 'number') return !Number.isNaN(v) && !(zeroIsEmpty && v === 0);
  if (typeof v === 'string') {
    const s = v.trim();
    if (s === '' || s === '--' || s === '-') return false;
    // The API sends numbers as strings ("0.00"), so a zero there is a zero.
    if (zeroIsEmpty && s !== '' && !Number.isNaN(Number(s)) && Number(s) === 0) return false;
    return true;
  }
  if (typeof v === 'boolean') return false;   // a flag is not a measurement
  if (Array.isArray(v)) return v.some(x => present(x, zeroIsEmpty));
  if (typeof v === 'object') return Object.values(v as object).some(x => present(x, zeroIsEmpty));
  return true;
}

export interface CardDataOptions {
  /**
   * For channels a machine fills with 0 when it has no such channel (pressure,
   * respiratory figures, event types, CSR, live pressures): a zero there is
   * the absence of a measurement. Off by default, because on a recorded night
   * a zero is real (no central apneas IS a result).
   */
  zeroIsEmpty?: boolean;
}

/**
 * Does this card have data?
 *
 * [source] is what the card is built from: an object, an array of rows (a
 * trend, a list), or a single value. With [fields], only those fields count,
 * so a card whose object always carries a status or a mode is judged by its
 * measurements; on an array, any row with any of those fields is enough.
 */
export function hasCardData(source: unknown, fields?: readonly string[],
                            opts: CardDataOptions = {}): boolean {
  const zero = !!opts.zeroIsEmpty;
  if (!fields?.length) return present(source, zero);
  const rows = Array.isArray(source) ? source : [source];
  return rows.some(r => r !== null && typeof r === 'object' &&
    fields.some(f => present((r as Record<string, unknown>)[f], zero)));
}

/** The measured fields each dashboard card is judged by. */
export const CARD_FIELDS = {
  /** /api/dashboard latest_night. A myAir-only install has none of these. */
  keyMetrics: ['ahi', 'usage_hours', 'leak_avg'],
  /** A daily summary row, for the events breakdown. */
  events: ['oai', 'cai', 'hi', 'rin', 'duration_minutes'],
  /** The real-time card; its status alone ("Completed") is not data. */
  realtime: ['sessionDuration', 'lastSessionTime', 'currentPressure', 'minPressure', 'maxPressure'],
  /** One insight. The "Not enough data" placeholder names no metric. */
  insight: ['metric'],
  ahiTrend: ['value'],
  usageTrend: ['value'],
  leakTrend: ['leak_50', 'leak_95'],
  eventsTrend: ['oai', 'cai', 'hi', 'rin'],
  respiratoryTrend: ['resp_rate_50', 'tid_vol_50', 'min_vent_50'],
  csrTrend: ['csr'],
  eprTrend: ['epr_level'],
} as const;
