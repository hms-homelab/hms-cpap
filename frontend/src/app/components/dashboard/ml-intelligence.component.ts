import { Component, Input, Output, EventEmitter } from '@angular/core';
import { CommonModule } from '@angular/common';

@Component({
  selector: 'app-ml-intelligence',
  standalone: true,
  imports: [CommonModule],
  template: `
    <div class="section-card">
      <div class="section-header">
        <div class="section-title">ML Intelligence</div>
        <div class="section-subtitle">
          Predictions from {{ mlStatus?.models?.[0]?.samples_used || 0 }} therapy days
          - Last run {{ mlStatus?.last_trained || 'never' }}
        </div>
      </div>
      <div class="metrics-row">
        <div class="ml-card-vert">
          <div class="ml-icon" [style.color]="ahiPredColor"><i class="fa-solid fa-bed"></i></div>
          <div class="ml-label">Predicted AHI</div>
          <div class="ml-value">{{ mlPredictions?.predicted_ahi?.toFixed(2) || '--' }}</div>
          <div class="ml-unit">events/hr</div>
        </div>
        <div class="ml-card-vert">
          <div class="ml-icon" style="color: #60a5fa;"><i class="fa-solid fa-chart-line"></i></div>
          <div class="ml-label">AHI Trend</div>
          <div class="ml-value">Stable</div>
        </div>
        <div class="ml-card-vert">
          <div class="ml-icon" [style.color]="hoursPredColor"><i class="fa-regular fa-clock"></i></div>
          <div class="ml-label">Predicted Hours</div>
          <div class="ml-value">{{ mlPredictions?.predicted_hours?.toFixed(1) || '--' }}h</div>
          <div class="ml-unit">tonight</div>
        </div>
      </div>
      <div class="metrics-row" style="margin-top: 0.5rem;">
        <div class="ml-card-vert">
          <div class="ml-icon" [style.color]="maskFitColor"><i class="fa-solid fa-head-side-mask"></i></div>
          <div class="ml-label">Mask Fit Risk</div>
          <div class="ml-value">{{ mlPredictions?.leak_risk_pct?.toFixed(0) || '--' }}%</div>
        </div>
        <div class="ml-card-vert">
          <div class="ml-icon" [style.color]="anomalyColor">
            <i [class]="mlPredictions?.anomaly_class === 'NORMAL' ? 'fa-solid fa-circle-check' : 'fa-solid fa-circle-xmark'"></i>
          </div>
          <div class="ml-label">Anomaly Status</div>
          <div class="ml-value">{{ mlPredictions?.anomaly_class || '--' }}</div>
        </div>
      </div>
      <div class="ml-meta">
        <span>Last trained: {{ mlStatus?.last_trained || 'never' }}</span>
        <span>Models: {{ mlStatus?.models?.length || 0 }}</span>
        <span>Samples: {{ mlStatus?.models?.[0]?.samples_used || 0 }}</span>
        <button class="btn-inference" (click)="runInference.emit()" [disabled]="inferring">
          {{ inferring ? 'Running...' : 'Run Inference' }}
        </button>
      </div>
      <div class="model-details" *ngIf="showDetails">
        <div class="model-row" *ngFor="let m of mlStatus?.models || []">
          <span class="model-name">{{ m.name }}</span>
          <span class="model-metric">{{ m.name.includes('predictor') || m.name.includes('detector') ? 'Acc' : 'R2' }}: {{ m.primary_metric.toFixed(3) }}</span>
          <span class="model-metric">{{ m.name.includes('predictor') || m.name.includes('detector') ? 'F1' : 'MAE' }}: {{ m.secondary_metric.toFixed(3) }}</span>
        </div>
      </div>
      <button class="btn-details" (click)="showDetails = !showDetails">
        {{ showDetails ? 'Hide model details' : 'Show model details' }}
      </button>
    </div>
  `,
  styles: [`
    .section-card {
      background: #1e1e2f; border: 1px solid #333; border-radius: 12px;
      padding: 1.25rem; margin-bottom: 1.5rem;
    }
    .section-header { margin-bottom: 0.75rem; }
    .section-title { color: #ce93d8; font-size: 1rem; font-weight: 600; }
    .section-subtitle { color: #888; font-size: 0.8rem; }
    .metrics-row { display: flex; gap: 0.75rem; flex-wrap: wrap; }
    .ml-card-vert {
      flex: 1; min-width: 120px; background: #15152a; border: 1px solid #333;
      border-radius: 10px; padding: 0.75rem; text-align: center;
    }
    .ml-icon { font-size: 1.3rem; margin-bottom: 0.25rem; }
    .ml-label { color: #888; font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.04em; }
    .ml-value { color: #e0e0e0; font-size: 1.3rem; font-weight: 700; margin: 0.15rem 0; }
    .ml-unit { color: #666; font-size: 0.7rem; }
    .ml-meta {
      display: flex; align-items: center; gap: 1rem; flex-wrap: wrap;
      font-size: 0.8rem; color: #888; margin-top: 0.75rem;
    }
    .btn-inference {
      background: #7e57c2; color: #fff; border: none; border-radius: 4px;
      padding: 0.35rem 1rem; font-size: 0.8rem; font-weight: 600;
      cursor: pointer; margin-left: auto;
    }
    .btn-inference:hover { background: #9575cd; }
    .btn-inference:disabled { opacity: 0.5; cursor: not-allowed; }
    .btn-details {
      background: none; border: none; color: #64b5f6; font-size: 0.75rem;
      cursor: pointer; padding: 0; margin-top: 0.5rem;
    }
    .btn-details:hover { text-decoration: underline; }
    .model-details { margin-top: 0.5rem; }
    .model-row {
      display: flex; gap: 1rem; padding: 0.25rem 0; font-size: 0.8rem;
      border-bottom: 1px solid #2a2a3d;
    }
    .model-name { color: #90caf9; min-width: 160px; }
    .model-metric { color: #aaa; font-family: monospace; }
    @media (max-width: 768px) { .metrics-row { flex-direction: column; } }
  `]
})
export class MlIntelligenceComponent {
  @Input() mlStatus: any = null;
  @Input() mlPredictions: any = null;
  @Input() inferring = false;
  @Output() runInference = new EventEmitter<void>();
  showDetails = false;

  get ahiPredColor(): string {
    const ahi = this.mlPredictions?.predicted_ahi ?? 99;
    return ahi < 2 ? '#4ade80' : ahi < 5 ? '#fb923c' : '#ef4444';
  }
  get hoursPredColor(): string {
    const h = this.mlPredictions?.predicted_hours ?? 0;
    return h >= 6 ? '#4ade80' : h >= 4 ? '#fb923c' : '#ef4444';
  }
  get maskFitColor(): string {
    const r = this.mlPredictions?.leak_risk_pct ?? 0;
    return r < 30 ? '#4ade80' : r < 60 ? '#fb923c' : '#ef4444';
  }
  get anomalyColor(): string {
    return this.mlPredictions?.anomaly_class === 'NORMAL' ? '#4ade80' : '#ef4444';
  }
}
