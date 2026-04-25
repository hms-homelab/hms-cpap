import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';

export interface OximetryRowData {
  spo2: number;
  heartRate: number;
  odi: number;
  active: boolean;
}

@Component({
  selector: 'app-oximetry-row',
  standalone: true,
  imports: [CommonModule],
  template: `
    <div class="section" *ngIf="data">
      <div class="section-header">
        <div class="section-title">O2 Ring Oximetry</div>
        <div class="section-subtitle">Wellue O2Ring -- SpO2 &amp; Heart Rate</div>
      </div>
      <div class="metrics-row">
        <div class="mu-card">
          <div class="mu-icon" [style.background]="spo2Color + '22'" [style.color]="spo2Color">
            <i class="fa-solid fa-droplet"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">SpO2</div>
            <div class="mu-value">{{ data.spo2.toFixed(1) }}%</div>
          </div>
        </div>

        <div class="mu-card">
          <div class="mu-icon" [style.background]="hrColor + '22'" [style.color]="hrColor">
            <i class="fa-solid fa-heart-pulse"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">Heart Rate</div>
            <div class="mu-value">{{ data.heartRate }} bpm</div>
          </div>
        </div>

        <div class="mu-card" *ngIf="data.odi">
          <div class="mu-icon" [style.background]="odiColor + '22'" [style.color]="odiColor">
            <i class="fa-solid fa-chart-line"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">ODI (3%)</div>
            <div class="mu-value">{{ data.odi.toFixed(1) }} /hr</div>
          </div>
        </div>

        <div class="mu-card">
          <div class="mu-icon" [style.background]="(data.active ? '#4ade80' : '#888') + '22'"
               [style.color]="data.active ? '#4ade80' : '#888'">
            <i class="fa-regular fa-circle-dot"></i>
          </div>
          <div class="mu-content">
            <div class="mu-primary">Ring Status</div>
            <div class="mu-value">{{ data.active ? 'Active' : 'Inactive' }}</div>
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
      padding: 0.85rem 1rem; min-width: 150px; flex: 1;
    }
    .mu-icon {
      width: 42px; height: 42px; border-radius: 50%;
      display: flex; align-items: center; justify-content: center; flex-shrink: 0;
      font-size: 1.1rem;
    }
    .mu-content { min-width: 0; }
    .mu-primary { color: #888; font-size: 0.8rem; }
    .mu-value { color: #e0e0e0; font-size: 1.2rem; font-weight: 700; }
    @media (max-width: 768px) { .metrics-row { flex-direction: column; } .mu-card { min-width: unset; } }
  `]
})
export class OximetryRowComponent {
  @Input() data: OximetryRowData | null = null;

  get spo2Color(): string {
    if (!this.data) return '#888';
    return this.data.spo2 >= 95 ? '#4ade80' : this.data.spo2 >= 90 ? '#fb923c' : '#ef4444';
  }
  get hrColor(): string {
    if (!this.data) return '#888';
    return this.data.heartRate > 0 && this.data.heartRate < 100 ? '#4ade80' : '#fb923c';
  }
  get odiColor(): string {
    if (!this.data) return '#888';
    return this.data.odi < 5 ? '#4ade80' : this.data.odi < 15 ? '#fb923c' : '#ef4444';
  }
}
