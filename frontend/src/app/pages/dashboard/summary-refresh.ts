// What "Regenerate AI summary" says, from the reply of
// POST /api/sessions/{date}/generate-summary. Kept free of Angular so
// `npm test` can run it.
//
// Issue #74: the server answers 200 either way, with `status: "started"` or
// `status: "failed"` (a night with no detailed session, such as a myAir-only
// one, or no LLM configured). The page said "queued" for both, so a summary
// that was never going to come looked like one on its way.

/** The reply (CpapController::sessionGenerateSummary). */
export interface SummaryRefreshReply {
  status?: string;
  date?: string;
  error?: string;
  message?: string;
  reason?: string;
}

export type SummaryRefreshOutcome =
  | { queued: true }
  | { queued: false; reason: string };

export function summaryRefreshOutcome(reply: SummaryRefreshReply | null | undefined): SummaryRefreshOutcome {
  if (reply?.status !== 'failed') return { queued: true };
  const reason = (reply.error || reply.reason || reply.message || '').trim();
  return { queued: false, reason };
}
