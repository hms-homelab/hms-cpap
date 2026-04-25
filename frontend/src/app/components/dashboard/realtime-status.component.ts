import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';

export interface RealtimeStatusData {
  sessionStatus: string;
  sessionDuration: string;
  lastSessionTime: string;
  currentPressure: number;
  minPressure: number;
  maxPressure: number;
}

@Component({
  selector: 'app-realtime-status',
  standalone: true,
  imports: [CommonModule],
  template: `
    <div class="section" *ngIf="data">
      <div class="section-header">
        <div class="section-title">Real-Time Status</div>
        <div class="section-subtitle">Current Session Information</div>
      </div>
      <div class="metrics-row">
        <div class="rt-card">
          <div class="rt-icon" [style.color]="data.sessionStatus === 'active' ? '#4ade80' : data.sessionStatus === 'completed' ? '#60a5fa' : '#888'">
            <i class="fa-solid fa-circle-info"></i>
          </div>
          <div class="rt-content">
            <div class="rt-label">Session Status</div>
            <div class="rt-value">{{ data.sessionStatus | titlecase }}</div>
          </div>
        </div>
        <div class="rt-card" *ngIf="data.sessionDuration">
          <div class="rt-icon" style="color: #60a5fa;">
            <i class="fa-solid fa-hourglass-half"></i>
          </div>
          <div class="rt-content">
            <div class="rt-label">Session Duration</div>
            <div class="rt-value">{{ data.sessionDuration }}</div>
          </div>
        </div>
        <div class="rt-card" *ngIf="data.lastSessionTime">
          <div class="rt-icon" style="color: #22d3ee;">
            <i class="fa-regular fa-calendar"></i>
          </div>
          <div class="rt-content">
            <div class="rt-label">Last Session</div>
            <div class="rt-value">{{ data.lastSessionTime }}</div>
          </div>
        </div>
      </div>
      <div class="metrics-row" style="margin-top: 0.5rem;" *ngIf="data.currentPressure || data.minPressure || data.maxPressure">
        <div class="rt-card">
          <div class="rt-icon" style="color: #60a5fa;">
            <i class="fa-solid fa-arrow-down"></i>
          </div>
          <div class="rt-content">
            <div class="rt-label">Min Pressure</div>
            <div class="rt-value">{{ data.minPressure.toFixed(1) }} cmH2O</div>
          </div>
        </div>
        <div class="rt-card">
          <div class="rt-icon" style="color: #4ade80;">
            <i class="fa-solid fa-gauge"></i>
          </div>
          <div class="rt-content">
            <div class="rt-label">Current Pressure</div>
            <div class="rt-value">{{ data.currentPressure.toFixed(1) }} cmH2O</div>
          </div>
        </div>
        <div class="rt-card">
          <div class="rt-icon" style="color: #fb923c;">
            <i class="fa-solid fa-arrow-up"></i>
          </div>
          <div class="rt-content">
            <div class="rt-label">Max Pressure</div>
            <div class="rt-value">{{ data.maxPressure.toFixed(1) }} cmH2O</div>
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
    .rt-card {
      display: flex; align-items: center; gap: 0.6rem;
      background: #1e1e2f; border: 1px solid #333; border-radius: 10px;
      padding: 0.75rem 1rem; flex: 1; min-width: 150px;
    }
    .rt-icon { font-size: 1.1rem; width: 24px; text-align: center; }
    .rt-content { }
    .rt-label { color: #888; font-size: 0.75rem; }
    .rt-value { color: #e0e0e0; font-size: 1.1rem; font-weight: 600; }
    @media (max-width: 768px) { .metrics-row { flex-direction: column; } }
  `]
})
export class RealtimeStatusComponent {
  @Input() data: RealtimeStatusData | null = null;
}
