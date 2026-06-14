import { Component, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { ActivatedRoute, RouterLink } from '@angular/router';
import { Chart, registerables } from 'chart.js';
import { forkJoin } from 'rxjs';
import { CpapApiService } from '../../services/cpap-api.service';
import { SignalData } from '../../models/session.model';
import { makeDataset } from '../../utils/chart-helpers';

Chart.register(...registerables);

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

      <div class="cmp-grid" *ngIf="!loading && !error">
        <div class="cmp-card" *ngFor="let sig of signals">
          <div class="cmp-header"><span class="t">{{ sig.title }}</span><span class="u">{{ sig.unit }}</span></div>
          <div class="cmp-chart"><canvas [id]="'cmp-' + sig.key"></canvas></div>
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
    .hint { color: #666; font-size: 0.75rem; margin-left: auto; }
    .cmp-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(360px, 1fr)); gap: 0.75rem; }
    .cmp-card { background: #1e1e2f; border-radius: 8px; padding: 0.6rem 0.75rem; }
    .cmp-header { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 0.25rem; }
    .cmp-header .t { color: #e0e0e0; font-size: 0.85rem; font-weight: 600; }
    .cmp-header .u { color: #888; font-size: 0.7rem; }
    .cmp-chart { height: 170px; position: relative; }
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

  private charts: Chart[] = [];
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
        setTimeout(() => this.render(), 50);
      },
      error: () => {
        this.error = 'Could not load one of the sessions.';
        this.loading = false;
      },
    });
  }

  ngOnDestroy(): void {
    this.charts.forEach(c => c.destroy());
  }

  private render(): void {
    for (const sig of this.signals) {
      const canvas = document.getElementById('cmp-' + sig.key) as HTMLCanvasElement | null;
      if (!canvas) continue;
      const dA = ((this.dataA?.[sig.key] as (number | null)[]) || []);
      const dB = ((this.dataB?.[sig.key] as (number | null)[]) || []);
      if (!dA.length && !dB.length) continue;

      const len = Math.max(dA.length, dB.length);
      const labels = Array.from({ length: len }, (_, i) =>
        `${Math.floor(i / 60)}h${(i % 60).toString().padStart(2, '0')}`);

      this.charts.push(new Chart(canvas, {
        type: 'line',
        data: {
          labels,
          datasets: [
            makeDataset(this.dateA, dA, COLOR_A),
            makeDataset(this.dateB, dB, COLOR_B),
          ],
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          interaction: { mode: 'index', intersect: false },
          plugins: {
            legend: { display: true, labels: { color: '#ccc', boxWidth: 12, font: { size: 10 } } },
            tooltip: { backgroundColor: '#1e1e2f', borderColor: '#333', borderWidth: 1 },
          },
          scales: {
            x: { ticks: { color: '#888', font: { size: 9 }, maxTicksLimit: 8 }, grid: { color: '#2a2a3a' } },
            y: { ticks: { color: '#888', font: { size: 9 } }, grid: { color: '#2a2a3a' } },
          },
          elements: { point: { radius: 0 } },
        },
      }));
    }
  }
}
