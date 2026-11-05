// The AHI of a night made of several sessions, as the session page shows it
// when it merges them. Kept free of Angular so `npm test` can run it.

/** The parts of a session row (GET /api/sessions/:date) the AHI is made of. */
export interface AhiCounts {
  duration_seconds?: string | number | null;
  duration_hours?: string | number | null;
  obstructive_apneas?: string | number | null;
  central_apneas?: string | number | null;
  clear_airway_apneas?: string | number | null;
  unclassified_apneas?: string | number | null;
  hypopneas?: string | number | null;
}

const n = (v: string | number | null | undefined): number => {
  const x = Number(v ?? 0);
  return Number.isFinite(x) ? x : 0;
};

/** One session's recorded seconds: its own count, else its rounded hours. */
export function sessionSeconds(s: AhiCounts): number {
  const secs = n(s.duration_seconds);
  return secs > 0 ? secs : n(s.duration_hours) * 3600;
}

/**
 * Apneas of every kind plus hypopneas, per hour over the sessions' summed
 * time: the formula the sessions list and the dashboard use for the same
 * night (sql::nightAhi on the server). Flow limitation, snore, RERA, large
 * leak and periodic breathing are events, not part of the AHI; issue #54 was
 * this page dividing ALL of them by the hours.
 */
export function nightAhi(sessions: AhiCounts[]): number {
  const seconds = sessions.reduce((sum, s) => sum + sessionSeconds(s), 0);
  if (seconds <= 0) return 0;
  const events = sessions.reduce((sum, s) =>
    sum + n(s.obstructive_apneas) + n(s.central_apneas) + n(s.clear_airway_apneas) +
          n(s.unclassified_apneas) + n(s.hypopneas), 0);
  return events * 3600 / seconds;
}
