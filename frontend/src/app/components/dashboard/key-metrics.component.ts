import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';
import { TranslatePipe, TranslateService } from '@ngx-translate/core';
import { formatIndex } from '../../utils/format';

export interface KeyMetricsData {
  ahi: number;
  usageHours: number;
  leakP95: number;
  totalEvents: number;
  compliancePct: number;
  sessionActive: boolean;
  mode: string;
}

@Component({
  selector: 'app-key-metrics',
  standalone: true,
  imports: [CommonModule, TranslatePipe],
  template: `
    <div class="section">
      <div class="section-header">
        <div class="section-title">{{ 'dashboard.keyMetrics.title' | translate }}</div>
        <div class="section-subtitle">{{ 'dashboard.keyMetrics.subtitle' | translate }}</div>
      </div>
      <div class="metrics-row">
        <div class="mu-card" *ngIf="data">
          <div class="mu-icon" [style.background]="ahiColor + '22'" [style.color]="ahiColor">
            <i class="fa-solid fa-heart-pulse"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">{{ 'dashboard.keyMetrics.ahiScore' | translate }}</div>
            <div class="mu-secondary">
              <span class="mu-value">{{ fmtIndex(data.ahi) }}</span>
              <span class="mu-assess">{{ ahiLabel }}</span>
            </div>
          </div>
        </div>

        <div class="mu-card" *ngIf="data">
          <div class="mu-icon" [style.background]="usageColor + '22'" [style.color]="usageColor">
            <i class="fa-solid fa-clock-rotate-left"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">{{ 'dashboard.keyMetrics.usage' | translate }}</div>
            <div class="mu-secondary">
              <span class="mu-value">{{ fmtDuration(data.usageHours) }}</span>
              <span class="mu-assess" *ngIf="data.usageHours >= 4">{{ 'dashboard.keyMetrics.targetMet' | translate }}</span>
            </div>
          </div>
        </div>

        <div class="mu-card" *ngIf="data">
          <div class="mu-icon" [style.background]="leakColor + '22'" [style.color]="leakColor">
            <i class="fa-solid fa-wind"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">{{ 'dashboard.keyMetrics.maskLeak' | translate }}</div>
            <div class="mu-secondary">
              <span class="mu-value">{{ data.leakP95.toFixed(1) }} L/min</span>
              <span class="mu-assess">{{ leakLabel }}</span>
            </div>
          </div>
        </div>

        <div class="mu-card" *ngIf="data">
          <div class="mu-icon" [style.background]="eventsColor + '22'" [style.color]="eventsColor">
            <i class="fa-solid fa-triangle-exclamation"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">{{ 'dashboard.keyMetrics.totalEvents' | translate }}</div>
            <div class="mu-secondary">
              <span class="mu-value">{{ data.totalEvents }}</span>
              <!-- "events/hr" is copy, not a unit, so it comes from the
                   dictionary with the value interpolated. The sibling frontend
                   keys this identically. -->
              <span class="mu-assess">{{ 'dashboard.keyMetrics.eventsPerHr' | translate:{ v: fmtIndex(data.ahi) } }}</span>
            </div>
          </div>
        </div>

        <div class="mu-card" *ngIf="data">
          <div class="mu-icon" [style.background]="sessionColor + '22'" [style.color]="sessionColor">
            <i class="fa-solid fa-moon"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">{{ 'dashboard.keyMetrics.session' | translate }}</div>
            <div class="mu-secondary">
              <span class="mu-value">{{ (data.sessionActive ? 'dashboard.keyMetrics.running' : 'dashboard.keyMetrics.completed') | translate }}</span>
            </div>
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
    .mu-card {
      display: flex; align-items: center; gap: 0.75rem;
      background: #1e1e2f; border: 1px solid #333; border-radius: 12px;
      padding: 0.85rem 1rem; min-width: 170px; flex: 1;
    }
    .mu-icon {
      width: 42px; height: 42px; border-radius: 50%;
      display: flex; align-items: center; justify-content: center; flex-shrink: 0;
      font-size: 1.1rem;
    }
    .mu-content { min-width: 0; }
    .mu-primary { color: #e0e0e0; font-size: 0.85rem; font-weight: 500; }
    .mu-secondary { display: flex; flex-direction: column; }
    .mu-value { color: #e0e0e0; font-size: 1.3rem; font-weight: 700; }
    .mu-assess { color: #888; font-size: 0.7rem; }
    @media (max-width: 768px) {
      .metrics-row { flex-direction: column; }
      .mu-card { min-width: unset; }
    }
  `]
})
export class KeyMetricsComponent {
  @Input() data: KeyMetricsData | null = null;

  constructor(private t: TranslateService) {}

  /** SDD-079: index group renders at two decimals. */
  readonly fmtIndex = formatIndex;

  get ahiColor(): string {
    if (!this.data) return '#888';
    return this.data.ahi < 5 ? '#4ade80' : this.data.ahi < 15 ? '#fb923c' : '#ef4444';
  }
  // SDD-080: the band thresholds are clinical and stay here; only the words
  // move to the catalog. Note "Severe" is "grave" in es/pt and NOT "severo",
  // which is a false friend meaning strict rather than clinically severe.
  get ahiLabel(): string {
    if (!this.data) return '';
    if (this.data.ahi < 5) return this.t.instant('dashboard.keyMetrics.ahiExcellent');
    if (this.data.ahi < 15) return this.t.instant('dashboard.keyMetrics.ahiMild');
    if (this.data.ahi < 30) return this.t.instant('dashboard.keyMetrics.ahiModerate');
    return this.t.instant('dashboard.keyMetrics.ahiSevere');
  }
  get usageColor(): string {
    if (!this.data) return '#888';
    return this.data.usageHours >= 4 ? '#4ade80' : this.data.usageHours >= 2 ? '#fb923c' : '#ef4444';
  }
  get leakColor(): string {
    if (!this.data) return '#888';
    return this.data.leakP95 < 10 ? '#4ade80' : this.data.leakP95 < 24 ? '#fb923c' : '#ef4444';
  }
  get leakLabel(): string {
    if (!this.data) return '';
    if (this.data.leakP95 < 10) return this.t.instant('dashboard.keyMetrics.leakExcellent');
    if (this.data.leakP95 < 24) return this.t.instant('dashboard.keyMetrics.leakAcceptable');
    return this.t.instant('dashboard.keyMetrics.leakHigh');
  }
  get eventsColor(): string {
    if (!this.data) return '#888';
    return this.data.totalEvents < 10 ? '#4ade80' : this.data.totalEvents < 30 ? '#fb923c' : '#ef4444';
  }
  get sessionColor(): string {
    if (!this.data) return '#888';
    return this.data.sessionActive ? '#4ade80' : '#888';
  }
  fmtDuration(hours: number): string {
    if (hours <= 0) return '0m';
    const totalMins = Math.round(hours * 60);
    const h = Math.floor(totalMins / 60);
    const m = totalMins % 60;
    return h > 0 ? `${h}h ${String(m).padStart(2, '0')}m` : `${m}m`;
  }
}
