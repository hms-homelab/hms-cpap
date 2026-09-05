import { Component, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { TranslatePipe } from '@ngx-translate/core';
import { CpapApiService } from '../../services/cpap-api.service';

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

  constructor(private api: CpapApiService) {}

  ngOnInit() {
    this.setDefaultDates();
    this.loadReports();
    this.refreshTimer = setInterval(() => this.loadReports(), 15000);
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

  loadReports() {
    this.api.listReports().subscribe({
      next: r => this.reports = r,
      error: () => {}
    });
  }

  generate() {
    if (!this.startDate || !this.endDate) return;
    this.generating = true;
    this.generateError = '';
    this.api.generateReport(this.startDate, this.endDate).subscribe({
      next: () => {
        this.generating = false;
        this.loadReports();
      },
      error: () => {
        this.generating = false;
        this.generateError = 'Failed to start report generation.';
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
