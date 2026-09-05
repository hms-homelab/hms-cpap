import { Component, OnInit, ChangeDetectorRef } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { TranslatePipe } from '@ngx-translate/core';
import { CpapApiService } from '../../services/cpap-api.service';
import { EventRow } from '../../models/session.model';

// SDD-009: the strings exactly as eventTypeToString() stores them; the value
// sent to the API must match the DB, not a display label.
const EVENT_TYPES = [
  'Obstructive', 'Central', 'Hypopnea', 'RERA',
  'Clear Airway', 'Apnea', 'CSR', 'Desaturation',
];

const TYPE_COLORS: Record<string, string> = {
  'Obstructive': '#f87171',
  'Central': '#fb923c',
  'Hypopnea': '#fbbf24',
  'RERA': '#4ade80',
  'Clear Airway': '#c084fc',
  'Apnea': '#f472b6',
  'CSR': '#94a3b8',
  'Desaturation': '#e57373',
};

@Component({
  selector: 'app-events',
  standalone: true,
  imports: [CommonModule, FormsModule, TranslatePipe],
  templateUrl: './events.component.html',
  styleUrls: ['./events.component.css'],
})
export class EventsComponent implements OnInit {
  readonly eventTypes = EVENT_TYPES;
  readonly pageSize = 100;

  rows: EventRow[] = [];
  loading = false;
  loadingMore = false;
  hasMore = false;
  loadError = '';

  startDate = '';
  endDate = '';
  minDuration: number | null = null;
  selected = new Set<string>(EVENT_TYPES);

  constructor(private api: CpapApiService, private router: Router,
              private cdr: ChangeDetectorRef) {}

  ngOnInit() {
    this.load();
  }

  isChecked(t: string): boolean {
    return this.selected.has(t);
  }

  toggleType(t: string) {
    if (this.selected.has(t)) this.selected.delete(t);
    else this.selected.add(t);
    this.load();
  }

  onFilterChange() {
    this.load();
  }

  load() {
    // No types checked is a valid filter with a known-empty answer; skip the
    // round trip rather than asking the API for nothing.
    if (this.selected.size === 0) {
      this.rows = [];
      this.hasMore = false;
      return;
    }
    this.loading = true;
    this.loadError = '';
    this.api.getEvents(this.buildFilters(0)).subscribe({
      next: rows => {
        this.rows = rows;
        this.hasMore = rows.length === this.pageSize;
        this.loading = false;
        this.cdr.detectChanges();
      },
      error: () => {
        this.loadError = 'Could not load events.';
        this.loading = false;
      },
    });
  }

  loadMore() {
    if (this.loadingMore || !this.hasMore) return;
    this.loadingMore = true;
    this.api.getEvents(this.buildFilters(this.rows.length)).subscribe({
      next: rows => {
        this.rows = [...this.rows, ...rows];
        this.hasMore = rows.length === this.pageSize;
        this.loadingMore = false;
        this.cdr.detectChanges();
      },
      error: () => { this.loadingMore = false; },
    });
  }

  private buildFilters(offset: number) {
    return {
      start: this.startDate || undefined,
      end: this.endDate || undefined,
      // All types checked means no filter; sending the full list anyway would
      // silently hide any event type added later.
      types: this.selected.size === this.eventTypes.length
        ? undefined : [...this.selected],
      minDuration: this.minDuration || undefined,
      limit: this.pageSize,
      offset,
    };
  }

  openNight(row: EventRow) {
    this.router.navigate(['/sessions', row.sleep_day]);
  }

  typeColor(t: string): string {
    return TYPE_COLORS[t] || '#94a3b8';
  }

  clockTime(ts: string): string {
    const sp = ts?.indexOf(' ') ?? -1;
    return sp >= 0 ? ts.slice(sp + 1) : ts;
  }

  fmtDur(val: string | null): string {
    const s = Math.round(+(val || 0));
    if (s <= 0) return '-';
    return s >= 60 ? `${Math.floor(s / 60)}m ${s % 60}s` : `${s}s`;
  }

  fmtDetails(details: string | null): string {
    if (!details) return '';
    try {
      const d = JSON.parse(details);
      return Object.entries(d).map(([k, v]) => `${k}: ${v}`).join(', ');
    } catch {
      return details;
    }
  }
}
