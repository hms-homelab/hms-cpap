import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';

export interface RespiratoryMetricsData {
  respRate: number;
  tidalVolume: number;
  minuteVent: number;
  inspiratoryTime: number;
  expiratoryTime: number;
  ieRatio: number;
  flowLimitation: number;
  avgFlowRate: number;
  currentFlowRate: number;
}

@Component({
  selector: 'app-respiratory-metrics',
  standalone: true,
  imports: [CommonModule],
  template: `
    <div class="section" *ngIf="data">
      <div class="section-header">
        <div class="section-title">Respiratory Metrics</div>
        <div class="section-subtitle">Breathing Patterns &amp; Ventilation</div>
      </div>
      <div class="metrics-row">
        <div class="resp-card">
          <div class="resp-icon" [style.color]="rrColor">
            <i class="fa-solid fa-lungs"></i>
          </div>
          <div class="resp-content">
            <div class="resp-label">Respiratory Rate</div>
            <div class="resp-value">{{ data.respRate.toFixed(1) }} br/min</div>
            <div class="resp-assess">{{ data.respRate >= 12 && data.respRate <= 20 ? 'Normal' : 'Check' }}</div>
          </div>
        </div>
        <div class="resp-card">
          <div class="resp-icon" style="color: #60a5fa;">
            <i class="fa-solid fa-wind"></i>
          </div>
          <div class="resp-content">
            <div class="resp-label">Tidal Volume</div>
            <div class="resp-value">{{ data.tidalVolume.toFixed(0) }} mL</div>
          </div>
        </div>
        <div class="resp-card">
          <div class="resp-icon" style="color: #22d3ee;">
            <i class="fa-solid fa-chart-line"></i>
          </div>
          <div class="resp-content">
            <div class="resp-label">Minute Ventilation</div>
            <div class="resp-value">{{ data.minuteVent.toFixed(1) }} L/min</div>
          </div>
        </div>
      </div>
      <div class="metrics-row" style="margin-top: 0.5rem;" *ngIf="data.flowLimitation || data.avgFlowRate">
        <div class="resp-card" *ngIf="data.flowLimitation">
          <div class="resp-icon" [style.color]="flColor">
            <i class="fa-solid fa-road-barrier"></i>
          </div>
          <div class="resp-content">
            <div class="resp-label">Flow Limitation</div>
            <div class="resp-value">{{ data.flowLimitation.toFixed(2) }}</div>
          </div>
        </div>
        <div class="resp-card" *ngIf="data.avgFlowRate">
          <div class="resp-icon" style="color: #60a5fa;">
            <i class="fa-solid fa-gauge-high"></i>
          </div>
          <div class="resp-content">
            <div class="resp-label">Avg Flow Rate</div>
            <div class="resp-value">{{ data.avgFlowRate.toFixed(1) }} L/min</div>
          </div>
        </div>
        <div class="resp-card" *ngIf="data.currentFlowRate">
          <div class="resp-icon" style="color: #22d3ee;">
            <i class="fa-solid fa-gauge"></i>
          </div>
          <div class="resp-content">
            <div class="resp-label">Current Flow</div>
            <div class="resp-value">{{ data.currentFlowRate.toFixed(1) }} L/min</div>
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
    .resp-card {
      display: flex; align-items: center; gap: 0.6rem;
      background: #1e1e2f; border: 1px solid #333; border-radius: 10px;
      padding: 0.75rem 1rem; flex: 1; min-width: 150px;
    }
    .resp-icon { font-size: 1.1rem; width: 24px; text-align: center; }
    .resp-content { }
    .resp-label { color: #888; font-size: 0.75rem; }
    .resp-value { color: #e0e0e0; font-size: 1.1rem; font-weight: 600; }
    .resp-assess { color: #888; font-size: 0.7rem; }
    @media (max-width: 768px) { .metrics-row { flex-direction: column; } }
  `]
})
export class RespiratoryMetricsComponent {
  @Input() data: RespiratoryMetricsData | null = null;

  get rrColor(): string {
    if (!this.data) return '#888';
    return (this.data.respRate >= 12 && this.data.respRate <= 20) ? '#4ade80' :
           (this.data.respRate >= 10 && this.data.respRate <= 25) ? '#fb923c' : '#ef4444';
  }
  get flColor(): string {
    if (!this.data) return '#888';
    return this.data.flowLimitation < 0.3 ? '#4ade80' : this.data.flowLimitation < 0.5 ? '#fb923c' : '#ef4444';
  }
}
