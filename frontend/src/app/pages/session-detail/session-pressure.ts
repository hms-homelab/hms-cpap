// Issue #56, the session page: which pressure its Pressure chart plots.
//
// The night's signals carry two pressures per minute. pressure_avg (with
// pressure_min/max) is measured at the mask; with EPR it drops by up to the EPR
// level on every breath out, so on a minimum of 7 with EPR 3 it sits near 5.
// therapy_pressure is what the machine delivered (the card's PLD Press, the
// same per-minute rows the dashboard's therapy pressure is computed from),
// which never goes below the prescribed minimum.
//
// The chart plots the therapy pressure, with the mask's as its own dashed line.
// A night with no therapy pressure (parsed before the machine's per-minute
// pressure was kept) plots the mask's as before, titled as the mask's.
//
// A BMC / React Health Luna card (machine_format 'bmc') carries the machine's
// own pressure reading, and nothing on the card says whether it is sensed at
// the mask or at the outlet. Its pressure_avg is drawn the same way but called
// plain "Pressure", never the mask's.
//
// A Philips DreamStation card (machine_format 'philips') holds flow only while
// a session is open; the machine writes the pressure it delivered at
// mask-off, a 2-minute average. While live the chart says pressure arrives
// when the session ends; after, it plots that delivered pressure.
//
// Plain functions with no Angular in them, so `npm test` runs them directly.

export type SessionPressureSource = 'therapy' | 'mask' | 'delivered' | 'pending';

export interface PressureLine {
  /** Key of the series in the signals response. */
  key: string;
  /** i18n key of the legend label. */
  labelKey: string;
  color: string;
  dashed: boolean;
}

export interface SessionPressurePlan {
  source: SessionPressureSource;
  /** The series the chart is about, and its overview thumbnail. */
  mainKey: string;
  /** i18n key of the chart title. */
  titleKey: string;
  /** i18n key of the line under the title saying what is drawn. */
  noteKey: string;
  /** Every line drawn, the main one first. */
  lines: PressureLine[];
  /** The mask's min/max band, drawn only when the mask is the chart. */
  band: { minKey: string; maxKey: string } | null;
  /** Draw the line across minutes with no value (a 2-minute series). */
  spanGaps?: boolean;
  /** No pressure yet: the chart says when it arrives instead of drawing. */
  pending?: boolean;
}

type Signals = Record<string, unknown> | null | undefined;

/** True when any minute of the series is a real reading (no machine delivers 0). */
export function hasReading(signals: Signals, key: string): boolean {
  const s = signals?.[key];
  if (!Array.isArray(s)) return false;
  return s.some(v => {
    const n = parseFloat(String(v ?? ''));
    return Number.isFinite(n) && n > 0;
  });
}

/**
 * True when any minute of the series holds a value, 0 included. A channel the
 * machine does not record is empty (null) in every minute, and gets no tile:
 * a Luna has no flow limitation or snore index. A ResMed night with no flow
 * limitation is 0 all night, which is a reading, and keeps its tile.
 */
export function hasAnyValue(signals: Signals, key: string): boolean {
  const s = signals?.[key];
  if (!Array.isArray(s)) return false;
  return s.some(v => v !== null && v !== undefined && v !== '' &&
    Number.isFinite(parseFloat(String(v))));
}

export const THERAPY_COLOR = '#ce93d8';
export const MASK_COLOR = '#64b5f6';

/**
 * What the session page's pressure chart plots, or null when the night has no
 * pressure at all (no chart rather than a flat line at zero).
 */
export function sessionPressurePlan(signals: Signals, live = false): SessionPressurePlan | null {
  const therapy = hasReading(signals, 'therapy_pressure');
  const mask = hasReading(signals, 'pressure_avg');
  // A Luna's measured pressure is not the mask's (see the header).
  const measuredNotMask = signals?.['machine_format'] === 'bmc';

  if (therapy) {
    const lines: PressureLine[] = [
      { key: 'therapy_pressure', labelKey: 'sessionDetail.pressure.legendTherapy',
        color: THERAPY_COLOR, dashed: false },
    ];
    if (mask) {
      lines.push({ key: 'pressure_avg',
        labelKey: measuredNotMask ? 'signal.pressure' : 'sessionDetail.pressure.legendMask',
        color: MASK_COLOR, dashed: true });
    }
    return {
      source: 'therapy',
      mainKey: 'therapy_pressure',
      titleKey: 'signal.therapyPressure',
      noteKey: !mask ? 'sessionDetail.pressure.noteTherapyOnly'
        : measuredNotMask ? 'sessionDetail.pressure.noteTherapyMeasured'
        : 'sessionDetail.pressure.noteTherapy',
      lines,
      band: null,
    };
  }
  if (mask) {
    const titleKey = measuredNotMask ? 'signal.pressure' : 'signal.maskPressure';
    return {
      source: 'mask',
      mainKey: 'pressure_avg',
      titleKey,
      noteKey: measuredNotMask ? 'sessionDetail.pressure.noteMeasured' : 'sessionDetail.pressure.noteMask',
      lines: [{ key: 'pressure_avg', labelKey: titleKey, color: THERAPY_COLOR, dashed: false }],
      band: { minKey: 'pressure_min', maxKey: 'pressure_max' },
    };
  }
  if (signals?.['machine_format'] === 'philips') {
    // A DreamStation's waveform is flow only. The pressure it delivered is
    // written at mask-off, a 2-minute average, in epr_pressure (every other
    // minute has none, so the line spans them).
    if (hasReading(signals, 'epr_pressure')) {
      return {
        source: 'delivered',
        mainKey: 'epr_pressure',
        titleKey: 'signal.pressureDelivered',
        noteKey: 'sessionDetail.pressureLive.deliveredPhilips',
        lines: [{ key: 'epr_pressure', labelKey: 'signal.pressureDelivered',
          color: THERAPY_COLOR, dashed: false }],
        band: null,
        spanGaps: true,
      };
    }
    // While the session is open there is none yet: say so, never an empty chart.
    if (live && hasAnyValue(signals, 'flow_avg')) {
      return {
        source: 'pending',
        mainKey: 'epr_pressure',
        titleKey: 'signal.pressure',
        noteKey: 'sessionDetail.pressureLive.pendingPhilips',
        lines: [],
        band: null,
        pending: true,
      };
    }
  }
  return null;
}
