// The Devices page's decisions, as plain data, so they are tested without a
// browser (devices-model.spec.ts, run with `npm test`). The page itself only
// renders these and calls the API.

/** One device as GET /api/devices lists it. */
export interface DeviceEntry {
  kind: 'mule' | 'sleepbridge' | 'o2ring' | 'ezshare' | 'flashair' | 'as11_ble' | 'ds2_ble';
  id: string;
  key: string;
  name: string;
  default_name: string;
  own_name: string | null;
  present: 'yes' | 'no' | 'unknown';
  last_heard: number;
  recording: 'yes' | 'no' | 'unknown';
  recording_reason: string;
  version: string;
  forgettable: boolean;
  detail: any;
}

export interface DevicesAnswer {
  devices: DeviceEntry[];
  stream_port: number;
  now: number;
}

/** A command as a device's detail lists it. */
export interface DeviceCommand {
  req_id: number;
  cmd: string;
  args?: any;
  state: 'queued' | 'sent' | 'answered';
  result?: any;
  answered_by_bridge?: boolean;
  answered_by_unit?: boolean;
}

// ── confirming and the recording warning ──────────────────────────────────

/**
 * An action waiting on the person, on one card: a plain confirm (a reset, a
 * move, forgetting) or the recording warning the server answered with
 * (409 {warning: "recording", reason}). In the page, never a dialog. One per
 * card at a time.
 */
export interface Pending {
  action: string;           // what the page does on yes ("reboot", "reset", ...)
  mode: 'confirm' | 'recording';
  textKey: string;          // the i18n key of the question (or of the cost, when recording)
  params: Record<string, unknown>;
  reason: string;           // the device's own words, when recording
  payload: any;             // what is posted again on yes
}

export class Confirmations {
  private byCard: Record<string, Pending> = {};

  get(card: string): Pending | null {
    return this.byCard[card] ?? null;
  }

  /** Ask before doing `action` (a destructive one). */
  ask(card: string, action: string, textKey: string, payload: any, params: Record<string, unknown> = {}): void {
    this.byCard[card] = { action, mode: 'confirm', textKey, params, reason: '', payload };
  }

  /** The server warned that the machine records: ask again, with its reason. */
  recording(card: string, action: string, costKey: string, reason: string, payload: any): void {
    this.byCard[card] = { action, mode: 'recording', textKey: costKey, params: {}, reason, payload };
  }

  /** Yes: what to post, now with confirm when it was the recording warning. */
  accept(card: string): Pending | null {
    const p = this.byCard[card] ?? null;
    delete this.byCard[card];
    if (!p) return null;
    if (p.mode === 'recording' && p.payload && typeof p.payload === 'object') {
      p.payload = { ...p.payload, confirm: true };
    }
    return p;
  }

  cancel(card: string): void {
    delete this.byCard[card];
  }
}

/** The server's recording warning, read off an HTTP error, or null. */
export function recordingWarning(e: any): { reason: string } | null {
  const body = e?.error;
  if (e?.status === 409 && body && body.warning === 'recording') return { reason: String(body.reason ?? '') };
  return null;
}

/** The error a failed request carries: the server's own words when it gave any. */
export function errorText(e: any): string {
  return e?.error?.error || e?.message || '';
}

/** The cost sentence shown with the recording warning, by action. */
export function costKey(action: string): string {
  switch (action) {
    case 'reboot':
      return 'devices.confirm.costs.reboot';
    case 'reset':
    case 'config_reset':
      return 'devices.confirm.costs.reset';
    case 'cloud':
    case 'move':
      return 'devices.confirm.costs.stream';
    case 'switch':
      return 'devices.confirm.costs.switch';
    case 'config':
      return 'devices.confirm.costs.network';
    default:
      return 'devices.confirm.costs.generic';
  }
}

// ── the Sleep Bridge's settings (firmware 1.0.1, every key, no fold) ──────

/** Every key a bridge's config takes, in the order shown. */
export const CONFIG_KEYS = [
  'ssid', 'pass', 'direct', 'wifi_fail', 'o2_on', 'o2_name', 'o2_sent',
  'serial', 'pair', 'jwt', 'boot', 'geo_epoch', 'spl_head', 'spl_drain',
] as const;

