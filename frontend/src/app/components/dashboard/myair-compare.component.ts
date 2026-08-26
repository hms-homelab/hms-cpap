import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MyAirComparisonRow } from '../../models/session.model';

/**
 * SDD-020: what we read off the card, beside what ResMed's own servers say about
 * the same night.
 *
 * ONLY RENDERED FOR myAir USERS. The dashboard gates this on
 * /api/capabilities features.myair, so someone with no myAir account never sees
 * a panel about a service they do not use.
 *
 * The point is not that the two agree. The composite scores are built from
 * different weights and will not match, and a stable offset between two formulas
 * is noise. What is worth looking at is a night where they DISAGREE about a
 * measurement they should both have read the same: that usually means the card
 * is missing a session ResMed's modem already uploaded, or the two disagree
 * about where the night ends.
 */
@Component({
  selector: 'app-myair-compare',
  standalone: true,
  imports: [CommonModule],
  template: `
    <div class="myair" *ngIf="rows?.length">
      <div class="head">
        <span class="title">CpapDash vs myAir</span>
        <span class="sub">{{ comparedCount }} of {{ rows.length }} nights have data on both sides</span>
      </div>

      <div class="scroll">
        <table>
          <thead>
            <tr>
              <th class="date">Night</th>
              <th colspan="3" class="group ours">CpapDash</th>
              <th colspan="3" class="group theirs">myAir</th>
              <th colspan="3" class="group delta">Difference</th>
            </tr>
            <tr class="sub-head">
              <th></th>
              <th>Usage</th><th>AHI</th><th>Leak</th>
              <th>Usage</th><th>AHI</th><th>Leak</th>
              <th>Usage</th><th>AHI</th><th>Leak</th>
            </tr>
          </thead>
          <tbody>
            <tr *ngFor="let r of rows" [class.no-data]="!r.myair_present">
              <td class="date">{{ shortDate(r.record_date) }}</td>

              <td>{{ hours(r.duration_minutes) }}</td>
              <td>{{ num(r.ahi, 2) }}</td>
              <td>{{ num(r.leak_95, 1) }}</td>

              <ng-container *ngIf="r.myair_present; else noData">
                <td>{{ hours(r.total_usage_min) }}</td>
                <td>{{ num(r.myair_ahi, 2) }}</td>
                <td>{{ num(r.leak_percentile, 1) }}</td>

                <td [class]="deltaClass(r.usage_delta_min, 15)">{{ delta(r.usage_delta_min, 0) }}m</td>
                <td [class]="deltaClass(r.ahi_delta, 1)">{{ delta(r.ahi_delta, 2) }}</td>
                <td [class]="deltaClass(r.leak_delta, 5)">{{ delta(r.leak_delta, 1) }}</td>
              </ng-container>
              <ng-template #noData>
                <!-- ResMed has no record of this night. That is a real and
                     interesting state, not a zero, so it is said rather than
                     drawn as a row of dashes that look like measurements. -->
                <td colspan="6" class="absent">ResMed has no data for this night</td>
              </ng-template>
            </tr>
          </tbody>
        </table>
      </div>

      <div class="foot">
        Differences are CpapDash minus myAir. The two score nights with different
        weights, so the composite numbers are not expected to match; a large gap
        in usage or AHI on a single night usually means one side saw a session the
        other did not.
      </div>
    </div>
  `,
  styles: [`
    .myair { background: #1e1e2f; border: 1px solid #333; border-radius: 8px; padding: 1rem 1.25rem; margin-bottom: 1.5rem; }
    .head { display: flex; align-items: baseline; gap: 0.75rem; flex-wrap: wrap; margin-bottom: 0.75rem; }
    .title { color: #e0e0e0; font-size: 0.95rem; font-weight: 600; }
    .sub { color: #888; font-size: 0.75rem; }
    .scroll { overflow-x: auto; }
    table { width: 100%; border-collapse: collapse; font-size: 0.8rem; min-width: 640px; }
    th, td { padding: 0.35rem 0.5rem; text-align: right; white-space: nowrap; }
    th.date, td.date { text-align: left; color: #bbb; }
    thead th { color: #888; font-weight: 600; font-size: 0.72rem; text-transform: uppercase; letter-spacing: 0.04em; }
    .group { text-align: center; border-bottom: 1px solid #333; }
    .group.ours { color: #64b5f6; }
    .group.theirs { color: #b39ddb; }
    .group.delta { color: #888; }
    .sub-head th { font-size: 0.68rem; text-transform: none; letter-spacing: 0; }
    tbody tr { border-top: 1px solid #2a2a3a; }
    tbody td { color: #ddd; }
    tr.no-data td { color: #777; }
    td.absent { text-align: center; font-style: italic; color: #777; }
    .close { color: #4ade80; }
    .apart { color: #fbbf24; }
    .foot { color: #888; font-size: 0.72rem; margin-top: 0.75rem; line-height: 1.5; }
  `]
})
export class MyAirCompareComponent {
  @Input() rows: MyAirComparisonRow[] = [];

  get comparedCount(): number {
    return (this.rows || []).filter(r => r.myair_present).length;
  }

  shortDate(d: string): string {
    if (!d) return '';
    const parsed = new Date(d.substring(0, 10) + 'T12:00:00');
    return parsed.toLocaleDateString('en-US', { month: 'short', day: 'numeric' });
  }

  private toNumber(v: string | number | null): number | null {
    if (v === null || v === undefined || v === '') return null;
    const n = typeof v === 'number' ? v : parseFloat(v);
    return isNaN(n) ? null : n;
  }

  num(v: string | number | null, places: number): string {
    const n = this.toNumber(v);
    return n === null ? '-' : n.toFixed(places);
  }

  hours(minutes: string | number | null): string {
    const n = this.toNumber(minutes);
    if (n === null) return '-';
    const h = Math.floor(n / 60);
    const m = Math.round(n % 60);
    return h > 0 ? `${h}h ${String(m).padStart(2, '0')}m` : `${m}m`;
  }

  delta(v: number | null, places: number): string {
    if (v === null || v === undefined) return '-';
    const sign = v > 0 ? '+' : '';
    return sign + v.toFixed(places);
  }

  /// Green when the two sides agree within a tolerance, amber when they do not.
  /// Amber is not an error: a real disagreement is the thing worth looking at.
  deltaClass(v: number | null, tolerance: number): string {
    if (v === null || v === undefined) return '';
    return Math.abs(v) <= tolerance ? 'close' : 'apart';
  }
}
