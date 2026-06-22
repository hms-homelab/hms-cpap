import { Component, OnInit, OnDestroy, ChangeDetectorRef, HostListener } from '@angular/core';
import { CommonModule } from '@angular/common';
import { Router, RouterLink } from '@angular/router';
import { CpapApiService } from '../../services/cpap-api.service';
import { SessionListItem } from '../../models/session.model';

@Component({
  selector: 'app-sessions',
  standalone: true,
  imports: [CommonModule, RouterLink],
  templateUrl: './sessions.component.html',
  styleUrls: ['./sessions.component.css']
})
export class SessionsComponent implements OnInit, OnDestroy {
  sessions: SessionListItem[] = [];
  oxiMap: Record<string, string> = {};
  hrMap: Record<string, string> = {};
  actionInProgress: Record<string, boolean> = {};
  actionMessage: Record<string, string> = {};
  openMenu: string | null = null;
  sleephqEnabled = false;   // gates the per-session "Upload to SleepHQ" action
  private refreshTimer: any = null;

  // Recency-based pagination: load the latest `pageSize` nights, then "Load more"
  // walks back through history a page at a time.
  readonly pageSize = 20;
  hasMore = true;
  loadingMore = false;

  // Compare-nights selection.
  compareMode = false;
  selectedDays: string[] = [];

  constructor(private api: CpapApiService, private cdr: ChangeDetectorRef, private router: Router) {}

  dayOf(s: SessionListItem): string {
    return s.sleep_day || this.sleepDay(s.session_start);
  }

  toggleCompareMode(): void {
    this.compareMode = !this.compareMode;
    this.selectedDays = [];
    this.openMenu = null;
  }

  isSelected(s: SessionListItem): boolean {
    return this.selectedDays.includes(this.dayOf(s));
  }

  toggleSelect(s: SessionListItem): void {
    const d = this.dayOf(s);
    const i = this.selectedDays.indexOf(d);
    if (i >= 0) this.selectedDays.splice(i, 1);
    else if (this.selectedDays.length < 2) this.selectedDays.push(d);
  }

  openCompare(): void {
    if (this.selectedDays.length === 2) {
      this.router.navigate(['/compare', this.selectedDays[0], this.selectedDays[1]]);
    }
  }

  onRow(s: SessionListItem): void {
    if (this.compareMode) this.toggleSelect(s);
    else this.router.navigate(['/sessions', this.dayOf(s)]);
  }

  ngOnInit() {
    this.loadSessions();
    this.refreshTimer = setInterval(() => this.loadSessions(), 30000);
    this.api.getConfig().subscribe({
      next: (cfg) => this.sleephqEnabled = !!cfg.sleephq?.enabled,
      error: () => {},
    });
  }

  ngOnDestroy() {
    if (this.refreshTimer) clearInterval(this.refreshTimer);
  }

  @HostListener('document:click')
  onDocumentClick() {
    this.openMenu = null;
  }

  toggleMenu(event: Event, day: string): void {
    event.stopPropagation();
    this.openMenu = this.openMenu === day ? null : day;
  }

  private loadSessions() {
    // Refresh keeps however many pages are already loaded so the 30s poll
    // doesn't snap the list back to the first page.
    const limit = Math.max(this.pageSize, this.sessions.length);
    this.api.getSessions(limit, 0).subscribe({
      next: s => {
        this.sessions = s;
        this.hasMore = s.length === limit;
        this.loadOximetryForSessions(s);
        this.cdr.detectChanges();
      },
      error: e => console.error('Sessions error:', e)
    });
  }

  loadMore() {
    if (this.loadingMore || !this.hasMore) return;
    this.loadingMore = true;
    this.api.getSessions(this.pageSize, this.sessions.length).subscribe({
      next: s => {
        this.sessions = [...this.sessions, ...s];
        this.hasMore = s.length === this.pageSize;
        this.loadingMore = false;
        this.loadOximetryForSessions(this.sessions);
        this.cdr.detectChanges();
      },
      error: e => {
        console.error('Load more error:', e);
        this.loadingMore = false;
      }
    });
  }

  private loadOximetryForSessions(sessions: SessionListItem[]) {
    // Fetch for all recent sessions (limit to 7) to get HR even when machine has SpO2
    const toFetch = sessions.slice(0, 7);
    for (const s of toFetch) {
      const day = s.sleep_day || this.sleepDay(s.session_start);
      if (this.hrMap[day] !== undefined) continue;
      const hasMachineSpo2 = s.avg_spo2 && +s.avg_spo2 > 0;
      if (!hasMachineSpo2) this.oxiMap[day] = '';
      this.hrMap[day] = '';
      this.api.getSessionOximetry(day).subscribe({
        next: (oxi) => {
          if (!hasMachineSpo2 && oxi?.spo2?.length) {
            const valid = oxi.spo2.map(v => Number(v)).filter(v => !isNaN(v) && v > 0 && v < 255);
            if (valid.length) {
              this.oxiMap[day] = (valid.reduce((a, b) => a + b, 0) / valid.length).toFixed(1);
            }
          }
          if (oxi?.heart_rate?.length) {
            const valid = (oxi.heart_rate as any[]).map(v => Number(v)).filter(v => !isNaN(v) && v > 20 && v < 250);
            if (valid.length) {
              this.hrMap[day] = Math.round(valid.reduce((a, b) => a + b, 0) / valid.length).toString();
            }
          }
          this.cdr.detectChanges();
        },
        error: () => {},
      });
    }
  }

  isRowBusy(s: any): boolean {
    const day = s.sleep_day || this.sleepDay(s.session_start);
    return !!(this.actionInProgress[day] || this.actionInProgress[day + '_sum'] ||
              this.actionInProgress[day + '_rep'] || this.actionInProgress[day + '_oxi'] ||
              this.actionInProgress[day + '_pdf'] || this.actionInProgress[day + '_shq']);
  }

