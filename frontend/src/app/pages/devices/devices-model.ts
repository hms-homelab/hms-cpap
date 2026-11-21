// The Devices page's decisions, as plain data, so they are tested without a
// browser (devices-model.spec.ts, run with `npm test`). The page itself only
// renders these and calls the API.

/** One device as GET /api/devices lists it. */
export interface DeviceEntry {
  kind: 'mule' | 'sleepbridge' | 'o2ring' | 'ezshare' | 'flashair' | 'as11_ble' | 'ds2_ble' | 'garmin';
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
  /** SDD-074: the "Machine clock" (the "Ring clock" on the o2ring card). */
  clock?: ClockView;
}

/** SDD-074: a card's clock, as GET /api/devices carries it. */
export interface ClockView {
  device_id: string;
  /** "" = same as this computer, "+10:00" a fixed offset, or a zone name. */
  setting: string;
  kind: 'host' | 'fixed' | 'zone';
  offset_now: number;
  offset_text: string;
  host_zone: string;
  host_offset_now: number;
  host_offset_text: string;
  /**
   * Issue #95: offset is the whole learned offset (drift + zone). from says
   * where it was learned: the machine's own card stamps, a BRP seen growing,
   * or "" (from before 7.0.0). offer and preview come from the server.
   */
  learned: {
    offset: number; text: string; at: number;
    from?: string; nights?: number; use?: string; offer?: boolean;
    preview?: { night_start: string; now: string; would_be: string } | null;
  } | null;
  difference_s: number;
  warn: boolean;
}

export interface DevicesAnswer {
  devices: DeviceEntry[];
  stream_port: number;
  now: number;
  /** SDD-081 D4: the OSCAR helper on a home PC, null when this install has none. */
  oscar_helper?: OscarHelper | null;
}

