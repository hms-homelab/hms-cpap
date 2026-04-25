import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';

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
  imports: [CommonModule],
  template: `
    <div class="section">
      <div class="section-header">
        <div class="section-title">Key Metrics</div>
        <div class="section-subtitle">Last Session Performance</div>
      </div>
      <div class="metrics-row">
        <div class="mu-card" *ngIf="data">
          <div class="mu-icon" [style.background]="ahiColor + '22'" [style.color]="ahiColor">
            <i class="fa-solid fa-heart-pulse"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">AHI Score</div>
            <div class="mu-secondary">
              <span class="mu-value">{{ data.ahi.toFixed(1) }}</span>
              <span class="mu-assess">{{ ahiLabel }}</span>
            </div>
          </div>
        </div>

        <div class="mu-card" *ngIf="data">
          <div class="mu-icon" [style.background]="usageColor + '22'" [style.color]="usageColor">
            <i class="fa-solid fa-clock-rotate-left"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">Usage</div>
            <div class="mu-secondary">
              <span class="mu-value">{{ fmtDuration(data.usageHours) }}</span>
              <span class="mu-assess" *ngIf="data.usageHours >= 4">4h+ target met</span>
            </div>
          </div>
        </div>

        <div class="mu-card" *ngIf="data">
          <div class="mu-icon" [style.background]="leakColor + '22'" [style.color]="leakColor">
            <i class="fa-solid fa-wind"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">Mask Leak</div>
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
            <div class="mu-primary">Total Events</div>
            <div class="mu-secondary">
              <span class="mu-value">{{ data.totalEvents }}</span>
              <span class="mu-assess">{{ data.ahi.toFixed(1) }} events/hr</span>
            </div>
          </div>
        </div>

        <div class="mu-card" *ngIf="data">
          <div class="mu-icon" [style.background]="sessionColor + '22'" [style.color]="sessionColor">
            <i class="fa-solid fa-moon"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">Session</div>
            <div class="mu-secondary">
              <span class="mu-value">{{ data.sessionActive ? 'Running' : 'Completed' }}</span>
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

  get ahiColor(): string {
    if (!this.data) return '#888';
    return this.data.ahi < 5 ? '#4ade80' : this.data.ahi < 15 ? '#fb923c' : '#ef4444';
  }
  get ahiLabel(): string {
    if (!this.data) return '';
    if (this.data.ahi < 5) return 'Excellent';
    if (this.data.ahi < 15) return 'Mild Apnea';
    if (this.data.ahi < 30) return 'Moderate';
    return 'Severe';
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
    if (this.data.leakP95 < 10) return 'Excellent';
    if (this.data.leakP95 < 24) return 'Acceptable';
    return 'High Leak';
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
