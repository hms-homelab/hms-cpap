import { Component, Input, ViewChild, ElementRef, AfterViewInit, OnChanges } from '@angular/core';
import { CommonModule } from '@angular/common';
import { TranslatePipe } from '@ngx-translate/core';
import Chart from 'chart.js/auto';
import { PressureSource } from './pressure-source';

export interface PressureSectionData {
  /** Issue #56: 'therapy' is what the machine delivered; 'mask' is measured at
   *  the mask, shown only for a night without the therapy pressure. */
  source: PressureSource;
  p50Pressure: number;
  p95Pressure: number;
  maxPressure: number;
  /** The mask's median beside a therapy pressure, as its own value. */
  maskP50: number | null;
  leakP95: number;
  currentPressure: number;
}

@Component({
  selector: 'app-pressure-section',
  standalone: true,
  imports: [CommonModule, TranslatePipe],
  template: `
    <div class="section" *ngIf="data">
      <div class="section-header">
        <div class="section-title"><i class="fa-solid fa-gauge-high"></i>
          {{ (data.source === 'therapy' ? 'dashboard.title.pressure' : 'dashboard.title.maskPressure') | translate }}</div>
        <div class="section-subtitle">
          {{ (data.source === 'therapy' ? 'dashboard.pressure.subtitleTherapy' : 'dashboard.pressure.subtitleMask') | translate }}</div>
      </div>
      <div class="gauge-row">
        <div class="gauge-wrap">
          <canvas #avgGauge width="160" height="100"></canvas>
          <div class="gauge-title">{{ 'dashboard.pressure.median' | translate }}</div>
        </div>
        <div class="gauge-wrap">
          <canvas #p95Gauge width="160" height="100"></canvas>
          <div class="gauge-title">{{ 'dashboard.pressure.p95' | translate }}</div>
        </div>
      </div>
      <div class="metrics-row">
        <div class="press-card" *ngIf="data.maxPressure">
          <div class="press-label">{{ 'dashboard.pressure.max' | translate }}</div>
          <div class="press-value">{{ data.maxPressure.toFixed(1) }} cmH2O</div>
        </div>
        <div class="press-card" *ngIf="data.maskP50">
          <div class="press-label">{{ 'dashboard.pressure.maskMedian' | translate }}</div>
          <div class="press-value">{{ data.maskP50.toFixed(1) }} cmH2O</div>
        </div>
        <div class="press-card" *ngIf="data.currentPressure">
          <div class="press-label">{{ 'dashboard.pressure.current' | translate }}</div>
          <div class="press-value">{{ data.currentPressure.toFixed(1) }} cmH2O</div>
        </div>
        <div class="press-card">
          <div class="press-label">{{ 'dashboard.pressure.leakRate' | translate }}</div>
          <div class="press-value" [style.color]="leakColor">{{ data.leakP95.toFixed(1) }} L/min</div>
        </div>
      </div>
    </div>
  `,
  styles: [`
    .section { margin-bottom: 1.5rem; }
    .section-header { margin-bottom: 0.75rem; }
    .section-title { color: #e0e0e0; font-size: 1rem; font-weight: 600; }
    .section-subtitle { color: #888; font-size: 0.8rem; }
    .gauge-row { display: flex; gap: 1.5rem; justify-content: center; margin-bottom: 0.75rem; flex-wrap: wrap; }
    .gauge-wrap { text-align: center; }
    .gauge-title { color: #888; font-size: 0.75rem; margin-top: 2px; }
    .metrics-row { display: flex; gap: 0.75rem; flex-wrap: wrap; }
    .press-card {
      flex: 1; min-width: 120px; background: #1e1e2f; border: 1px solid #333;
      border-radius: 10px; padding: 0.7rem 0.85rem; text-align: center;
    }
    .press-label { color: #888; font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.04em; }
    .press-value { color: #e0e0e0; font-size: 1.1rem; font-weight: 600; }
    @media (max-width: 768px) { .metrics-row { flex-direction: column; } }
  `]
})
export class PressureSectionComponent implements AfterViewInit, OnChanges {
  @Input() data: PressureSectionData | null = null;
  @ViewChild('avgGauge') avgGaugeRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('p95Gauge') p95GaugeRef!: ElementRef<HTMLCanvasElement>;
  private charts: Chart[] = [];

  get leakColor(): string {
    if (!this.data) return '#e0e0e0';
    return this.data.leakP95 < 10 ? '#4ade80' : this.data.leakP95 < 24 ? '#fb923c' : '#ef4444';
  }

  ngAfterViewInit() { this.renderGauges(); }
  ngOnChanges() { setTimeout(() => this.renderGauges(), 50); }

  private renderGauges() {
    if (!this.data || !this.avgGaugeRef) return;
    this.charts.forEach(c => c.destroy());
    this.charts = [];
    this.charts.push(this.makeGauge(this.avgGaugeRef, this.data.p50Pressure, 4, 20));
    this.charts.push(this.makeGauge(this.p95GaugeRef, this.data.p95Pressure, 4, 20));
  }

  private makeGauge(ref: ElementRef<HTMLCanvasElement>, value: number, min: number, max: number): Chart {
    const pct = Math.min((value - min) / (max - min), 1);
    const color = value < 15 ? '#4ade80' : value < 18 ? '#fbbf24' : '#ef4444';
    return new Chart(ref.nativeElement, {
      type: 'doughnut',
      data: { datasets: [{ data: [pct * 100, 100 - pct * 100], backgroundColor: [color, '#2a2a3a'], borderWidth: 0 }] },
      options: {
        cutout: '72%', rotation: -90, circumference: 180, responsive: false,
        plugins: { legend: { display: false }, tooltip: { enabled: false } },
      },
      plugins: [{
        id: 'gaugeText',
        afterDraw: (chart: any) => {
          const ctx = chart.ctx;
          ctx.save();
          ctx.font = 'bold 18px system-ui';
          ctx.fillStyle = color;
          ctx.textAlign = 'center';
          ctx.fillText(value.toFixed(1), chart.width / 2, chart.height - 20);
          ctx.font = '10px system-ui';
          ctx.fillStyle = '#888';
          ctx.fillText('cmH2O', chart.width / 2, chart.height - 6);
          ctx.restore();
        }
      }]
    });
  }
}