/** Never shown and never kept here: written only. */
export const SECRET_KEYS = new Set(['pass', 'pair', 'jwt']);
/** Read only at the bridge's start: "Restart to apply". */
export const NEXT_BOOT_KEYS = new Set(['o2_on', 'o2_name', 'serial', 'pair', 'jwt', 'boot', 'geo_epoch', 'spl_head', 'spl_drain']);
const NETWORK_KEYS = new Set(['ssid', 'pass']);
const IDENTITY_KEYS = new Set(['serial', 'pair', 'jwt']);
const SEQUENCE_KEYS = new Set(['boot', 'geo_epoch', 'spl_head', 'spl_drain']);
const BOOLEAN_KEYS = new Set(['o2_on']);
const NUMBER_KEYS = new Set(['direct', 'wifi_fail', 'boot', 'geo_epoch', 'spl_head', 'spl_drain']);

export interface ConfigRow {
  key: string;
  value: string;      // as shown; '' for a secret
  secret: boolean;
  nextBoot: boolean;
  pending: boolean;   // written, waiting for a restart
  type: 'boolean' | 'number' | 'string';
}

/** The table of a `get` answer's values, every key, the secrets blank. */
export function configRows(values: Record<string, unknown> | null | undefined, pending: string[] = []): ConfigRow[] {
  return CONFIG_KEYS.map((key) => {
    const secret = SECRET_KEYS.has(key);
    const raw = values ? values[key] : undefined;
    return {
      key,
      value: secret || raw === undefined || raw === null ? '' : String(raw),
      secret,
      nextBoot: NEXT_BOOT_KEYS.has(key),
      pending: pending.includes(key),
      type: BOOLEAN_KEYS.has(key) ? 'boolean' : NUMBER_KEYS.has(key) ? 'number' : 'string',
    };
  });
}

/**
 * The `set` object from the form: only what changed, typed as the bridge
 * wants it (true/false, whole numbers, strings). A secret goes when it was
 * typed at all. `error` names the first field that does not parse.
 */
export function buildSet(rows: ConfigRow[], edits: Record<string, string>): { set: Record<string, unknown>; error: string } {
  const set: Record<string, unknown> = {};
  for (const r of rows) {
    const e = edits[r.key];
    if (e === undefined) continue;
    if (r.secret) {
      if (e !== '') set[r.key] = e;
      continue;
    }
    if (e === r.value) continue;
    if (r.type === 'boolean') {
      if (e !== 'true' && e !== 'false') return { set: {}, error: r.key };
      set[r.key] = e === 'true';
    } else if (r.type === 'number') {
      if (!/^-?\d+$/.test(e.trim())) return { set: {}, error: r.key };
      set[r.key] = Number(e.trim());
    } else {
      if (e.length > 64) return { set: {}, error: r.key };
      set[r.key] = e;
    }
  }
  // A new network is never tried with the old one's password (the bridge's rule).
  if ('ssid' in set && !('pass' in set)) set['pass'] = '';
  return { set, error: '' };
}

/** The risky groups a set touches, each its own confirm, in this order. */
export function riskyGroups(set: Record<string, unknown>): ('network' | 'identity' | 'sequence')[] {
  const keys = Object.keys(set);
  const out: ('network' | 'identity' | 'sequence')[] = [];
  if (keys.some((k) => NETWORK_KEYS.has(k))) out.push('network');
  if (keys.some((k) => IDENTITY_KEYS.has(k))) out.push('identity');
  if (keys.some((k) => SEQUENCE_KEYS.has(k))) out.push('sequence');
  return out;
}

/** The keys of `set` in one group, for its confirm. */
export function groupKeys(set: Record<string, unknown>, group: 'network' | 'identity' | 'sequence'): string[] {
  const g = group === 'network' ? NETWORK_KEYS : group === 'identity' ? IDENTITY_KEYS : SEQUENCE_KEYS;
  return Object.keys(set).filter((k) => g.has(k));
}

/** Whether a set warns while the machine records (it carries the network). */
export function setTouchesNetwork(set: Record<string, unknown>): boolean {
  return Object.keys(set).some((k) => NETWORK_KEYS.has(k));
}

// ── the release line and the commands ─────────────────────────────────────

