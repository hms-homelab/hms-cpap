import { Component, EventEmitter, Input, Output } from '@angular/core';
import { TranslatePipe } from '@ngx-translate/core';
import { CpapApiService } from '../../services/cpap-api.service';
import { formatLabel } from './bridge-source-model';

/**
 * SDD-073 D8 / D10: one Sleep Bridge's folder and card format, each with
 * Copy, and "Use this bridge as my source", which does what the setup's D4
 * does (POST /api/sleepbridge/<bridge>/use) and restarts hms-cpap in place.
 * The Devices page and Settings show this same block, so the two can never
 * set the source two ways.
 */
@Component({
  selector: 'app-bridge-source',
  standalone: true,
  imports: [TranslatePipe],
  template: `
    <div class="bs" [class.listed]="head">
      @if (head) {
        <div class="head">
          <span class="mono name">{{ sender }}</span>
          <span class="pill" [class.on]="online">
            {{ (online ? 'settings.source.bridges.online' : 'settings.source.bridges.offline') | translate }}</span>
        </div>
      }
      <div class="line">
        <span class="k">{{ 'bridgeSource.folder' | translate }}</span>
        <span class="v mono">{{ cardRoot || ('bridgeSource.noFolder' | translate) }}</span>
        @if (cardRoot) {
          <button type="button" class="mini btn-secondary" (click)="copy(cardRoot, 'folder')">
            {{ (copied === 'folder' ? 'bridgeSource.copied' : 'bridgeSource.copy') | translate }}</button>
        }
      </div>
      <div class="line">
        <span class="k">{{ 'bridgeSource.format' | translate }}</span>
        <span class="v">{{ label ?? ('bridgeSource.formatUnknown' | translate) }}</span>
        @if (cardFormat) {
          <button type="button" class="mini btn-secondary" (click)="copy(cardFormat, 'format')">
            {{ (copied === 'format' ? 'bridgeSource.copied' : 'bridgeSource.copy') | translate }}</button>
        }
      </div>
      @if (inUse) {
        <div class="inuse">&#10003; {{ 'bridgeSource.inUse' | translate }}</div>
      } @else if (state === 'restarting') {
        <div class="note">{{ 'bridgeSource.restarting' | translate:{ format: usedFormat } }}</div>
      } @else if (state === 'saved') {
        <div class="note">{{ 'bridgeSource.saved' | translate:{ format: usedFormat } }}</div>
      } @else {
        <button type="button" class="use btn-primary" [disabled]="!cardRoot || state === 'busy'" (click)="use()">
          {{ 'bridgeSource.use' | translate }}</button>
        @if (state === 'failed') {
          <div class="err">{{ 'bridgeSource.failed' | translate:{ error: error } }}</div>
        }
      }
    </div>
  `,
  styles: [`
    .bs { display: flex; flex-direction: column; gap: 0.35rem; margin: 0.4rem 0; }
    .bs.listed { padding-top: 0.5rem; border-top: 1px solid #2a2a2a; }
    .head { display: flex; align-items: center; gap: 0.6rem; }
    .name { color: #e0e0e0; font-size: 0.9rem; }
    .pill { font-size: 0.72rem; padding: 0.05rem 0.5rem; border-radius: 10px; background: #333; color: #aaa; }
    .pill.on { background: #1b5e20; color: #c8e6c9; }
    .line { display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap; }
    .k { color: #9e9e9e; font-size: 0.82rem; min-width: 6.5rem; }
    .v { color: #e0e0e0; font-size: 0.88rem; word-break: break-all; }
    .mono { font-family: ui-monospace, Menlo, Consolas, monospace; font-size: 0.82rem; }
    /* Colours: styles.scss (.btn-secondary, .btn-primary); sizes here. */
    .mini { padding: 0.1rem 0.5rem; font-size: 0.75rem; font-weight: 400; }
    .use { align-self: flex-start; padding: 0.45rem 0.9rem; font-size: 0.85rem; margin-top: 0.2rem; }
    .inuse { color: #81c784; font-size: 0.88rem; }
    .note { color: #ffcc80; font-size: 0.85rem; }
    .err { color: #ef9a9a; font-size: 0.85rem; }
  `]
})
export class BridgeSourceComponent {
  @Input() sender = '';
  @Input() cardRoot = '';
  @Input() cardFormat = '';
  /** The configured local folder is this bridge's folder already. */
  @Input() inUse = false;
  /** Restart in place once the source is set (true from Devices and Settings). */
  @Input() restart = true;
  /** Settings lists bridges: the serial and whether it is online, above the lines. */
  @Input() head = false;
  @Input() online = false;
  /** The source was set: the format it was set with. */
  @Output() used = new EventEmitter<string>();

  state: 'idle' | 'busy' | 'restarting' | 'saved' | 'failed' = 'idle';
  error = '';
  usedFormat = '';
  copied: '' | 'folder' | 'format' = '';

  constructor(private api: CpapApiService) {}

  get label(): string | null { return formatLabel(this.cardFormat); }

  use(): void {
    this.state = 'busy';
    this.error = '';
    this.api.useSleepBridge(this.sender, this.restart).subscribe({
      next: (r) => {
        if (!r?.ok) {
          this.state = 'failed';
          this.error = r?.error ?? '';
          return;
        }
        this.usedFormat = formatLabel(r.format) ?? r.format ?? '';
        this.state = r.restarting ? 'restarting' : 'saved';
        this.used.emit(r.format ?? '');
      },
      error: (e) => {
        this.state = 'failed';
        this.error = e?.error?.error || ('HTTP ' + (e?.status ?? '?'));
      },
    });
  }

  copy(text: string, which: 'folder' | 'format'): void {
    const done = () => { this.copied = which; };
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(done);
      return;
    }
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    document.execCommand('copy');
    document.body.removeChild(ta);
    done();
  }
}
