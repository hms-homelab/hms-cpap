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
}

@Component({
  selector: 'app-events-breakdown',
  standalone: true,
  imports: [CommonModule, TranslatePipe],
  template: `
    <div class="section" *ngIf="data">
      <div class="section-header">
        <div class="section-title"><i class="fa-solid fa-chart-pie"></i> {{ 'dashboard.title.events' | translate }}</div>
        <div class="section-subtitle">{{ 'dashboard.subtitle.events' | translate }}</div>
      </div>
      <div class="metrics-row">
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
      <div class="metrics-row" style="margin-top: 0.5rem;">
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
    @media (max-width: 768px) { .metrics-row { flex-direction: column; } }
  `]
})
export class EventsBreakdownComponent {
  @Input() data: EventsBreakdownData | null = null;
}
