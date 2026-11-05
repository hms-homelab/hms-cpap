import { Component, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { TranslatePipe } from '@ngx-translate/core';
import { CpapApiService } from '../../services/cpap-api.service';

type ZoneState = 'idle' | 'uploading' | 'processing' | 'complete' | 'error';

/** What became of a .cpapcard's card layout (the upload's `card_layout`). */
interface CardLayoutResult {
  error?: string;
  stored?: boolean;
  /** Set when the Sleep Bridge is off: kept for later. */
  message?: string;
  bridges?: { sender: string; applied: boolean; reason?: string }[];
}

@Component({
  selector: 'app-upload',
  standalone: true,
  imports: [CommonModule, TranslatePipe],
  templateUrl: './upload.component.html',
  styleUrls: ['./upload.component.css']
})
export class UploadComponent implements OnDestroy {
  // CPAP zip
  zipState: ZoneState = 'idle';
  zipDrag = false;
  zipError = '';
  zipDates: string[] = [];
  zipStatusText = '';
  layout: CardLayoutResult | null = null;
  private pollTimer: any = null;

  // O2Ring CSV
  oxiState: ZoneState = 'idle';
  oxiDrag = false;
  oxiError = '';
  oxiSamples = 0;
  oxiAvgSpo2 = 0;

  constructor(private api: CpapApiService) {}

  ngOnDestroy() {
    if (this.pollTimer) clearInterval(this.pollTimer);
  }

  // ---- CPAP zip ----
  onZipDrop(ev: DragEvent) {
    ev.preventDefault();
    this.zipDrag = false;
    const file = ev.dataTransfer?.files?.[0];
    if (file) this.uploadZip(file);
  }

  onZipPick(ev: Event) {
    const file = (ev.target as HTMLInputElement).files?.[0];
    if (file) this.uploadZip(file);
  }

  uploadZip(file: File) {
    this.zipState = 'uploading';
    this.zipError = '';
    this.zipDates = [];
    this.layout = null;
    this.api.uploadCpapZip(file).subscribe({
      next: (res) => {
        this.zipDates = res?.dates ?? [];
        this.layout = res?.card_layout ?? null;
        this.zipState = 'processing';
        this.zipStatusText = `Added ${this.zipDates.length} night(s); parsing...`;
        this.pollBackfill();
      },
      error: (err) => {
        this.zipState = 'error';
        this.zipError = err.error?.error || 'Upload failed.';
      }
    });
  }

  /** How many Sleep Bridges took the card's layout. */
  layoutTaken(): number {
    return (this.layout?.bridges ?? []).filter((b) => b.applied).length;
  }

  /** The bridges that did not, each with the server's reason. */
  layoutRefusedBy() {
    return (this.layout?.bridges ?? []).filter((b) => !b.applied);
  }

  private pollBackfill() {
    if (this.pollTimer) clearInterval(this.pollTimer);
    this.pollTimer = setInterval(() => {
      this.api.getBackfillStatus().subscribe({
        next: (s) => {
          const status = s?.status ?? '';
          if (status === 'running') {
            this.zipStatusText = 'Parsing nights...';
          } else if (status === 'complete' || status === 'idle') {
            clearInterval(this.pollTimer);
            this.pollTimer = null;
            this.zipState = 'complete';
          } else if (status === 'error') {
            clearInterval(this.pollTimer);
            this.pollTimer = null;
            this.zipState = 'error';
            this.zipError = s?.error_message || 'Parsing failed.';
          }
        },
        error: () => { /* keep polling */ }
      });
    }, 2000);
  }

  // ---- O2Ring CSV ----
  onOxiDrop(ev: DragEvent) {
    ev.preventDefault();
    this.oxiDrag = false;
    const file = ev.dataTransfer?.files?.[0];
    if (file) this.uploadOxi(file);
  }

  onOxiPick(ev: Event) {
    const file = (ev.target as HTMLInputElement).files?.[0];
    if (file) this.uploadOxi(file);
  }

  uploadOxi(file: File) {
    this.oxiState = 'uploading';
    this.oxiError = '';
    this.api.uploadOximetryCsv(file).subscribe({
      next: (res) => {
        this.oxiSamples = res?.samples ?? 0;
        this.oxiAvgSpo2 = res?.avg_spo2 ?? 0;
        this.oxiState = 'complete';
      },
      error: (err) => {
        this.oxiState = 'error';
        this.oxiError = err.error?.error || 'Oximetry upload failed.';
      }
    });
  }
}
