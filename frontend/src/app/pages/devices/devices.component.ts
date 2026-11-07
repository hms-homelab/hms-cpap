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
  switchView, groupCode, ds2PairView, ds2PairRunning, ds2RefusalKey, ds2Trigger, isIsoDay,
} from './devices-model';
import { sourceImage } from '../../utils/source-image';

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
  cmdsOpen: Record<string, boolean> = {};
  switching: Record<string, number> = {};   // the cloud switch's req_id, per card
  finding: Record<string, boolean> = {};
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
  /** The unit's printed pairing code, typed for the firmware check. */
  fwCode: Record<string, string> = {};

  // The Sleep Bridge's settings form, per bridge.
  configEdits: Record<string, Record<string, string>> = {};
  configRisk: Record<string, { group: 'network' | 'identity' | 'sequence'; keys: string[]; set: Record<string, unknown>; left: ('network' | 'identity' | 'sequence')[] } | null> = {};

  // The AirSense 11 pairing over this computer's Bluetooth.
  as11Pairing = false;
  as11Code = '';
  as11Message = '';
  as11Err = false;

  // SDD-064: the DreamStation 2 pairing over this computer's Bluetooth.
  /** The panel is open: the notes before starting, then the pairing itself. */
  ds2Panel = false;
  /** The status, polled every second while a pairing runs. */
  ds2Live: any = null;
  ds2Message = '';
  ds2Err = false;
  readonly groupCode = groupCode;
  /** The import date being edited; null shows the server's. */
  ds2ImportDraft: string | null = null;
  private ds2Timer: any = null;

  private timer: any = null;
  private tables = new Map<string, { src: unknown; table: { rows: ConfigRow[]; read: boolean; risks: any[] } }>();

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
    this.ds2StopPolling();
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
    if (p.action === 'ds2_forget') {
      this.api.ds2ForgetPairing().subscribe({
        next: (r: any) => {
          this.note(d, this.t.instant(r?.unpaired_os ? 'devices.ds2.forgotten' : 'devices.ds2.forgottenOsKeeps'), false);
          this.ds2Live = null;
          this.load(false);
        },
        error: (e) => this.note(d, this.t.instant('devices.ds2.forgetFailed', { error: errorText(e) }), true),
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
    this.note(d, this.t.instant('devices.result.sending'), false);
    const req = d.kind === 'mule'
      ? this.api.sendMuleCommand(d.id, payload.cmd, payload)
      : this.api.sleepBridgeCommand({ sender: d.id, ...payload });
    req.subscribe({
      next: (r: any) => {
        this.busy[d.key] = false;
        if (action === 'cloud') {
          // Followed on each refresh by the command's own answer (cloudState).
          this.switching[d.key] = Number(r?.req_id ?? 0);
          delete this.notes[d.key];
          this.load(false);
          return;
        }
        const state = r?.command?.state;
        this.note(d, this.t.instant(state === 'queued' ? 'devices.result.queued' : 'devices.result.sent'), false);
        this.load(false);
      },
      error: (e) => {
        this.busy[d.key] = false;
        const w = recordingWarning(e);
        if (w) {
          delete this.notes[d.key];
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

  private cloudPayload(d: DeviceEntry): any {
    return d.kind === 'mule' ? { cmd: 'config', hms_url: '' } : { cmd: 'stream', url: '' };
  }

  askCloud(d: DeviceEntry): void {
    const key = d.kind === 'mule' ? 'devices.mule.cloudConfirm' : 'devices.sleepbridge.cloudConfirm';
    this.confirms.ask(d.key, 'cloud', key, this.cloudPayload(d), { name: d.name });
  }

  /**
   * The switch to the cloud as the device answers it: switching while the
   * command waits or is out; switched once it answered ok (or left);
   * failed when it answered that it could not reach the cloud and stayed.
   */
  cloudState(d: DeviceEntry): '' | 'switching' | 'switched' | 'failed' {
    const id = this.switching[d.key];
    if (!id) return '';
    const c = this.commands(d).find((k) => k.req_id === id);
    if (c && c.state === 'answered') return c.result?.ok === false ? 'failed' : 'switched';
    if (d.present === 'no' && (!c || c.state === 'sent')) return 'switched';
    return 'switching';
  }

  /** Try again after a switch the device could not make: the same, confirmed already. */
  retryCloud(d: DeviceEntry): void {
    delete this.switching[d.key];
    this.send(d, 'cloud', { ...this.cloudPayload(d), confirm: true });
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

  /** The Firmware line asks for the unit's pairing code: none kept, nothing running. */
  needsFwCode(d: DeviceEntry): boolean {
    return d.kind === 'mule' && !!d.detail?.firmware && !d.detail.firmware.pairing_set && !d.detail.ota;
  }

  /** Keeps the printed code once CpapDash says it is this unit's, then reads the release. */
  saveFwCode(d: DeviceEntry): void {
    const code = (this.fwCode[d.key] || '').trim();
    if (!code) return;
    this.busy[d.key] = true;
    this.api.setMulePairingCode(d.id, code).subscribe({
      next: () => { this.busy[d.key] = false; this.fwCode[d.key] = ''; delete this.notes[d.key]; this.load(true); },
      error: (e) => {
        this.busy[d.key] = false;
        this.note(d, this.t.instant('devices.mule.fwCodeFailed', { error: errorText(e) }), true);
      },
    });
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
    this.note(d, this.t.instant('devices.result.starting'), false);
    first.subscribe({
      next: () => { this.busy[d.key] = false; delete this.notes[d.key]; this.load(false); },
      error: (e) => {
        this.busy[d.key] = false;
        const w = recordingWarning(e);
        if (w) {
          delete this.notes[d.key];
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

  /**
   * The settings table, built once per read of the device (the command
   * list's identity changes only when a refresh brings a new one). Built on
   * every change-detection pass instead, its rows and their inputs were
   * recreated each pass, and each new input scheduled another pass.
   */
  configTable(d: DeviceEntry): { rows: ConfigRow[]; read: boolean; risks: any[] } {
    const src = this.commands(d);
    const hit = this.tables.get(d.key);
    if (hit && hit.src === src) return hit.table;
    const v = latestConfigValues(src);
    const table = { rows: configRows(v?.values, v?.pending ?? []), read: !!v, risks: v?.risks ?? [] };
    this.tables.set(d.key, { src, table });
    return table;
  }

  trackRow(_: number, row: ConfigRow): string {
    return row.key;
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
    delete this.notes[d.key];
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
          : r.status === 'adapter_found' ? 'devices.o2ring.adapterFound'
          : r.status === 'radio_off' ? 'devices.o2ring.adapterRadioOff'
          : r.status === 'unauthorized' ? 'devices.o2ring.adapterUnauthorized'
          : r.status === 'bluez_error' ? 'devices.o2ring.adapterUnchecked'
          : 'devices.o2ring.adapterNone';
        const ok = r.status === 'adapter_found';
        this.note(d, this.t.instant(key, { adapter: r.adapter ?? '' }), !ok);
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
    this.finding[d.key] = true;
    this.api.discoverDevices().subscribe({
      next: (r) => {
        this.busy[d.key] = false;
        this.finding[d.key] = false;
        const lines = r.count ? this.t.instant('devices.source.found', { n: r.count }) + ': ' + r.devices.map((x) => x.base_url || x.host).join(', ')
                              : this.t.instant('devices.source.findNone');
        this.note(d, lines + (d.kind === 'flashair' ? ' ' + this.t.instant('devices.source.findFlashairNote') : ''), !r.count);
      },
      error: (e) => { this.busy[d.key] = false; this.finding[d.key] = false; this.note(d, this.t.instant('devices.result.failed', { error: errorText(e) }), true); },
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
    // The radio's advice is the OS's: rf-kill on Linux, the Mac's and the PC's own settings.
    const backend = this.devices.find((d) => d.kind === 'as11_ble')?.detail?.backend ?? '';
    const radioOff = backend === 'corebluetooth' ? 'devices.as11.errRadioOffMac'
                   : backend === 'winrt' ? 'devices.as11.errRadioOffWin'
                   : 'devices.as11.errRadioOff';
    const key: Record<string, string> = {
      connect: 'devices.as11.errConnect', radio_off: radioOff, verify: 'devices.as11.errVerify',
      protocol: 'devices.as11.errTimeout', bad_code: 'devices.as11.errBadCode', no_pairing: 'devices.as11.errNoPairing',
      disabled: 'devices.as11.errDisabled', not_compiled: 'devices.as11.notCompiled',
    };
    this.as11Message = this.t.instant(key[r?.result] ?? 'devices.as11.errGeneric');
    this.as11Err = true;
  }

  // ── SDD-064: a DreamStation 2 over Bluetooth ─────────────────────────────

  /** The freshest status: the second-by-second poll while pairing, else the list's. */
  ds2Status(d: DeviceEntry): any {
    return this.ds2Live ?? d.detail;
  }

  ds2View(d: DeviceEntry): { phase: string; key: string } {
    return ds2PairView(this.ds2Status(d));
  }

  /** Pair: first the notes (blower off, one bond), then Start. */
  ds2OpenPanel(): void {
    this.ds2Panel = true;
    this.ds2Message = '';
    this.ds2Err = false;
    this.ds2Live = null;
  }

  ds2ClosePanel(): void {
    this.ds2Panel = false;
    this.ds2Message = '';
    this.ds2Live = null;
    this.ds2StopPolling();
  }

  ds2PairStart(d: DeviceEntry): void {
    this.ds2Message = '';
    this.ds2Err = false;
    this.api.ds2PairStart().subscribe({
      next: () => this.ds2StartPolling(),
      error: (e) => this.ds2Refused(d, e?.error),
    });
  }

  ds2Answer(match: boolean): void {
    this.api.ds2PairConfirm(match).subscribe({
      next: () => this.ds2Poll(),
      error: () => this.ds2Poll(),
    });
  }

  ds2PairCancel(): void {
    this.api.ds2PairCancel().subscribe({ next: () => this.ds2Poll(), error: () => this.ds2Poll() });
  }

  ds2ReadNow(d: DeviceEntry): void {
    this.api.ds2Read().subscribe({
      next: () => { this.note(d, this.t.instant('devices.ds2.readStarted'), false); this.load(false); },
      error: (e) => this.note(d, this.t.instant(ds2RefusalKey(e?.error?.result, d.detail?.backend)), true),
    });
  }

  /** "When another source says the night ended (Sleep Bridge, O2 ring), or with Read now", or "Read now" only. */
  ds2ReadsLine(d: DeviceEntry): string {
    const t = ds2Trigger(d.detail);
    const sources = t.sources.map((k) => this.t.instant(k)).join(', ');
    return this.t.instant(t.key, { sources });
  }

  ds2ImportValue(d: DeviceEntry): string {
    return this.ds2ImportDraft ?? d.detail?.import_from ?? '';
  }

  ds2SaveImportFrom(d: DeviceEntry): void {
    const date = this.ds2ImportValue(d);
    if (!isIsoDay(date)) {
      this.note(d, this.t.instant('devices.ds2.importBadDate'), true);
      return;
    }
    this.busy[d.key] = true;
    this.api.ds2ImportFrom(date).subscribe({
      next: (r: any) => {
        this.busy[d.key] = false;
        this.ds2ImportDraft = null;
        this.note(d, this.t.instant('devices.ds2.importSaved', { date: r?.import_from ?? date }), false);
        this.load(false);
      },
      error: (e) => {
        this.busy[d.key] = false;
        const result = e?.error?.result;
        this.note(d, this.t.instant(result === 'bad_date' ? 'devices.ds2.importBadDate'
                                  : result === 'not_paired' ? 'devices.ds2.notPaired'
                                  : 'devices.ds2.importFailed', { error: errorText(e) }), true);
      },
    });
  }

  askDs2Forget(d: DeviceEntry): void {
    this.confirms.ask(d.key, 'ds2_forget', 'devices.ds2.forgetPairingConfirm', null);
  }

  private ds2Refused(d: DeviceEntry, r: any): void {
    this.ds2Message = this.t.instant(ds2RefusalKey(r?.result, d.detail?.backend));
    this.ds2Err = true;
  }

  private ds2StartPolling(): void {
    this.ds2StopPolling();
    this.ds2Poll();
    this.ds2Timer = setInterval(() => this.ds2Poll(), 1000);
  }

  private ds2StopPolling(): void {
    if (this.ds2Timer) clearInterval(this.ds2Timer);
    this.ds2Timer = null;
  }

  private ds2Poll(): void {
    this.api.getDs2BleStatus().subscribe({
      next: (s) => {
        this.ds2Live = s;
        if (!ds2PairRunning(s)) {
          this.ds2StopPolling();
          this.load(false);
        }
      },
      error: () => {},
    });
  }

  // ── the card's anatomy ───────────────────────────────────────────────────

  /** The one status pill: Recording when the machine records, else here, away or unknown. */
  pill(d: DeviceEntry): { key: string; cls: string } {
    if (d.recording === 'yes') return { key: 'devices.recording.badge', cls: 'rec' };
    return { key: 'devices.presence.' + d.present, cls: d.present };
  }

  /** The version on offer for the Firmware row's one line, or ''. */
  offer(d: DeviceEntry): string {
    if (d.kind === 'mule') return this.muleOffer(d);
    const r = this.bridgeRelease(d);
    return r.button ? d.detail?.release?.version ?? '' : '';
  }

  upToDate(d: DeviceEntry): boolean {
    if (d.kind === 'mule') return !!d.detail?.firmware && !this.muleOffer(d) && !d.detail.firmware.manifest_error;
    return false;   // the release line says it for a bridge
  }

  /** The action's own label: the confirm's verb uses the same words. */
  verbKey(action: string): string {
    switch (action) {
      case 'reboot': return 'devices.action.restart';
      case 'reset':
      case 'config_reset': return 'devices.action.resetWifi';
      case 'cloud': return 'devices.action.backToCloud';
      case 'forget': return 'devices.forget.button';
      case 'as11_forget': return 'devices.as11.forgetPairing';
      case 'ds2_forget': return 'devices.ds2.forgetPairing';
      case 'switch': return 'devices.confirm.verb.change';
      case 'config':
      case 'config_risk': return 'devices.config.send';
      default: return 'devices.confirm.verb.send';
    }
  }

  isDestructive(action: string): boolean {
    return action === 'reset' || action === 'config_reset' || action === 'forget' || action === 'as11_forget' ||
           action === 'ds2_forget';
  }

  /** The kind line under the name, unless the name already says it. */
  showKind(d: DeviceEntry): boolean {
    return !!d.own_name || this.t.instant('devices.kind.' + d.kind) !== d.name;
  }

  /** The kind has something under More. */
  hasMore(d: DeviceEntry): boolean {
    return d.kind === 'mule' || d.kind === 'sleepbridge';
  }

  /** The machine as the device names it; nothing assumed beyond that. */
  machineName(reported: string): string {
    const known: Record<string, string> = { resmed: 'ResMed', luna: 'Luna', bmc: 'Luna' };
    return known[reported] ?? reported;
  }

  // ── shared ───────────────────────────────────────────────────────────────

  private note(d: DeviceEntry, text: string, err: boolean): void {
    this.notes[d.key] = { text, err };
  }

  /** The kind's picture (public/sources/), or null to fall back to icon(). */
  readonly sourceImage = (kind: string): string | null => sourceImage(kind);

  icon(kind: string): string {
    switch (kind) {
      case 'mule': return 'fa-solid fa-microchip';
      case 'sleepbridge': return 'fa-solid fa-sd-card';
      case 'o2ring': return 'fa-solid fa-heart-pulse';
      case 'ezshare':
      case 'flashair': return 'fa-solid fa-wifi';
      case 'as11_ble':
      case 'ds2_ble': return 'fa-brands fa-bluetooth-b';
      default: return 'fa-solid fa-plug';
    }
  }

  ms(epoch: number | null | undefined): number {
    return (epoch ?? 0) * 1000;
  }
}
