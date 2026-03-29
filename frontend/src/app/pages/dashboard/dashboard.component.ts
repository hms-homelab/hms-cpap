import { Component, OnInit, ViewChild, ElementRef, AfterViewInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { CpapApiService } from '../../services/cpap-api.service';
import { MetricCardComponent } from '../../components/metric-card/metric-card.component';
import { DashboardData } from '../../models/session.model';
import Chart from 'chart.js/auto';

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
      </div>
      <div class="charts">
        <div class="chart-container">
          <canvas #ahiChart></canvas>
        </div>
        <div class="chart-container">
          <canvas #usageChart></canvas>
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
    @media (max-width: 768px) { .charts { grid-template-columns: 1fr; } }
  `]
})
export class DashboardComponent implements OnInit, AfterViewInit {
  @ViewChild('ahiChart') ahiChartRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('usageChart') usageChartRef!: ElementRef<HTMLCanvasElement>;

  data: DashboardData | null = null;
  private ahiChart: Chart | null = null;
  private usageChart: Chart | null = null;

  constructor(private api: CpapApiService) {}

  ngOnInit() {
    this.api.getDashboard().subscribe(d => {
      this.data = d;
      if (this.ahiChartRef) this.renderCharts();
    });
  }

  ngAfterViewInit() {
    if (this.data) this.renderCharts();
  }

  private renderCharts() {
    if (!this.data) return;

    const labels = this.data.ahi_trend.map(p => p.date.slice(5)); // MM-DD
    const chartDefaults = { color: '#888', borderColor: '#444' };

    this.ahiChart?.destroy();
    this.ahiChart = new Chart(this.ahiChartRef.nativeElement, {
      type: 'line',
      data: {
        labels,
        datasets: [{
          label: 'AHI',
          data: this.data.ahi_trend.map(p => +p.value),
          borderColor: '#64b5f6',
          backgroundColor: 'rgba(100,181,246,0.1)',
          fill: true,
          tension: 0.3,
          pointRadius: 3,
        }]
      },
      options: {
        responsive: true,
        plugins: {
          legend: { display: false },
          title: { display: true, text: 'AHI Trend (30 days)', color: '#e0e0e0' }
        },
        scales: {
          x: { ticks: { color: '#888' }, grid: { color: '#333' } },
          y: { ticks: { color: '#888' }, grid: { color: '#333' }, beginAtZero: true }
        }
      }
    });

    this.usageChart?.destroy();
    this.usageChart = new Chart(this.usageChartRef.nativeElement, {
      type: 'bar',
      data: {
        labels,
        datasets: [{
          label: 'Usage (hours)',
          data: this.data.usage_trend.map(p => +p.value),
          backgroundColor: this.data.usage_trend.map(p => +p.value >= 4 ? 'rgba(76,175,80,0.7)' : 'rgba(255,152,0,0.7)'),
          borderRadius: 4,
        }]
      },
      options: {
        responsive: true,
        plugins: {
          legend: { display: false },
          title: { display: true, text: 'Usage Hours (30 days)', color: '#e0e0e0' }
        },
        scales: {
          x: { ticks: { color: '#888' }, grid: { color: '#333' } },
          y: { ticks: { color: '#888' }, grid: { color: '#333' }, beginAtZero: true }
        }
      }
    });
  }
}
