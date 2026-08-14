import { Component, OnDestroy, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { CpapApiService, LogTail } from '../../services/cpap-api.service';

@Component({
  selector: 'app-logs',
  standalone: true,
  imports: [CommonModule, FormsModule],
  templateUrl: './logs.component.html',
  styleUrls: ['./logs.component.css']
})
export class LogsComponent implements OnInit, OnDestroy {
  loading = true;
  error = '';
  data: LogTail | null = null;

  lineOptions = [200, 1000, 2000, 10000];
  lines = 2000;

  autoRefresh = false;
  copied = false;

  private timer: any = null;
  private copiedTimer: any = null;

  constructor(private api: CpapApiService) {}

  ngOnInit(): void {
    this.refresh();
  }

  ngOnDestroy(): void {
    if (this.timer) clearInterval(this.timer);
    if (this.copiedTimer) clearTimeout(this.copiedTimer);
  }

  refresh(): void {
    this.loading = true;
    this.api.getLogs(this.lines).subscribe({
      next: d => { this.data = d; this.error = ''; this.loading = false; },
      error: e => { this.error = e?.message || 'Could not read the log'; this.loading = false; }
    });
  }

  toggleAutoRefresh(): void {
    this.autoRefresh = !this.autoRefresh;
    if (this.timer) { clearInterval(this.timer); this.timer = null; }
    if (this.autoRefresh) this.timer = setInterval(() => this.refresh(), 5000);
  }

  get text(): string {
    return this.data?.lines?.join('\n') ?? '';
  }

  /**
   * Copy the whole tail. navigator.clipboard needs a secure context, and this
   * UI is served over plain HTTP on the LAN, so the textarea fallback is the
   * path that actually runs for most users rather than a nicety.
   */
  async copy(): Promise<void> {
    const text = this.text;
    if (!text) return;

    let ok = false;
    try {
      if (navigator.clipboard && window.isSecureContext) {
        // Raced against a timeout, because writeText does not always settle.
        // When the clipboard-write permission is pending or denied it can sit
        // there forever instead of rejecting, and the button would then never
        // say anything at all -- neither "Copied" nor an error. Observed in a
        // headless browser; the same state is reachable in a real one.
        await Promise.race([
          navigator.clipboard.writeText(text),
          new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), 1500))
        ]);
        ok = true;
      }
    } catch { /* fall through to the textarea */ }

    if (!ok) {
      const ta = document.createElement('textarea');
      ta.value = text;
      ta.setAttribute('readonly', '');
      ta.style.position = 'fixed';
      ta.style.left = '-9999px';
      document.body.appendChild(ta);
      ta.select();
      try { ok = document.execCommand('copy'); } catch { ok = false; }
      document.body.removeChild(ta);
    }

    if (ok) {
      this.copied = true;
      if (this.copiedTimer) clearTimeout(this.copiedTimer);
      this.copiedTimer = setTimeout(() => (this.copied = false), 2000);
    } else {
      this.error = 'Could not copy. Select the log text and copy it manually.';
    }
  }

  download(): void {
    const blob = new Blob([this.text], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'hms-cpap.log';
    a.click();
    URL.revokeObjectURL(url);
  }

  get sizeLabel(): string {
    const b = this.data?.size_bytes ?? 0;
    if (b < 1024) return `${b} B`;
    if (b < 1024 * 1024) return `${(b / 1024).toFixed(1)} KB`;
    return `${(b / (1024 * 1024)).toFixed(1)} MB`;
  }
}
