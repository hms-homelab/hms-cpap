import { Component, OnInit, OnDestroy, inject } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { TranslatePipe, TranslateService } from '@ngx-translate/core';
import { CpapApiService } from '../../services/cpap-api.service';
import { LanguageService } from '../../services/language.service';
import { usageTargetHoursOf } from '../dashboard/card-data';
import {
  ReportKind, RULE_DEFAULT_SHARE, SPANS, hoursLabel, localDate, nightOf, requestBody,
  ruleInRange, spanDates,
} from './report-form';

@Component({
  selector: 'app-reports',
  standalone: true,
  imports: [CommonModule, FormsModule, TranslatePipe],
  templateUrl: './reports.component.html',
  styleUrls: ['./reports.component.css']
})
export class ReportsComponent implements OnInit, OnDestroy {
  startDate = '';
  endDate = '';
  generating = false;
  generateError = '';
  reports: any[] = [];
  downloading: Record<number, boolean> = {};
  private refreshTimer: any = null;

  // SDD-082: CpapDash's three report kinds.
  kind: ReportKind = 'standard';
  readonly spans = SPANS;
  /** 30, 60 or 90 nights ending at the latest night; null once a date is typed. */
  complianceSpan: 30 | 60 | 90 | null = null;
  /** B5: the hours default to the usage target from Settings, the percent to 70. */
  ruleHours = 4;
  ruleShare = RULE_DEFAULT_SHARE;
  /** The latest therapy night, what the span choice ends on. */
  private latestNight = '';

  private translate = inject(TranslateService);
  private lang = inject(LanguageService);

  constructor(private api: CpapApiService) {}

  ngOnInit() {
    this.setDefaultDates();
    this.loadReports();
    this.refreshTimer = setInterval(() => this.loadReports(), 15000);
    this.api.getConfig().subscribe({
      next: (cfg) => { this.ruleHours = usageTargetHoursOf(cfg?.usage_target_hours); },
      error: () => {},
    });
    this.api.getDashboard().subscribe({
      next: (d: any) => { this.latestNight = nightOf(d?.latest_night?.sleep_day); },
      error: () => {},
    });
  }

  ngOnDestroy() {
    if (this.refreshTimer) clearInterval(this.refreshTimer);
  }

  private setDefaultDates() {
    const now = new Date();
    const y = now.getFullYear();
    const m = now.getMonth();
    const firstOfMonth = new Date(y, m, 1);
    this.startDate = firstOfMonth.toISOString().slice(0, 10);
    this.endDate = now.toISOString().slice(0, 10);
  }

  pickKind(k: ReportKind) {
    this.kind = k;
    // The compliance report opens on the last 30 nights, as CpapDash's does.
    if (k === 'compliance' && this.complianceSpan === null) this.pickSpan(30);
  }

  /** Fill the dates with the last N nights ending at the latest night. */
  pickSpan(n: 30 | 60 | 90) {
    const span = spanDates(n, this.latestNight || localDate(new Date()));
    this.startDate = span.start;
    this.endDate = span.end;
    this.complianceSpan = n;
  }

  /** A date typed by hand is no longer one of the three spans. */
  dateEdited() {
    this.complianceSpan = null;
  }

  get ruleHoursLabel(): string {
    return hoursLabel(Number(this.ruleHours));
  }

  loadReports() {
    this.api.listReports().subscribe({
      next: r => this.reports = r,
      error: () => {}
    });
  }

  generate() {
    if (!this.startDate || !this.endDate) return;
    this.generateError = '';
    if (this.kind === 'compliance' && !ruleInRange(Number(this.ruleHours), Number(this.ruleShare))) {
      this.generateError = this.translate.instant('reports.messages.ruleOutOfRange');
      return;
    }
    this.generating = true;
    const extra = requestBody(this.kind, Number(this.ruleHours), Number(this.ruleShare), this.lang.current());
    this.api.generateReport(this.startDate, this.endDate, extra).subscribe({
      next: () => {
        this.generating = false;
        this.loadReports();
      },
      error: (e) => {
        this.generating = false;
        // The server's own words for a refused rule; the generic line otherwise.
        this.generateError = e?.error?.error || this.translate.instant('reports.messages.generateFailed');
      }
    });
  }

  download(report: any) {
    if (report.status !== 'ready') return;
    this.downloading[report.id] = true;
    const a = document.createElement('a');
    a.href = this.api.downloadReportUrl(report.id);
    a.download = report.filename || `cpap_report_${report.id}.pdf`;
    a.click();
    setTimeout(() => delete this.downloading[report.id], 2000);
  }

  /**
   * SDD-080: returns a translation KEY, not a label. The template pipes it
   * through `| translate`.
   *
   * An unknown status falls back to the raw server string rather than to a key
   * that does not exist, so a status the frontend has not been taught yet still
   * renders as itself instead of as "reports.status.whatever".
   */
  private static readonly KNOWN_STATUSES = ['pending', 'generating', 'ready', 'error'];

  statusKey(s: string): string {
    return ReportsComponent.KNOWN_STATUSES.includes(s) ? `reports.status.${s}` : s;
  }

  fmtDate(dt: string): string {
    if (!dt) return '—';
    return dt.slice(0, 16).replace('T', ' ');
  }

  isGenerating(r: any): boolean {
    return r.status === 'pending' || r.status === 'generating';
  }
}
