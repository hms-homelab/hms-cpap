// Issue #56: which pressure the dashboard shows as the night's therapy pressure.
//
// A night's row carries two pressures. mask_press_* is measured at the mask;
// with EPR it drops by up to the EPR level on every breath out, so on a minimum
// of 7 with EPR 3 its median sits near 5. therapy_press_* is what the machine
// delivered (the card's PLD Press), which never goes below the prescribed
// minimum. The panel and the trend show the therapy pressure, and the mask's
// only for a night or a month that has none, labelled as the mask's.
//
// Plain functions with no Angular in them, so `npm test` runs them directly.

export type PressureSource = 'therapy' | 'mask';

export interface NightPressure {
  source: PressureSource;
  p50: number;
  p95: number;
  max: number;
  /** The mask's median beside a therapy pressure, as its own value; null when
   *  the night has none or it is already the value shown. */
  maskP50: number | null;
}

type Row = Record<string, unknown>;

/** A reading, or 0 for NULL, empty, not a number, or not above zero. */
function reading(row: Row, key: string): number {
  const v = parseFloat(String(row?.[key] ?? ''));
  return Number.isFinite(v) && v > 0 ? v : 0;
}

/**
 * The night's pressure for the dashboard panel, or null when the night has no
 * pressure at all (a Sefam or a Löwenstein: no panel rather than "0.0 cmH2O").
 */
export function nightPressure(row: Row | null | undefined): NightPressure | null {
  if (!row) return null;
  const t50 = reading(row, 'therapy_press_50');
  const t95 = reading(row, 'therapy_press_95');
  const tMax = reading(row, 'therapy_press_max');
  const m50 = reading(row, 'mask_press_50');
  const m95 = reading(row, 'mask_press_95');
  const mMax = reading(row, 'mask_press_max');

  if (t50 || t95 || tMax) {
    return { source: 'therapy', p50: t50, p95: t95, max: tMax, maskP50: m50 || null };
  }
  if (m50 || m95 || mMax) {
    return { source: 'mask', p50: m50, p95: m95, max: mMax, maskP50: null };
  }
  return null;
}

export interface TrendSeries {
  key: string;
  /** i18n key of the legend label. */
  labelKey: string;
  color: string;
  fill?: string;
  dashed?: boolean;
  /** A pressure of 0 is no reading (no machine delivers 0): plot a gap. */
  zeroIsGap: boolean;
}

export interface PressureTrendPlan {
  source: PressureSource;
  /** i18n key of the chart title. */
  titleKey: string;
  series: TrendSeries[];
}

/**
 * What the 30-day pressure trend plots, or null when no night in it has any
 * pressure (no chart rather than a flat line at zero).
 *
 * A month with therapy pressure plots its median and 95th percentile, plus the
 * mask's median as its own dashed line: it shows the gap EPR makes, and it
 * fills the nights that have no therapy pressure without putting a mask figure
 * on the therapy line. A month with none plots the mask's, titled as the mask's.
 */
export function pressureTrendPlan(rows: Row[] | null | undefined): PressureTrendPlan | null {
  const data = rows ?? [];
  const any = (keys: string[]) => data.some(r => keys.some(k => reading(r, k) > 0));

  if (any(['therapy_press_50', 'therapy_press_95'])) {
    return {
      source: 'therapy',
      titleKey: 'dashboard.chart.pressureTrend',
      series: [
        { key: 'therapy_press_50', labelKey: 'dashboard.pressure.trendP50', color: '#ce93d8', zeroIsGap: true },
        { key: 'therapy_press_95', labelKey: 'dashboard.pressure.trendP95', color: '#ba68c8', fill: '-1', zeroIsGap: true },
        { key: 'mask_press_50', labelKey: 'dashboard.pressure.trendMaskP50', color: '#64b5f6', dashed: true, zeroIsGap: true },
      ],
    };
  }
  if (any(['mask_press_50', 'mask_press_95'])) {
    return {
      source: 'mask',
      titleKey: 'dashboard.chart.maskPressureTrend',
      series: [
        { key: 'mask_press_50', labelKey: 'dashboard.pressure.trendP50', color: '#ce93d8', zeroIsGap: true },
        { key: 'mask_press_95', labelKey: 'dashboard.pressure.trendP95', color: '#ba68c8', fill: '-1', zeroIsGap: true },
      ],
    };
  }
  return null;
}
