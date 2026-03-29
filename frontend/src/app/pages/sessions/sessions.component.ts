import { Component, OnInit, ChangeDetectorRef } from '@angular/core';
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
            <th>Date</th><th>Duration</th><th>AHI</th><th>Events</th>
            <th>OA</th><th>CA</th><th>H</th><th>RERA</th><th>SpO2</th>
          </tr>
        </thead>
        <tbody>
          <tr *ngFor="let s of sessions" [routerLink]="['/sessions', sleepDay(s.session_start)]" class="clickable">
            <td>{{ sleepDay(s.session_start) }}</td>
            <td>{{ s.duration_hours }}h</td>
            <td [class.elevated]="+s.ahi > 5">{{ s.ahi }}</td>
            <td>{{ s.total_events }}</td>
            <td>{{ s.obstructive_apneas }}</td>
            <td>{{ s.central_apneas }}</td>
            <td>{{ s.hypopneas }}</td>
            <td>{{ s.reras }}</td>
            <td>{{ s.avg_spo2 ? s.avg_spo2 + '%' : '-' }}</td>
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
  `]
})
export class SessionsComponent implements OnInit {
  sessions: SessionListItem[] = [];

  constructor(private api: CpapApiService, private cdr: ChangeDetectorRef) {}

  ngOnInit() {
    this.api.getSessions(30, 30).subscribe({
      next: s => { this.sessions = s; this.cdr.detectChanges(); },
      error: e => console.error('Sessions error:', e)
    });
  }

  sleepDay(sessionStart: string): string {
    if (!sessionStart) return '';
    // PG returns "2026-03-27 23:34:06" — replace space with T for valid ISO
    const d = new Date(sessionStart.replace(' ', 'T'));
    if (isNaN(d.getTime())) return sessionStart.slice(0, 10);
    d.setHours(d.getHours() - 12);
    return d.toISOString().slice(0, 10);
  }
}
