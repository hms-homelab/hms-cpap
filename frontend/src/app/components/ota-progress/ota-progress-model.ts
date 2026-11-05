// What a firmware update shows, as plain data, so it is tested without a
// browser (ota-progress-model.spec.ts, run with `npm test`).
//
// The job is the server's (GET /api/devices, each card's detail.ota): one
// update of one unit, made of steps, one per image. Nothing here keeps time
// or guesses: the view is what the last read said. The bar is shown only
// while bytes move (transferring), from counted bytes; every other phase is
// words. A unit that went quiet after taking an image is not failed: the
// view says when it was last heard from.

export interface OtaStep {
  target: string;
  phase: string;
  bytes: number;
  total: number;
  bytes_meaning?: string;
  version_before: string;
  version_target: string;
  version_after: string;
}

export interface OtaJob {
  job_id: string;
  serial: string;
  kind: string;
  source: string;
  confirmed_while_recording: boolean;
  phase: string;
  step: number;
  steps: number;
  steps_detail: OtaStep[];
  hold_reason: string | null;
  held_since: number | null;
  reason: string | null;
  detail: string | null;
  retryable?: boolean;
  last_heard: number;
  started_at: number;
  updated_at: number;
  ended_at: number | null;
}

export type OtaState =
  | 'none'
  | 'held'
  | 'queued'
  | 'preparing'
  | 'transferring'
  | 'verifying'
  | 'restarting'
  | 'done'
  | 'failed'
  | 'cancelled';

export interface OtaView {
  state: OtaState;
  ended: boolean;
  /** 0..100, only while transferring with a known total. */
  pct: number | null;
  bytes: number;
  total: number;
  /** "Step n of m: target" when the job has more than one step. */
  step: { n: number; m: number; target: string } | null;
  before: string;
  target: string;
  after: string;
  reason: string;
  detail: string;
  retryable: boolean;
  /** Restarting: when the unit was last heard (epoch s), shown as a fact. */
  notHeardSince: number | null;
  confirmedWhileRecording: boolean;
}

const KNOWN: OtaState[] = ['held', 'queued', 'preparing', 'transferring', 'verifying', 'restarting', 'done', 'failed', 'cancelled'];

/** The view of `job`; a null job is 'none'. */
export function otaView(job: OtaJob | null | undefined): OtaView {
  const empty: OtaView = {
    state: 'none', ended: true, pct: null, bytes: 0, total: 0, step: null, before: '', target: '', after: '',
    reason: '', detail: '', retryable: false, notHeardSince: null, confirmedWhileRecording: false,
  };
  if (!job || !Array.isArray(job.steps_detail)) return empty;
  const idx = Math.max(0, Math.min((job.step || 1) - 1, job.steps_detail.length - 1));
  const s = job.steps_detail[idx];
  const raw = (job.phase || s?.phase || '') as OtaState;
  const state: OtaState = KNOWN.includes(raw) ? raw : 'queued';
  const ended = state === 'done' || state === 'failed' || state === 'cancelled';
  // The versions the person cares about: the first step's before, the last
  // step's after (a mule-then-miner job reads as one update).
  const first = job.steps_detail[0];
  const last = job.steps_detail[job.steps_detail.length - 1];
  const total = s?.total || 0;
  const bytes = Math.min(s?.bytes || 0, total || Infinity);
  return {
    state,
    ended,
    pct: state === 'transferring' && total > 0 ? Math.floor((bytes * 100) / total) : null,
    bytes,
    total,
    step: job.steps > 1 && s ? { n: idx + 1, m: job.steps, target: s.target } : null,
    before: s?.version_before || first?.version_before || '',
    target: s?.version_target || '',
    after: last?.version_after || s?.version_after || '',
    reason: job.reason || '',
    detail: job.detail || '',
    retryable: state === 'failed' && !!job.retryable,
    notHeardSince: state === 'restarting' && job.last_heard > 0 && job.last_heard <= job.updated_at ? job.last_heard : null,
    confirmedWhileRecording: !!job.confirmed_while_recording,
  };
}

/** The bar's fill as a fraction for scaleX: 0 to 1, 0 when there is no bar. */
export function barFraction(v: OtaView): number {
  if (v.pct === null) return 0;
  return Math.max(0, Math.min(1, v.pct / 100));
}

/** The i18n key of a failure's sentence, under devices.ota.fail. */
export function failKey(reason: string): string {
  const known = ['refused_image', 'connection_lost', 'back_on_old_version', 'bake_failed', 'image_unavailable',
                 'offline', 'interrupted', 'superseded'];
  return 'devices.ota.fail.' + (known.includes(reason) ? reason : 'other');
}

/** The advice under a failure, under devices.ota.advice, or '' for none. */
export function adviceKey(reason: string): string {
  switch (reason) {
    case 'refused_image':
    case 'back_on_old_version':
      return 'devices.ota.advice.retry';
    case 'connection_lost':
      return 'devices.ota.advice.checkPower';
    case 'bake_failed':
    case 'image_unavailable':
      return 'devices.ota.advice.later';
    case 'offline':
      return 'devices.ota.advice.bringOnline';
    case 'interrupted':
      return 'devices.ota.advice.wait';
    default:
      return '';
  }
}

/** A byte count as {key, n} for devices.ota.units: B, KB, MB, one decimal. */
export function sizeParts(n: number): { key: string; n: string } {
  if (n < 1024) return { key: 'devices.ota.units.b', n: String(n) };
  if (n < 1024 * 1024) return { key: 'devices.ota.units.kb', n: (n / 1024).toFixed(1) };
  return { key: 'devices.ota.units.mb', n: (n / (1024 * 1024)).toFixed(1) };
}

/** "how long ago" as {key, n} for devices.ota.ago, from two epoch seconds. */
export function agoParts(then: number, now: number): { key: string; n: number } {
  const s = Math.max(0, Math.floor(now - then));
  if (s < 60) return { key: 'devices.ota.ago.seconds', n: s };
  if (s < 3600) return { key: 'devices.ota.ago.minutes', n: Math.floor(s / 60) };
  if (s < 86400) return { key: 'devices.ota.ago.hours', n: Math.floor(s / 3600) };
  return { key: 'devices.ota.ago.days', n: Math.floor(s / 86400) };
}

/**
 * Finished jobs the person dismissed, per job id, kept for the page's life
 * so the next poll does not bring them back.
 */
export class Dismissed {
  private ids = new Set<string>();
  dismiss(job: OtaJob | null | undefined): void {
    if (job?.job_id) this.ids.add(job.kind + '/' + job.serial + '/' + job.job_id);
  }
  /** Whether `job` is shown: not dismissed, or not ended (a running job always shows). */
  shown(job: OtaJob | null | undefined): boolean {
    if (!job) return false;
    if (!otaView(job).ended) return true;
    return !this.ids.has(job.kind + '/' + job.serial + '/' + job.job_id);
  }
}
