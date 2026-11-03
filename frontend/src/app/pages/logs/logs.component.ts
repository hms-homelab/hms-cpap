import { AfterViewChecked, Component, ElementRef, OnDestroy, OnInit, ViewChild } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { TranslatePipe } from '@ngx-translate/core';
import { CpapApiService, LogTail } from '../../services/cpap-api.service';
import { LogView } from './log-view';

// SDD-059. The page opens at the latest line and follows it while the reader
// is at the bottom, an idea from @rccoleman (#49). A refresh asks only for
// what was written since the last one; scrolling to the top loads the page
// before. Nothing refetches the whole tail.
@Component({
  selector: 'app-logs',
  standalone: true,
  imports: [CommonModule, FormsModule, TranslatePipe],
  templateUrl: './logs.component.html',
  styleUrls: ['./logs.component.css']
})
export class LogsComponent implements OnInit, OnDestroy, AfterViewChecked {
  loading = true;
  loadingOlder = false;
  error = '';
  data: LogTail | null = null;     // the last open's answer: path, size, state
  view = new LogView();

  lineOptions = [200, 1000, 2000, 10000];
  lines = 2000;                    // the first load, and each page loaded upward

  autoRefresh = false;             // SDD-059 D1: off on every visit
  copied = false;

  @ViewChild('logPre') private logPre?: ElementRef<HTMLPreElement>;

  private timer: any = null;
  private copiedTimer: any = null;
  /** After the next render: scroll to the end, or keep the reader's line. */
  private pending: 'end' | { height: number; top: number } | null = null;

  constructor(private api: CpapApiService) {}

  ngOnInit(): void {
    this.open();
  }

  ngOnDestroy(): void {
    if (this.timer) clearInterval(this.timer);
    if (this.copiedTimer) clearTimeout(this.copiedTimer);
  }

  /** The last `lines` lines, scrolled to the end. Also a new line count. */
  open(): void {
    this.loading = true;
    this.api.getLogs(this.lines).subscribe({
      next: d => {
        this.data = d; this.error = ''; this.loading = false;
        this.view.reset(d);
        this.pending = 'end';
      },
      error: e => { this.error = e?.message || 'Could not read the log'; this.loading = false; }
    });
  }

  /** What was written since the last answer, appended. */
  refresh(): void {
    if (this.view.empty || !this.data?.file_id) { this.open(); return; }
    const follow = this.atBottom();
    this.loading = true;
    this.api.getLogsAfter(this.view.end, this.view.fileId, this.lines).subscribe({
      next: d => {
        this.error = ''; this.loading = false;
        this.data = { ...(this.data as LogTail), size_bytes: d.size_bytes, path: d.path, active: d.active };
        this.view.append(d);
        if (follow || d.rotated) this.pending = 'end';
        if (d.more) this.refresh();   // a burst bigger than one answer
      },
      error: e => { this.error = e?.message || 'Could not read the log'; this.loading = false; }
    });
  }

  /** The reader reached the top: the page before what is shown. */
  onScroll(): void {
    const pre = this.logPre?.nativeElement;
    if (!pre || pre.scrollTop > 40 || this.loadingOlder || this.view.empty || this.view.atStart) return;
    this.loadingOlder = true;
    this.api.getLogsBefore(this.view.start, this.lines).subscribe({
      next: d => {
        this.loadingOlder = false;
        this.pending = { height: pre.scrollHeight, top: pre.scrollTop };
        this.view.prepend(d);
      },
      error: () => { this.loadingOlder = false; }
    });
  }

  /** Runs after the new text is in the DOM, so scrollHeight is the new one. */
  ngAfterViewChecked(): void {
    const pre = this.logPre?.nativeElement;
    if (!this.pending || !pre) return;
    if (this.pending === 'end') {
      pre.scrollTop = pre.scrollHeight;
    } else {
      // The line the reader was looking at stays under their eyes.
      pre.scrollTop = this.pending.top + (pre.scrollHeight - this.pending.height);
    }
    this.pending = null;
  }

  private atBottom(): boolean {
    const pre = this.logPre?.nativeElement;
    return !pre || pre.scrollHeight - pre.scrollTop - pre.clientHeight < 40;
  }

  toggleAutoRefresh(): void {
    this.autoRefresh = !this.autoRefresh;
    if (this.timer) { clearInterval(this.timer); this.timer = null; }
    if (this.autoRefresh) this.timer = setInterval(() => this.refresh(), 5000);
  }

  get text(): string {
    return this.view.text;
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
