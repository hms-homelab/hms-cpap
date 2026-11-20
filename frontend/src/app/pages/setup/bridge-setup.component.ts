import { Component, EventEmitter, Input, NgZone, OnDestroy, Output } from '@angular/core';
import { FormsModule } from '@angular/forms';
import { TranslatePipe, TranslateService } from '@ngx-translate/core';
import { CpapApiService } from '../../services/cpap-api.service';
import {
  BRIDGE_ANIMATIONS, BRIDGE_PICTURES, BridgeStep, CardOutcome, QrRect, WatchFrame, WatchedStatus, cardOutcome, cardOutcomeKey,
  findBridge, formatChanged, helloBridge, nextBridgeStep, onlineBridges, portNote,
  prefillAddress, previousBridgeStep, qrRects, snapshotMark, streamStateKey, validBridgeAddress,
} from './bridge-setup';
import { SleepBridgeWatch } from '../../services/sleepbridge-watch';

/**
 * SDD-073 D9: the Sleep Bridge choice of the web wizard, one step a screen
 * (D1 to D6), for Docker, the Pi and native Linux. Every wait listens on the
 * watch socket (api/sleepbridge/watch) for the event it names: the bridge's
 * hello, its card statement, the CpapDash stream's state. Nothing here runs
 * on a timer, and nothing gives up by clock; a closed socket is said, and
 * reopened only when the person asks.
 */