/** SDD-081: the remembered OSCAR helper. */
export interface OscarHelper {
  remembered: boolean;
  name: string;
  connected: boolean;
  version?: string;
  update?: boolean;
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

// ── SDD-074: each machine is read on its own clock ────────────────────────

export type ClockChoice = 'host' | 'fixed' | 'zone';

/** Seconds east of UTC as the setting writes it: 36000 -> "+10:00", -16200 -> "-04:30". */
export function offsetSetting(seconds: number): string {
  const s = Math.round(Number(seconds) || 0);
  const a = Math.abs(s);
  const two = (n: number) => n.toString().padStart(2, '0');
  return `${s < 0 ? '-' : '+'}${two(Math.floor(a / 3600))}:${two(Math.floor((a % 3600) / 60))}`;
}

/** Every fixed offset offered, UTC-12:00 to UTC+14:00 in 15 minute steps, as setting text. */
export function fixedOffsets(): string[] {
  const out: string[] = [];
  for (let s = -12 * 3600; s <= 14 * 3600; s += 900) out.push(offsetSetting(s));
  return out;
}

/** Which of the three a setting is: "" this computer, "+10:00" a fixed offset, else a zone name. */
export function clockChoiceOf(setting: string | null | undefined): ClockChoice {
  const s = (setting ?? '').trim();
  if (!s) return 'host';
  return /^[+-]\d{2}:\d{2}$/.test(s) ? 'fixed' : 'zone';
}

/** What the form edits, per card, until it is saved. */
export interface ClockDraft {
  choice: ClockChoice;
  fixed: string;    // "+10:00"
  zone: string;     // "Australia/Sydney"
  query: string;    // the zone search text
  open: boolean;    // the zone list is showing
}

/** The form as the saved clock fills it. A fixed offset starts on the learned one, or UTC+00:00. */
export function clockDraftOf(clock: ClockView): ClockDraft {
  const choice = clockChoiceOf(clock.setting);
  const fallback = clock.learned ? offsetSetting(clock.learned.offset) : offsetSetting(clock.host_offset_now);
  return {
    choice,
    fixed: choice === 'fixed' ? clock.setting : fallback,
    zone: choice === 'zone' ? clock.setting : '',
    query: '',
    open: false,
  };
}

/** The setting the form sends, or null while it is not complete (a time zone not picked yet). */
export function clockSettingOf(draft: ClockDraft): string | null {
  if (draft.choice === 'host') return '';
  if (draft.choice === 'fixed') return draft.fixed || null;
  return draft.zone.trim() || null;
}

/** Save is offered only for a complete form that differs from what is saved. */
export function clockChanged(draft: ClockDraft, clock: ClockView): boolean {
  const s = clockSettingOf(draft);
  return s !== null && s !== (clock.setting ?? '');
}

/**
 * The zone names a search shows: every word typed must appear (no case, an
 * underscore reads as a space), names starting with the text first. Nothing
 * typed shows the whole list.
 */
export function filterZones(zones: string[], query: string, limit = 200): string[] {
  const words = query.toLowerCase().replace(/_/g, ' ').split(/[\s/]+/).filter(Boolean);
  if (!words.length) return zones.slice(0, limit);
  const norm = (z: string) => z.toLowerCase().replace(/_/g, ' ');
  const hits = zones.filter((z) => words.every((w) => norm(z).includes(w)));
  const first = words[0];
  const starts = (z: string) => norm(z).split('/').some((part) => part.startsWith(first));
  return [...hits.filter(starts), ...hits.filter((z) => !starts(z))].slice(0, limit);
}

/**
 * How far apart the machine's files and the clock in use are, as the i18n key
 * and its numbers: minutes under an hour, else hours (and minutes when any).
 */
export function clockDifference(seconds: number): { key: string; params: Record<string, unknown> } {
  const min = Math.round(Math.abs(Number(seconds) || 0) / 60);
  if (min < 60) return { key: 'devices.clock.diffMinutes', params: { m: min } };
  const h = Math.floor(min / 60);
  const m = min % 60;
  return m ? { key: 'devices.clock.diffHoursMinutes', params: { h, m } } : { key: 'devices.clock.diffHours', params: { h } };
}

/**
 * What the card says about the clock learned from the machine's files
 * (decision 1: a suggestion only). Null when nothing was learned. `use` is
 * the fixed offset "Use this" saves.
 *
 * Issue #95: `offer` is the server's word: true only when the machine's own
 * card stamps said it on two separate nights and it is not the setting
 * already. The card shows the suggestion (and its warning) only then, never
 * a one-click fix otherwise (item 10). `preview` is when the last night
 * would start with it, said beside the button.
 */
export function learnedOffer(clock: ClockView | null | undefined): {
  text: string; at: number; use: string; offer: boolean;
  preview: { now: string; would_be: string } | null;
} | null {
  const l = clock?.learned;
  if (!l) return null;
  const use = l.use || offsetSetting(l.offset);
  const already = (clock?.setting ?? '') === use;
  const offer = l.offer === true && !already;
  const preview = offer && l.preview?.now && l.preview?.would_be ? { now: l.preview.now, would_be: l.preview.would_be } : null;
  return { text: l.text || use, at: Number(l.at) || 0, use, offer, preview };
}

/**
 * Item 10, layout B: does this card have destructive actions (shown apart,
 * at the right of its actions row)? A Mule & Miner and a Sleep Bridge always
 * do (Reset Wi-Fi, Forget this device); a paired AirSense 11 or DreamStation 2
 * its Forget pairing.
 */
export function hasDangerActions(d: { kind: string; detail?: any }): boolean {
  if (d.kind === 'mule' || d.kind === 'sleepbridge') return true;
  if (d.kind === 'as11_ble' || d.kind === 'ds2_ble') return !d.detail?.not_compiled && !!d.detail?.paired;
  return false;
}

/**
 * #100: a Mule & Miner that reads a card is listed in a limited state: its
 * reason, "Switch to the CpapDash cloud" and (once it is away) Forget, and
 * nothing that needs its stream.
 */
export function muleLimited(d: { kind: string; detail?: any }): boolean {
  return d.kind === 'mule' && d.detail?.limited === true;
}

/**
 * The switch to the cloud as the device answers it: switching while the
 * command waits or is out; switched once it answered ok; failed when it
 * answered that it could not and stayed. A Sleep Bridge that goes away with
 * the command out has switched. A Mule & Miner always answers on this socket
 * before it moves (a 6.0.0 unit first closes it to test the cloud and opens
 * it again to answer, #100), so while it is away it is still switching.
 */
export function cloudSwitchState(
  kind: string, present: string, c: DeviceCommand | null | undefined,
): 'switching' | 'switched' | 'failed' {
  if (c && c.state === 'answered') return c.result?.ok === false ? 'failed' : 'switched';
  if (kind !== 'mule' && present === 'no' && (!c || c.state === 'sent')) return 'switched';
  return 'switching';
}

/** A "Re-read my nights" in progress on one card. */
export interface Reread {
  backup: number;                // tables backed up before the re-read
  before: string;                // the backfill's completed_at when it was asked
  seenRunning: boolean;
  status: any;                   // the latest GET /api/backfill/status
}

/**
 * The line a re-read shows, from the backfill worker's own status: reading
 * (with folders when it counts them) while it runs; done once it ran, or once
 * it finished after the click (completed_at moved); before that, the backup.
 */
export function rereadLine(r: Reread): { key: string; params: Record<string, unknown>; done: boolean; err: boolean } {
  const s = r.status ?? {};
  if (s.status === 'running') {
    return s.folders_total > 0
      ? { key: 'devices.clock.rereadProgress', params: { done: s.folders_done ?? 0, total: s.folders_total }, done: false, err: false }
      : { key: 'devices.clock.rereadStarted', params: { n: r.backup }, done: false, err: false };
  }
  const finished = r.seenRunning || (!!s.completed_at && s.completed_at !== r.before);
  if (!finished) return { key: 'devices.clock.rereadStarted', params: { n: r.backup }, done: false, err: false };
  if (s.status === 'error')
    return { key: 'devices.clock.rereadFailed', params: { error: s.error_message ?? '' }, done: true, err: true };
  const params = { n: s.sessions_saved ?? 0, errors: s.errors ?? 0 };
  return s.errors > 0
    ? { key: 'devices.clock.rereadDoneErrors', params, done: true, err: true }
    : { key: 'devices.clock.rereadDone', params, done: true, err: false };
}

/** The address and port this page reaches the service on, for the empty state. */
export function hostOf(location: { hostname: string }): string {
  return location.hostname || 'localhost';
}