/** Which sentence the Sleep Bridge's Firmware line says about the release. */
export function releaseLine(release: any): { key: string; params: Record<string, unknown>; button: boolean } {
  if (!release || release.state === 'off' || release.state === 'unchecked') return { key: '', params: {}, button: false };
  if (release.state === 'not_published') return { key: 'devices.sleepbridge.release.notPublished', params: {}, button: false };
  if (release.state === 'failed')
    return { key: 'devices.sleepbridge.release.failed', params: { error: release.error ?? '' }, button: false };
  if (release.variant === null || release.variant === undefined)
    return { key: 'devices.sleepbridge.release.noVariant', params: {}, button: false };
  if (release.available)
    return { key: 'devices.sleepbridge.release.available', params: { version: release.version }, button: true };
  if (release.error) return { key: 'devices.sleepbridge.release.failed', params: { error: release.error }, button: false };
  return { key: 'devices.sleepbridge.release.upToDate', params: { version: release.version }, button: false };
}

/** A command's state as the list says it: key and its parameters. */
export function commandState(c: DeviceCommand): { key: string; params: Record<string, unknown> } {
  if (c.state === 'queued') return { key: 'devices.commands.state.queued', params: {} };
  if (c.state === 'sent') {
    if (c.answered_by_bridge === false || c.answered_by_unit === false)
      return { key: 'devices.commands.state.unanswered', params: {} };
    return { key: 'devices.commands.state.sent', params: {} };
  }
  const r = c.result;
  if (r && r.ok === false) return { key: 'devices.commands.state.refused', params: { error: r.error ?? '' } };
  return { key: 'devices.commands.state.answered', params: {} };
}

/** The newest command of `cmd` in a list (newest first), or null. */
export function latestCommand(list: DeviceCommand[] | undefined, cmd: string): DeviceCommand | null {
  return (list ?? []).find((c) => c.cmd === cmd) ?? null;
}

/** A bridge's or a unit's newest config answer that carried values. */
export function latestConfigValues(list: DeviceCommand[] | undefined): { values: Record<string, unknown>; pending: string[]; risks: any[] } | null {
  for (const c of list ?? []) {
    if (c.cmd !== 'config' || c.state !== 'answered' || !c.result) continue;
    if (c.result.values && typeof c.result.values === 'object') {
      return {
        values: c.result.values,
        pending: Array.isArray(c.result.pending_reboot) ? c.result.pending_reboot : [],
        risks: Array.isArray(c.result.risks) ? c.result.risks : [],
      };
    }
  }
  return null;
}

/** The Mule & Miner's switch, as its last state reports it and as last asked. */
export function switchView(unit: any, key: 'as11_enabled' | 'o2_enabled'): { on: boolean | null; pending: string } {
  const state = unit?.state ?? {};
  const on = key === 'as11_enabled' ? state?.as11?.enabled : state?.o2_enabled;
  const asked = (unit?.commands ?? []).find((c: DeviceCommand) => c.cmd === 'config' && c.args?.config && key in c.args.config);
  let pending = '';
  if (asked) {
    if (asked.state === 'queued') pending = 'devices.mule.switchQueued';
    else if (asked.state === 'sent') pending = 'devices.mule.switchSent';
    else if (asked.result?.ok === false) pending = 'devices.mule.switchRefused';
    else if (on !== asked.args.config[key]) pending = 'devices.mule.switchAnswered';
  }
  return { on: typeof on === 'boolean' ? on : null, pending };
}

// ── SDD-064: the DreamStation 2 pairing, as the page shows it ──────────────

/** "042517" -> "042 517", so six digits are easy to compare at a glance. */
export function groupCode(code: string | null | undefined): string {
  const c = (code ?? '').replace(/\D/g, '');
  return c.length === 6 ? `${c.slice(0, 3)} ${c.slice(3)}` : c;
}

export type Ds2Phase = 'idle' | 'working' | 'confirm' | 'os_dialog' | 'paired' | 'failed';

/**
 * What the pairing panel shows for a status answer: the phase, and the i18n
 * key of its one line. `radio_off` is worded per OS, as the AirSense 11's is.
 */
