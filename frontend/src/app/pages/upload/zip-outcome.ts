// What the upload page shows for a CPAP zip, from /api/backfill/status.
// Kept free of Angular so `npm test` can run it.
//
// Issue #55: the zip is read on the backfill worker after the upload's reply
// has gone, so the reply's `dates` is always empty. The worker keeps its
// answer in the status's `upload`, under the id the reply carries; the page
// waits for THAT, not merely for the worker to look idle (which it does
// before it has even picked the zip up).

/** The status's `upload` object (BackfillService::triggerZipImport). */
export interface ZipUpload {
  id: number;
  state: 'queued' | 'reading' | 'done';
  error?: string;
  dates?: string[];
  sessions_found?: number;
}

export interface BackfillStatusLike {
  status?: string;
  error_message?: string;
  upload?: ZipUpload;
}

export type ZipOutcome =
  | { done: false; reading: boolean }
  | { done: true; error: string }
  | { done: true; error: null; dates: string[] };

/**
 * [uploadId] is the reply's `upload_id`; absent from a server that imports
 * inline, whose reply already holds the answer ([replyDates]).
 */
export function zipOutcome(uploadId: number | null | undefined,
                           s: BackfillStatusLike | null | undefined,
                           replyDates: string[]): ZipOutcome {
  const status = s?.status ?? '';
  if (!uploadId) {
    if (status === 'running') return { done: false, reading: false };
    if (status === 'error') return { done: true, error: s?.error_message || '' };
    return { done: true, error: null, dates: replyDates };
  }
  const up = s?.upload;
  if (!up || up.id !== uploadId) return { done: false, reading: false };
  if (up.state !== 'done') return { done: false, reading: up.state === 'reading' };
  if (up.error) return { done: true, error: up.error };
  // The zip is read; the nights it found may still be importing.
  if (status === 'running') return { done: false, reading: false };
  if (status === 'error') return { done: true, error: s?.error_message || '' };
  return { done: true, error: null, dates: up.dates ?? [] };
}
