// Type-only imports, so `npm test` (Node's own runner) can load this file.
import type { ChartDataset } from 'chart.js';
import type { SessionEvent } from '../models/session.model';

export const EVENT_COLORS: Record<string, string> = {
  obstructive_apnea: '#f87171',
  OBSTRUCTIVE: '#f87171',
  central_apnea: '#fb923c',
  CENTRAL: '#fb923c',
  hypopnea: '#fbbf24',
  HYPOPNEA: '#fbbf24',
  rera: '#4ade80',
  RERA: '#4ade80',
  clear_airway: '#60a5fa',
  CLEAR_AIRWAY: '#60a5fa',
  apnea: '#f87171',
  APNEA: '#f87171',
  csr: '#a78bfa',
  CSR: '#a78bfa',
  desaturation: '#fb923c',
  DESATURATION: '#fb923c',
};

/** Case-insensitive color lookup ("Obstructive" -> OBSTRUCTIVE). */
export function eventColor(type: string): string {
  if (!type) return '#888';
  return EVENT_COLORS[type]
    || EVENT_COLORS[type.toUpperCase()]
    || EVENT_COLORS[type.toLowerCase()]
    || '#888';
}

/** SDD-001: an SpO2 desaturation, drawn on the SpO2 chart and no other. */
export function isDesaturation(e: SessionEvent): boolean {
  return (e?.event_type || '').toLowerCase() === 'desaturation';
}

/**
 * SDD-001 F1: the colour key for a chart's event spans, one entry per event
 * type present, in the order they first occur. `type` is the stored name
 * ("Obstructive", "Clear Airway", "Desaturation"); the page translates it.
 */
export function eventLegend(events: SessionEvent[]): { type: string; color: string }[] {
  const seen = new Set<string>();
  const out: { type: string; color: string }[] = [];
  for (const e of events || []) {
    const t = e?.event_type;
    if (!t || seen.has(t)) continue;
    seen.add(t);
    out.push({ type: t, color: eventColor(t) });
  }
  return out;
}

/** Fractional index of an absolute time within a (sorted) per-bucket timeline. */
export function fractionalIndex(timeMs: number, tsMs: number[]): number {
  const n = tsMs.length;
  if (!n) return 0;
  if (timeMs <= tsMs[0]) return 0;
  if (timeMs >= tsMs[n - 1]) return n - 1;
  for (let i = 0; i < n - 1; i++) {
    if (timeMs >= tsMs[i] && timeMs < tsMs[i + 1]) {
      const span = tsMs[i + 1] - tsMs[i] || 1;
      return i + (timeMs - tsMs[i]) / span;
    }
  }
  return n - 1;
}

export function formatTimestamps(timestamps: string[]): string[] {
  return timestamps.map(ts => {
    if (!ts) return '';
    // Handle "2026-03-31 23:30:00" or ISO format
    const d = new Date(ts.replace(' ', 'T'));
    const h = d.getHours().toString().padStart(2, '0');
    const m = d.getMinutes().toString().padStart(2, '0');
    return `${h}:${m}`;
  });
}

export function eventAnnotations(events: SessionEvent[], labels: string[], timestamps: string[]): any[] {
  if (!events?.length || !timestamps?.length) return [];

  const tsMs = timestamps.map(t => new Date((t || '').replace(' ', 'T')).getTime());
  // Minimum span width (in index units) so short events stay visible on a
  // per-minute axis (a 10s apnea is < 0.2 of a bucket).
  const MIN_W = 0.35;

  const annotations: any[] = [];
  for (const e of events) {
    const eventTime = e.event_timestamp?.replace(' ', 'T');
    if (!eventTime) continue;
    const startMs = new Date(eventTime).getTime();
    const dur = Number(e.duration_seconds) || 0;
    const color = eventColor(e.event_type);

    if (dur > 0) {
      const x1 = fractionalIndex(startMs, tsMs);
      let x2 = fractionalIndex(startMs + dur * 1000, tsMs);
      if (x2 - x1 < MIN_W) x2 = x1 + MIN_W;
      annotations.push({
        type: 'box',
        xMin: x1,
        xMax: x2,
        backgroundColor: color + '33', // translucent fill
        borderColor: color + '88',
        borderWidth: 1,
      });
    } else {
      // Zero-duration events (e.g. RERA) render as a dashed marker line.
      const x = fractionalIndex(startMs, tsMs);
      annotations.push({
        type: 'line',
        xMin: x,
        xMax: x,
        borderColor: color,
        borderWidth: 1,
        borderDash: [2, 2],
      });
    }
  }
  return annotations;
}

