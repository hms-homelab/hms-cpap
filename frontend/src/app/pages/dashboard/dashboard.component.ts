import { Component, OnInit, ViewChild, ElementRef, AfterViewInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { CpapApiService } from '../../services/cpap-api.service';
import { MetricCardComponent } from '../../components/metric-card/metric-card.component';
import { DashboardData, TrendPoint } from '../../models/session.model';
import Chart from 'chart.js/auto';

const MODE_LABELS: Record<string, string> = {
  '0': 'CPAP', '1': 'APAP', '7': 'ASV', '8': 'ASVAuto',
};

@Component({
  selector: 'app-dashboard',
  standalone: true,
  imports: [CommonModule, MetricCardComponent],
  template: `
    <div class="dashboard">
      <h2>Dashboard</h2>
      <div class="cards" *ngIf="data">
        <app-metric-card label="Last Night AHI" [value]="data.latest_night.ahi" unit="events/h" />
        <app-metric-card label="Usage" [value]="data.latest_night.usage_hours" unit="hours" />
        <app-metric-card label="Compliance" [value]="data.latest_night.compliance_pct" unit="%" />
        <app-metric-card label="Avg Leak" [value]="data.latest_night.leak_avg" unit="L/min" />
        <app-metric-card label="Mode" [value]="modeName" unit="" />
      </div>
      <div class="charts">
        <div class="chart-container">
          <canvas #ahiChart></canvas>
        </div>
        <div class="chart-container">
          <canvas #usageChart></canvas>
        </div>
        <div class="chart-container">
          <canvas #pressureChart></canvas>
        </div>
        <div class="chart-container">
          <canvas #leakChart></canvas>
        </div>
        <div class="chart-container wide">
          <canvas #eventsChart></canvas>
        </div>
        <div class="chart-container wide">
          <canvas #respiratoryChart></canvas>
        </div>
        <!-- CSR: hidden in CPAP mode (mode 0) -->
        <div class="chart-container" *ngIf="!isCpapMode">
          <canvas #csrChart></canvas>
        </div>
        <!-- EPR: hidden in CPAP mode -->
        <div class="chart-container" *ngIf="!isCpapMode">
          <canvas #eprChart></canvas>
        </div>
      </div>
    </div>
  `,
  styles: [`
    .dashboard { padding: 1.5rem; }
    h2 { color: #e0e0e0; margin-bottom: 1rem; }
    .cards { display: flex; gap: 1rem; flex-wrap: wrap; margin-bottom: 1.5rem; }
    .charts { display: grid; grid-template-columns: 1fr 1fr; gap: 1.5rem; }
    .chart-container { background: #1e1e2f; border: 1px solid #333; border-radius: 8px; padding: 1rem; }
    .chart-container.wide { grid-column: 1 / -1; }
    @media (max-width: 768px) { .charts { grid-template-columns: 1fr; } }
  `]
})
export class DashboardComponent implements OnInit, AfterViewInit {
  @ViewChild('ahiChart') ahiChartRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('usageChart') usageChartRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('pressureChart') pressureChartRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('leakChart') leakChartRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('eventsChart') eventsChartRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('respiratoryChart') respiratoryChartRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('csrChart') csrChartRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('eprChart') eprChartRef!: ElementRef<HTMLCanvasElement>;

  data: DashboardData | null = null;
  modeName = '';
  isCpapMode = true;
  private charts: Chart[] = [];

  constructor(private api: CpapApiService) {}

  ngOnInit() {
    this.api.getDashboard().subscribe(d => {
      this.data = d;
      const mode = d.latest_night.therapy_mode || '0';
      this.modeName = MODE_LABELS[mode] || 'Unknown';
      this.isCpapMode = mode === '0';
      // Delay to let *ngIf render conditional elements
      setTimeout(() => {
        if (this.ahiChartRef) this.renderCharts();
      }, 50);
    });
  }

  ngAfterViewInit() {
    if (this.data) this.renderCharts();
  }

  private renderCharts() {
    if (!this.data) return;
    this.charts.forEach(c => c.destroy());
    this.charts = [];

    const labels = this.data.ahi_trend.map(p => p.date.slice(5));
    const darkScales = (beginAtZero = true) => ({
      x: { ticks: { color: '#888' }, grid: { color: '#333' } },
      y: { ticks: { color: '#888' }, grid: { color: '#333' }, beginAtZero },
    });

    // AHI Trend
    this.charts.push(new Chart(this.ahiChartRef.nativeElement, {
      type: 'line',
      data: {
        labels,
        datasets: [{
          label: 'AHI', data: this.data.ahi_trend.map(p => +p.value),
          borderColor: '#64b5f6', backgroundColor: 'rgba(100,181,246,0.1)',
          fill: true, tension: 0.3, pointRadius: 3,
        }]
      },
      options: {
        responsive: true,
        plugins: { legend: { display: false }, title: { display: true, text: 'AHI Trend (30 days)', color: '#e0e0e0' } },
        scales: darkScales(),
      }
    }));

    // Usage Hours
    this.charts.push(new Chart(this.usageChartRef.nativeElement, {
      type: 'bar',
      data: {
        labels,
        datasets: [{
          label: 'Usage (hours)', data: this.data.usage_trend.map(p => +p.value),
          backgroundColor: this.data.usage_trend.map(p => +p.value >= 4 ? 'rgba(76,175,80,0.7)' : 'rgba(255,152,0,0.7)'),
          borderRadius: 4,
        }]
      },
      options: {
        responsive: true,
        plugins: { legend: { display: false }, title: { display: true, text: 'Usage Hours (30 days)', color: '#e0e0e0' } },
        scales: darkScales(),
      }
    }));

    // Fetch additional trends
    this.api.getTrend('pressure', 30).subscribe(d => this.renderTrendChart(d, this.pressureChartRef, 'Pressure Trend (30 days)', [
      { key: 'mask_press_50', label: 'P50', color: '#ce93d8' },
      { key: 'mask_press_95', label: 'P95', color: '#ba68c8', fill: '-1' },
    ]));

    this.api.getTrend('leak', 30).subscribe(d => this.renderTrendChart(d, this.leakChartRef, 'Leak Trend (30 days)', [
      { key: 'leak_50', label: 'L50', color: '#ffb74d' },
      { key: 'leak_95', label: 'L95', color: '#ff9800', fill: '-1' },
    ]));

    this.api.getTrend('events', 30).subscribe(d => this.renderEventsChart(d));

    this.api.getTrend('respiratory', 30).subscribe(d => this.renderRespiratoryChart(d));

    if (!this.isCpapMode) {
      this.api.getTrend('csr', 30).subscribe(d => this.renderCsrChart(d));
      this.api.getTrend('epr', 30).subscribe(d => this.renderEprChart(d));
    }
  }

  private renderTrendChart(data: TrendPoint[], ref: ElementRef<HTMLCanvasElement>, title: string, series: { key: string; label: string; color: string; fill?: string }[]) {
    if (!data?.length || !ref?.nativeElement) return;
    const labels = data.map(p => (p['record_date'] || '').slice(5));
    this.charts.push(new Chart(ref.nativeElement, {
      type: 'line',
      data: {
        labels,
        datasets: series.map(s => ({
          label: s.label, data: data.map(p => +(p[s.key] || 0)),
          borderColor: s.color, backgroundColor: s.color + '1a',
          fill: s.fill || false, tension: 0.3, pointRadius: 2, borderWidth: 1.5,
        })),
      },
      options: {
        responsive: true,
        plugins: {
          legend: { labels: { color: '#ccc', font: { size: 10 } } },
          title: { display: true, text: title, color: '#e0e0e0' },
        },
        scales: {
          x: { ticks: { color: '#888' }, grid: { color: '#333' } },
          y: { ticks: { color: '#888' }, grid: { color: '#333' } },
        },
      },
    }));
  }

  private renderEventsChart(data: TrendPoint[]) {
    if (!data?.length || !this.eventsChartRef?.nativeElement) return;
    const labels = data.map(p => (p['record_date'] || '').slice(5));
    this.charts.push(new Chart(this.eventsChartRef.nativeElement, {
      type: 'bar',
      data: {
        labels,
        datasets: [
          { label: 'OA', data: data.map(p => +(p['oai'] || 0)), backgroundColor: 'rgba(248,113,113,0.7)', stack: 'e' },
          { label: 'CA', data: data.map(p => +(p['cai'] || 0)), backgroundColor: 'rgba(251,146,60,0.7)', stack: 'e' },
          { label: 'H', data: data.map(p => +(p['hi'] || 0)), backgroundColor: 'rgba(251,191,36,0.7)', stack: 'e' },
          { label: 'RERA', data: data.map(p => +(p['rin'] || 0)), backgroundColor: 'rgba(74,222,128,0.7)', stack: 'e' },
        ],
      },
      options: {
        responsive: true,
        plugins: {
          legend: { labels: { color: '#ccc', font: { size: 10 } } },
          title: { display: true, text: 'Event Breakdown (30 days)', color: '#e0e0e0' },
        },
        scales: {
          x: { stacked: true, ticks: { color: '#888' }, grid: { color: '#333' } },
          y: { stacked: true, ticks: { color: '#888' }, grid: { color: '#333' }, beginAtZero: true },
        },
      },
    }));
  }

  private renderRespiratoryChart(data: TrendPoint[]) {
    if (!data?.length || !this.respiratoryChartRef?.nativeElement) return;
    const labels = data.map(p => (p['record_date'] || '').slice(5));
    this.charts.push(new Chart(this.respiratoryChartRef.nativeElement, {
      type: 'line',
      data: {
        labels,
        datasets: [
          {
            label: 'Resp Rate (br/min)', data: data.map(p => +(p['resp_rate_50'] || 0)),
            borderColor: '#81c784', tension: 0.3, pointRadius: 2, borderWidth: 1.5, yAxisID: 'y',
          },
          {
            label: 'Tidal Vol (mL)', data: data.map(p => +(p['tid_vol_50'] || 0) * 1000),
            borderColor: '#4dd0e1', tension: 0.3, pointRadius: 2, borderWidth: 1.5, yAxisID: 'y1',
          },
          {
            label: 'Min Vent (L/min)', data: data.map(p => +(p['min_vent_50'] || 0)),
            borderColor: '#aed581', tension: 0.3, pointRadius: 2, borderWidth: 1.5, yAxisID: 'y',
          },
        ],
      },
      options: {
        responsive: true,
        plugins: {
          legend: { labels: { color: '#ccc', font: { size: 10 } } },
          title: { display: true, text: 'Respiratory Trends (30 days)', color: '#e0e0e0' },
        },
        scales: {
          x: { ticks: { color: '#888' }, grid: { color: '#333' } },
          y: { position: 'left', ticks: { color: '#888' }, grid: { color: '#333' }, title: { display: true, text: 'Rate / Vent', color: '#888' } },
          y1: { position: 'right', ticks: { color: '#4dd0e1' }, grid: { drawOnChartArea: false }, title: { display: true, text: 'Tidal Vol (mL)', color: '#4dd0e1' } },
        },
      },
    }));
  }

  private renderCsrChart(data: TrendPoint[]) {
    if (!data?.length || !this.csrChartRef?.nativeElement) return;
    const labels = data.map(p => (p['record_date'] || '').slice(5));
    this.charts.push(new Chart(this.csrChartRef.nativeElement, {
      type: 'bar',
      data: {
        labels,
        datasets: [{
          label: 'CSR (min)', data: data.map(p => +(p['csr'] || 0)),
          backgroundColor: 'rgba(239,83,80,0.6)', borderRadius: 3,
        }],
      },
      options: {
        responsive: true,
        plugins: {
          legend: { display: false },
          title: { display: true, text: 'Cheyne-Stokes Respiration (30 days)', color: '#e0e0e0' },
        },
        scales: {
          x: { ticks: { color: '#888' }, grid: { color: '#333' } },
          y: { ticks: { color: '#888' }, grid: { color: '#333' }, beginAtZero: true, title: { display: true, text: 'Minutes', color: '#888' } },
        },
      },
    }));
  }

  private renderEprChart(data: TrendPoint[]) {
    if (!data?.length || !this.eprChartRef?.nativeElement) return;
    const labels = data.map(p => (p['record_date'] || '').slice(5));
    this.charts.push(new Chart(this.eprChartRef.nativeElement, {
      type: 'line',
      data: {
        labels,
        datasets: [{
          label: 'EPR Level', data: data.map(p => +(p['epr_level'] || 0)),
          borderColor: '#9575cd', backgroundColor: 'rgba(149,117,205,0.15)',
          fill: true, tension: 0, pointRadius: 3, borderWidth: 2, stepped: true,
        }],
      },
      options: {
        responsive: true,
        plugins: {
          legend: { display: false },
          title: { display: true, text: 'EPR Level (30 days)', color: '#e0e0e0' },
        },
        scales: {
          x: { ticks: { color: '#888' }, grid: { color: '#333' } },
          y: { ticks: { color: '#888', stepSize: 1 }, grid: { color: '#333' }, min: 0, max: 3, title: { display: true, text: 'Level', color: '#888' } },
        },
      },
    }));
  }
}