@Component({
  selector: 'app-bridge-setup',
  standalone: true,
  imports: [FormsModule, TranslatePipe],
  template: `
    <div class="step">
      @switch (step) {
        @case ('card') {
          <h1>{{ 'setup.bridge.card.title' | translate }}</h1>
          <img class="pic" [src]="pics.card" alt="" />
          <p class="subtitle">{{ 'setup.bridge.card.body' | translate }}</p>
          <p class="subtitle">{{ 'setup.bridge.card.where' | translate }}</p>
          <label class="file-pick">
            <input type="file" accept=".cpapcard,.zip" (change)="pickCard($event)" />
            <span class="btn-secondary">{{ 'setup.bridge.card.choose' | translate }}</span>
          </label>
          @if (card) {
            <p class="ok">&#10003; {{ 'setup.bridge.card.picked' | translate:{ name: card.name } }}</p>
            <p class="hint">{{ 'setup.bridge.card.later' | translate }}</p>
          }
        }

        @case ('assemble') {
          <h1>{{ 'setup.bridge.assemble.title' | translate }}</h1>
          <ol class="pics">
            <li><picture><source [attr.srcset]="anims.cardIntoBridge" type="image/webp" media="(prefers-reduced-motion: no-preference)" />
              <img class="pic" [src]="pics.cardIntoBridge" alt="" /></picture>
              <span>{{ 'setup.bridge.assemble.cardIntoBridge' | translate }}</span></li>
            <li><picture><source [attr.srcset]="anims.bridgeOntoExtender" type="image/webp" media="(prefers-reduced-motion: no-preference)" />
              <img class="pic" [src]="pics.bridgeOntoExtender" alt="" /></picture>
              <span>{{ 'setup.bridge.assemble.bridgeOntoExtender' | translate }}</span></li>
            <li><picture><source [attr.srcset]="anims.power" type="image/webp" media="(prefers-reduced-motion: no-preference)" />
              <img class="pic" [src]="pics.power" alt="" /></picture>
              <span>{{ 'setup.bridge.assemble.power' | translate }}</span></li>
          </ol>
          <p class="warn">{{ 'setup.bridge.assemble.notYet' | translate }}</p>
        }

        @case ('connect') {
          <h1>{{ 'setup.bridge.connect.title' | translate }}</h1>
          <picture><source [attr.srcset]="anims.joinWifi" type="image/webp" media="(prefers-reduced-motion: no-preference)" />
            <img class="pic" [src]="pics.joinWifi" alt="" /></picture>
          <p class="subtitle">{{ 'setup.bridge.connect.body' | translate }}</p>
          @if (addressConfirmed) {
            <div class="address-row">
              <div class="address">{{ address }}</div>
              @if (qr) {
                <svg class="qr" [attr.viewBox]="'0 0 ' + qr.side + ' ' + qr.side" role="img"
                     [attr.aria-label]="address" shape-rendering="crispEdges">
                  <rect [attr.width]="qr.side" [attr.height]="qr.side" fill="#fff" />
                  @for (r of qr.rects; track $index) {
                    <rect [attr.x]="r.x" [attr.y]="r.y" width="1" height="1" fill="#000" />
                  }
                </svg>
              }
            </div>
            <div class="row-btns">
              <button class="btn-secondary" (click)="copy(address)">
                {{ (copied ? 'setup.bridge.copied' : 'setup.bridge.copy') | translate }}</button>
              <button class="btn-secondary" (click)="addressConfirmed = false">
                {{ 'setup.bridge.connect.change' | translate }}</button>
            </div>
            @if (port !== null) {
              <p class="hint">{{ 'setup.bridge.connect.port' | translate:{ port: port } }}</p>
            }
          } @else {
            <label class="fld">
              <span>{{ 'setup.bridge.connect.askAddress' | translate }}</span>
              <input [(ngModel)]="address" name="bridgeaddr" placeholder="192.168.1.20" />
            </label>
            <p class="hint">{{ 'setup.bridge.connect.askHint' | translate }}</p>
            <button class="btn-secondary" [disabled]="!addressValid" (click)="confirmAddress()">
              {{ 'setup.bridge.connect.useAddress' | translate }}</button>
          }
          @if (macos) {
            <p class="info">{{ 'setup.bridge.connect.macFirewall' | translate }}</p>
          }
        }

        @case ('wait') {
          <h1>{{ 'setup.bridge.wait.title' | translate }}</h1>
          @if (!sender) {
            <p class="waiting"><span class="dot"></span>{{ 'setup.bridge.wait.waiting' | translate }}</p>
            @if (alreadyOnline.length) {
              <p class="hint">{{ 'setup.bridge.wait.alreadyOnline' | translate }}</p>
              <div class="row-btns">
                @for (s of alreadyOnline; track s) {
                  <button class="btn-secondary" (click)="found(s)">{{ s }}</button>
                }
              </div>
            }
            <p class="hint">{{ 'setup.bridge.wait.unreachable' | translate:{ port: listenPort } }}</p>
          } @else {
            <p class="ok big">&#10003; {{ 'setup.bridge.wait.found' | translate:{ sender: sender } }}</p>
            @if (useState === 'busy') {
              <p class="waiting"><span class="dot"></span>{{ 'setup.bridge.wait.setting' | translate }}</p>
            } @else if (useState === 'done') {
              <p class="ok">&#10003; {{ 'setup.bridge.wait.sourceSet' | translate:{ format: formatName(setFormat) } }}</p>
              @if (!formatKnown) {
                <p class="hint">{{ 'setup.bridge.wait.formatGuessed' | translate }}</p>
              }
            } @else if (useState === 'failed') {
              <p class="err">{{ 'setup.bridge.wait.sourceFailed' | translate:{ error: useError } }}</p>
              <button class="btn-secondary" (click)="useBridge()">{{ 'setup.bridge.tryAgain' | translate }}</button>
            }
          }
        }

        @case ('machine') {
          <h1>{{ 'setup.bridge.machine.title' | translate }}</h1>
          <ol class="pics">
            <li><picture><source [attr.srcset]="anims.intoMachine" type="image/webp" media="(prefers-reduced-motion: no-preference)" />
              <img class="pic" [src]="pics.intoMachine" alt="" /></picture>
              <span>{{ 'setup.bridge.machine.intoMachine' | translate }}</span></li>
            <li><img class="pic" [src]="pics.powerCycle" alt="" />
              <span>{{ 'setup.bridge.machine.powerCycle' | translate }}</span></li>
          </ol>
          @if (outcome === 'waiting') {
            <p class="waiting"><span class="dot"></span>{{ cardKey | translate }}</p>
          } @else if (outcome === 'seen') {
            <p class="ok big">&#10003; {{ cardKey | translate }}</p>
          } @else {
            <p class="err">{{ cardKey | translate }}</p>
          }
        }

        @case ('online') {
          <h1>{{ 'setup.bridge.online.title' | translate }}</h1>
          <p class="subtitle">{{ 'setup.bridge.online.body' | translate }}</p>
          <!-- A link, not a button (SDD-073 D6), with the external-link
               mark after the words: it opens CpapDash (Settings > hms-cpap),
               the same page as the desktop app's link. -->
          <p class="token-link">
            <a href="https://cpapdash.com/settings" target="_blank" rel="noopener">
              {{ 'setup.bridge.online.open' | translate }}<svg class="ext" viewBox="0 0 24 24" width="14" height="14"
                aria-hidden="true" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"
                stroke-linejoin="round"><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/>
                <path d="M15 3h6v6"/><path d="M10 14 21 3"/></svg></a>
          </p>
          <label class="fld">
            <span>{{ 'setup.bridge.online.token' | translate }}</span>
            <input type="password" [(ngModel)]="token" name="cdtoken" autocomplete="off" />
          </label>
          <label class="switch">
            <input type="checkbox" [(ngModel)]="sendNights" name="cdstream" />
            <span>{{ 'setup.bridge.online.send' | translate }}</span>
          </label>
          <button class="btn-secondary" [disabled]="(!token.trim() && sendNights) || saving" (click)="saveToken()">
            {{ 'setup.bridge.online.save' | translate }}</button>
          @if (saveError) {
            <p class="err">{{ saveError }}</p>
          }
          <p [class.ok]="streamState === 'connected'" class="state">
            @if (streamState === 'connected') { &#10003; }
            {{ streamKey | translate }}</p>
        }
      }

      @if (watchClosed && (step === 'wait' || step === 'machine' || step === 'online')) {
        <p class="err">{{ 'setup.bridge.closed' | translate }}
          <button class="link-inline" (click)="openWatch()">{{ 'setup.bridge.reconnect' | translate }}</button></p>
      }

      <div class="actions">
        <button class="btn-secondary" (click)="goBack()">{{ 'setup.back' | translate }}</button>
        <a class="skip" href="" (click)="$event.preventDefault(); goNext()">{{ 'setup.bridge.skip' | translate }}</a>
        <button class="btn-primary" (click)="goNext()" [disabled]="!canNext">{{ 'setup.next' | translate }}</button>
      </div>
    </div>
  `,
  styles: [`
    h1 { font-size: 1.5rem; color: #e0e0e0; margin: 0 0 0.75rem; }
    .subtitle { color: #b0b0b0; line-height: 1.5; margin: 0 0 0.8rem; }
    .hint { font-size: 0.8rem; color: #9e9e9e; line-height: 1.45; margin: 0.5rem 0; }
    /* Each picture keeps its own shape: the renders are 16:9, the rest 4:3. */
    picture { display: block; }
    .pic { display: block; width: 100%; max-width: 420px; height: auto;
           border-radius: 10px; border: 1px solid #333; background: #222; margin: 0 auto 0.75rem; }
    .pics { list-style: none; padding: 0; margin: 0; counter-reset: n; }
    .pics li { counter-increment: n; margin-bottom: 1rem; }
    .pics li span { display: block; color: #e0e0e0; text-align: center; }
    .pics li span::before { content: counter(n) ". "; color: #64b5f6; font-weight: 600; }
    .info { background: #13263d; border: 1px solid #2f6aa8; color: #cfe3ff; border-radius: 8px;
            padding: 0.7rem 0.9rem; line-height: 1.45; margin: 0.75rem 0 0; }
    .warn { background: #3a2a10; border: 1px solid #a06800; color: #ffcc80; border-radius: 8px;
            padding: 0.7rem 0.9rem; font-weight: 600; text-align: center; }
    .address-row { display: flex; align-items: center; justify-content: center; gap: 1.25rem;
                   flex-wrap: wrap; margin: 0.5rem 0 0.75rem; }
    .address { font-size: 2.4rem; font-weight: 700; color: #fff; letter-spacing: 0.02em;
               font-variant-numeric: tabular-nums; word-break: break-all; }
    .qr { width: 132px; height: 132px; border-radius: 6px; }
    .row-btns { display: flex; gap: 0.6rem; flex-wrap: wrap; justify-content: center; margin: 0.4rem 0; }
    .fld { display: block; margin: 0.8rem 0; }
    .fld > span { display: block; font-size: 0.8rem; color: #9e9e9e; margin-bottom: 0.25rem; }
    .fld input { width: 100%; padding: 0.55rem; background: #121212; color: #e0e0e0;
                 border: 1px solid #333; border-radius: 6px; font-size: 1rem; box-sizing: border-box; }
    .file-pick input { display: none; }
    .file-pick { display: inline-block; cursor: pointer; }
    .ok { color: #81c784; }
    .ok.big, .big { font-size: 1.15rem; font-weight: 600; }
    .err { color: #ef9a9a; }
    .state { margin-top: 0.75rem; }
    .waiting { color: #e0e0e0; display: flex; align-items: center; gap: 0.6rem; }
    .dot { width: 10px; height: 10px; border-radius: 50%; background: #64b5f6; flex: none;
           animation: pulse 1.4s ease-in-out infinite; }
    @keyframes pulse { 0%, 100% { opacity: 0.25; } 50% { opacity: 1; } }
    @media (prefers-reduced-motion: reduce) { .dot { animation: none; } }
    .switch { display: flex; align-items: center; gap: 0.6rem; color: #e0e0e0; margin: 0.4rem 0 0.9rem; }
    .switch input { width: 18px; height: 18px; accent-color: #64b5f6; }
    .actions { display: flex; align-items: center; justify-content: space-between; gap: 0.75rem;
               margin-top: 1.5rem; }
    .skip { color: var(--btn-blue-text); font-size: 0.88rem; margin-left: auto; text-decoration: underline; }
    /* Button colours: styles.scss (.btn-primary, .btn-secondary); sizes here. */
    .btn-primary { padding: 0.65rem 1.4rem; font-size: 0.95rem; }
    .btn-secondary { padding: 0.55rem 1.1rem; font-size: 0.92rem; }
    .token-link { margin: 0 0 0.4rem; }
    .token-link a { color: var(--btn-blue-text); text-decoration: underline; }
    .token-link .ext { margin-left: 0.3em; vertical-align: -0.1em; }
    .link-inline { background: none; border: none; padding: 0 0.4rem; color: var(--btn-blue-text);
                   font: inherit; text-decoration: underline; cursor: pointer; }
    @media (max-width: 480px) { .address { font-size: 1.9rem; } }
  `]
})
export class BridgeSetupComponent implements OnDestroy {
  /** The receiver's port (GET /api/sleepbridge). */
  @Input() set listen(p: number) { this.listenPort = p; this.port = portNote(p); }
  /** HmsCpap runs on a Mac: its firewall may ask to let the bridge in (D7). */
  @Input() macos = false;
  /** The sequence is over: the .cpapcard picked on D1, if any. */
  @Output() finished = new EventEmitter<File | null>();
  /** Back from the first step, to the source step. */
  @Output() leave = new EventEmitter<void>();

