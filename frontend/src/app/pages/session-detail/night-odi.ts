// SDD-001 F2: the night's SpO2 desaturations and ODI, for the SpO2 card and
// the events breakdown. Kept free of Angular (and of imports) so `npm test`
// can run it.
//
// The same rule as the night aggregate behind Home Assistant (sql::nightOdi):
// the desaturations of the sessions that recorded SpO2, per hour of those
// sessions. A session without SpO2 adds neither desaturations nor hours. Null
// for a night with no SpO2, so the page shows no ODI rather than an ODI of 0.

export interface OdiSession {
  avg_spo2?: string | number | null;
  spo2_drops?: string | number | null;
  duration_seconds?: string | number | null;
  duration_hours?: string | number | null;
}

const num = (v: unknown): number => {
  const n = Number(v ?? 0);
  return Number.isFinite(n) ? n : 0;
};

/** One session's recorded seconds, as night-ahi's sessionSeconds reads them. */
const seconds = (s: OdiSession): number => {
  const secs = num(s.duration_seconds);
  return secs > 0 ? secs : num(s.duration_hours) * 3600;
};

export function nightDesaturations(sessions: OdiSession[]): { drops: number; odi: number } | null {
  let drops = 0, secs = 0, any = false;
  for (const s of sessions || []) {
    if (!(num(s.avg_spo2) > 0) || s.spo2_drops == null) continue;
    any = true;
    drops += num(s.spo2_drops);
    secs += seconds(s);
  }
  if (!any) return null;
  return { drops, odi: secs > 0 ? drops * 3600 / secs : 0 };
}
