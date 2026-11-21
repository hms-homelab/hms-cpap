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
  switchView, groupCode, ds2PairView, ds2PairRunning, ds2RefusalKey, ds2Reads, ds2NextRead, isIsoDay,
  as11ResetRefusalKey, ClockChoice, ClockDraft, ClockView, Reread, clockChanged, clockDifference, clockDraftOf,
  clockSettingOf, filterZones, fixedOffsets, hasDangerActions, learnedOffer, rereadLine,
} from './devices-model';
import {
  MuleDs2Pairing, muleDs2ErrorKey, muleDs2ModePayload, muleDs2On, muleDs2Pairing, muleDs2Reading, muleDs2RefusalKey,
  muleDs2Switch,
} from './mule-ds2-model';
import { sourceImage } from '../../utils/source-image';
import { BridgeSourceComponent } from '../../components/bridge-source/bridge-source.component';
import { bridgeInUse } from '../../components/bridge-source/bridge-source-model';

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
  imports: [CommonModule, FormsModule, RouterLink, TranslatePipe, OtaProgressComponent, BridgeSourceComponent],
  templateUrl: './devices.component.html',
  styleUrls: ['./devices.component.css', './devices-clock.css', './devices-actions.css'],
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

  // SDD-065: a DreamStation 2 through a Mule & Miner, per unit card.
  /** The pairing panel is open on this card. */
  muleDs2Panel: Record<string, boolean> = {};
  /** The import day being edited on this card; absent shows the saved one. */
  muleDs2ImportDraft: Record<string, string> = {};

  // SDD-074: the Machine clock (the Ring clock on the ring's card), per card.
  readonly fixedOffsets = fixedOffsets();
  readonly clockChanged = clockChanged;
  readonly learnedOffer = learnedOffer;
  readonly hasDanger = hasDangerActions;
  /** What the person changed and has not saved yet; absent shows the saved clock. */
  private clockEdits: Record<string, ClockDraft> = {};
  private clockShown: Record<string, { setting: string; draft: ClockDraft }> = {};
  clockBusy: Record<string, boolean> = {};
  clockNotes: Record<string, Note> = {};
  /** The zone names, read once when Time zone is first chosen. */
  zones: string[] | null = null;
  private zonesAsked = false;
  /** "Re-read my nights" (decision 3), per card, followed on each refresh. */
  rereads: Record<string, Reread> = {};
  rereadNotes: Record<string, Note> = {};

  private timer: any = null;
  private tables = new Map<string, { src: unknown; table: { rows: ConfigRow[]; read: boolean; risks: any[] } }>();

  constructor(private api: CpapApiService, private t: TranslateService) {}

  /** SDD-073 D8: the configured source, to say which bridge's folder is in use. */
  private sourceConfig: { transport?: string; local_dir?: string } | null = null;

  loadConfig(): void {
    this.api.getConfig().subscribe({
      next: (c: any) => { this.sourceConfig = { transport: c?.transport, local_dir: c?.local_dir }; },
      error: () => {},
    });
  }

  bridgeInUse(cardRoot: string | undefined): boolean {
    return bridgeInUse(this.sourceConfig, cardRoot);
  }

  ngOnInit(): void {
    this.loadConfig();
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
        this.followRereads();
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

  // ── Mule & Miner: a DreamStation 2 through the miner (SDD-065) ───────────

  muleDs2On(d: DeviceEntry): boolean {
    return muleDs2On(d.detail);
  }

  muleDs2Switch(d: DeviceEntry) {
    return muleDs2Switch(d.detail);
  }

  /** The mode on or off: the miner restarts with it. */
  muleDs2Toggle(d: DeviceEntry, on: boolean): void {
    this.send(d, 'switch', muleDs2ModePayload(on));
  }

  /** What this computer keeps for the unit's DreamStation 2. */
  muleDs2Local(d: DeviceEntry): any {
    return d.detail?.ds2_local ?? {};
  }

  muleDs2Pair(d: DeviceEntry): MuleDs2Pairing {
    return muleDs2Pairing(d.detail, this.now);
  }

  muleDs2Reading(d: DeviceEntry): boolean {
    return muleDs2Reading(d.detail);
  }

  muleDs2ErrorText(token: string): string {
    return this.t.instant(muleDs2ErrorKey(token), { error: token });
  }

  muleDs2OpenPanel(d: DeviceEntry): void {
    this.muleDs2Panel[d.key] = true;
  }

  muleDs2ClosePanel(d: DeviceEntry): void {
    this.muleDs2Panel[d.key] = false;
  }

  muleDs2PairStart(d: DeviceEntry): void {
    this.muleDs2Panel[d.key] = true;
    this.send(d, 'ds2_bond', { cmd: 'ds2_bond', action: 'start' });
  }

  /** The person's answer to the code: confirm with the code shown, or cancel. */
  muleDs2Answer(d: DeviceEntry, match: boolean): void {
    const p = this.muleDs2Pair(d);
    if (match && p.code) this.send(d, 'ds2_bond', { cmd: 'ds2_bond', action: 'confirm', code: p.code });
    else this.send(d, 'ds2_bond', { cmd: 'ds2_bond', action: 'cancel' });
  }

  askMuleDs2Forget(d: DeviceEntry): void {
    this.confirms.ask(d.key, 'ds2_bond', 'devices.muleDs2.forgetConfirm', { cmd: 'ds2_bond', action: 'forget' });
  }

  muleDs2ReadNow(d: DeviceEntry): void {
    this.busy[d.key] = true;
    this.api.muleDs2Read(d.id).subscribe({
      next: () => {
        this.busy[d.key] = false;
        this.note(d, this.t.instant('devices.ds2.readStarted'), false);
        this.load(false);
      },
      error: (e) => {
        this.busy[d.key] = false;
        this.note(d, this.t.instant(muleDs2RefusalKey(e?.error?.result)), true);
      },
    });
  }

  muleDs2ReadsLine(d: DeviceEntry): string {
    // SDD-065 D4a: the same choice and wording as the direct read's.
    const r = ds2Reads(this.muleDs2Local(d));
    const sources = r.sources.map((k) => this.t.instant(k)).join(', ');
    return this.t.instant(r.key, { sources, time: r.time });
  }

  muleDs2NextRead(d: DeviceEntry): number {
    return ds2NextRead(this.muleDs2Local(d));
  }

  muleDs2ImportValue(d: DeviceEntry): string {
    return this.muleDs2ImportDraft[d.key] ?? this.muleDs2Local(d).import_from ?? '';
  }

  muleDs2SaveImportFrom(d: DeviceEntry): void {
    const date = this.muleDs2ImportValue(d);
    if (!isIsoDay(date)) {
      this.note(d, this.t.instant('devices.ds2.importBadDate'), true);
      return;
    }
    this.busy[d.key] = true;
    this.api.muleDs2ImportFrom(d.id, date).subscribe({
      next: (r: any) => {
        this.busy[d.key] = false;
        delete this.muleDs2ImportDraft[d.key];
        this.note(d, this.t.instant('devices.ds2.importSaved', { date: r?.import_from ?? date }), false);
        this.load(false);
      },
      error: (e) => {
        this.busy[d.key] = false;
        this.note(d, this.t.instant(e?.error?.result === 'bad_date' ? 'devices.ds2.importBadDate'
                                  : 'devices.ds2.importFailed', { error: errorText(e) }), true);
      },
    });
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

  // ── Garmin (SDD-078) ─────────────────────────────────────────────────────

  garminCode = '';

  /** "Fetch from Garmin now": an event; the card's refresh shows the result. */
  garminFetch(d: DeviceEntry): void {
    this.busy[d.key] = true;
    delete this.notes[d.key];
    this.api.garminFetch().subscribe({
      next: () => { this.busy[d.key] = false; this.note(d, this.t.instant('devices.garmin.asked'), false); this.load(false); },
      error: (e) => { this.busy[d.key] = false; this.note(d, this.t.instant('devices.result.failed', { error: errorText(e) }), true); },
    });
  }

  garminSubmitCode(d: DeviceEntry): void {
    const code = this.garminCode.trim();
    if (!code || this.busy[d.key]) return;
    this.busy[d.key] = true;
    delete this.notes[d.key];
    this.api.garminMfa(code).subscribe({
      next: () => { this.busy[d.key] = false; this.garminCode = ''; this.load(false); },
      error: (e) => { this.busy[d.key] = false; this.note(d, this.t.instant('devices.result.failed', { error: errorText(e) }), true); },
    });
  }

  /** The card's error line, in the page's language where Garmin's kind is known. */
  garminError(d: DeviceEntry): string {
    const kind = d.detail?.error_kind || 'failed';
    const known = ['changed', 'bad_password', 'bad_mfa_code', 'blocked', 'unreachable', 'expired'];
    return known.includes(kind)
      ? this.t.instant('devices.garmin.err.' + kind)
      : this.t.instant('devices.garmin.err.failed', { message: d.detail?.error || '' });
  }

  /** What the last fetch did. */
  garminFetchLine(d: DeviceEntry): string {
    const f = d.detail?.last_fetch;
    if (!f) return '';
    if (!f.ok) return this.garminError(d);
    return f.stored > 0 ? this.t.instant('devices.garmin.fetchStored', { n: f.stored })
                        : this.t.instant('devices.garmin.fetchNothing');
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

  /** #67: one click, no confirm: forget the machine's OS object and power-cycle the adapter. */
  as11ResetBluetooth(d: DeviceEntry): void {
    this.busy[d.key] = true;
    this.api.as11ResetBluetooth().subscribe({
      next: (r: any) => {
        this.busy[d.key] = false;
        const others: string[] = r?.others_connected ?? [];
        this.note(d, this.t.instant('devices.as11.resetDone') +
          (others.length ? ' ' + this.t.instant('devices.as11.resetOthers', { devices: others.join(', ') }) : ''), false);
        this.load(false);
      },
      error: (e) => {
        this.busy[d.key] = false;
        // The service's reason code, translated; never its English.
        this.note(d, this.t.instant(as11ResetRefusalKey(e?.error?.reason)), true);
        this.load(false);
      },
    });
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

  /** SDD-080 D7: "Read now", one OSCAR read of the card. */
  oscarReadNow(d: DeviceEntry): void {
    this.busy[d.key] = true;
    delete this.notes[d.key];
    this.api.oscarReadNow().subscribe({
      next: () => { this.busy[d.key] = false; this.note(d, this.t.instant('devices.oscar.readStarted'), false); this.load(false); },
      error: (e) => { this.busy[d.key] = false; this.note(d, this.t.instant('devices.result.failed', { error: errorText(e) }), true); },
    });
  }

  /** SDD-080 D7: "OSCAR 2.1.1, last read 06:12, 1 session", or why it did not read. */
  oscarLine(o: any): string {
    if (o.reading) return this.t.instant('devices.oscar.reading');
    if (o.containerised && (o.state === 'no_program' || o.state === 'not_set_up'))
      return this.t.instant('devices.oscar.state_not_here');
    const time = o.last_read_at
      ? new Date(o.last_read_at * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '';
    const version = (o.version || '').split('+')[0];
    // SDD-081: OSCAR on a home PC says which PC.
    const name = o.helper?.name || '';
    if (o.where === 'helper' && name) {
      const state = o.state || 'never';
      if (['imported', 'nothing_new', 'busy', 'never'].includes(state))
        return this.t.instant('devices.oscar.helper_' + state, { version, time, sessions: o.sessions ?? 0, name });
      if (state === 'helper_offline' || state === 'helper_update')
        return this.t.instant('devices.oscar.state_' + state, { name });
    }
    return this.t.instant('devices.oscar.state_' + (o.state || 'never'),
                          { version, time, sessions: o.sessions ?? 0 });
  }

  oscarWarn(o: any): boolean {
    return !o.reading && !['imported', 'nothing_new', 'never'].includes(o.state);
  }

  /** SDD-081 D4: Forget the helper, one click; the next PC to connect is remembered. */
  helperBusy = false;
  helperNote: Note | null = null;
  forgetHelper(): void {
    const name = this.answer?.oscar_helper?.name ?? '';
    this.helperBusy = true;
    this.helperNote = null;
    this.api.forgetOscarHelper().subscribe({
      next: () => {
        this.helperBusy = false;
        this.helperNote = { text: this.t.instant('devices.oscarHelper.forgotten', { name }), err: false };
        this.load(true);
      },
      error: (e) => {
        this.helperBusy = false;
        this.helperNote = { text: this.t.instant('devices.result.failed', { error: errorText(e) }), err: true };
      },
    });
  }

  ds2ReadNow(d: DeviceEntry): void {
    this.api.ds2Read().subscribe({
      next: () => { this.note(d, this.t.instant('devices.ds2.readStarted'), false); this.load(false); },
      error: (e) => this.note(d, this.t.instant(ds2RefusalKey(e?.error?.result, d.detail?.backend)), true),
    });
  }

  /** SDD-064 D5a: "Once a day at 11:00, and when another source says the night ended (...)", and the rest. */
  ds2ReadsLine(d: DeviceEntry): string {
    const r = ds2Reads(d.detail);
    const sources = r.sources.map((k) => this.t.instant(k)).join(', ');
    return this.t.instant(r.key, { sources, time: r.time });
  }

  ds2NextRead(d: DeviceEntry): number {
    return ds2NextRead(d.detail);
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

  // ── SDD-074: the Machine clock and the Ring clock ────────────────────────

  /** The form on this card: what was changed, else the saved clock as a form. */
  clockDraft(d: DeviceEntry): ClockDraft {
    const edit = this.clockEdits[d.key];
    if (edit) return edit;
    const c = d.clock!;
    const shown = this.clockShown[d.key];
    if (!shown || shown.setting !== c.setting) this.clockShown[d.key] = { setting: c.setting, draft: clockDraftOf(c) };
    return this.clockShown[d.key].draft;
  }

  private editClock(d: DeviceEntry): ClockDraft {
    if (!this.clockEdits[d.key]) this.clockEdits[d.key] = { ...this.clockDraft(d) };
    delete this.clockNotes[d.key];
    return this.clockEdits[d.key];
  }

  /** "Same as this computer (America/New_York, UTC-05:00 now)". */
  hostLine(c: ClockView): string {
    return c.host_zone
      ? this.t.instant('devices.clock.host', { zone: c.host_zone, offset: c.host_offset_text })
      : this.t.instant('devices.clock.hostNoZone', { offset: c.host_offset_text });
  }

  onClockChoice(d: DeviceEntry, choice: ClockChoice): void {
    const e = this.editClock(d);
    e.choice = choice;
    if (choice === 'zone') {
      e.open = !e.zone;
      this.loadZones();
    }
  }

  onClockFixed(d: DeviceEntry, fixed: string): void {
    this.editClock(d).fixed = fixed;
  }

  /** Typing searches; a name typed out in full counts as picked. */
  onZoneQuery(d: DeviceEntry, q: string): void {
    const e = this.editClock(d);
    e.query = q;
    const exact = (this.zones ?? []).find((z) => z.toLowerCase() === q.trim().toLowerCase());
    e.zone = exact ?? '';
    e.open = !exact;
  }

  openZones(d: DeviceEntry): void {
    this.editClock(d).open = true;
    this.loadZones();
  }

  pickZone(d: DeviceEntry, zone: string): void {
    const e = this.editClock(d);
    e.zone = zone;
    e.query = zone;
    e.open = false;
  }

  zoneMatches(cd: ClockDraft): string[] {
    return this.zones ? filterZones(this.zones, cd.query) : [];
  }

  private loadZones(): void {
    if (this.zonesAsked) return;
    this.zonesAsked = true;
    this.api.getClockZones().subscribe({
      next: (r) => (this.zones = Array.isArray(r?.zones) ? r.zones : []),
      error: () => { this.zones = []; this.zonesAsked = false; },
    });
  }

  /** One click: the form's setting, or `setting` itself ("Use this"). No confirm. */
  saveClock(d: DeviceEntry, setting?: string): void {
    const c = d.clock;
    if (!c) return;
    const value = setting ?? clockSettingOf(this.clockDraft(d));
    if (value === null) return;
    this.clockBusy[d.key] = true;
    delete this.clockNotes[d.key];
    this.api.setDeviceClock(c.device_id, value).subscribe({
      next: (r) => {
        this.clockBusy[d.key] = false;
        // Every card of this machine shows the one setting again.
        for (const o of this.devices) {
          if (o.clock?.device_id !== c.device_id) continue;
          delete this.clockEdits[o.key];
          this.clockOpen[o.key] = false;
          if (r?.clock) o.clock = r.clock;
        }
        this.clockNotes[d.key] = { text: this.t.instant('devices.clock.saved'), err: false };
        this.load(false);
      },
      error: (e) => {
        this.clockBusy[d.key] = false;
        this.clockNotes[d.key] = { text: this.t.instant('devices.clock.failed', { error: errorText(e) }), err: true };
      },
    });
  }

  diffText(c: ClockView): string {
    const p = clockDifference(c.difference_s);
    return this.t.instant(p.key, p.params);
  }

  /** Decision 3: back up, then read every archived night again. One click, no dialog. */
  reread(d: DeviceEntry): void {
    const c = d.clock;
    if (!c) return;
    this.clockBusy[d.key] = true;
    delete this.rereads[d.key];
    this.rereadNotes[d.key] = { text: this.t.instant('devices.clock.rereadAsking'), err: false };
    // The worker's last end before asking, so this run's end is told from it.
    const ask = (before: string) =>
      this.api.rereadDeviceNights(c.device_id).subscribe({
        next: (r) => {
          this.clockBusy[d.key] = false;
          delete this.rereadNotes[d.key];
          this.rereads[d.key] = { backup: (r?.backup ?? []).length, before, seenRunning: false, status: null };
        },
        error: (e) => {
          this.clockBusy[d.key] = false;
          this.rereadNotes[d.key] = { text: this.t.instant('devices.clock.rereadFailed', { error: errorText(e) }), err: true };
        },
      });
    this.api.getBackfillStatus().subscribe({
      next: (s) => ask(String(s?.completed_at ?? '')),
      error: () => ask(''),
    });
  }

  rereadView(d: DeviceEntry) {
    const r = this.rereads[d.key];
    return r ? rereadLine(r) : null;
  }

  rereadRunning(d: DeviceEntry): boolean {
    const v = this.rereadView(d);
    return !!v && !v.done;
  }

  /** On the page's own refresh: the backfill worker's status, while a re-read is not done. */
  private followRereads(): void {
    const open = Object.values(this.rereads).filter((r) => !rereadLine(r).done);
    if (!open.length) return;
    this.api.getBackfillStatus().subscribe({
      next: (s) => {
        for (const r of open) {
          r.status = s;
          if (s?.status === 'running') r.seenRunning = true;
        }
      },
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
    // Item 10: the ring's adapter check and the AirSense 11's Reset Bluetooth
    // (when the adapter is not stuck) are rarely used, so they live here too.
    if (d.kind === 'o2ring') return !d.detail?.through_bridge;
    if (d.kind === 'as11_ble') return !d.detail?.not_compiled && !d.detail?.bluetooth_stuck;
    return d.kind === 'mule' || d.kind === 'sleepbridge';
  }

  // ── Item 10: the Machine clock as one line, its picker opened on Change ──
  clockOpen: Record<string, boolean> = {};

  toggleClock(d: DeviceEntry): void {
    this.clockOpen[d.key] = !this.clockOpen[d.key];
  }

  /** "Same as this computer (UTC-05:00 now)", "UTC+10:00" or a zone with its offset now. */
  clockSummary(c: ClockView): string {
    // The zone's name is in the picker; the line keeps to the offset.
    if (c.kind === 'host') return this.t.instant('devices.clock.hostNoZone', { offset: c.host_offset_text });
    if (c.kind === 'fixed') return 'UTC' + c.setting;
    return `${c.setting} (UTC${c.offset_text})`;
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
      case 'garmin': return 'fa-solid fa-stopwatch';
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
