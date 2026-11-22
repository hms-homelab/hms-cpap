// SDD-082: the Reports page's form rules, kept apart from the component so
// `npm test` runs them with Node's own test runner.
//
// The same picker CpapDash's Reports page has: three kinds, for the
// compliance report a 30 / 60 / 90 choice ending at the latest night, the
// rule's hours and percent, and the bounds the server holds the rule to.

export type ReportKind = 'standard' | 'clinician' | 'compliance';

export const REPORT_KINDS: readonly ReportKind[] = ['standard', 'clinician', 'compliance'];
export const SPANS: readonly (30 | 60 | 90)[] = [30, 60, 90];
export const RULE_DEFAULT_SHARE = 70;

/** The bounds POST /api/reports/generate holds a rule to (0.5 to 24 h, 1 to 100 %). */
export function ruleInRange(hours: number, share: number): boolean {
  return Number.isFinite(hours) && hours >= 0.5 && hours <= 24 &&
         Number.isInteger(share) && share >= 1 && share <= 100;
}

/** "YYYY-MM-DD" of a date on this browser's calendar. */
export function localDate(d: Date): string {
  const y = d.getFullYear();
  const m = String(d.getMonth() + 1).padStart(2, '0');
  const day = String(d.getDate()).padStart(2, '0');
  return `${y}-${m}-${day}`;
}

/** The dashboard's latest night ("YYYYMMDD" or "YYYY-MM-DD") as "YYYY-MM-DD", or ''. */
export function nightOf(sleepDay: unknown): string {
  if (typeof sleepDay !== 'string') return '';
  if (/^\d{8}$/.test(sleepDay)) return `${sleepDay.slice(0, 4)}-${sleepDay.slice(4, 6)}-${sleepDay.slice(6, 8)}`;
  if (/^\d{4}-\d{2}-\d{2}/.test(sleepDay)) return sleepDay.slice(0, 10);
  return '';
}

/** The last `n` nights ending at `end` ("YYYY-MM-DD"), both ends included. */
export function spanDates(n: number, end: string): { start: string; end: string } {
  const endMs = new Date(end + 'T12:00:00').getTime();
  return { start: localDate(new Date(endMs - (n - 1) * 86400000)), end };
}

/** The rule's hours as typed: 4 reads "4", 4.5 reads "4.5". */
export function hoursLabel(hours: number): string {
  return Number.isFinite(hours) ? String(hours) : '';
}

/** What the generate call sends beyond the dates. */
export function requestBody(kind: ReportKind, hours: number, share: number, language: string):
    { kind?: ReportKind; rule?: { hours: number; share: number }; language: string } {
  const body: { kind?: ReportKind; rule?: { hours: number; share: number }; language: string } = { language };
  if (kind !== 'standard') body.kind = kind;
  if (kind === 'compliance') body.rule = { hours: Number(hours), share: Number(share) };
  return body;
}
