import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';
import { TranslatePipe } from '@ngx-translate/core';

export interface EventsBreakdownData {
  obstructive: number;
  central: number;
  hypopneas: number;
  reras: number;
  totalEvents: number;
  maxEventDuration: number;
  avgEventDuration: number;

  /**
   * SDD-024. False when the machine counts events but does not say what they
   * were, which is the Sefam S.Box: it flags apneas and never marks a hypopnea
   * or splits obstructive from central.
   *
   * The four cards below cannot be drawn for such a night. Drawing them from
   * the zeros the machine left is not a neutral fallback, it is an assertion:
   * "Hypopneas 0", painted GREEN by the same rule that rewards a real zero,
   * tells the user their night was clean on the one axis nobody measured.
   */
  classified: boolean;
}

@Component({
  selector: 'app-events-breakdown',
  standalone: true,
  imports: [CommonModule, TranslatePipe],
  template: `
    <div class="section" *ngIf="data">
      <div class="section-header">
        <div class="section-title"><i class="fa-solid fa-chart-pie"></i> {{ 'dashboard.title.events' | translate }}</div>
        <div class="section-subtitle">{{ (data.classified ? 'dashboard.subtitle.events' : 'dashboard.subtitle.eventsUnclassified') | translate }}</div>
      </div>

      <!-- Machines that count apneas without classifying them. One honest card
           and the reason, instead of four cards of zeros. -->
      <div class="metrics-row" *ngIf="!data.classified">
        <div class="evt-card">
          <div class="evt-icon" style="color: #60a5fa;">
            <i class="fa-solid fa-ban"></i>
          </div>
          <div class="evt-content">
            <div class="evt-label">{{ 'dashboard.events.apneas' | translate }}</div>
            <div class="evt-value">{{ data.totalEvents }}</div>
          </div>
        </div>
      </div>
      <div class="evt-note" *ngIf="!data.classified">{{ 'dashboard.events.unclassifiedNote' | translate }}</div>

      <div class="metrics-row" *ngIf="data.classified">
        <div class="evt-card">
          <div class="evt-icon" [style.color]="data.obstructive === 0 ? '#4ade80' : data.obstructive < 10 ? '#fb923c' : '#ef4444'">
            <i class="fa-solid fa-ban"></i>
          </div>
          <div class="evt-content">
            <div class="evt-label">{{ 'dashboard.events.obstructive' | translate }}</div>
            <div class="evt-value">{{ data.obstructive }}</div>
          </div>
        </div>
        <div class="evt-card">
          <div class="evt-icon" [style.color]="data.central === 0 ? '#4ade80' : data.central < 5 ? '#fb923c' : '#ef4444'">
            <i class="fa-solid fa-circle-exclamation"></i>
          </div>
          <div class="evt-content">
            <div class="evt-label">{{ 'dashboard.events.central' | translate }}</div>
            <div class="evt-value">{{ data.central }}</div>
          </div>
        </div>
        <div class="evt-card">
          <div class="evt-icon" [style.color]="data.hypopneas === 0 ? '#4ade80' : data.hypopneas < 10 ? '#fb923c' : '#ef4444'">
            <i class="fa-solid fa-gauge-simple"></i>
          </div>
          <div class="evt-content">
            <div class="evt-label">{{ 'dashboard.events.hypopneas' | translate }}</div>
            <div class="evt-value">{{ data.hypopneas }}</div>
          </div>
        </div>
      </div>
      <div class="metrics-row" style="margin-top: 0.5rem;" *ngIf="data.classified">
        <div class="evt-card">
          <div class="evt-icon" style="color: #60a5fa;">
            <i class="fa-solid fa-water"></i>
          </div>
          <div class="evt-content">
            <div class="evt-label">{{ 'dashboard.events.reras' | translate }}</div>
            <div class="evt-value">{{ data.reras }}</div>
            <div class="evt-sub">Respiratory Effort</div>
          </div>
        </div>
        <div class="evt-card" *ngIf="data.maxEventDuration">
          <div class="evt-icon" [style.color]="data.maxEventDuration < 10 ? '#4ade80' : data.maxEventDuration < 20 ? '#fb923c' : '#ef4444'">
            <i class="fa-solid fa-stopwatch"></i>
          </div>
          <div class="evt-content">
            <div class="evt-label">{{ 'dashboard.events.maxEvent' | translate }}</div>
            <div class="evt-value">{{ data.maxEventDuration.toFixed(1) }}s</div>
          </div>
        </div>
        <div class="evt-card" *ngIf="data.avgEventDuration">
          <div class="evt-icon" style="color: #60a5fa;">
            <i class="fa-regular fa-stopwatch"></i>
          </div>
          <div class="evt-content">
            <div class="evt-label">{{ 'dashboard.events.avgEvent' | translate }}</div>
            <div class="evt-value">{{ data.avgEventDuration.toFixed(1) }}s</div>
          </div>
        </div>
      </div>
    </div>
  `,
  styles: [`
    .section { margin-bottom: 1.5rem; }
    .section-header { margin-bottom: 0.75rem; }
    .section-title { color: #e0e0e0; font-size: 1rem; font-weight: 600; }
    .section-subtitle { color: #888; font-size: 0.8rem; }
    .metrics-row { display: flex; gap: 0.75rem; flex-wrap: wrap; }
    .evt-card {
      display: flex; align-items: center; gap: 0.6rem;
      background: #1e1e2f; border: 1px solid #333; border-radius: 10px;
      padding: 0.75rem 1rem; flex: 1; min-width: 140px;
    }
    .evt-icon { font-size: 1.1rem; width: 24px; text-align: center; }
    .evt-content { }
    .evt-label { color: #888; font-size: 0.8rem; }
    .evt-value { color: #e0e0e0; font-size: 1.2rem; font-weight: 700; }
    .evt-sub { color: #666; font-size: 0.7rem; }
    .evt-note {
      color: #888; font-size: 0.78rem; line-height: 1.45;
      margin-top: 0.6rem; max-width: 60ch;
    }
    @media (max-width: 768px) { .metrics-row { flex-direction: column; } }
  `]
})
export class EventsBreakdownComponent {
  @Input() data: EventsBreakdownData | null = null;
}
