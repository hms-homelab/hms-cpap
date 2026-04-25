import { Component, OnInit, ViewChild, ElementRef, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { ActivatedRoute } from '@angular/router';
import { forkJoin, of } from 'rxjs';
import { catchError } from 'rxjs/operators';
import { CpapApiService } from '../../services/cpap-api.service';
import { MetricCardComponent } from '../../components/metric-card/metric-card.component';
import { SessionDetail, SessionEvent, SignalData, VitalsData, OximetryData } from '../../models/session.model';
import { formatTimestamps, eventAnnotations, makeDataset, makeFillBand, EVENT_COLORS } from '../../utils/chart-helpers';
import { Chart, ChartDataset, registerables } from 'chart.js';
import annotationPlugin from 'chartjs-plugin-annotation';
import zoomPlugin from 'chartjs-plugin-zoom';

Chart.register(...registerables, annotationPlugin, zoomPlugin);

interface SignalDef {
  key: string;
  title: string;
  unit: string;
  color: string;
  yMin?: number;
  yMax?: number;
  hasBand?: boolean;       // has min/max fill band
  bandMinKey?: string;
  bandMaxKey?: string;
  showEvents?: boolean;    // overlay event markers
  source: 'signals' | 'vitals' | 'oximetry';
  fill?: boolean;
  minMode?: number;        // only show if therapy_mode >= this (e.g., 1=not CPAP, 7=ASV only)
}

const SIGNAL_DEFS: SignalDef[] = [
  { key: 'flow_avg', title: 'Flow Rate', unit: 'L/min', color: '#64b5f6', hasBand: true, bandMinKey: 'flow_min', bandMaxKey: 'flow_max', showEvents: true, source: 'signals' },
  { key: 'pressure_avg', title: 'Pressure', unit: 'cmH2O', color: '#ce93d8', hasBand: true, bandMinKey: 'pressure_min', bandMaxKey: 'pressure_max', showEvents: true, source: 'signals' },
  { key: 'mask_pressure', title: 'Mask Pressure', unit: 'cmH2O', color: '#ba68c8', source: 'signals' },
  { key: 'leak_rate', title: 'Leak Rate', unit: 'L/min', color: '#ffb74d', showEvents: true, source: 'signals', fill: true },
  { key: 'flow_limitation', title: 'Flow Limitation', unit: '0-1', color: '#ef5350', yMin: 0, yMax: 1, source: 'signals', fill: true },
  { key: 'snore_index', title: 'Snore Index', unit: '0-5', color: '#ff8a65', yMin: 0, source: 'signals', fill: true },
  { key: 'respiratory_rate', title: 'Respiratory Rate', unit: 'br/min', color: '#81c784', source: 'signals' },
  { key: 'tidal_volume', title: 'Tidal Volume', unit: 'mL', color: '#4dd0e1', source: 'signals' },
  { key: 'minute_ventilation', title: 'Minute Ventilation', unit: 'L/min', color: '#aed581', source: 'signals' },
  { key: 'ie_ratio', title: 'I:E Ratio', unit: 'ratio', color: '#fff176', source: 'signals' },
  { key: 'epr_pressure', title: 'EPR Pressure', unit: 'cmH2O', color: '#9575cd', source: 'signals', minMode: 1 },
  { key: 'target_ventilation', title: 'Target Ventilation', unit: 'L/min', color: '#4fc3f7', source: 'signals', minMode: 7 },
  { key: 'spo2', title: 'SpO2', unit: '%', color: '#e57373', yMin: 85, yMax: 100, hasBand: true, bandMinKey: 'spo2_min', bandMaxKey: 'spo2', source: 'vitals' },
  { key: 'heart_rate', title: 'Heart Rate', unit: 'bpm', color: '#f06292', hasBand: true, bandMinKey: 'hr_min', bandMaxKey: 'hr_max', source: 'vitals' },
  { key: 'spo2', title: 'O2Ring SpO2', unit: '%', color: '#ef5350', yMin: 85, yMax: 100, source: 'oximetry' },
  { key: 'heart_rate', title: 'O2Ring Heart Rate', unit: 'bpm', color: '#ec407a', source: 'oximetry' },
];

@Component({
  selector: 'app-session-detail',
  standalone: true,
  imports: [CommonModule, MetricCardComponent, FormsModule],
  template: `
    <div class="detail-page" *ngIf="session">
      <div class="top-bar">
        <h2>Session: {{ date }}</h2>
        <div class="date-nav">
          <button class="nav-btn" (click)="prevDay()">&lt;</button>
          <input type="date" [value]="date" (change)="goToDate($event)" class="date-input" />
          <button class="nav-btn" (click)="nextDay()">&gt;</button>
        </div>
      </div>

      <!-- LIVE BANNER -->
      <div class="live-banner" *ngIf="isLive">
        <span class="live-dot"></span>
        <span class="live-text">LIVE</span>
        <span class="live-info">Started {{ liveStartTime }} &mdash; {{ liveDuration }}</span>
        <span class="live-poll">Refreshing every 65s</span>
      </div>

      <div class="cards">
        <app-metric-card label="AHI" [value]="session.ahi || '0'" unit="events/h"
          icon="fa-solid fa-heart-pulse" [iconColor]="ahiColor" />
        <app-metric-card label="Duration" [value]="isLive ? liveDuration : fmtDuration(session.duration_hours)" unit=""
          icon="fa-solid fa-clock-rotate-left" iconColor="#60a5fa" />
        <app-metric-card label="Events" [value]="session.total_events || '0'" unit=""
          icon="fa-solid fa-triangle-exclamation" [iconColor]="eventsColor" />
        <app-metric-card *ngIf="hasSpo2"
          label="SpO2" [value]="displaySpo2.toFixed(1)" unit="%"
          icon="fa-solid fa-droplet" [iconColor]="spo2Color" />
        <app-metric-card *ngIf="hasHr"
          label="Heart Rate" [value]="'' + displayHr" unit="bpm"
          icon="fa-solid fa-heart-pulse" iconColor="#f06292" />
      </div>

      <div class="event-summary" *ngIf="session.total_events && +session.total_events > 0">
        <span class="event-badge" style="background: #f87171">OA: {{ session.obstructive_apneas || 0 }}</span>
        <span class="event-badge" style="background: #fb923c">CA: {{ session.central_apneas || 0 }}</span>
        <span class="event-badge" style="background: #fbbf24; color: #000">H: {{ session.hypopneas || 0 }}</span>
        <span class="event-badge" style="background: #4ade80; color: #000">RERA: {{ session.reras || 0 }}</span>
      </div>

      <!-- DETAIL VIEW -->
      <div class="detail-section" *ngIf="selectedSignal">
        <div class="detail-header">
          <h3>{{ selectedSignal.title }} <span class="detail-unit">({{ selectedSignal.unit }})</span></h3>
          <div class="detail-controls">
            <div class="range-buttons">
              <button *ngFor="let r of rangeOptions" [class.active]="activeRange === r.value"
                (click)="setRange(r.value)">{{ r.label }}</button>
            </div>
            <button class="reset-btn" (click)="resetZoom()">Reset Zoom</button>
          </div>
        </div>
        <div class="detail-chart-container">
          <canvas #detailCanvas></canvas>
        </div>
        <div class="slider-container" *ngIf="activeRange !== 'all'">
          <input type="range" class="time-slider" [min]="0" [max]="sliderMax"
            [(ngModel)]="sliderPos" (input)="onSliderChange()" />
          <div class="slider-labels">
            <span>{{ sliderStartLabel }}</span>
            <span>{{ sliderEndLabel }}</span>
          </div>
        </div>
      </div>

      <!-- OVERVIEW STRIP -->
      <div class="overview-section" *ngIf="signalLabels.length">
        <h3>Overview <span class="hint">(click to expand)</span></h3>
        <div class="overview-grid">
          <div *ngFor="let sig of availableSignals; let i = index"
            class="overview-card" [class.selected]="selectedSignal?.key === sig.key"
            (click)="selectSignal(sig)">
            <div class="overview-header">
              <span class="ov-title">{{ sig.title }}</span>
              <span class="ov-unit">{{ sig.unit }}</span>
            </div>
            <canvas [id]="'ov-' + sig.key"></canvas>
          </div>
        </div>
      </div>

      <!-- Event Distribution -->
      <div class="doughnut-section" *ngIf="hasEvents">
        <h3>Event Distribution</h3>
        <div class="doughnut-container">
          <canvas #doughnutCanvas></canvas>
        </div>
      </div>

      <div class="loading" *ngIf="!signalLabels.length && !loadError">
        <p>Loading signal data...</p>
      </div>
      <div class="loading" *ngIf="loadError">
        <p>{{ loadError }}</p>
      </div>
    </div>
  `,
  styles: [`
    .detail-page { padding: 1rem; max-width: 1200px; margin: 0 auto; }
    .top-bar { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; margin-bottom: 0.75rem; }
    h2 { color: #e0e0e0; font-size: 1.1rem; margin: 0; }
    h3 { color: #ccc; font-size: 0.9rem; margin: 0.75rem 0 0.5rem; }
    .hint { color: #666; font-size: 0.7rem; font-weight: normal; }
    .date-nav { display: flex; gap: 0.25rem; align-items: center; }
    .nav-btn { background: #2a2a3a; border: 1px solid #444; color: #ccc; padding: 0.3rem 0.6rem; border-radius: 4px; cursor: pointer; font-size: 0.9rem; }
    .nav-btn:hover { background: #3a3a4a; }
    .date-input { background: #1e1e2f; border: 1px solid #444; color: #e0e0e0; padding: 0.3rem 0.5rem; border-radius: 4px; font-size: 0.8rem; }
    .cards { display: flex; gap: 0.75rem; flex-wrap: wrap; margin-bottom: 0.5rem; }
    .event-summary { display: flex; gap: 0.5rem; flex-wrap: wrap; margin-bottom: 0.75rem; }
    .event-badge { color: #fff; padding: 0.2rem 0.5rem; border-radius: 12px; font-size: 0.7rem; font-weight: 600; }

    /* Detail view */
    .detail-section { background: #1e1e2f; border-radius: 8px; padding: 0.75rem; margin-bottom: 0.5rem; }
    .detail-header { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 0.5rem; }
    .detail-header h3 { margin: 0; }
    .detail-unit { color: #888; font-size: 0.75rem; font-weight: normal; }
    .detail-controls { display: flex; gap: 0.5rem; align-items: center; flex-wrap: wrap; }
    .range-buttons { display: flex; gap: 2px; }
    .range-buttons button { background: #2a2a3a; border: 1px solid #444; color: #aaa; padding: 0.2rem 0.5rem; font-size: 0.7rem; cursor: pointer; border-radius: 3px; }
    .range-buttons button.active { background: #64b5f6; color: #000; border-color: #64b5f6; }
    .reset-btn { background: #333; border: 1px solid #555; color: #ccc; padding: 0.2rem 0.5rem; font-size: 0.7rem; cursor: pointer; border-radius: 3px; }
    .detail-chart-container { margin-top: 0.5rem; height: 300px; position: relative; }
    .detail-chart-container canvas { width: 100% !important; height: 100% !important; }

    /* Slider */
    .slider-container { margin-top: 0.5rem; }
    .time-slider { width: 100%; accent-color: #64b5f6; }
    .slider-labels { display: flex; justify-content: space-between; color: #888; font-size: 0.65rem; }

    /* Overview strip */
    .overview-section { margin-top: 0.5rem; }
    .overview-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 4px; }
    .overview-card { background: #1e1e2f; border: 2px solid transparent; border-radius: 6px; padding: 0.4rem 0.5rem 0.2rem; cursor: pointer; transition: border-color 0.15s; }
    .overview-card:hover { border-color: #444; }
    .overview-card.selected { border-color: #64b5f6; }
    .overview-header { display: flex; justify-content: space-between; margin-bottom: 2px; }
    .ov-title { color: #ccc; font-size: 0.65rem; font-weight: 600; }
    .ov-unit { color: #666; font-size: 0.6rem; }
    .overview-card canvas { width: 100% !important; height: 60px !important; }

    /* Doughnut */
    .doughnut-section { margin-top: 0.75rem; }
    .doughnut-container { max-width: 300px; margin: 0 auto; }

    /* Live banner */
    .live-banner {
      display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap;
      background: linear-gradient(135deg, rgba(76,175,80,0.15), rgba(76,175,80,0.05));
      border: 1px solid rgba(76,175,80,0.4); border-radius: 8px;
      padding: 0.6rem 1rem; margin-bottom: 0.75rem;
    }
    .live-dot {
      width: 10px; height: 10px; border-radius: 50%; background: #4caf50;
      animation: pulse-dot 1.5s ease-in-out infinite;
    }
    .live-text { color: #4caf50; font-weight: 700; font-size: 0.85rem; letter-spacing: 1px; }
    .live-info { color: #ccc; font-size: 0.8rem; }
    .live-poll { color: #666; font-size: 0.7rem; margin-left: auto; }
    @keyframes pulse-dot {
      0%, 100% { opacity: 1; box-shadow: 0 0 0 0 rgba(76,175,80,0.4); }
      50% { opacity: 0.6; box-shadow: 0 0 0 6px rgba(76,175,80,0); }
    }

    .loading { color: #666; padding: 2rem; text-align: center; }
    @media (max-width: 768px) {
      .cards { flex-direction: column; }
      .overview-grid { grid-template-columns: 1fr; }
      .detail-chart-container { height: 250px; }
    }
  `]
})
export class SessionDetailComponent implements OnInit, OnDestroy {
  @ViewChild('detailCanvas') detailCanvasRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('doughnutCanvas') doughnutRef!: ElementRef<HTMLCanvasElement>;

  date = '';
  session: SessionDetail | null = null;
  loadError = '';
  hasEvents = false;

  // Live session
  isLive = false;
  liveStartTime = '';
  liveDuration = '';
  private pollTimer: any = null;
  private durationTimer: any = null;
  private sessionStart: Date | null = null;

  // Raw data
  private signalData: SignalData | null = null;
  private vitalsData: VitalsData | null = null;
  private oximetryData: OximetryData | null = null;
  private events: SessionEvent[] = [];

  // Labels
  signalLabels: string[] = [];
  private signalTimestamps: string[] = [];
  private vitalsLabels: string[] = [];
  private vitalsTimestamps: string[] = [];
  private oximetryLabels: string[] = [];
  private oximetryTimestamps: string[] = [];

  // Overview charts
  availableSignals: SignalDef[] = [];
  private overviewCharts: Chart[] = [];

  // Detail chart
  selectedSignal: SignalDef | null = null;
  private detailChart: Chart | null = null;

  // Time range
  rangeOptions = [
    { label: '30m', value: 30 },
    { label: '1h', value: 60 },
    { label: '2h', value: 120 },
    { label: 'All', value: 'all' as const },
  ];
  activeRange: number | 'all' = 'all';
  sliderPos = 0;
  sliderMax = 100;
  sliderStartLabel = '';
  sliderEndLabel = '';

  // Visible window (indices)
  private viewStart = 0;
  private viewEnd = 0;

  constructor(private route: ActivatedRoute, private api: CpapApiService) {}

  ngOnInit() {
    this.date = this.route.snapshot.paramMap.get('date') || '';
    this.loadSession();
  }

  ngOnDestroy() {
    this.destroyCharts();
    this.stopPolling();
  }

  private destroyCharts() {
    this.overviewCharts.forEach(c => c.destroy());
    this.overviewCharts = [];
    this.detailChart?.destroy();
    this.detailChart = null;
  }

  private stopPolling() {
    if (this.pollTimer) { clearInterval(this.pollTimer); this.pollTimer = null; }
    if (this.durationTimer) { clearInterval(this.durationTimer); this.durationTimer = null; }
  }

  private updateLiveDuration() {
    if (!this.session) return;
    // Use actual parsed BRP data duration (not wall clock).
    // Wall clock (now - session_start) overestimates because it includes
    // the delay before Fysetc starts serving files and any downtime gaps.
    this.liveDuration = this.fmtDuration(this.session.duration_hours);
  }

  fmtDuration(val: string | number | undefined): string {
    const hours = +(val || 0);
    if (hours <= 0) return '0m';
    const totalMins = Math.round(hours * 60);
    const h = Math.floor(totalMins / 60);
    const m = totalMins % 60;
    return h > 0 ? `${h}h ${String(m).padStart(2, '0')}m` : `${m}m`;
  }

  get ahiColor(): string {
    const ahi = +(this.session?.ahi || 0);
    return ahi < 5 ? '#4ade80' : ahi < 15 ? '#fb923c' : '#ef4444';
  }
  get eventsColor(): string {
    const e = +(this.session?.total_events || 0);
    return e < 10 ? '#4ade80' : e < 30 ? '#fb923c' : '#ef4444';
  }
  get spo2Color(): string {
    const v = this.displaySpo2;
    return v >= 95 ? '#4ade80' : v >= 90 ? '#fb923c' : v > 0 ? '#ef4444' : '#888';
  }

  get displaySpo2(): number {
    const machine = +(this.session?.avg_spo2 || 0);
    if (machine > 0) return machine;
    if (!this.oximetryData?.spo2?.length) return 0;
    const valid = this.oximetryData.spo2
      .map(v => Number(v)).filter(v => !isNaN(v) && v > 0 && v < 255);
    return valid.length ? valid.reduce((a, b) => a + b, 0) / valid.length : 0;
  }

  get displayHr(): number {
    const machine = +(this.session?.avg_heart_rate || 0);
    if (machine > 0) return machine;
    if (!this.oximetryData?.heart_rate?.length) return 0;
    const valid = (this.oximetryData.heart_rate as any[])
      .map(v => Number(v)).filter(v => !isNaN(v) && v > 0 && v < 255);
    return valid.length ? Math.round(valid.reduce((a, b) => a + b, 0) / valid.length) : 0;
  }

  get hasSpo2(): boolean { return this.displaySpo2 > 0; }
  get hasHr(): boolean { return this.displayHr > 0; }

  private loadSession() {
    this.destroyCharts();
    this.stopPolling();
    this.selectedSignal = null;
    this.signalLabels = [];
    this.loadError = '';
    this.session = null;
    this.isLive = false;

    forkJoin({
      detail: this.api.getSessionDetail(this.date).pipe(catchError(() => of([]))),
      signals: this.api.getSessionSignals(this.date).pipe(catchError(() => of(null))),
      vitals: this.api.getSessionVitals(this.date).pipe(catchError(() => of(null))),
      oximetry: this.api.getSessionOximetry(this.date).pipe(catchError(() => of(null))),
      events: this.api.getSessionEvents(this.date).pipe(catchError(() => of([]))),
    }).subscribe(({ detail, signals, vitals, oximetry, events }) => {
      // Merge all sessions for the night into one combined view
      if (detail.length > 0) {
        this.session = this.mergeSessions(detail);
      }

      // Detect live session (any session in the night still in-progress)
      this.isLive = detail.some(s => !s.session_end);
      if (this.isLive && this.session) {
        // Use earliest session start for live duration
        const earliest = detail.reduce((a, b) =>
          a.session_start < b.session_start ? a : b);
        this.sessionStart = new Date(earliest.session_start.replace(' ', 'T'));
        this.liveStartTime = this.sessionStart.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
        this.updateLiveDuration();
        this.durationTimer = setInterval(() => this.updateLiveDuration(), 10000);
        this.pollTimer = setInterval(() => this.refreshLiveData(), 65000);
      }

      this.signalData = signals;
      this.vitalsData = vitals;
      this.oximetryData = oximetry;
      this.events = events as SessionEvent[];

      if (signals?.timestamps?.length) {
        this.signalTimestamps = signals.timestamps;
        this.signalLabels = formatTimestamps(signals.timestamps);
      }
      if (vitals?.timestamps?.length) {
        this.vitalsTimestamps = vitals.timestamps;
        this.vitalsLabels = formatTimestamps(vitals.timestamps);
      }
      if (oximetry?.timestamps?.length) {
        this.oximetryTimestamps = oximetry.timestamps;
        this.oximetryLabels = formatTimestamps(oximetry.timestamps);
      }

      // Determine available signals (skip those with no data or wrong mode)
      const therapyMode = +(this.session?.therapy_mode || '0');
      this.availableSignals = SIGNAL_DEFS.filter(s => {
        if (s.source === 'oximetry') return oximetry?.timestamps?.length;
        if (s.source === 'vitals') return vitals?.timestamps?.length;
        if (!signals?.timestamps?.length) return false;
        if (s.minMode !== undefined && therapyMode < s.minMode) return false;
        return true;
      });

      if (!this.signalLabels.length && !this.vitalsLabels.length) {
        this.loadError = 'No signal data available for this session.';
        return;
      }

      // Event doughnut
      if (this.session && +(this.session.total_events || 0) > 0) {
        this.hasEvents = true;
      }

      // Render after DOM update
      setTimeout(() => {
        this.renderOverviewCharts();
        if (this.hasEvents) this.renderDoughnut();
        // Auto-select first signal
        if (this.availableSignals.length) {
          this.selectSignal(this.availableSignals[0]);
        }
      }, 50);
    });
  }

  selectSignal(sig: SignalDef) {
    this.selectedSignal = sig;
    this.activeRange = 'all';
    setTimeout(() => this.renderDetailChart(), 50);
  }

  setRange(range: number | 'all') {
    this.activeRange = range;
    if (range === 'all') {
      this.renderDetailChart();
      return;
    }
    const ts = this.getTimestamps();
    this.sliderMax = Math.max(0, ts.length - range);
    this.sliderPos = 0;
    this.updateSliderWindow();
    this.renderDetailChart();
  }

  onSliderChange() {
    this.updateSliderWindow();
    this.renderDetailChart();
  }

  resetZoom() {
    this.activeRange = 'all';
    this.renderDetailChart();
  }

  prevDay() {
    const d = new Date(this.date);
    d.setDate(d.getDate() - 1);
    this.date = d.toISOString().slice(0, 10);
    this.loadSession();
  }

  nextDay() {
    const d = new Date(this.date);
    d.setDate(d.getDate() + 1);
    this.date = d.toISOString().slice(0, 10);
    this.loadSession();
  }

  goToDate(event: Event) {
    const input = event.target as HTMLInputElement;
    if (input.value) {
      this.date = input.value;
      this.loadSession();
    }
  }

  private refreshLiveData() {
    // Re-fetch signals and events without full page reload
    const currentSignal = this.selectedSignal;
    forkJoin({
      detail: this.api.getSessionDetail(this.date).pipe(catchError(() => of([]))),
      signals: this.api.getSessionSignals(this.date).pipe(catchError(() => of(null))),
      vitals: this.api.getSessionVitals(this.date).pipe(catchError(() => of(null))),
      oximetry: this.api.getSessionOximetry(this.date).pipe(catchError(() => of(null))),
      events: this.api.getSessionEvents(this.date).pipe(catchError(() => of([]))),
    }).subscribe(({ detail, signals, vitals, oximetry, events }) => {
      if (detail.length > 0) this.session = this.mergeSessions(detail);
      this.signalData = signals;
      this.vitalsData = vitals;
      this.oximetryData = oximetry;
      this.events = events as SessionEvent[];

      if (signals?.timestamps?.length) {
        this.signalTimestamps = signals.timestamps;
        this.signalLabels = formatTimestamps(signals.timestamps);
      }
      if (vitals?.timestamps?.length) {
        this.vitalsTimestamps = vitals.timestamps;
        this.vitalsLabels = formatTimestamps(vitals.timestamps);
      }
      if (oximetry?.timestamps?.length) {
        this.oximetryTimestamps = oximetry.timestamps;
        this.oximetryLabels = formatTimestamps(oximetry.timestamps);
      }

      // Session completed while we were watching
      if (!detail.some(s => !s.session_end)) {
        this.isLive = false;
        this.stopPolling();
      }

      // Re-render charts
      this.destroyCharts();
      setTimeout(() => {
        this.renderOverviewCharts();
        if (currentSignal) {
          this.selectedSignal = currentSignal;
          this.renderDetailChart();
        }
      }, 50);
    });
  }

  private mergeSessions(sessions: SessionDetail[]): SessionDetail {
    if (sessions.length === 1) return sessions[0];
    const first = sessions[0];
    const totalSeconds = sessions.reduce((sum, s) => sum + (+s.duration_hours || 0) * 3600, 0);
    const totalEvents = sessions.reduce((sum, s) => sum + +(s.total_events || 0), 0);
    const totalHours = totalSeconds / 3600;
    const allEvents = sessions.flatMap(s => s.events || []);

    return {
      ...first,
      duration_hours: totalHours.toFixed(2),
      ahi: totalHours > 0 ? (totalEvents / totalHours).toFixed(2) : '0',
      total_events: totalEvents.toString(),
      obstructive_apneas: sessions.reduce((sum, s) => sum + +(s.obstructive_apneas || 0), 0).toString(),
      central_apneas: sessions.reduce((sum, s) => sum + +(s.central_apneas || 0), 0).toString(),
      hypopneas: sessions.reduce((sum, s) => sum + +(s.hypopneas || 0), 0).toString(),
      reras: sessions.reduce((sum, s) => sum + +(s.reras || 0), 0).toString(),
      session_end: sessions.some(s => !s.session_end) ? null : sessions[sessions.length - 1].session_end,
      events: allEvents,
    } as SessionDetail;
  }

  private getTimestamps(): string[] {
    if (!this.selectedSignal) return [];
    if (this.selectedSignal.source === 'oximetry') return this.oximetryTimestamps;
    return this.selectedSignal.source === 'vitals' ? this.vitalsTimestamps : this.signalTimestamps;
  }

  private getLabels(): string[] {
    if (!this.selectedSignal) return [];
    if (this.selectedSignal.source === 'oximetry') return this.oximetryLabels;
    return this.selectedSignal.source === 'vitals' ? this.vitalsLabels : this.signalLabels;
  }

  private getData(key: string): (number | null)[] {
    if (!this.selectedSignal) return [];
    const src = this.selectedSignal.source === 'oximetry' ? this.oximetryData
      : this.selectedSignal.source === 'vitals' ? this.vitalsData : this.signalData;
    return (src as any)?.[key] || [];
  }

  private updateSliderWindow() {
    if (this.activeRange === 'all') return;
    const labels = this.getLabels();
    const range = this.activeRange as number;
    this.viewStart = this.sliderPos;
    this.viewEnd = Math.min(this.sliderPos + range, labels.length);
    this.sliderStartLabel = labels[this.viewStart] || '';
    this.sliderEndLabel = labels[this.viewEnd - 1] || '';
  }

  private renderDetailChart() {
    if (!this.selectedSignal || !this.detailCanvasRef?.nativeElement) return;
    this.detailChart?.destroy();

    const sig = this.selectedSignal;
    let labels = this.getLabels();
    let mainData = this.getData(sig.key);
    let timestamps = this.getTimestamps();
    let evtAnns: any[] = [];

    // Slice for range
    if (this.activeRange !== 'all') {
      const s = this.viewStart;
      const e = this.viewEnd;
      labels = labels.slice(s, e);
      mainData = mainData.slice(s, e);
      timestamps = timestamps.slice(s, e);
    }

    // Build datasets
    const datasets: ChartDataset<'line'>[] = [makeDataset(sig.title, mainData, sig.color, { fill: sig.fill })];

    if (sig.hasBand && sig.bandMinKey && sig.bandMaxKey) {
      let minD = this.getData(sig.bandMinKey);
      let maxD = this.getData(sig.bandMaxKey);
      if (this.activeRange !== 'all') {
        minD = minD.slice(this.viewStart, this.viewEnd);
        maxD = maxD.slice(this.viewStart, this.viewEnd);
      }
      datasets.push(...makeFillBand(sig.title, minD, maxD, sig.color));
    }

    // Event annotations
    if (sig.showEvents && this.events.length) {
      evtAnns = eventAnnotations(this.events, labels, timestamps);
    }

    const canvas = this.detailCanvasRef.nativeElement;

    this.detailChart = new Chart(canvas, {
      type: 'line',
      data: { labels, datasets },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: false,
        interaction: { mode: 'index', intersect: false },
        plugins: {
          legend: { display: false },
          tooltip: {
            backgroundColor: '#1e1e2f',
            titleColor: '#e0e0e0',
            bodyColor: '#ccc',
            borderColor: '#444',
            borderWidth: 1,
            callbacks: {
              title: (items) => {
                const idx = items[0]?.dataIndex;
                if (idx === undefined) return '';
                const ts = timestamps[idx];
                if (!ts) return labels[idx] || '';
                const d = new Date(ts.replace(' ', 'T'));
                return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
              }
            }
          },
          annotation: {
            annotations: evtAnns.reduce((acc: any, a: any, i: number) => {
              acc['evt' + i] = a;
              return acc;
            }, {}),
          },
          zoom: {
            pan: { enabled: true, mode: 'x' },
            zoom: {
              wheel: { enabled: true },
              pinch: { enabled: true },
              mode: 'x',
            },
          },
        },
        scales: {
          x: {
            ticks: {
              color: '#888',
              font: { size: 10 },
              maxRotation: 0,
              autoSkip: true,
              maxTicksLimit: 20,
            },
            grid: { color: '#2a2a3a' },
          },
          y: {
            min: sig.yMin,
            max: sig.yMax,
            ticks: { color: '#888', font: { size: 10 } },
            grid: { color: '#2a2a3a' },
          },
        },
      },
    });
  }

  private renderOverviewCharts() {
    this.overviewCharts.forEach(c => c.destroy());
    this.overviewCharts = [];

    for (const sig of this.availableSignals) {
      const canvas = document.getElementById('ov-' + sig.key) as HTMLCanvasElement;
      if (!canvas) continue;

      const labels = sig.source === 'oximetry' ? this.oximetryLabels
        : sig.source === 'vitals' ? this.vitalsLabels : this.signalLabels;
      const src = sig.source === 'oximetry' ? this.oximetryData
        : sig.source === 'vitals' ? this.vitalsData : this.signalData;
      const data = (src as any)?.[sig.key] || [];

      const chart = new Chart(canvas, {
        type: 'line',
        data: {
          labels,
          datasets: [{
            data: data as number[],
            borderColor: sig.color,
            backgroundColor: sig.color + '22',
            borderWidth: 1,
            pointRadius: 0,
            tension: 0.3,
            fill: true,
          }],
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          animation: false,
          plugins: { legend: { display: false }, tooltip: { enabled: false } },
          scales: {
            x: { display: false },
            y: { display: false, min: sig.yMin, max: sig.yMax },
          },
        },
      });
      this.overviewCharts.push(chart);
    }
  }

  private renderDoughnut() {
    if (!this.doughnutRef?.nativeElement || !this.session) return;
    const el = this.doughnutRef.nativeElement;

    const labels = ['Obstructive', 'Central', 'Hypopnea', 'RERA'];
    const data = [
      +(this.session.obstructive_apneas || 0),
      +(this.session.central_apneas || 0),
      +(this.session.hypopneas || 0),
      +(this.session.reras || 0),
    ];
    const colors = ['#f87171', '#fb923c', '#fbbf24', '#4ade80'];

    new Chart(el, {
      type: 'doughnut',
      data: {
        labels,
        datasets: [{ data, backgroundColor: colors, borderColor: '#1e1e2f', borderWidth: 2 }],
      },
      options: {
        responsive: true,
        plugins: {
          legend: { position: 'right', labels: { color: '#ccc', font: { size: 11 } } },
        },
      },
    });
  }
}