/**
 * SDD-001 F3: the flow chart shows breath boundaries once its visible window is
 * this short or shorter. Wider, a night's 15 breaths a minute are a smear.
 */
export const BREATH_WINDOW_SEC = 60;

function toMs(ts: string): number {
  return new Date((ts || '').replace(' ', 'T')).getTime();
}

/** Seconds between the chart's visible x bounds, read off its timestamps. */
export function visibleSeconds(timestamps: string[], xMin: number, xMax: number): number {
  if (!timestamps?.length) return Infinity;
  const at = (x: number) => {
    const i = Math.max(0, Math.min(timestamps.length - 1, x));
    const lo = Math.floor(i), hi = Math.ceil(i);
    const a = toMs(timestamps[lo]), b = toMs(timestamps[hi]);
    return a + (b - a) * (i - lo);
  };
  return Math.abs(at(xMax) - at(xMin)) / 1000;
}

/**
 * SDD-001 F3: a faint line at each breath onset inside [xMin, xMax], and every
 * other breath shaded, so a run of breaths reads one from the next. Empty when
 * the window is wider than BREATH_WINDOW_SEC: the overlay is gated on zoom.
 */
export function breathAnnotations(onsets: string[], timestamps: string[],
                                  xMin: number, xMax: number): any[] {
  if (!onsets?.length || !timestamps?.length) return [];
  if (visibleSeconds(timestamps, xMin, xMax) > BREATH_WINDOW_SEC) return [];
  const tsMs = timestamps.map(toMs);
  const firstMs = tsMs[0], lastMs = tsMs[tsMs.length - 1];
  const xs: number[] = [];
  for (const o of onsets) {
    const ms = toMs(o);
    if (!(ms >= firstMs && ms <= lastMs)) continue;
    const x = fractionalIndex(ms, tsMs);
    if (x >= xMin && x <= xMax) xs.push(x);
  }
  const out: any[] = [];
  xs.forEach((x, i) => {
    out.push({ type: 'line', xMin: x, xMax: x, borderColor: '#ffffff33', borderWidth: 1 });
    if (i % 2 === 0 && i + 1 < xs.length) {
      out.push({ type: 'box', xMin: x, xMax: xs[i + 1], backgroundColor: '#ffffff0d', borderWidth: 0 });
    }
  });
  return out;
}

export function makeDataset(
  label: string,
  data: (number | null)[],
  color: string,
  opts?: Partial<ChartDataset<'line'>>
): ChartDataset<'line'> {
  return {
    label,
    data: data as number[],
    borderColor: color,
    backgroundColor: color + '33',
    borderWidth: 1.5,
    pointRadius: 0,
    pointHitRadius: 4,
    tension: 0.2,
    fill: false,
    ...opts,
  };
}

export function makeFillBand(
  label: string,
  dataMin: (number | null)[],
  dataMax: (number | null)[],
  color: string
): ChartDataset<'line'>[] {
  return [
    {
      label: label + ' max',
      data: dataMax as number[],
      borderColor: 'transparent',
      backgroundColor: color + '18',
      borderWidth: 0,
      pointRadius: 0,
      tension: 0.2,
      fill: '+1',
    },
    {
      label: label + ' min',
      data: dataMin as number[],
      borderColor: 'transparent',
      backgroundColor: 'transparent',
      borderWidth: 0,
      pointRadius: 0,
      tension: 0.2,
      fill: false,
    },
  ];
}
