import { Component, OnInit, OnDestroy, ChangeDetectorRef } from '@angular/core';
import { CommonModule } from '@angular/common';
import { RouterLink } from '@angular/router';
import { CpapApiService } from '../../services/cpap-api.service';
import { SessionListItem } from '../../models/session.model';

@Component({
  selector: 'app-sessions',
  standalone: true,
  imports: [CommonModule, RouterLink],
  template: `
    <div class="sessions-page">
      <h2>Sessions ({{ sessions.length }})</h2>
      <table class="session-table" *ngIf="sessions.length > 0">
        <thead>
          <tr>
            <th>Date</th><th>Status</th><th>Duration</th><th>AHI</th><th>Events</th>
            <th>OA</th><th>CA</th><th>H</th><th>RERA</th><th>SpO2</th>
          </tr>
        </thead>
        <tbody>
          <tr *ngFor="let s of sessions" [routerLink]="['/sessions', s.sleep_day || sleepDay(s.session_start)]"
              class="clickable" [class.live-row]="isLive(s)">
            <td>{{ s.sleep_day || sleepDay(s.session_start) }}</td>
            <td>
              <span *ngIf="isLive(s)" class="live-badge">LIVE</span>
              <span *ngIf="!isLive(s)" class="done-badge">Done</span>
            </td>
            <td>{{ fmtDuration(s.duration_hours) }}</td>
            <td [class.elevated]="+s.ahi > 5">{{ (+s.ahi).toFixed(1) || '-' }}</td>
            <td>{{ s.total_events || '-' }}</td>
            <td>{{ s.obstructive_apneas || '-' }}</td>
            <td>{{ s.central_apneas || '-' }}</td>
            <td>{{ s.hypopneas || '-' }}</td>
            <td>{{ s.reras || '-' }}</td>
            <td>
              <span *ngIf="s.avg_spo2 && +s.avg_spo2 > 0">{{ s.avg_spo2 }}%</span>
              <span *ngIf="!(s.avg_spo2 && +s.avg_spo2 > 0) && oxiMap[s.sleep_day || sleepDay(s.session_start)]">{{ oxiMap[s.sleep_day || sleepDay(s.session_start)] }}%</span>
              <span *ngIf="!(s.avg_spo2 && +s.avg_spo2 > 0) && !oxiMap[s.sleep_day || sleepDay(s.session_start)]">-</span>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  `,
  styles: [`
    .sessions-page { padding: 1.5rem; }
    h2 { color: #e0e0e0; margin-bottom: 1rem; }
    .session-table { width: 100%; border-collapse: collapse; }
    .session-table th { text-align: left; padding: 0.6rem; color: #888; border-bottom: 2px solid #333; font-size: 0.8rem; text-transform: uppercase; }
    .session-table td { padding: 0.6rem; color: #e0e0e0; border-bottom: 1px solid #2a2a3a; }
    .clickable { cursor: pointer; }
    .clickable:hover td { background: rgba(100,181,246,0.06); }
    .elevated { color: #ff8a65; font-weight: 600; }
    .live-row td { border-bottom-color: rgba(76,175,80,0.3); }
    .live-badge {
      background: #4caf50; color: #fff; padding: 0.15rem 0.5rem; border-radius: 10px;
      font-size: 0.65rem; font-weight: 700; letter-spacing: 0.5px;
      animation: pulse-live 1.5s ease-in-out infinite;
    }
    .done-badge { color: #666; font-size: 0.7rem; }
    @keyframes pulse-live {
      0%, 100% { opacity: 1; }
      50% { opacity: 0.5; }
    }
  `]
})
export class SessionsComponent implements OnInit, OnDestroy {
  sessions: SessionListItem[] = [];
  oxiMap: Record<string, string> = {};
  private refreshTimer: any = null;

  constructor(private api: CpapApiService, private cdr: ChangeDetectorRef) {}

  ngOnInit() {
    this.loadSessions();
    this.refreshTimer = setInterval(() => this.loadSessions(), 30000);
  }

  ngOnDestroy() {
    if (this.refreshTimer) clearInterval(this.refreshTimer);
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
}