  readonly pics = BRIDGE_PICTURES;
  readonly anims = BRIDGE_ANIMATIONS;
  step: BridgeStep = 'card';

  // D1
  card: File | null = null;

  // D3
  listenPort = 8894;
  port: number | null = null;
  address = prefillAddress(window.location.hostname);
  addressConfirmed = !!this.address;
  qr: { side: number; rects: QrRect[] } | null = null;
  copied = false;

  // D4
  sender = '';
  alreadyOnline: string[] = [];
  useState: 'idle' | 'busy' | 'done' | 'failed' = 'idle';
  useError = '';
  setFormat = '';
  formatKnown = true;
  private openedAt: number | null = null;

  // D5
  private statedAtHello = 0;
  outcome: CardOutcome = 'waiting';
  private status: WatchedStatus | undefined;

  // D6
  token = '';
  sendNights = true;
  saving = false;
  saveError = '';
  streamState = '';

  watchClosed = false;
  private watch: SleepBridgeWatch;

  constructor(private api: CpapApiService, private t: TranslateService, zone: NgZone) {
    this.watch = new SleepBridgeWatch(zone, (f) => this.onFrame(f), () => { this.watchClosed = true; });
    if (this.addressConfirmed) this.loadQr();
  }

  ngOnDestroy(): void {
    this.closeWatch();
  }

