import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { TranslatePipe } from '@ngx-translate/core';
import { CpapApiService, UpdateStatus } from '../../services/cpap-api.service';
import { UpdateFlowService } from '../../services/update-flow.service';

const DISMISSED_KEY = 'hms-cpap.update.dismissed';

/**
 * SDD-041 §3.6: one line on the dashboard when a newer release exists, because
 * the dashboard is the page a user actually opens.
 *
 * Renders nothing at all unless the service says an update is AVAILABLE, which
 * already means newer, stable, and carrying a verified file for this platform.
 * A container never gets here: the service reports it containerised and never
 * offers anything (D5).
 *
 * Dismissing hides it for THAT version only. The next release shows again,
 * because a dismissal was about one version, not about updates in general.
 */
@Component({
  selector: 'app-update-banner',
  standalone: true,
  imports: [CommonModule, TranslatePipe],
  template: `
    <div class="update" *ngIf="status?.available && (!dismissed || flow.phase !== 'idle')"
         [class.warn]="flow.phase === 'failed' || flow.phase === 'busy'">
      <ng-container [ngSwitch]="flow.phase">
        <span class="text" *ngSwitchCase="'applying'">
          {{ ('updateBanner.step.' + (flow.step || 'downloading')) | translate:{ latest: status!.latest } }}
        </span>
        <span class="text" *ngSwitchCase="'restarting'">
          {{ 'updateBanner.restarting' | translate:{ latest: status!.latest } }}
        </span>
        <span class="text" *ngSwitchCase="'busy'">{{ 'updateBanner.busy' | translate }}</span>
        <span class="text" *ngSwitchCase="'failed'">
          {{ 'updateBanner.failed' | translate:{ error: flow.error } }}
        </span>
        <span class="text" *ngSwitchDefault>
          {{ 'updateBanner.available' | translate:{ latest: status!.latest, current: status!.current } }}
        </span>
      </ng-container>

      <!-- D1: the one click, only where this install can actually apply it. -->
      <button class="apply" type="button" *ngIf="status!.can_apply && flow.phase === 'idle'"
              (click)="flow.apply(status!)">{{ 'updateBanner.apply' | translate }}</button>
      <button class="apply" type="button" *ngIf="flow.phase === 'busy'"
              (click)="flow.apply(status!, true)">{{ 'updateBanner.applyNow' | translate }}</button>
      <a class="notes" *ngIf="status!.release_url && flow.phase !== 'restarting'" [href]="status!.release_url"
         target="_blank" rel="noopener">{{ 'updateBanner.whatsNew' | translate }}</a>
      <button class="dismiss" type="button" *ngIf="flow.phase === 'idle'" (click)="dismiss()"
              [attr.aria-label]="'updateBanner.dismiss' | translate">&times;</button>
    </div>
  `,
  styles: [`
    .update { display: flex; align-items: center; gap: 0.75rem; flex-wrap: wrap;
              background: rgba(100, 181, 246, 0.08); border: 1px solid rgba(100, 181, 246, 0.35);
              border-radius: 8px; padding: 0.6rem 1rem; margin-bottom: 1rem; }
    .text { color: #cfe6fb; font-size: 0.85rem; flex: 1 1 auto; min-width: 0; }
    .update.warn { background: rgba(251, 191, 36, 0.08); border-color: rgba(251, 191, 36, 0.4); }
    .update.warn .text { color: #fde68a; }
    .notes { color: #64b5f6; font-size: 0.8rem; white-space: nowrap; }
    .apply { background: #64b5f6; color: #0d1b2a; border: none; border-radius: 6px;
             padding: 0.35rem 0.8rem; font-size: 0.8rem; font-weight: 600; cursor: pointer;
             white-space: nowrap; }
    .apply:hover { background: #90caf9; }
    .dismiss { background: none; border: none; color: #888; font-size: 1.1rem; line-height: 1;
               cursor: pointer; padding: 0 0.25rem; }
    .dismiss:hover { color: #e0e0e0; }
  `]
})
export class UpdateBannerComponent implements OnInit {
  status: UpdateStatus | null = null;
  dismissed = false;

  constructor(private api: CpapApiService, public flow: UpdateFlowService) {}

  ngOnInit() {
    this.api.getUpdateStatus().subscribe({
      next: s => {
        this.status = s;
        this.dismissed = !!s.latest && readDismissed() === s.latest;
      },
      // No updater (an old backend, a failed call): no banner, and no error
      // either, since nothing about the user's data depends on it.
      error: () => { this.status = null; },
    });
  }

  dismiss() {
    this.dismissed = true;
    if (this.status?.latest) writeDismissed(this.status.latest);
  }
}

function readDismissed(): string | null {
  try { return localStorage.getItem(DISMISSED_KEY); } catch { return null; }
}

function writeDismissed(version: string) {
  try { localStorage.setItem(DISMISSED_KEY, version); } catch { /* private window: once per load */ }
}