  rowMessage(s: any): string {
    const day = s.sleep_day || this.sleepDay(s.session_start);
    return this.actionMessage[day] || this.actionMessage[day + '_sum'] ||
           this.actionMessage[day + '_rep'] || this.actionMessage[day + '_oxi'] ||
           this.actionMessage[day + '_pdf'] || this.actionMessage[day + '_shq'] || '';
  }

  isLive(s: any): boolean {
    return s.has_live === 't' || s.has_live === '1' || s.has_live === true || !s.session_end;
  }

  fmtDuration(val: string | number | undefined): string {
    const hours = +(val || 0);
    if (hours <= 0) return '0m';
    const totalMins = Math.round(hours * 60);
    const h = Math.floor(totalMins / 60);
    const m = totalMins % 60;
    return h > 0 ? `${h}h ${String(m).padStart(2, '0')}m` : `${m}m`;
  }

  sleepDay(sessionStart: string): string {
    if (!sessionStart) return '';
    const d = new Date(sessionStart.replace(' ', 'T'));
    if (isNaN(d.getTime())) return sessionStart.slice(0, 10);
    d.setHours(d.getHours() - 12);
    return d.toISOString().slice(0, 10);
  }

  forceComplete(event: Event, s: any): void {
    event.stopPropagation();
    const day = s.sleep_day || this.sleepDay(s.session_start);
    this.openMenu = null;
    this.actionInProgress[day] = true;
    this.api.forceCompleteSession(day).subscribe({
      next: (r) => {
        this.actionMessage[day] = r.status === 'completed' ? 'Completed' : 'Already done';
        this.actionInProgress[day] = false;
        setTimeout(() => this.loadSessions(), 2000);
      },
      error: () => {
        this.actionMessage[day] = 'Failed';
        this.actionInProgress[day] = false;
      }
    });
  }

  generateSummary(event: Event, s: any): void {
    event.stopPropagation();
    const day = s.sleep_day || this.sleepDay(s.session_start);
    this.openMenu = null;
    this.actionInProgress[day + '_sum'] = true;
    this.api.generateSessionSummary(day).subscribe({
      next: () => {
        this.actionMessage[day + '_sum'] = 'Queued';
        this.actionInProgress[day + '_sum'] = false;
      },
      error: () => {
        this.actionMessage[day + '_sum'] = 'Failed';
        this.actionInProgress[day + '_sum'] = false;
      }
    });
  }

  reparse(event: Event, s: any): void {
    event.stopPropagation();
    const day = s.sleep_day || this.sleepDay(s.session_start);
    this.openMenu = null;
    this.actionInProgress[day + '_rep'] = true;
    this.api.reparseSession(day).subscribe({
      next: () => {
        this.actionInProgress[day + '_rep'] = false;
        setTimeout(() => this.loadSessions(), 2000);
      },
      error: () => {
        this.actionInProgress[day + '_rep'] = false;
      }
    });
  }

  downloadDayPdf(event: Event, s: any): void {
    event.stopPropagation();
    const day = s.sleep_day || this.sleepDay(s.session_start);
    this.openMenu = null;
    const key = day + '_pdf';
    this.actionInProgress[key] = true;
    this.api.generateReport(day, day).subscribe({
      next: (r) => {
        this.pollAndDownload(r.report_id, key);
      },
      error: () => {
        this.actionMessage[key] = 'Failed';
        this.actionInProgress[key] = false;
      }
    });
  }

  private pollAndDownload(reportId: number, key: string, attempts = 0): void {
    this.api.getReportStatus(reportId).subscribe({
      next: (r) => {
        if (r.status === 'ready') {
          this.actionInProgress[key] = false;
          const a = document.createElement('a');
          a.href = this.api.downloadReportUrl(reportId);
          a.download = r.filename || `cpap_report_${reportId}.pdf`;
          a.click();
        } else if (r.status === 'error') {
          this.actionMessage[key] = 'Failed';
          this.actionInProgress[key] = false;
        } else if (attempts < 30) {
          setTimeout(() => this.pollAndDownload(reportId, key, attempts + 1), 2000);
        } else {
          this.actionMessage[key] = 'Timeout';
          this.actionInProgress[key] = false;
        }
      },
      error: () => {
        this.actionMessage[key] = 'Failed';
        this.actionInProgress[key] = false;
      }
    });
  }

  fetchOximetry(event: Event, s: any): void {
    event.stopPropagation();
    const day = s.sleep_day || this.sleepDay(s.session_start);
    this.openMenu = null;
    this.actionInProgress[day + '_oxi'] = true;
    this.api.collectOximetry().subscribe({
      next: () => {
        this.actionInProgress[day + '_oxi'] = false;
        // Reload oximetry for this row
        delete this.oxiMap[day];
        delete this.hrMap[day];
        this.loadSessions();
      },
      error: () => {
        this.actionInProgress[day + '_oxi'] = false;
      }
    });
  }

  uploadToSleepHq(event: Event, s: any): void {
    event.stopPropagation();
    const day = s.sleep_day || this.sleepDay(s.session_start);
    this.openMenu = null;
    const key = day + '_shq';
    this.actionInProgress[key] = true;
    // hms-cpap stores DATALOG folders as YYYYMMDD; the API takes that form.
    const folder = day.replace(/-/g, '');
    this.api.exportSleepHq(folder).subscribe({
      next: () => {
        this.actionMessage[key] = 'Queued';
        this.actionInProgress[key] = false;
      },
      error: () => {
        this.actionMessage[key] = 'Failed';
        this.actionInProgress[key] = false;
      }
    });
  }
}
