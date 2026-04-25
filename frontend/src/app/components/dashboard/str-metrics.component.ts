import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';

export interface StrMetricsData {
  ahi: number;
  usageHours: number;
  leakP95: number;
  oai: number;
  cai: number;
  hi: number;
  rin: number;
}

@Component({
  selector: 'app-str-metrics',
  standalone: true,
  imports: [CommonModule],
  template: `
    <div class="section" *ngIf="data">
      <div class="section-header">
        <div class="section-title">STR Daily Metrics</div>
        <div class="section-subtitle">Official ResMed indices from STR.edf</div>
      </div>
      <div class="metrics-row">
        <div class="mu-card-vert">
          <div class="mu-icon" [style.background]="ahiColor + '22'" [style.color]="ahiColor">
            <i class="fa-solid fa-heart-pulse"></i>
          </div>
          <div class="mu-label">STR AHI</div>
          <div class="mu-value">{{ data.ahi.toFixed(1) }}</div>
        </div>
        <div class="mu-card-vert">
          <div class="mu-icon" [style.background]="usageColor + '22'" [style.color]="usageColor">
            <i class="fa-regular fa-clock"></i>
          </div>
          <div class="mu-label">STR Usage</div>
          <div class="mu-value">{{ data.usageHours.toFixed(1) }}h</div>
        </div>
        <div class="mu-card-vert">
          <div class="mu-icon" [style.background]="leakColor + '22'" [style.color]="leakColor">
            <i class="fa-solid fa-droplet-slash"></i>
          </div>
          <div class="mu-label">STR Leak P95</div>
          <div class="mu-value">{{ data.leakP95.toFixed(1) }} L/min</div>
        </div>
      </div>
      <div class="metrics-row" style="margin-top: 0.5rem;">
        <div class="idx-card">
          <i class="fa-solid fa-octagon-xmark idx-icon" style="color: #f87171;"></i>
          <div class="idx-label">OAI</div>
          <div class="idx-value">{{ data.oai.toFixed(1) }}</div>
        </div>
        <div class="idx-card">
          <i class="fa-solid fa-circle-exclamation idx-icon" style="color: #fb923c;"></i>
          <div class="idx-label">CAI</div>
          <div class="idx-value">{{ data.cai.toFixed(1) }}</div>
        </div>
        <div class="idx-card">
          <i class="fa-solid fa-gauge idx-icon" style="color: #fbbf24;"></i>
          <div class="idx-label">HI</div>
          <div class="idx-value">{{ data.hi.toFixed(1) }}</div>
        </div>
        <div class="idx-card">
          <i class="fa-solid fa-water idx-icon" style="color: #60a5fa;"></i>
          <div class="idx-label">RERA</div>
          <div class="idx-value">{{ data.rin.toFixed(1) }}</div>
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
    .mu-card-vert {
      display: flex; flex-direction: column; align-items: center;
      background: #1e1e2f; border: 1px solid #333; border-radius: 12px;
      padding: 0.85rem 1rem; min-width: 120px; flex: 1; text-align: center;
    }
    .mu-icon {
      width: 42px; height: 42px; border-radius: 50%;
      display: flex; align-items: center; justify-content: center; margin-bottom: 0.4rem;
      font-size: 1.1rem;
    }
    .mu-label { color: #888; font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.04em; }
    .mu-value { color: #e0e0e0; font-size: 1.3rem; font-weight: 700; }
    .idx-card {
      display: flex; align-items: center; gap: 0.5rem;
      background: #1e1e2f; border: 1px solid #333; border-radius: 10px;
      padding: 0.6rem 0.85rem; flex: 1; min-width: 100px;
    }
    .idx-icon { font-size: 1rem; width: 20px; text-align: center; }
    .idx-label { color: #888; font-size: 0.8rem; flex: 1; }
    .idx-value { color: #e0e0e0; font-size: 1.1rem; font-weight: 600; }
    @media (max-width: 768px) { .metrics-row { flex-direction: column; } }
  `]
})
export class StrMetricsComponent {
  @Input() data: StrMetricsData | null = null;

  get ahiColor(): string {
    if (!this.data) return '#888';
    return this.data.ahi < 5 ? '#4ade80' : this.data.ahi < 15 ? '#fb923c' : '#ef4444';
  }
  get usageColor(): string {
    if (!this.data) return '#888';
    return this.data.usageHours >= 4 ? '#4ade80' : this.data.usageHours >= 2 ? '#fb923c' : '#ef4444';
  }
  get leakColor(): string {
    if (!this.data) return '#888';
    return this.data.leakP95 < 10 ? '#4ade80' : this.data.leakP95 < 24 ? '#fb923c' : '#ef4444';
  }
}