export function ds2PairView(status: any): { phase: Ds2Phase; key: string } {
  const p = status?.pairing ?? {};
  const state: string = p.state ?? 'idle';
  const backend: string = status?.backend ?? '';
  switch (state) {
    case 'idle': return { phase: 'idle', key: '' };
    case 'scanning': return { phase: 'working', key: 'devices.ds2.scanning' };
    case 'pairing':
      // macOS asks in its own window, and the machine's code shows meanwhile.
      return status?.os_dialog ? { phase: 'os_dialog', key: 'devices.ds2.osDialog' }
                               : { phase: 'working', key: 'devices.ds2.pairing' };
    case 'confirm': return { phase: 'confirm', key: 'devices.ds2.confirmQuestion' };
    case 'os_dialog': return { phase: 'os_dialog', key: 'devices.ds2.osDialog' };
    case 'paired': return { phase: 'paired', key: 'devices.ds2.done' };
    case 'radio_off':
      return { phase: 'failed', key: backend === 'corebluetooth' ? 'devices.ds2.errRadioOffMac'
                                   : backend === 'winrt' ? 'devices.ds2.errRadioOffWin' : 'devices.ds2.errRadioOff' };
    case 'rejected': return { phase: 'failed', key: 'devices.ds2.errRejected' };
    case 'timeout': return { phase: 'failed', key: 'devices.ds2.errTimeout' };
    case 'not_found': return { phase: 'failed', key: 'devices.ds2.errNotFound' };
    case 'unsupported': return { phase: 'failed', key: 'devices.ds2.errUnsupported' };
    case 'cancelled': return { phase: 'failed', key: 'devices.ds2.cancelled' };
    default: return { phase: 'failed', key: 'devices.ds2.errGeneric' };
  }
}

/** The pairing is still running: the page polls the status every second. */
export function ds2PairRunning(status: any): boolean {
  const ph = ds2PairView(status).phase;
  return ph === 'working' || ph === 'confirm' || ph === 'os_dialog';
}

/** A refusal of a DS2 request (409 result), as its i18n key. */
export function ds2RefusalKey(result: string | undefined, backend = ''): string {
  switch (result) {
    case 'busy': return 'devices.ds2.errBusy';
    case 'not_paired': return 'devices.ds2.notPaired';
    case 'disabled': return 'devices.ds2.errDisabled';
    case 'not_compiled': return 'devices.ds2.notCompiled';
    case 'radio_off':
      return backend === 'corebluetooth' ? 'devices.ds2.errRadioOffMac'
           : backend === 'winrt' ? 'devices.ds2.errRadioOffWin' : 'devices.ds2.errRadioOff';
    default: return 'devices.ds2.errGeneric';
  }
}

/** The sources that can say a night ended, in the order the line names them. */
const NIGHT_SOURCES = ['sleep_bridge', 'mule_ble', 'as11_ble', 'o2ring', 'ezshare', 'flashair', 'local'];

/**
 * When the DreamStation is read (SDD-064 D5a): once a day at read_at, or only
 * when asked; in both modes also when another source here says the night
 * ended. The i18n key of the line, the i18n keys of the sources it names
 * (unknown ones left out; none means the night-end clause is left out), and
 * the time for the daily mode.
 */
export function ds2Reads(status: any): { key: string; sources: string[]; time: string } {
  const named: string[] = Array.isArray(status?.night_sources) ? status.night_sources : [];
  const sources = NIGHT_SOURCES.filter((s) => named.includes(s)).map((s) => 'devices.ds2.source.' + s);
  const time: string = status?.read_at || '11:00';
  if (status?.read_mode === 'manual')
    return { key: sources.length ? 'devices.ds2.readsManualNight' : 'devices.ds2.readsManualOnly', sources, time };
  return { key: sources.length ? 'devices.ds2.readsDailyNight' : 'devices.ds2.readsDaily', sources, time };
}

/** The next daily read, epoch s, or 0 when none is planned (manual, or nothing paired). */
export function ds2NextRead(status: any): number {
  return status?.read_mode !== 'manual' && Number(status?.next_read_at) > 0 ? Number(status.next_read_at) : 0;
}

/** A real calendar day as YYYY-MM-DD, nothing else. */
export function isIsoDay(s: string | null | undefined): boolean {
  if (!s || !/^\d{4}-\d{2}-\d{2}$/.test(s)) return false;
  const d = new Date(s + 'T00:00:00Z');
  return !isNaN(d.getTime()) && d.toISOString().slice(0, 10) === s;
}

/**
 * #67: the line for a Reset Bluetooth that was not done, by the `reason` code
 * the service sends. Never the service's own English: an unknown code (or
 * none) is a generic line.
 */
export function as11ResetRefusalKey(reason: string | null | undefined): string {
  switch (reason) {
    case 'streaming':       return 'devices.as11.resetNotStreaming';
    case 'o2ring_transfer': return 'devices.as11.resetNotO2Ring';
    case 'pairing_open':    return 'devices.as11.resetNotPairing';
    default:                return 'devices.as11.resetNotGeneric';
  }
}

/** The address and port this page reaches the service on, for the empty state. */
export function hostOf(location: { hostname: string }): string {
  return location.hostname || 'localhost';
}
