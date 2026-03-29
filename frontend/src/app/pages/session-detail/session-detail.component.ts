import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { ActivatedRoute } from '@angular/router';
import { CpapApiService } from '../../services/cpap-api.service';
import { MetricCardComponent } from '../../components/metric-card/metric-card.component';
import { SessionDetail } from '../../models/session.model';

@Component({
  selector: 'app-session-detail',
  standalone: true,
  imports: [CommonModule, MetricCardComponent],
  template: `
    <div class="detail-page" *ngIf="session">
      <h2>Session: {{ date }}</h2>
      <div class="cards">
        <app-metric-card label="AHI" [value]="session.ahi || '0'" unit="events/h" />
        <app-metric-card label="Duration" [value]="session.duration_hours || '0'" unit="hours" />
        <app-metric-card label="Events" [value]="session.total_events || '0'" unit="" />
        <app-metric-card label="SpO2" [value]="session.avg_spo2 || '-'" unit="%" />
      </div>
      <div class="events-section" *ngIf="session.events?.length">
        <h3>Respiratory Events</h3>
        <table class="events-table">
          <thead><tr><th>Time</th><th>Type</th><th>Duration</th></tr></thead>
          <tbody>
            <tr *ngFor="let e of session.events">
              <td>{{ e.event_timestamp | date:'HH:mm:ss' }}</td>
              <td>{{ e.event_type }}</td>
              <td>{{ e.duration_seconds }}s</td>
            </tr>
          </tbody>
        </table>
      </div>
      <div class="placeholder" *ngIf="!session.events?.length">
        <p>Charts coming in Phase 2</p>
      </div>
    </div>
  `,
  styles: [`
    .detail-page { padding: 1.5rem; }
    h2, h3 { color: #e0e0e0; }
    .cards { display: flex; gap: 1rem; flex-wrap: wrap; margin: 1rem 0; }
    .events-table { width: 100%; border-collapse: collapse; margin-top: 0.5rem; }
    .events-table th { text-align: left; padding: 0.5rem; color: #888; border-bottom: 2px solid #333; font-size: 0.8rem; }
    .events-table td { padding: 0.5rem; color: #e0e0e0; border-bottom: 1px solid #2a2a3a; }
    .placeholder { color: #666; padding: 2rem; text-align: center; }
  `]
})
export class SessionDetailComponent implements OnInit {
  date = '';
  session: SessionDetail | null = null;

  constructor(private route: ActivatedRoute, private api: CpapApiService) {}

  ngOnInit() {
    this.date = this.route.snapshot.paramMap.get('date') || '';
    this.api.getSessionDetail(this.date).subscribe(sessions => {
      if (sessions.length > 0) this.session = sessions[0];
    });
  }
}
