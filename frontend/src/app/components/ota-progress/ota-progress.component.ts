import { Component, EventEmitter, Input, Output } from '@angular/core';
import { CommonModule } from '@angular/common';
import { TranslatePipe, TranslateService } from '@ngx-translate/core';

import { Dismissed, OtaJob, OtaView, adviceKey, agoParts, barFraction, failKey, otaView, sizeParts } from './ota-progress-model';

/**
 * Where a firmware update is: the same states and words for every device.
 * Input: the job the server last reported, the version the unit runs and
 * the one on offer. No timer of its own: it renders the last read, and the
 * page's refresh brings the next.
 *
 * Two places on a card, one component:
 * - mode "offer", on the Firmware row: one line, "Firmware X is available.
 *   Update now", the link being the action; and, when the server answered
 *   that the machine records, a quiet inline note with "Not now" and
 *   "Update anyway". Nothing while a job is shown.
 * - mode "progress", the slot under the facts: only while a job is shown
 *   (running, or ended and not yet dismissed).
 */
@Component({
  selector: 'app-ota-progress',
  standalone: true,
  imports: [CommonModule, TranslatePipe],
  template: `
    <!-- ── offer: the Firmware row's one line ── -->
    <ng-container *ngIf="mode === 'offer' && !showJob">
      <div class="offer" *ngIf="available && warning === null">
        {{ 'devices.ota.available' | translate:{ target: available } }}
        <button type="button" class="link" (click)="update.emit()" [disabled]="busy || !canUpdate">
          {{ 'devices.ota.updateNow' | translate }}
        </button>
      </div>
      <span class="quiet" *ngIf="!available && upToDate">{{ 'devices.ota.upToDate' | translate }}</span>
      <!-- The recording warning: a quiet note in the row, never a block. -->
      <div class="note-warn" *ngIf="warning !== null" role="status">
        <span class="note-text">{{ 'devices.confirm.recordingLine' | translate:{ reason: warning || '' } }}</span>
        <span class="note-actions">
          <button type="button" class="btn" (click)="notNow.emit()">{{ 'devices.ota.notNow' | translate }}</button>
          <button type="button" class="btn primary" (click)="updateAnyway.emit()" [disabled]="busy">{{ 'devices.ota.anyway' | translate }}</button>
        </span>
      </div>
    </ng-container>

    <!-- ── progress: only while a job is shown ── -->
    <div class="ota" *ngIf="mode === 'progress' && showJob" [class.err]="v.state === 'failed'" [class.ok]="v.state === 'done'">
      <div class="ota-step" *ngIf="v.step && !v.ended">
        {{ 'devices.ota.step' | translate:{ n: v.step.n, m: v.step.m, target: v.step.target } }}
      </div>
      <ng-container [ngSwitch]="v.state">
        <div *ngSwitchCase="'held'" class="ota-line">{{ 'devices.ota.held' | translate:{ reason: job?.hold_reason || '' } }}</div>
        <div *ngSwitchCase="'queued'" class="ota-line indeterminate">{{ 'devices.ota.queued' | translate }}</div>
        <div *ngSwitchCase="'preparing'" class="ota-line indeterminate">{{ 'devices.ota.preparing' | translate }}</div>
        <div *ngSwitchCase="'transferring'" class="ota-line">
          <div class="bar" role="progressbar" [attr.aria-valuenow]="v.pct" aria-valuemin="0" aria-valuemax="100">
            <div class="fill" [style.transform]="'scaleX(' + fraction() + ')'"></div>
          </div>
          <span class="ota-bytes">{{ 'devices.ota.transferring' | translate:{ pct: v.pct ?? 0, bytes: size(v.bytes), total: size(v.total) } }}</span>
        </div>
        <div *ngSwitchCase="'verifying'" class="ota-line indeterminate">{{ 'devices.ota.verifying' | translate }}</div>
        <div *ngSwitchCase="'restarting'" class="ota-line indeterminate">
          {{ 'devices.ota.restarting' | translate:{ target: v.target || '?' } }}
          <div class="hint" *ngIf="v.notHeardSince">
            {{ 'devices.ota.notHeard' | translate:{ time: (v.notHeardSince * 1000 | date:'short') } }}
          </div>
        </div>
        <div *ngSwitchCase="'done'" class="ota-line">{{ 'devices.ota.done' | translate:{ before: v.before || '?', after: v.after || '?' } }}</div>
        <div *ngSwitchCase="'cancelled'" class="ota-line">{{ 'devices.ota.cancelled' | translate:{ before: v.before || '?' } }}</div>
        <div *ngSwitchCase="'failed'" class="ota-line">
          {{ failSentence() | translate:{ detail: v.detail || v.reason, before: v.before || '?' } }}
          <div class="hint" *ngIf="advice()">{{ advice() | translate }}</div>
        </div>
      </ng-container>
      <div class="hint" *ngIf="!v.ended && job?.updated_at && now">
        {{ 'devices.ota.updatedAgo' | translate:{ ago: (ago().key | translate:{ n: ago().n }) } }}
      </div>
      <div class="hint" *ngIf="v.confirmedWhileRecording">{{ 'devices.ota.confirmedWhileRecording' | translate }}</div>
      <div class="ota-actions" *ngIf="v.ended">
        <button type="button" class="link muted" (click)="onDismiss()">{{ 'devices.ota.dismiss' | translate }}</button>
        <button type="button" class="link" *ngIf="v.retryable" (click)="tryAgain.emit()" [disabled]="busy || !canUpdate">
          {{ 'devices.ota.tryAgain' | translate }}
        </button>
      </div>
    </div>
  `,
  styles: [`
    :host { display: contents; }
    .offer { color: #e0e0e0; }
    .quiet { color: #8a90a6; }
    .ota { display: flex; flex-direction: column; gap: 0.35rem; font-size: 0.85rem; color: #ddd;
      padding: 0.6rem 0.7rem; border: 1px solid #2a2d45; border-radius: 6px; background: #13152a; }
    .ota.err { border-color: #f8717155; }
    .ota.err .ota-line { color: #fca5a5; }
    .ota.ok .ota-line { color: #86efac; }
    .ota-step { color: #9ab; font-size: 0.8rem; }
    .ota-line.indeterminate::before { content: ''; display: inline-block; width: 0.55rem; height: 0.55rem;
      margin-right: 0.45rem; border-radius: 50%; background: #9575cd; animation: pulse 1.2s ease-in-out infinite; }
    @keyframes pulse { 0%, 100% { opacity: 0.25; } 50% { opacity: 1; } }
    /* A fixed track; the fill is scaled, never resized, so a refresh moves it
       on the compositor without a layout pass. */
    .bar { height: 8px; width: 100%; max-width: 360px; border-radius: 4px; background: #2a2d45; overflow: hidden;
      margin: 0.2rem 0; }
    .fill { width: 100%; height: 100%; background: #7e57c2; transform-origin: left center;
      transition: transform 0.4s ease; will-change: transform; }
    @media (prefers-reduced-motion: reduce) {
      .ota-line.indeterminate::before { animation: none; opacity: 0.7; }
      .fill { transition: none; }
    }
    .ota-bytes { font-variant-numeric: tabular-nums; color: #bbb; }
    .ota-actions { display: flex; gap: 1rem; flex-wrap: wrap; }
    .hint { font-size: 0.75rem; color: #8a90a6; }
    .note-warn { display: flex; flex-wrap: wrap; align-items: baseline; gap: 0.25rem 0.9rem; margin-top: 0.25rem;
      padding: 0.35rem 0.55rem; border: 1px solid rgba(240, 180, 90, 0.3); border-radius: 6px; background: rgba(240, 180, 90, 0.05);
      color: #f3d9a8; font-size: 0.83rem; }
    .note-text { flex: 1 1 14rem; }
    .note-actions { display: inline-flex; gap: 0.5rem; }
    /* The same buttons as the card's own questions: Cancel left, the verb right. */
    .btn { padding: 0.38rem 0.85rem; border-radius: 5px; border: 1px solid #4a4e66; background: transparent;
      color: #cfd3e2; cursor: pointer; font-size: 0.82rem; line-height: 1.3; font-family: inherit; }
    .btn.primary { border-color: #9575cd; color: #c5b3ec; }
    .btn:disabled { opacity: 0.45; cursor: not-allowed; }
    .btn:focus-visible { outline: 2px solid #9575cd; outline-offset: 2px; }
    .link { background: none; border: none; padding: 0; margin-left: 0.25rem; color: #b39ddb; cursor: pointer;
      font: inherit; text-decoration: underline; text-underline-offset: 2px; }
    .link.muted { color: #9aa0b4; margin-left: 0; }
    .link.warn { color: #f0b45a; margin-left: 0; }
    .link:disabled { color: #666; cursor: not-allowed; text-decoration: none; }
    .link:focus-visible { outline: 2px solid #9575cd; outline-offset: 2px; }
  `],
})
export class OtaProgressComponent {
  /** offer: the Firmware row's line; progress: the job's slot. */
  @Input() mode: 'offer' | 'progress' = 'offer';
  /** The job from the server, or null. */
  @Input() job: OtaJob | null = null;
  /** The version on offer ('' when none). */
  @Input() available = '';
  /** Say "up to date" when nothing is on offer. */
  @Input() upToDate = false;
  /** Online and nothing in the way: the link may be pressed. */
  @Input() canUpdate = true;
  @Input() busy = false;
  /** The recording warning's reason while it is shown; null when not. */
  @Input() warning: string | null = null;
  /** Now, epoch seconds, from the page's refresh (for "updated N ago"). */
  @Input() now = 0;
  /** Shared across the page so a dismissed job stays dismissed. */
  @Input() dismissed: Dismissed | null = null;

  @Output() update = new EventEmitter<void>();
  @Output() updateAnyway = new EventEmitter<void>();
  @Output() notNow = new EventEmitter<void>();
  @Output() tryAgain = new EventEmitter<void>();

  constructor(private t: TranslateService) {}

  get v(): OtaView {
    return otaView(this.job);
  }

  get showJob(): boolean {
    if (!this.job) return false;
    return this.dismissed ? this.dismissed.shown(this.job) : true;
  }

  /** The bar's fill as a fraction of the track, 0 to 1. */
  fraction(): number {
    return barFraction(this.v);
  }

  failSentence(): string {
    return failKey(this.v.reason);
  }

  advice(): string {
    return adviceKey(this.v.reason);
  }

  size(n: number): string {
    const p = sizeParts(n);
    return this.t.instant(p.key, { n: p.n });
  }

  ago(): { key: string; n: number } {
    return agoParts(this.job?.updated_at ?? this.now, this.now);
  }

  onDismiss(): void {
    this.dismissed?.dismiss(this.job);
  }
}
