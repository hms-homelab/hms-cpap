import { Component, OnInit, OnDestroy, ChangeDetectorRef, HostListener } from '@angular/core';
import { CommonModule } from '@angular/common';
import { RouterLink } from '@angular/router';
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
  actionInProgress: Record<string, boolean> = {};
  actionMessage: Record<string, string> = {};
  openMenu: string | null = null;
  private refreshTimer: any = null;

  constructor(private api: CpapApiService, private cdr: ChangeDetectorRef) {}

  ngOnInit() {
    this.loadSessions();
    this.refreshTimer = setInterval(() => this.loadSessions(), 30000);
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
    this.api.getSessions(30, 30).subscribe({
      next: s => {
        this.sessions = s;
        this.loadOximetryForSessions(s);
        this.cdr.detectChanges();
      },
      error: e => console.error('Sessions error:', e)
    });
  }

  private loadOximetryForSessions(sessions: SessionListItem[]) {
    const noSpo2 = sessions.filter(s => !(s.avg_spo2 && +s.avg_spo2 > 0));
    // Only fetch for recent sessions without machine SpO2 (limit to 7 to avoid spam)
    const toFetch = noSpo2.slice(0, 7);
    for (const s of toFetch) {
      const day = s.sleep_day || this.sleepDay(s.session_start);
      if (this.oxiMap[day] !== undefined) continue;
      this.oxiMap[day] = '';
      this.api.getSessionOximetry(day).subscribe({
        next: (oxi) => {
          if (oxi?.spo2?.length) {
            const valid = oxi.spo2.map(v => Number(v)).filter(v => !isNaN(v) && v > 0 && v < 255);
            if (valid.length) {
              this.oxiMap[day] = (valid.reduce((a, b) => a + b, 0) / valid.length).toFixed(1);
              this.cdr.detectChanges();
            }
          }
        },
        error: () => {},
      });
    }
  }

  isRowBusy(s: any): boolean {
    const day = s.sleep_day || this.sleepDay(s.session_start);
    return !!(this.actionInProgress[day] || this.actionInProgress[day + '_sum'] ||
              this.actionInProgress[day + '_rep'] || this.actionInProgress[day + '_oxi']);
  }

  rowMessage(s: any): string {
    const day = s.sleep_day || this.sleepDay(s.session_start);
    return this.actionMessage[day] || this.actionMessage[day + '_sum'] ||
           this.actionMessage[day + '_rep'] || this.actionMessage[day + '_oxi'] || '';
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
        this.loadSessions();
      },
      error: () => {
        this.actionInProgress[day + '_oxi'] = false;
      }
    });
  }
}
