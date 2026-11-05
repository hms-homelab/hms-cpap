import { Component, EventEmitter, Input, Output } from '@angular/core';
import { CommonModule } from '@angular/common';
import { TranslatePipe, TranslateService } from '@ngx-translate/core';

import { Dismissed, OtaJob, OtaView, adviceKey, agoParts, barFraction, failKey, otaView, sizeParts } from './ota-progress-model';

/**
 * Where a firmware update is: the same states and words
 * for every device. Input: the job the server last reported, the version the
 * unit runs and the one on offer. No timer of its own: it renders the last
 * read, and the page's refresh brings the next.
 *
 * It sits on a card's Firmware line. With no job running it is the Update
 * button (or "up to date"); while a job runs the button is gone and the job's
 * phase shows; a finished job stays until dismissed.
 */
@Component({
  selector: 'app-ota-progress',
  standalone: true,
  imports: [CommonModule, TranslatePipe],
  template: `
    <div class="ota" [class.err]="v.state === 'failed'" [class.ok]="v.state === 'done'">
      <!-- The recording warning: in the page, never a dialog. -->
      <div class="restart-banner" *ngIf="warning !== null">
        <span class="restart-text">
          {{ 'devices.ota.warnRecording' | translate }}
          <span class="why" *ngIf="warning">{{ 'devices.confirm.reason' | translate:{ reason: warning } }}</span>
        </span>
        <div class="restart-actions">
          <button type="button" class="myair-btn danger" (click)="updateAnyway.emit()" [disabled]="busy">
            {{ 'devices.ota.anyway' | translate }}
          </button>
          <button type="button" class="myair-btn secondary" (click)="notNow.emit()">
            {{ 'devices.ota.notNow' | translate }}
          </button>
        </div>
      </div>

      <ng-container *ngIf="warning === null">
        <!-- No job shown: the offer. -->
        <!-- One line, no big button: "Firmware X is available. Update now". -->
        <ng-container *ngIf="!showJob">
          <div class="offer" *ngIf="available">
            {{ 'devices.ota.available' | translate:{ target: available } }}
            <button type="button" class="link" (click)="update.emit()" [disabled]="busy || !canUpdate">
              {{ 'devices.ota.updateNow' | translate }}
            </button>
          </div>
          <span class="hint inline" *ngIf="!available && upToDate">{{ 'devices.ota.upToDate' | translate }}</span>
        </ng-container>

        <!-- A job. -->
        <ng-container *ngIf="showJob">
          <div class="ota-step" *ngIf="v.step && !v.ended">
            {{ 'devices.ota.step' | translate:{ n: v.step.n, m: v.step.m, target: v.step.target } }}
          </div>
          <ng-container [ngSwitch]="v.state">
            <div *ngSwitchCase="'held'" class="ota-line">{{ 'devices.ota.held' | translate:{ reason: job?.hold_reason || '' } }}</div>
            <div *ngSwitchCase="'queued'" class="ota-line">{{ 'devices.ota.queued' | translate }}</div>
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
            <button type="button" class="myair-btn" *ngIf="v.retryable" (click)="tryAgain.emit()" [disabled]="busy || !canUpdate">
              {{ 'devices.ota.tryAgain' | translate }}
            </button>
            <button type="button" class="myair-btn secondary" (click)="onDismiss()">
              {{ 'devices.ota.dismiss' | translate }}
            </button>
          </div>
        </ng-container>
      </ng-container>
    </div>
  `,
  styles: [`
    .ota { display: flex; flex-direction: column; gap: 0.35rem; font-size: 0.85rem; color: #ddd; }
    .ota.err .ota-line { color: #f87171; }
    .ota.ok .ota-line { color: #4ade80; }
    .ota-step { color: #9ab; font-size: 0.8rem; }
    .ota-line.indeterminate::before { content: ''; display: inline-block; width: 0.6rem; height: 0.6rem;
      margin-right: 0.45rem; border-radius: 50%; background: #64b5f6; animation: pulse 1.2s ease-in-out infinite; }
    @keyframes pulse { 0%, 100% { opacity: 0.25; } 50% { opacity: 1; } }
    /* A fixed track; the fill is scaled, never resized, so a refresh moves it
       on the compositor without a layout pass. */
    .bar { height: 8px; width: 100%; max-width: 360px; border-radius: 4px; background: #2a2d45; overflow: hidden;
      margin: 0.2rem 0; }
    .fill { width: 100%; height: 100%; background: #1976d2; transform-origin: left center;
      transition: transform 0.4s ease; will-change: transform; }
    @media (prefers-reduced-motion: reduce) {
      .ota-line.indeterminate::before { animation: none; opacity: 0.7; }
      .fill { transition: none; }
    }
    .ota-bytes { font-variant-numeric: tabular-nums; color: #bbb; }
    .ota-actions { display: flex; gap: 0.6rem; flex-wrap: wrap; }
    .offer { color: #fbbf24; }
    .link { background: none; border: none; padding: 0; margin-left: 0.25rem; color: #64b5f6; cursor: pointer;
      font: inherit; text-decoration: underline; text-underline-offset: 2px; }
    .link:disabled { color: #666; cursor: not-allowed; text-decoration: none; }
    .link:focus-visible { outline: 2px solid #64b5f6; outline-offset: 2px; }
    .why { display: block; color: #aaa; font-size: 0.8rem; margin-top: 0.25rem; }
    .hint { font-size: 0.75rem; color: #888; }
    .hint.inline { display: inline; }
    .myair-btn { padding: 0.4rem 0.9rem; border-radius: 5px; border: 1px solid #1976d2; background: #1976d2;
      color: #fff; cursor: pointer; font-size: 0.82rem; }
    .myair-btn:disabled { opacity: 0.5; cursor: not-allowed; }
    .myair-btn.danger { background: transparent; border-color: #f87171; color: #f87171; }
    .myair-btn.secondary { background: transparent; border-color: #555; color: #aaa; }
    .restart-banner { display: flex; flex-wrap: wrap; align-items: center; gap: 0.6rem; padding: 0.6rem 0.75rem;
      border: 1px solid #ffb74d55; border-radius: 6px; background: #2a2416; color: #f5e6c8; }
    .restart-text { flex: 1; min-width: 220px; line-height: 1.45; }
    .restart-actions { display: flex; gap: 0.5rem; flex-wrap: wrap; }
  `],
})
export class OtaProgressComponent {
  /** The job from the server, or null. */
  @Input() job: OtaJob | null = null;
  /** The version on offer ('' when none): the Update button's target. */
  @Input() available = '';
  /** Show "up to date" when nothing is on offer. */
  @Input() upToDate = false;
  /** Online and nothing in the way: the button may be pressed. */
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

  constructor(private t: TranslateService) {}

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
