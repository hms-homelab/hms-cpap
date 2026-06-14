import { Component, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { ActivatedRoute, RouterLink } from '@angular/router';
import { Chart, registerables } from 'chart.js';
import zoomPlugin from 'chartjs-plugin-zoom';
import { forkJoin } from 'rxjs';
import { CpapApiService } from '../../services/cpap-api.service';
import { SignalData } from '../../models/session.model';
import { makeDataset } from '../../utils/chart-helpers';

Chart.register(...registerables, zoomPlugin);

interface CmpSig { key: keyof SignalData; title: string; unit: string; }

const COMPARE_SIGNALS: CmpSig[] = [
  { key: 'flow_avg', title: 'Flow Rate', unit: 'L/min' },
  { key: 'pressure_avg', title: 'Pressure', unit: 'cmH2O' },
  { key: 'leak_rate', title: 'Leak Rate', unit: 'L/min' },
  { key: 'respiratory_rate', title: 'Respiratory Rate', unit: '/min' },
  { key: 'tidal_volume', title: 'Tidal Volume', unit: 'mL' },
  { key: 'minute_ventilation', title: 'Minute Ventilation', unit: 'L/min' },
  { key: 'flow_limitation', title: 'Flow Limitation', unit: '' },
  { key: 'snore_index', title: 'Snore', unit: '' },
];

const COLOR_A = '#60a5fa';
const COLOR_B = '#fb923c';

@Component({
  selector: 'app-compare',
  standalone: true,
  imports: [CommonModule, RouterLink],
  template: `
    <div class="cmp-page">
      <div class="top-bar">
        <h2><i class="fa-solid fa-code-compare"></i> Compare Nights</h2>
        <a routerLink="/sessions" class="back-link"><i class="fa-solid fa-arrow-left"></i> Sessions</a>
      </div>

      <div class="legend">
        <span class="key"><span class="swatch" [style.background]="colorA"></span>{{ dateA }}</span>
        <span class="key"><span class="swatch" [style.background]="colorB"></span>{{ dateB }}</span>
        <span class="hint">Both nights aligned to elapsed time</span>
      </div>

      <div class="loading" *ngIf="loading"><p>Loading both nights…</p></div>
      <div class="loading" *ngIf="error"><p>{{ error }}</p></div>

      <!-- Maximized detail view of the selected signal (zoom like session detail) -->
      <div class="detail-section" *ngIf="!loading && !error && selected" [class.expanded]="isExpanded">
        <div class="detail-header">
          <h3>{{ selected.title }} <span class="unit">({{ selected.unit }})</span></h3>
          <div class="controls">
            <button (click)="zoom(0.5)" title="Zoom in"><i class="fa-solid fa-magnifying-glass-plus"></i></button>
            <button (click)="zoom(2)" title="Zoom out"><i class="fa-solid fa-magnifying-glass-minus"></i></button>
            <button (click)="resetZoom()">Reset Zoom</button>
            <button (click)="toggleExpand()">
              <i class="fa-solid" [class.fa-expand]="!isExpanded" [class.fa-compress]="isExpanded"></i>
              {{ isExpanded ? 'Close' : 'Full screen' }}
            </button>
          </div>
        </div>
        <div class="detail-chart"><canvas id="cmp-detail"></canvas></div>
        <div class="detail-hint">Scroll / pinch to zoom, drag to pan</div>
      </div>

      <!-- Overview grid: click a chart to maximize it -->
      <div *ngIf="!loading && !error">
        <h3 class="ov-title">Signals <span class="hint">(click to maximize)</span></h3>
        <div class="cmp-grid">
          <div class="cmp-card" *ngFor="let sig of signals"
               [class.active]="selected?.key === sig.key" (click)="select(sig)">
            <div class="cmp-header"><span class="t">{{ sig.title }}</span><span class="u">{{ sig.unit }}</span></div>
            <div class="cmp-chart"><canvas [id]="'cmp-' + sig.key"></canvas></div>
          </div>
        </div>
      </div>
    </div>
  `,
  styles: [`
    .cmp-page { padding: 1rem; max-width: 1200px; margin: 0 auto; }
    .top-bar { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.5rem; }
    h2 { color: #e0e0e0; font-size: 1.1rem; margin: 0; }
    h2 i { color: #60a5fa; margin-right: 0.4rem; }
    .back-link { color: #aaa; text-decoration: none; font-size: 0.85rem; }
    .back-link:hover { color: #fff; }
    .legend { display: flex; gap: 1.25rem; align-items: center; margin-bottom: 1rem; }
    .key { color: #ccc; font-size: 0.85rem; display: flex; align-items: center; gap: 0.4rem; }
    .swatch { width: 14px; height: 3px; border-radius: 2px; display: inline-block; }
    .hint { color: #666; font-size: 0.72rem; font-weight: normal; }
    .ov-title { color: #ccc; font-size: 0.9rem; margin: 0.5rem 0; }

    .detail-section { background: #1e1e2f; border-radius: 8px; padding: 0.75rem; margin-bottom: 1rem; }
    .detail-header { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; }
    .detail-header h3 { margin: 0; color: #e0e0e0; font-size: 0.95rem; }
    .detail-header .unit { color: #888; font-size: 0.75rem; }
    .controls { display: flex; gap: 0.4rem; align-items: center; }
    .controls button { background: #2a2a3a; border: 1px solid #444; color: #ccc; padding: 0.25rem 0.55rem;
      font-size: 0.72rem; cursor: pointer; border-radius: 4px; }
    .controls button:hover { background: #34344a; }
    .detail-chart { height: 320px; position: relative; margin-top: 0.5rem; }
    .detail-chart canvas { width: 100% !important; height: 100% !important; }
    .detail-hint { color: #666; font-size: 0.7rem; text-align: center; margin-top: 0.25rem; }
    .detail-section.expanded { position: fixed; inset: 0; z-index: 1000; margin: 0; border-radius: 0;
      overflow: auto; box-shadow: 0 0 0 100vmax rgba(0,0,0,0.6); }
    .detail-section.expanded .detail-chart { height: calc(100vh - 150px); }

    .cmp-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); gap: 0.6rem; }
    .cmp-card { background: #1e1e2f; border-radius: 8px; padding: 0.5rem 0.6rem; cursor: pointer;
      border: 1px solid transparent; }
    .cmp-card:hover { border-color: #3a3a55; }
    .cmp-card.active { border-color: #60a5fa; }
    .cmp-header { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 0.2rem; }
    .cmp-header .t { color: #e0e0e0; font-size: 0.8rem; font-weight: 600; }
    .cmp-header .u { color: #888; font-size: 0.68rem; }
    .cmp-chart { height: 120px; position: relative; }
    .cmp-chart canvas { width: 100% !important; height: 100% !important; }
    .loading { color: #888; padding: 2rem; text-align: center; }
  `],
})
export class CompareComponent implements OnInit, OnDestroy {
  dateA = '';
  dateB = '';
  loading = true;
  error = '';
  signals = COMPARE_SIGNALS;
  colorA = COLOR_A;
  colorB = COLOR_B;
  selected: CmpSig | null = null;
  isExpanded = false;

  private overviewCharts: Chart[] = [];
  private detailChart: Chart | null = null;
  private dataA?: SignalData;
  private dataB?: SignalData;

  constructor(private route: ActivatedRoute, private api: CpapApiService) {}

  ngOnInit(): void {
    this.dateA = this.route.snapshot.paramMap.get('a') || '';
    this.dateB = this.route.snapshot.paramMap.get('b') || '';
    forkJoin({
      a: this.api.getSessionSignals(this.dateA),
      b: this.api.getSessionSignals(this.dateB),
    }).subscribe({
      next: ({ a, b }) => {
        this.dataA = a;
        this.dataB = b;
        this.loading = false;
        this.selected = this.signals[0];
        setTimeout(() => { this.renderOverview(); this.renderDetail(); }, 50);
      },
      error: () => { this.error = 'Could not load one of the sessions.'; this.loading = false; },
    });
  }

  ngOnDestroy(): void {
    this.overviewCharts.forEach(c => c.destroy());
    this.detailChart?.destroy();
  }

  select(sig: CmpSig): void {
    this.selected = sig;
    setTimeout(() => this.renderDetail(), 30);
  }

  toggleExpand(): void {
    this.isExpanded = !this.isExpanded;
    setTimeout(() => this.detailChart?.resize(), 60);
  }

  zoom(factor: number): void {
    this.detailChart?.zoom(factor);
  }

  resetZoom(): void {
    this.detailChart?.resetZoom();
  }

  private series(sig: CmpSig) {
    const dA = (this.dataA?.[sig.key] as (number | null)[]) || [];
    const dB = (this.dataB?.[sig.key] as (number | null)[]) || [];
    const len = Math.max(dA.length, dB.length);
    const labels = Array.from({ length: len }, (_, i) =>
      `${Math.floor(i / 60)}h${(i % 60).toString().padStart(2, '0')}`);
    return { dA, dB, labels };
  }

  private renderOverview(): void {
    this.overviewCharts.forEach(c => c.destroy());
    this.overviewCharts = [];
    for (const sig of this.signals) {
      const canvas = document.getElementById('cmp-' + sig.key) as HTMLCanvasElement | null;
      if (!canvas) continue;
      const { dA, dB, labels } = this.series(sig);
      if (!dA.length && !dB.length) continue;
      this.overviewCharts.push(new Chart(canvas, {
        type: 'line',
        data: { labels, datasets: [makeDataset(this.dateA, dA, COLOR_A), makeDataset(this.dateB, dB, COLOR_B)] },
        options: {
          responsive: true, maintainAspectRatio: false,
          plugins: { legend: { display: false } },
          scales: { x: { display: false }, y: { ticks: { color: '#777', font: { size: 8 } }, grid: { color: '#2a2a3a' } } },
          elements: { point: { radius: 0 } },
        },
      }));
    }
  }

  private renderDetail(): void {
    this.detailChart?.destroy();
    this.detailChart = null;
    if (!this.selected) return;
    const canvas = document.getElementById('cmp-detail') as HTMLCanvasElement | null;
    if (!canvas) return;
    const { dA, dB, labels } = this.series(this.selected);
    this.detailChart = new Chart(canvas, {
      type: 'line',
      data: { labels, datasets: [makeDataset(this.dateA, dA, COLOR_A), makeDataset(this.dateB, dB, COLOR_B)] },
      options: {
        responsive: true, maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        plugins: {
          legend: { display: true, labels: { color: '#ccc', boxWidth: 12, font: { size: 10 } } },
          tooltip: { backgroundColor: '#1e1e2f', borderColor: '#333', borderWidth: 1 },
          zoom: {
            zoom: { wheel: { enabled: true }, pinch: { enabled: true }, mode: 'x' },
            pan: { enabled: true, mode: 'x' },
          },
        },
        scales: {
          x: { ticks: { color: '#888', font: { size: 9 }, maxTicksLimit: 10 }, grid: { color: '#2a2a3a' } },
          y: { ticks: { color: '#888', font: { size: 9 } }, grid: { color: '#2a2a3a' } },
        },
        elements: { point: { radius: 0 } },
      },
    });
  }
}