  get addressValid(): boolean { return validBridgeAddress(this.address); }
  get cardKey(): string { return cardOutcomeKey(this.outcome); }
  get streamKey(): string { return streamStateKey(this.streamState, this.savedOff); }
  /** What the last save here set the switch to (null: not saved yet). */
  savedOff: boolean | null = null;

  /** Next waits only where the step's own event is the point of it. */
  get canNext(): boolean {
    switch (this.step) {
      case 'connect': return this.addressConfirmed;
      case 'wait': return !!this.sender && this.useState === 'done';
      default: return true;
    }
  }

  formatName(f: string): string {
    return f === 'bmc' ? 'BMC / React Health Luna' : f === 'resmed' ? 'ResMed' : f;
  }

  pickCard(ev: Event): void {
    const f = (ev.target as HTMLInputElement).files?.[0];
    if (f) this.card = f;
  }

  confirmAddress(): void {
    this.address = this.address.trim();
    if (!this.addressValid) return;
    this.addressConfirmed = true;
    this.loadQr();
  }

  private loadQr(): void {
    this.qr = null;
    this.api.getSleepBridgeQr(this.address).subscribe({
      next: (r) => { this.qr = qrRects(r.rows ?? []); },
      error: () => { this.qr = null; },
    });
  }

  copy(text: string): void {
    const done = () => { this.copied = true; };
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

  goNext(): void {
    const n = nextBridgeStep(this.step);
    if (!n) {
      this.closeWatch();
      this.finished.emit(this.card);
      return;
    }
    this.enter(n);
  }

  goBack(): void {
    const p = previousBridgeStep(this.step);
    if (!p) {
      this.closeWatch();
      this.leave.emit();
      return;
    }
    this.step = p;
  }

  private enter(s: BridgeStep): void {
    this.step = s;
    // The socket opens where the first wait is, and stays open to the end.
    if ((s === 'wait' || s === 'machine' || s === 'online') && !this.watch.open && !this.watchClosed) {
      this.openWatch();
    }
  }

  // ── the watch socket ─────────────────────────────────────────────────────

  openWatch(): void {
    this.watchClosed = false;
    this.watch.connect();
  }

  private closeWatch(): void {
    this.watch.close();
  }

  private onFrame(f: WatchFrame): void {
    if (f.t === 'cpapdash') {
      this.streamState = f.stream?.state ?? '';
      return;
    }
    this.status = f.status ?? this.status;
    if (this.openedAt === null) {
      // The server's own clock, from the first snapshot: a bridge whose
      // session started later than this is the one plugged in now.
      this.openedAt = snapshotMark(f.status);
      this.alreadyOnline = onlineBridges(f.status);
    } else if (!this.sender) {
      this.alreadyOnline = onlineBridges(f.status);
    }
    if (!this.sender) {
      const s = helloBridge(f, this.openedAt);
      if (s) this.found(s);
      return;
    }
    const b = findBridge(this.status, this.sender);
    this.outcome = cardOutcome(b, this.statedAtHello);
    // D5: the format the bridge recognised is known now, and is another.
    if (this.useState === 'done' && formatChanged(this.setFormat, b)) this.useBridge();
  }

  /** D4: the bridge is heard: its folder becomes the source. */
  found(sender: string): void {
    this.sender = sender;
    this.statedAtHello = findBridge(this.status, sender)?.card_stated_at ?? 0;
    this.outcome = 'waiting';
    this.useBridge();
  }

  useBridge(): void {
    if (!this.sender) return;
    this.useState = 'busy';
    this.useError = '';
    this.api.useSleepBridge(this.sender, false).subscribe({
      next: (r) => {
        if (!r?.ok) {
          this.useState = 'failed';
          this.useError = r?.error ?? '';
          return;
        }
        this.setFormat = r.format ?? '';
        this.formatKnown = r.format_known !== false;
        this.useState = 'done';
      },
      error: (e) => {
        this.useState = 'failed';
        this.useError = e?.error?.error || ('HTTP ' + (e?.status ?? '?'));
      },
    });
  }

  // ── D6 ───────────────────────────────────────────────────────────────────

  saveToken(): void {
    this.saving = true;
    this.saveError = '';
    // The token only when one was typed: turning sending off keeps the saved one.
    const cpapdash: Record<string, unknown> = { stream: this.sendNights };
    if (this.token.trim()) cpapdash['token'] = this.token.trim();
    this.api.updateConfig({ cpapdash } as any).subscribe({
      next: () => { this.saving = false; this.savedOff = !this.sendNights; },
      error: (e) => {
        this.saving = false;
        this.saveError = e?.error?.error || this.t.instant('setup.bridge.online.saveFailed');
      },
    });
  }
}
