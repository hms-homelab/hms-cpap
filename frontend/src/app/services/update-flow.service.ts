import { Injectable, OnDestroy } from '@angular/core';
import { HttpErrorResponse } from '@angular/common/http';
import { CpapApiService, UpdateStatus } from './cpap-api.service';

/**
 * SDD-041: applying an update, followed from the click to the new version.
 *
 * Shared by the dashboard banner and Settings so the one click behaves the same
 * in both places. The service downloads and verifies, then hands over and goes
 * away while the install is replaced, so "following" means polling
 * /api/update while it answers, then /health until it answers as the NEW
 * version, then reloading the page so the new UI is what the user sees.
 */
export type UpdatePhase =
  | 'idle'
  | 'busy'        // refused: a night is still arriving; the user may say "now"
  | 'applying'    // downloading / verifying / handing off
  | 'restarting'  // the app is being replaced; waiting for the new version
  | 'failed';

@Injectable({ providedIn: 'root' })
export class UpdateFlowService implements OnDestroy {
  phase: UpdatePhase = 'idle';
  step = '';        // the service's apply_step while applying
  error = '';
  target = '';      // the version being installed
  private timer: ReturnType<typeof setInterval> | null = null;
  private deadline = 0;

  constructor(private api: CpapApiService) {}

  ngOnDestroy() { this.stopPolling(); }

  apply(status: UpdateStatus, now = false) {
    this.target = status.latest;
    this.error = '';
    this.phase = 'applying';
    this.step = 'downloading';
    this.api.applyUpdate(now).subscribe({
      next: () => this.follow(),
      error: (e: HttpErrorResponse) => {
        const msg = e.error?.error ?? '';
        if (e.status === 409 && /still being collected/.test(msg)) {
          this.phase = 'busy';
        } else {
          this.phase = 'failed';
          this.error = msg || 'The update could not be started.';
        }
      },
    });
  }

  private follow() {
    this.stopPolling();
    // Fifteen minutes covers a slow download of the largest file, the swap and
    // the new version's first start. Past that, say so rather than spin.
    this.deadline = Date.now() + 15 * 60 * 1000;
    this.timer = setInterval(() => this.poll(), 2000);
  }

  private poll() {
    if (Date.now() > this.deadline) {
      this.stopPolling();
      this.phase = 'failed';
      this.error = 'The update did not finish. Open Settings, Updates for the last result.';
      return;
    }
    if (this.phase === 'applying') {
      this.api.getUpdateStatus().subscribe({
        next: s => {
          if (s.applying) { this.step = s.apply_step; return; }
          // Stopped applying while still answering: it refused, and says why.
          this.stopPolling();
          this.phase = 'failed';
          this.error = s.error || 'The update was not applied.';
        },
        // No answer: the service has handed over and is being replaced.
        error: () => { this.phase = 'restarting'; },
      });
      return;
    }
    // restarting: wait for the new version itself, then show its UI.
    this.api.getHealth().subscribe({
      next: h => {
        if (h.version === this.target) {
          this.stopPolling();
          location.reload();
          return;
        }
        // Up again, but as the OLD version: the helper rolled back, and its
        // result says at which step and why.
        this.api.getUpdateStatus().subscribe(s => {
          const r = s.last_result;
          if (r?.present && !r.ok && r.version === this.target) {
            this.stopPolling();
            this.phase = 'failed';
            this.error = r.message;
          }
        });
      },
      error: () => { /* still coming up */ },
    });
  }

  private stopPolling() {
    if (this.timer) { clearInterval(this.timer); this.timer = null; }
  }
}
