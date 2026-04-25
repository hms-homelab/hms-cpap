import { Component, OnInit, ViewChild, ElementRef, AfterViewInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { CpapApiService } from '../../services/cpap-api.service';
import { KeyMetricsComponent, KeyMetricsData } from '../../components/dashboard/key-metrics.component';
import { OximetryRowComponent, OximetryRowData } from '../../components/dashboard/oximetry-row.component';
import { AiSummaryComponent } from '../../components/dashboard/ai-summary.component';
import { TherapyInsightsComponent } from '../../components/dashboard/therapy-insights.component';
import { StrMetricsComponent, StrMetricsData } from '../../components/dashboard/str-metrics.component';
import { EventsBreakdownComponent, EventsBreakdownData } from '../../components/dashboard/events-breakdown.component';
import { PressureSectionComponent, PressureSectionData } from '../../components/dashboard/pressure-section.component';
import { RespiratoryMetricsComponent, RespiratoryMetricsData } from '../../components/dashboard/respiratory-metrics.component';
import { RealtimeStatusComponent, RealtimeStatusData } from '../../components/dashboard/realtime-status.component';
import { MlIntelligenceComponent } from '../../components/dashboard/ml-intelligence.component';
import { DashboardData, TrendPoint, OximetryData, SessionListItem } from '../../models/session.model';
import Chart from 'chart.js/auto';

const MODE_LABELS: Record<string, string> = {
  '0': 'CPAP', '1': 'APAP', '7': 'ASV', '8': 'ASVAuto',
};

@Component({
  selector: 'app-dashboard',
  standalone: true,
  imports: [CommonModule, KeyMetricsComponent, OximetryRowComponent, AiSummaryComponent,
            TherapyInsightsComponent, StrMetricsComponent, EventsBreakdownComponent,
            PressureSectionComponent, RespiratoryMetricsComponent, RealtimeStatusComponent,
            MlIntelligenceComponent],
  template: `
    <div class="dashboard">
      <h2>ResMed AirSense 10 Sleep Therapy</h2>
      <div class="dash-subtitle" *ngIf="data">Last Session - {{ formatDate(data.latest_night.date) }}</div>

      <!-- Live Session Banner -->
      <div class="live-banner" *ngIf="liveSession">
        <div class="live-indicator"><span class="live-dot"></span> LIVE SESSION</div>
        <div class="live-gauges">
          <div class="gauge-container">
            <canvas #ahiGauge width="140" height="140"></canvas>
            <div class="gauge-label">AHI</div>
          </div>
          <div class="gauge-container">
            <canvas #durationGauge width="140" height="140"></canvas>
            <div class="gauge-label">Duration</div>
          </div>
          <div class="gauge-container" *ngIf="liveSpO2">
            <canvas #spo2Gauge width="140" height="140"></canvas>
            <div class="gauge-label">SpO2</div>
          </div>
          <div class="gauge-container" *ngIf="liveHR">
            <canvas #hrGauge width="140" height="140"></canvas>
            <div class="gauge-label">Heart Rate</div>
          </div>
          <div class="gauge-container">
            <canvas #eventsPie width="140" height="140"></canvas>
            <div class="gauge-label">Events ({{liveSession.total_events}})</div>
          </div>
        </div>
      </div>

      <!-- 1. Key Metrics -->
      <app-key-metrics [data]="keyMetrics" />

      <!-- 2. O2 Ring Oximetry -->
      <app-oximetry-row [data]="oximetryData" *ngIf="oximetryData" />

      <!-- Two-column layout for Summary + Insights -->
      <div class="two-col" *ngIf="aiSummaryText || insights.length">
        <!-- 3. AI Session Summary -->
        <app-ai-summary [summaryText]="aiSummaryText" *ngIf="aiSummaryText" />
        <!-- 4. Therapy Insights -->
        <app-therapy-insights [insights]="insights" *ngIf="insights.length" />
      </div>

      <!-- 5. STR Daily Metrics -->
      <app-str-metrics [data]="strMetrics" *ngIf="strMetrics" />

      <!-- 6. Sleep Events Breakdown -->
      <app-events-breakdown [data]="eventsData" *ngIf="eventsData" />

      <!-- 7. Therapy Pressure -->
      <app-pressure-section [data]="pressureData" *ngIf="pressureData" />

      <!-- 8. Respiratory Metrics -->
      <app-respiratory-metrics [data]="respiratoryData" *ngIf="respiratoryData" />

      <!-- 9. Real-Time Status -->
      <app-realtime-status [data]="realtimeData" *ngIf="realtimeData" />

      <!-- 10. ML Intelligence -->
      <app-ml-intelligence [mlStatus]="mlStatus" [mlPredictions]="mlPredictions"
        (runInference)="runInference()" [inferring]="inferring" *ngIf="mlStatus?.models_loaded" />

      <!-- 11. Historical Charts -->
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
    .dashboard { padding: 1.5rem; max-width: 1200px; margin: 0 auto; }
    h2 { color: #e0e0e0; margin-bottom: 0.25rem; font-size: 1.3rem; }
    .dash-subtitle { color: #888; font-size: 0.85rem; margin-bottom: 1.25rem; }
    .live-banner { background: #1a2a1a; border: 1px solid #2d5a2d; border-radius: 10px; padding: 1rem 1.5rem; margin-bottom: 1.5rem; }
    .live-indicator { display: flex; align-items: center; gap: 0.5rem; color: #4ade80; font-weight: 700; font-size: 0.85rem; letter-spacing: 0.1em; margin-bottom: 0.75rem; }
    .live-dot { width: 10px; height: 10px; border-radius: 50%; background: #4ade80; animation: pulse-live 1.5s ease-in-out infinite; }
    @keyframes pulse-live { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }
    .live-gauges { display: flex; gap: 1.5rem; flex-wrap: wrap; justify-content: center; }
    .gauge-container { display: flex; flex-direction: column; align-items: center; }
    .gauge-label { color: #888; font-size: 0.75rem; margin-top: 6px; text-transform: uppercase; letter-spacing: 0.05em; }
    .two-col { display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; margin-bottom: 1.5rem; }
    @media (max-width: 900px) { .two-col { grid-template-columns: 1fr; } }
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

  @ViewChild('ahiGauge') ahiGaugeRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('durationGauge') durationGaugeRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('spo2Gauge') spo2GaugeRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('hrGauge') hrGaugeRef!: ElementRef<HTMLCanvasElement>;
  @ViewChild('eventsPie') eventsPieRef!: ElementRef<HTMLCanvasElement>;

  data: DashboardData | null = null;
  liveSession: SessionListItem | null = null;
  liveSpO2 = '';
  liveHR = '';
  modeName = '';
  isCpapMode = true;
  private charts: Chart[] = [];
  private liveCharts: Chart[] = [];

  // Section data
  keyMetrics: KeyMetricsData | null = null;
  oximetryData: OximetryRowData | null = null;
  aiSummaryText = '';
  insights: any[] = [];
  strMetrics: StrMetricsData | null = null;
  eventsData: EventsBreakdownData | null = null;
  pressureData: PressureSectionData | null = null;
  respiratoryData: RespiratoryMetricsData | null = null;
  realtimeData: RealtimeStatusData | null = null;

  // ML state
  mlStatus: any = null;
  mlPredictions: any = null;
  inferring = false;

  constructor(private api: CpapApiService) {}

  fmtDuration(val: string | number | undefined): string {
    const hours = +(val || 0);
    if (hours <= 0) return '0m';
    const totalMins = Math.round(hours * 60);
    const h = Math.floor(totalMins / 60);
    const m = totalMins % 60;
    return h > 0 ? `${h}h ${String(m).padStart(2, '0')}m` : `${m}m`;
  }

  formatDate(dateStr: string): string {
    if (!dateStr) return '';
    const d = new Date(dateStr + 'T12:00:00');
    return d.toLocaleDateString('en-US', { month: 'short', day: 'numeric' });
  }

  ngOnInit() {
    // Check for live session + O2Ring data
    this.api.getRealtime().subscribe(rt => {
      if (rt.session) {
        this.liveSession = rt.session;
      }
      if (rt.oximetry?.active) {
        this.liveSpO2 = rt.oximetry.spo2 > 0 ? String(rt.oximetry.spo2) : '';
        this.liveHR = rt.oximetry.hr > 0 ? String(rt.oximetry.hr) : '';
      }
      this.realtimeData = {
        sessionStatus: rt.session ? 'active' : 'completed',
        sessionDuration: rt.session?.duration_hours || '',
        lastSessionTime: '',
        currentPressure: rt.session?.current_pressure || 0,
        minPressure: rt.session?.min_pressure || 0,
        maxPressure: rt.session?.max_pressure || 0,
      };
      setTimeout(() => this.renderLiveCharts(), 50);
    });

    this.api.getDashboard().subscribe(d => {
      this.data = d;
      const mode = d.latest_night.therapy_mode || '0';
      this.modeName = MODE_LABELS[mode] || 'Unknown';
      this.isCpapMode = mode === '0';

      // Populate key metrics (initial from STR, overridden by sessions below)
      this.keyMetrics = {
        ahi: parseFloat(d.latest_night.ahi) || 0,
        usageHours: parseFloat(d.latest_night.usage_hours) || 0,
        leakP95: parseFloat(d.latest_night.leak_avg) || 0,
        totalEvents: 0,
        compliancePct: parseFloat(d.latest_night.compliance_pct) || 0,
        sessionActive: !!this.liveSession,
        mode: this.modeName,
      };

      // Fetch session-aggregated data to match sessions table values
      this.api.getSessions(7, 1).subscribe({
        next: (sessions) => {
          if (sessions?.length && this.keyMetrics) {
            const s = sessions[0];
            this.keyMetrics.ahi = parseFloat(s.ahi) || this.keyMetrics.ahi;
            this.keyMetrics.usageHours = parseFloat(s.duration_hours) || this.keyMetrics.usageHours;
            this.keyMetrics.totalEvents = parseInt(s.total_events) || 0;
          }
        },
        error: () => {},
      });

      // Fetch STR daily summary for latest night
      if (d.latest_night.date) {
        this.api.getDailySummaryForDate(d.latest_night.date).subscribe({
          next: (rows) => {
            if (rows?.length) {
              const r = rows[0];
              this.strMetrics = {
                ahi: parseFloat(r.ahi) || 0,
                usageHours: (parseFloat(r.duration_minutes) || 0) / 60,
                leakP95: parseFloat(r.leak_95) || 0,
                oai: parseFloat(r.oai) || 0,
                cai: parseFloat(r.cai) || 0,
                hi: parseFloat(r.hi) || 0,
                rin: parseFloat(r.rin) || 0,
              };
              const oa = Math.round((parseFloat(r.oai) || 0) * (parseFloat(r.duration_minutes) || 0) / 60);
              const ca = Math.round((parseFloat(r.cai) || 0) * (parseFloat(r.duration_minutes) || 0) / 60);
              const hy = Math.round((parseFloat(r.hi) || 0) * (parseFloat(r.duration_minutes) || 0) / 60);
              const re = Math.round((parseFloat(r.rin) || 0) * (parseFloat(r.duration_minutes) || 0) / 60);
              this.eventsData = {
                obstructive: oa, central: ca, hypopneas: hy, reras: re,
                totalEvents: oa + ca + hy + re,
                maxEventDuration: 0, avgEventDuration: 0,
              };
              if (this.keyMetrics) {
                this.keyMetrics.totalEvents = oa + ca + hy + re;
                this.keyMetrics.leakP95 = parseFloat(r.leak_95) || 0;
              }
              this.pressureData = {
                avgPressure: parseFloat(r.mask_press_50) || 0,
                p95Pressure: parseFloat(r.mask_press_95) || 0,
                p50Pressure: parseFloat(r.mask_press_50) || 0,
                maxPressure: parseFloat(r.mask_press_max) || 0,
                leakP95: parseFloat(r.leak_95) || 0,
                currentPressure: 0,
              };
              this.respiratoryData = {
                respRate: parseFloat(r.resp_rate_50) || 0,
                tidalVolume: (parseFloat(r.tid_vol_50) || 0) * 1000,
                minuteVent: parseFloat(r.min_vent_50) || 0,
                inspiratoryTime: 0, expiratoryTime: 0, ieRatio: 0,
                flowLimitation: 0, avgFlowRate: 0, currentFlowRate: 0,
              };
              if (this.realtimeData) {
                this.realtimeData.lastSessionTime = d.latest_night.date;
              }
            }
          },
          error: () => {},
        });

        // Fetch O2Ring oximetry for latest night
        this.api.getSessionOximetry(d.latest_night.date).subscribe({
          next: (oxi) => {
            if (oxi?.spo2?.length) {
              const validSpo2 = oxi.spo2.map(v => Number(v)).filter(v => !isNaN(v) && v > 0 && v < 255);
              const validHr = oxi.heart_rate.map(v => Number(v)).filter(v => !isNaN(v) && v > 0 && v < 255);
              if (validSpo2.length) {
                const avg = validSpo2.reduce((a, b) => a + b, 0) / validSpo2.length;
                let odi = 0;
                const hours = validSpo2.length * 4 / 3600;
                if (hours > 0) {
                  let desatCount = 0, inDesat = false;
                  for (let i = 0; i < validSpo2.length; i++) {
                    let baseline = validSpo2[i];
                    const start = Math.max(0, i - 30);
                    for (let j = start; j < i; j++) {
                      if (validSpo2[j] > baseline) baseline = validSpo2[j];
                    }
                    const drop = baseline - validSpo2[i];
                    if (drop >= 3 && !inDesat) { desatCount++; inDesat = true; }
                    else if (drop < 1) { inDesat = false; }
                  }
                  odi = desatCount / hours;
                }
                const avgHr = validHr.length
                  ? validHr.reduce((a, b) => a + b, 0) / validHr.length : 0;
                this.oximetryData = {
                  spo2: avg, heartRate: Math.round(avgHr), odi,
                  active: false,
                };
              }
            }
          },
          error: () => {},
        });
      }

      setTimeout(() => {
        if (this.ahiChartRef) this.renderCharts();
      }, 50);
    });

    // Load AI summary
    this.api.getLatestSummary().subscribe({
      next: (rows) => {
        if (rows?.length && rows[0].summary_text) {
          this.aiSummaryText = rows[0].summary_text;
        }
      },
      error: () => {},
    });

    // Load therapy insights
    this.api.getInsights().subscribe({
      next: (items) => { this.insights = items || []; },
      error: () => {},
    });

    // Load ML status + predictions
    this.api.getMlStatus().subscribe({
      next: (s) => {
        this.mlStatus = s;
        if (s.models_loaded) {
          this.mlPredictions = s.predictions || null;
        }
      },
      error: () => {},
    });
  }

  runInference(): void {
    this.inferring = true;
    // Trigger training (which also runs inference after)
    // For pure inference, we use the same train trigger — it will just predict if models exist
    this.api.triggerMlTraining().subscribe({
      next: () => {
        // Poll for updated predictions
        setTimeout(() => {
          this.api.getMlStatus().subscribe({
            next: (s) => {
              this.mlStatus = s;
              this.mlPredictions = s.predictions || this.mlPredictions;
              this.inferring = false;
            },
            error: () => { this.inferring = false; },
          });
        }, 5000);
      },
      error: () => { this.inferring = false; },
    });
  }

  ngAfterViewInit() {
    if (this.data) this.renderCharts();
  }

  private renderLiveCharts() {
    if (!this.liveSession) return;
    this.liveCharts.forEach(c => c.destroy());
    this.liveCharts = [];

    const makeGauge = (ref: ElementRef<HTMLCanvasElement> | undefined, value: number, max: number, color: string, unit: string) => {
      if (!ref) return;
      const pct = Math.min(value / max, 1);
      const chart = new Chart(ref.nativeElement, {
        type: 'doughnut',
        data: {
          datasets: [{
            data: [pct * 100, 100 - pct * 100],
            backgroundColor: [color, '#2a2a3a'],
            borderWidth: 0,
          }]
        },
        options: {
          cutout: '75%',
          rotation: -90,
          circumference: 180,
          responsive: false,
          plugins: { legend: { display: false }, tooltip: { enabled: false } },
        },
        plugins: [{
          id: 'gaugeText',
          afterDraw: (chart: any) => {
            const ctx = chart.ctx;
            ctx.save();
            ctx.font = 'bold 22px system-ui';
            ctx.fillStyle = color;
            ctx.textAlign = 'center';
            ctx.fillText(value % 1 === 0 ? String(value) : value.toFixed(1), chart.width / 2, chart.height - 30);
            ctx.font = '11px system-ui';
            ctx.fillStyle = '#888';
            ctx.fillText(unit, chart.width / 2, chart.height - 15);
            ctx.restore();
          }
        }]
      });
      this.liveCharts.push(chart);
    };

    const ahi = parseFloat(this.liveSession.ahi || '0');
    const hours = parseFloat(this.liveSession.duration_hours || '0');
    const ahiColor = ahi < 5 ? '#4ade80' : ahi < 15 ? '#fb923c' : '#ef4444';
    // Duration thresholds: <4h red (below insurance compliance min),
    // 4–6h yellow (compliant but short), >=6h green.
    const durationColor = hours < 4 ? '#ef4444' : hours < 6 ? '#fbbf24' : '#4ade80';
    makeGauge(this.ahiGaugeRef, ahi, 10, ahiColor, 'events/h');
    makeGauge(this.durationGaugeRef, hours, 10, durationColor, this.fmtDuration(hours));

    if (this.liveSpO2) {
      const spo2 = parseInt(this.liveSpO2);
      const spo2Color = spo2 >= 95 ? '#4ade80' : spo2 >= 90 ? '#fb923c' : '#ef4444';
      makeGauge(this.spo2GaugeRef, spo2, 100, spo2Color, '%');
    }
    if (this.liveHR) {
      makeGauge(this.hrGaugeRef, parseInt(this.liveHR), 120, '#667eea', 'bpm');
    }

    // Events pie
    if (this.eventsPieRef) {
      const oa = parseInt(this.liveSession.obstructive_apneas || '0');
      const ca = parseInt(this.liveSession.central_apneas || '0');
      const h = parseInt(this.liveSession.hypopneas || '0');
      const r = parseInt(this.liveSession.reras || '0');
      const total = oa + ca + h + r;
      if (total > 0) {
        const chart = new Chart(this.eventsPieRef.nativeElement, {
          type: 'doughnut',
          data: {
            labels: ['OA', 'CA', 'Hypopnea', 'RERA'],
            datasets: [{
              data: [oa, ca, h, r],
              backgroundColor: ['#dc6b6b', '#c9966b', '#c9b96b', '#6b8dc9'],
              borderWidth: 0,
            }]
          },
          options: {
            responsive: false,
            plugins: {
              legend: { display: false },
              tooltip: { enabled: true },
            },
          },
          plugins: [{
            id: 'pieCenter',
            afterDraw: (chart: any) => {
              const ctx = chart.ctx;
              ctx.save();
              ctx.font = 'bold 20px system-ui';
              ctx.fillStyle = '#e0e0e0';
              ctx.textAlign = 'center';
              ctx.fillText(String(total), chart.width / 2, chart.height / 2 + 6);
              ctx.restore();
            }
          }]
        });
        this.liveCharts.push(chart);
      } else {
        // No events - show a "clean" indicator
        const chart = new Chart(this.eventsPieRef.nativeElement, {
          type: 'doughnut',
          data: {
            datasets: [{
              data: [1],
              backgroundColor: ['#4ade80'],
              borderWidth: 0,
            }]
          },
          options: {
            responsive: false,
            plugins: { legend: { display: false }, tooltip: { enabled: false } },
          },
          plugins: [{
            id: 'cleanText',
            afterDraw: (chart: any) => {
              const ctx = chart.ctx;
              ctx.save();
              ctx.font = 'bold 16px system-ui';
              ctx.fillStyle = '#4ade80';
              ctx.textAlign = 'center';
              ctx.fillText('0', chart.width / 2, chart.height / 2 + 6);
              ctx.restore();
            }
          }]
        });
        this.liveCharts.push(chart);
      }
    }
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
