import { Component, OnDestroy, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { RouterLink } from '@angular/router';
import { TranslatePipe, TranslateService } from '@ngx-translate/core';
import { Observable } from 'rxjs';

import { CpapApiService } from '../../services/cpap-api.service';
import { OtaProgressComponent } from '../../components/ota-progress/ota-progress.component';
import { Dismissed } from '../../components/ota-progress/ota-progress-model';
import {
  Confirmations, ConfigRow, DeviceCommand, DeviceEntry, DevicesAnswer, buildSet, commandState, configRows, costKey,
  errorText, groupKeys, hostOf, latestCommand, latestConfigValues, recordingWarning, releaseLine, riskyGroups,
  switchView,
} from './devices-model';

/** A line under a card: what the last action did. */
interface Note {
  text: string;
  err: boolean;
}

/** An update the server warned about, waiting for "Update anyway". */
interface UpdateWarning {
  reason: string;
  again: () => Observable<any>;
}

/**
 * Every device this hms-cpap reads or talks to, one card each (SDD-062):
 * whether it is here, whether its machine records, what it runs, and its
 * own commands. One read (GET /api/devices) every 3 s while the page is
 * visible; command follow-ups and update progress ride the same read.
 * Settings keeps the values; this page fires actions with a lifecycle.
 */
@Component({
  selector: 'app-devices',
  standalone: true,
  imports: [CommonModule, FormsModule, RouterLink, TranslatePipe, OtaProgressComponent],
  templateUrl: './devices.component.html',
  styleUrls: ['./devices.component.css'],
})
export class DevicesComponent implements OnInit, OnDestroy {
  answer: DevicesAnswer | null = null;
  loading = true;
  loadError = '';
  now = Math.floor(Date.now() / 1000);
  host = hostOf(window.location);

  open: Record<string, boolean> = {};
  busy: Record<string, boolean> = {};
  notes: Record<string, Note> = {};
  confirms = new Confirmations();
  dismissed = new Dismissed();
  updateWarnings: Record<string, UpdateWarning> = {};

  renaming = '';
  renameDraft = '';

  restartTarget: Record<string, 'mule' | 'miner' | 'both'> = {};
  fileTarget: Record<string, 'mule' | 'miner'> = {};
  pairCode: Record<string, string> = {};

  // The Sleep Bridge's settings form, per bridge.
  configEdits: Record<string, Record<string, string>> = {};
  configRisk: Record<string, { group: 'network' | 'identity' | 'sequence'; keys: string[]; set: Record<string, unknown>; left: ('network' | 'identity' | 'sequence')[] } | null> = {};

  // The AirSense 11 pairing over this computer's Bluetooth.
  as11Pairing = false;
  as11Code = '';
  as11Message = '';
  as11Err = false;

  private timer: any = null;

  constructor(private api: CpapApiService, private t: TranslateService) {}

  ngOnInit(): void {
    this.load(true);
    // D15: 3 s polling while the page is visible, nothing while hidden.
    this.timer = setInterval(() => {
      if (!document.hidden) this.load(false);
    }, 3000);
  }

  ngOnDestroy(): void {
    if (this.timer) clearInterval(this.timer);
  }

  load(fresh: boolean): void {
    this.api.getDevices(fresh).subscribe({
      next: (a) => {
        this.answer = a;
        this.now = a.now || Math.floor(Date.now() / 1000);
        this.loading = false;
        this.loadError = '';
      },
      error: (e) => {
        this.loading = false;
        this.loadError = errorText(e) || 'HTTP ' + (e?.status ?? '?');
      },
    });
  }

  get devices(): DeviceEntry[] {
    return this.answer?.devices ?? [];
  }

  trackKey(_: number, d: DeviceEntry): string {
    return d.key;
  }

  // ── names and forgetting ─────────────────────────────────────────────────

  startRename(d: DeviceEntry): void {
    this.renaming = d.key;
    this.renameDraft = d.own_name ?? '';
  }

  saveRename(d: DeviceEntry): void {
    this.busy[d.key] = true;
    this.api.renameDevice(d.kind, d.id, this.renameDraft).subscribe({
      next: () => { this.busy[d.key] = false; this.renaming = ''; this.load(false); },
      error: (e) => { this.busy[d.key] = false; this.note(d, this.t.instant('devices.rename.failed', { error: errorText(e) }), true); },
    });
  }

  askForget(d: DeviceEntry): void {
    this.confirms.ask(d.key, 'forget', 'devices.forget.confirm', null, { name: d.name });
  }

  // ── what a confirm or the recording warning gives back ───────────────────

  yes(d: DeviceEntry): void {
    const p = this.confirms.accept(d.key);
    if (!p) return;
    if (p.action === 'forget') {
      this.api.forgetDevice(d.kind, d.id).subscribe({
        next: () => this.load(false),
        error: (e) => this.note(d, this.t.instant('devices.forget.failed', { error: errorText(e) }), true),
      });
      return;
    }
    if (p.action === 'as11_forget') {
      this.api.as11ForgetPairing().subscribe({
        next: () => { this.note(d, this.t.instant('devices.as11.forgotten'), false); this.load(false); },
        error: (e) => this.note(d, this.t.instant('devices.as11.forgetFailed', { error: errorText(e) }), true),
      });
      return;
    }
    if (p.action === 'config_risk') {
      this.nextRisk(d);
      return;
    }
    this.send(d, p.action, p.payload);
  }

  no(d: DeviceEntry): void {
    this.confirms.cancel(d.key);
    this.configRisk[d.key] = null;
  }

  // ── commands, Mule & Miner and Sleep Bridge ──────────────────────────────

  /** One command; a 409 recording warning opens the in-page question. */
  send(d: DeviceEntry, action: string, payload: any): void {
    this.busy[d.key] = true;
    const req = d.kind === 'mule'
      ? this.api.sendMuleCommand(d.id, payload.cmd, payload)
      : this.api.sleepBridgeCommand({ sender: d.id, ...payload });
    req.subscribe({
      next: (r: any) => {
        this.busy[d.key] = false;
        const state = r?.command?.state;
        this.note(d, this.t.instant(state === 'queued' ? 'devices.result.queued' : 'devices.result.sent'), false);
        this.load(false);
      },
      error: (e) => {
        this.busy[d.key] = false;
        const w = recordingWarning(e);
        if (w) {
          this.confirms.recording(d.key, action, costKey(action), w.reason, payload);
          return;
        }
        this.note(d, this.t.instant('devices.result.failed', { error: errorText(e) }), true);
      },
    });
  }

  restart(d: DeviceEntry): void {
    const target = this.restartTarget[d.key] || 'both';
    this.send(d, 'reboot', d.kind === 'mule' ? { cmd: 'reboot', target } : { cmd: 'reboot' });
  }

  askReset(d: DeviceEntry): void {
    if (d.kind === 'mule') this.confirms.ask(d.key, 'reset', 'devices.mule.resetConfirm', { cmd: 'reset' });
    else this.confirms.ask(d.key, 'config_reset', 'devices.sleepbridge.resetConfirm', { cmd: 'config_reset' });
  }

  askCloud(d: DeviceEntry): void {
    const payload = d.kind === 'mule' ? { cmd: 'config', hms_url: '' } : { cmd: 'stream', url: '' };
    const key = d.kind === 'mule' ? 'devices.mule.cloudConfirm' : 'devices.sleepbridge.cloudConfirm';
    this.confirms.ask(d.key, 'cloud', key, payload, { name: d.name });
  }

  logs(d: DeviceEntry): void {
    this.send(d, 'logs', { cmd: 'logs', n: 500 });
  }

  restate(d: DeviceEntry): void {
    this.send(d, 'restate', { cmd: 'restate' });
  }

  cancelCommand(d: DeviceEntry, c: DeviceCommand): void {
    const req = d.kind === 'mule' ? this.api.cancelMuleCommand(c.req_id) : this.api.cancelSleepBridgeCommand(c.req_id);
    req.subscribe({
      next: () => { this.note(d, this.t.instant('devices.commands.cancelled'), false); this.load(false); },
      error: (e) => this.note(d, this.t.instant('devices.commands.cancelFailed', { error: errorText(e) }), true),
    });
  }

  commands(d: DeviceEntry): DeviceCommand[] {
    return (d.detail?.commands ?? []) as DeviceCommand[];
  }

  stateOf(c: DeviceCommand) {
    return commandState(c);
  }

  waitingFor(d: DeviceEntry, cmd: string): boolean {
    const c = latestCommand(this.commands(d), cmd);
    return !!c && c.state !== 'answered';
  }

  // ── the logs panel ───────────────────────────────────────────────────────

  copyLogs(d: DeviceEntry): void {
    const text = d.detail?.last_logs?.text ?? '';
    const done = () => this.note(d, this.t.instant('devices.logs.copied'), false);
    // The clipboard API needs a secure context; hms-cpap is plain HTTP on the
    // LAN, so the old way is the fallback.
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

  downloadLogs(d: DeviceEntry): void {
    const blob = new Blob([d.detail?.last_logs?.text ?? ''], { type: 'text/plain' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `${d.id}-logs.txt`;
    a.click();
    URL.revokeObjectURL(a.href);
  }

  // ── Mule & Miner: switches, pairing ──────────────────────────────────────

  switchOf(d: DeviceEntry, key: 'as11_enabled' | 'o2_enabled') {
    return switchView(d.detail, key);
  }

  toggle(d: DeviceEntry, key: 'as11_enabled' | 'o2_enabled', on: boolean): void {
    this.send(d, 'switch', { cmd: 'config', [key]: on });
  }

  pairStart(d: DeviceEntry): void {
    this.send(d, 'as11_pair', { cmd: 'as11_pair', start: true });
  }

  pairSend(d: DeviceEntry): void {
    const code = (this.pairCode[d.key] || '').trim();
    if (!code) return;
    this.pairCode[d.key] = '';
    this.send(d, 'as11_pair', { cmd: 'as11_pair', code });
  }

  pairCancel(d: DeviceEntry): void {
    this.send(d, 'as11_pair', { cmd: 'as11_pair', cancel: true });
  }

  // ── firmware updates ─────────────────────────────────────────────────────

  muleOffer(d: DeviceEntry): string {
    const f = d.detail?.firmware;
    if (!f?.update_available) return '';
    const parts: string[] = [];
    if (f.update_available.mule) parts.push(this.t.instant('devices.mule.mule') + ' ' + f.latest.mule);
    if (f.update_available.miner) parts.push(this.t.instant('devices.mule.miner') + ' ' + f.latest.miner);
    return parts.join(', ');
  }

  bridgeRelease(d: DeviceEntry) {
    return releaseLine(d.detail?.release);
  }

  canUpdate(d: DeviceEntry): boolean {
    return d.present === 'yes' && !this.busy[d.key];
  }

  /** An update call; a 409 recording warning shows on the Firmware line. */
  private runUpdate(d: DeviceEntry, first: Observable<any>, again: () => Observable<any>): void {
    this.busy[d.key] = true;
    delete this.updateWarnings[d.key];
    first.subscribe({
      next: () => { this.busy[d.key] = false; this.load(false); },
      error: (e) => {
        this.busy[d.key] = false;
        const w = recordingWarning(e);
        if (w) {
          this.updateWarnings[d.key] = { reason: w.reason, again };
          return;
        }
        this.note(d, this.t.instant('devices.result.failed', { error: errorText(e) }), true);
      },
    });
  }

  update(d: DeviceEntry): void {
    if (d.kind === 'mule')
      this.runUpdate(d, this.api.startMuleUpdate(d.id, 'both'), () => this.api.startMuleUpdate(d.id, 'both', true));
    else
      this.runUpdate(d, this.api.startSleepBridgeRelease(d.id), () => this.api.startSleepBridgeRelease(d.id, true));
  }

  updateAnyway(d: DeviceEntry): void {
    const w = this.updateWarnings[d.key];
    if (!w) return;
    delete this.updateWarnings[d.key];
    this.runUpdate(d, w.again(), w.again);
  }

  notNow(d: DeviceEntry): void {
    delete this.updateWarnings[d.key];
  }

  /** Try again: the same source as the failed job (the release, or the manifest). */
  tryAgain(d: DeviceEntry): void {
    this.update(d);
  }

  file(d: DeviceEntry, ev: Event): void {
    const input = ev.target as HTMLInputElement;
    const f = input.files?.[0];
    input.value = '';
    if (!f) return;
    if (d.kind === 'mule') {
      const target = this.fileTarget[d.key] || 'mule';
      this.runUpdate(d, this.api.uploadMuleImage(d.id, target, f), () => this.api.uploadMuleImage(d.id, target, f, true));
    } else {
      this.runUpdate(d, this.api.uploadSleepBridgeImage(d.id, f), () => this.api.uploadSleepBridgeImage(d.id, f, true));
    }
  }

  warningOf(d: DeviceEntry): string | null {
    const w = this.updateWarnings[d.key];
    return w ? w.reason : null;
  }

  // ── the Sleep Bridge's settings ──────────────────────────────────────────

  configTable(d: DeviceEntry): { rows: ConfigRow[]; read: boolean; risks: any[] } {
    const v = latestConfigValues(this.commands(d));
    return { rows: configRows(v?.values, v?.pending ?? []), read: !!v, risks: v?.risks ?? [] };
  }

  pendingKeys(d: DeviceEntry): string[] {
    return latestConfigValues(this.commands(d))?.pending ?? [];
  }

  edits(d: DeviceEntry): Record<string, string> {
    return (this.configEdits[d.key] ??= {});
  }

  configGet(d: DeviceEntry): void {
    this.send(d, 'config_get', { cmd: 'config', get: true });
  }

  /** Every risky group of the set has its own question, one after the other. */
  configSend(d: DeviceEntry): void {
    const table = this.configTable(d);
    const { set, error } = buildSet(table.rows, this.edits(d));
    if (error) {
      this.note(d, this.t.instant('devices.result.failed', { error: this.t.instant('devices.config.names.' + error) }), true);
      return;
    }
    if (Object.keys(set).length === 0) {
      this.note(d, this.t.instant('devices.config.nothing'), false);
      return;
    }
    const groups = riskyGroups(set);
    this.configRisk[d.key] = { group: groups[0], keys: [], set, left: groups };
    this.nextRisk(d);
  }

  private nextRisk(d: DeviceEntry): void {
    const r = this.configRisk[d.key];
    if (!r) return;
    const group = r.left.shift();
    if (group) {
      r.group = group;
      r.keys = groupKeys(r.set, group);
      const names = r.keys.map((k) => this.t.instant('devices.config.names.' + k)).join(', ');
      this.confirms.ask(d.key, 'config_risk', 'devices.config.riskyConfirm.' + group, null, { keys: names });
      return;
    }
    // Every question answered yes: the person confirmed it, so it goes with
    // confirm (a network change while recording is warned like any other).
    this.configRisk[d.key] = null;
    const hadRisk = riskyGroups(r.set).length > 0;
    this.configEdits[d.key] = {};
    this.send(d, 'config', hadRisk ? { cmd: 'config', set: r.set, confirm: true } : { cmd: 'config', set: r.set });
  }

  restartToApply(d: DeviceEntry): void {
    this.send(d, 'reboot', { cmd: 'reboot' });
  }

  riskText(r: any): string {
    const key = r?.risk === 'sequence_reuse' || r?.risk === 'identity' ? r.risk : 'other';
    return this.t.instant('devices.config.risk.' + key, { key: r?.key ?? '', from: r?.from ?? '', to: r?.to ?? '', risk: r?.risk ?? '' });
  }

  // ── the O2 ring ──────────────────────────────────────────────────────────

  readRing(d: DeviceEntry): void {
    this.busy[d.key] = true;
    this.api.collectOximetry().subscribe({
      next: (r: any) => {
        this.busy[d.key] = false;
        this.note(d, this.t.instant(r?.status === 'collected' ? 'devices.o2ring.readCollected' : 'devices.o2ring.readNone'), false);
        this.load(false);
      },
      error: (e) => { this.busy[d.key] = false; this.note(d, this.t.instant('devices.result.failed', { error: errorText(e) }), true); },
    });
  }

  checkAdapter(d: DeviceEntry): void {
    this.api.testBle().subscribe({
      next: (r) => {
        const key = !r.compiled ? 'devices.o2ring.adapterNotCompiled'
          : r.available ? 'devices.o2ring.adapterFound' : 'devices.o2ring.adapterNone';
        this.note(d, this.t.instant(key, { adapter: r.adapter ?? '' }), !r.available);
      },
      error: (e) => this.note(d, this.t.instant('devices.o2ring.adapterError', { error: errorText(e) }), true),
    });
  }

  // ── the card source ──────────────────────────────────────────────────────

  testCard(d: DeviceEntry): void {
    this.busy[d.key] = true;
    this.api.testSource(d.kind as 'ezshare' | 'flashair').subscribe({
      next: (r) => {
        this.busy[d.key] = false;
        const ok = r.status === 'ok';
        this.note(d, ok ? this.t.instant('devices.source.testOk') : this.t.instant('devices.source.testFailed', { error: r.error ?? '' }), !ok);
        this.load(false);
      },
      error: (e) => { this.busy[d.key] = false; this.note(d, this.t.instant('devices.source.testFailed', { error: errorText(e) }), true); },
    });
  }

  syncNow(d: DeviceEntry): void {
    this.api.syncNow().subscribe({
      next: (r) => {
        const known = ['requested', 'already_requested', 'already_running'];
        const key = known.includes(r.outcome) ? 'devices.source.syncOutcome.' + r.outcome : 'devices.source.syncOutcome.other';
        this.note(d, this.t.instant(key, { outcome: r.outcome }), !r.accepted);
      },
      error: (e) => this.note(d, this.t.instant('devices.result.failed', { error: errorText(e) }), true),
    });
  }

  find(d: DeviceEntry): void {
    this.busy[d.key] = true;
    this.api.discoverDevices().subscribe({
      next: (r) => {
        this.busy[d.key] = false;
        const lines = r.count ? this.t.instant('devices.source.found', { n: r.count }) + ': ' + r.devices.map((x) => x.base_url || x.host).join(', ')
                              : this.t.instant('devices.source.findNone');
        this.note(d, lines + (d.kind === 'flashair' ? ' ' + this.t.instant('devices.source.findFlashairNote') : ''), !r.count);
      },
      error: (e) => { this.busy[d.key] = false; this.note(d, this.t.instant('devices.result.failed', { error: errorText(e) }), true); },
    });
  }

  // ── the AirSense 11 over Bluetooth ───────────────────────────────────────

  as11Pull(d: DeviceEntry): void {
    this.api.as11Pull().subscribe({
      next: () => { this.note(d, this.t.instant('devices.as11.pullAsked'), false); this.load(false); },
      error: (e) => this.note(d, this.t.instant('devices.as11.pullFailed', { error: errorText(e) }), true),
    });
  }

  askAs11Forget(d: DeviceEntry): void {
    this.confirms.ask(d.key, 'as11_forget', 'devices.as11.forgetPairingConfirm', null);
  }

  as11PairStart(): void {
    this.as11Pairing = true;
    this.as11Message = '';
    this.as11Code = '';
    this.api.as11PairStart().subscribe({
      next: (r) => { this.as11Pairing = false; if (!r?.ok) this.as11Fail(r); this.load(false); },
      error: (e) => { this.as11Pairing = false; this.as11Fail(e?.error); },
    });
  }

  as11PairConfirm(): void {
    const code = this.as11Code;
    this.as11Code = '';
    this.as11Pairing = true;
    this.api.as11PairConfirm(code).subscribe({
      next: (r) => {
        this.as11Pairing = false;
        if (r?.ok) { this.as11Message = this.t.instant('devices.as11.done'); this.as11Err = false; }
        else this.as11Fail(r);
        this.load(false);
      },
      error: (e) => { this.as11Pairing = false; this.as11Fail(e?.error); },
    });
  }

  as11PairCancel(): void {
    this.api.as11PairCancel().subscribe({ next: () => this.load(false), error: () => this.load(false) });
  }

  private as11Fail(r: any): void {
    const key: Record<string, string> = {
      connect: 'devices.as11.errConnect', radio_off: 'devices.as11.errRadioOff', verify: 'devices.as11.errVerify',
      protocol: 'devices.as11.errTimeout', bad_code: 'devices.as11.errBadCode', no_pairing: 'devices.as11.errNoPairing',
      disabled: 'devices.as11.errDisabled', not_compiled: 'devices.as11.notCompiled',
    };
    this.as11Message = this.t.instant(key[r?.result] ?? 'devices.as11.errGeneric');
    this.as11Err = true;
  }

  // ── shared ───────────────────────────────────────────────────────────────

  private note(d: DeviceEntry, text: string, err: boolean): void {
    this.notes[d.key] = { text, err };
  }

  icon(kind: string): string {
    switch (kind) {
      case 'mule': return 'fa-solid fa-microchip';
      case 'sleepbridge': return 'fa-solid fa-sd-card';
      case 'o2ring': return 'fa-solid fa-heart-pulse';
      case 'ezshare':
      case 'flashair': return 'fa-solid fa-wifi';
      case 'as11_ble': return 'fa-brands fa-bluetooth-b';
      default: return 'fa-solid fa-plug';
    }
  }

  ms(epoch: number | null | undefined): number {
    return (epoch ?? 0) * 1000;
  }
}
