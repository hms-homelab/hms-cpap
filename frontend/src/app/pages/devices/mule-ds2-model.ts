// SDD-065: a DreamStation 2 read through a Mule & Miner, as the Devices page
// shows it on the unit's card. Plain data, tested in mule-ds2-model.spec.ts.
//
// What the unit reports is in its state (`state.ds2`: enabled, bonded, peer,
// busy, bonding {code, expires_s}, last_error); what this computer keeps is
// in `ds2_local` (the import day, the last read, what starts a read); the
// pairing and the reads are the unit's commands (`commands`, newest first).

import type { DeviceCommand } from './devices-model';

/** The unit is in DreamStation 2 mode (its miner reads one over Bluetooth). */
export function muleDs2On(unit: any): boolean {
  return unit?.state?.ds2?.enabled === true;
}

/** The DreamStation 2 switch beside the AirSense 11 and ring switches. */
export function muleDs2Switch(unit: any): { on: boolean; pending: string } {
  const on = muleDs2On(unit);
  const asked = (unit?.commands ?? []).find(
    (c: DeviceCommand) => c.cmd === 'config' && c.args?.config && 'ble_machine' in c.args.config);
  let pending = '';
  if (asked) {
    const want = asked.args.config.ble_machine === 'ds2';
    if (asked.state === 'queued') pending = 'devices.mule.switchQueued';
    else if (asked.state === 'sent') pending = 'devices.mule.switchSent';
    else if (asked.result?.ok === false) pending = 'devices.mule.switchRefused';
    else if (on !== want) pending = 'devices.mule.switchAnswered';
  }
  return { on, pending };
}

/** The config that turns the mode on or off (the miner restarts with it). */
export function muleDs2ModePayload(on: boolean): any {
  return { cmd: 'config', config: { ble_machine: on ? 'ds2' : '' } };
}

const ERR_TOKENS = new Set([
  'mode_off', 'busy', 'not_found', 'connect', 'not_paired', 'bond_lost', 'protocol', 'aborted', 'pair_failed',
  'link_lost', 'other_machine', 'code_mismatch', 'no_answer', 'stream_broken', 'storage', 'no_bytes', 'too_large',
  'no_serial', 'bad_file', 'offline',
]);

/** A token from the unit or from this computer, as its i18n key. */
export function muleDs2ErrorKey(token: string | null | undefined): string {
  const t = token ?? '';
  if (/^status_[0-9a-f]{2}$/.test(t)) return 'devices.muleDs2.err.protocol';
  return ERR_TOKENS.has(t) ? 'devices.muleDs2.err.' + t : 'devices.muleDs2.err.other';
}

export type MuleDs2Phase = 'idle' | 'working' | 'confirm' | 'confirming' | 'paired' | 'failed';

export interface MuleDs2Pairing {
  phase: MuleDs2Phase;
  key: string;           // the one line under the panel
  code: string;          // six digits while `confirm`
  secondsLeft: number;   // while `confirm`
  error: string;         // the token, for the line's parameter
}

/**
 * The pairing as the unit answers it. `now` is epoch seconds. The newest
 * ds2_bond decides: still going, the code held, bonded, or why not.
 */
export function muleDs2Pairing(unit: any, now: number): MuleDs2Pairing {
  const idle: MuleDs2Pairing = { phase: 'idle', key: '', code: '', secondsLeft: 0, error: '' };
  const held = unit?.state?.ds2?.bonding;
  const last: DeviceCommand | undefined = (unit?.commands ?? []).find((c: DeviceCommand) => c.cmd === 'ds2_bond');
  if (!last) {
    // A code held without a command from here (asked from elsewhere): still shown.
    if (held?.code) return { ...idle, phase: 'confirm', key: 'devices.muleDs2.confirmQuestion', code: held.code,
                             secondsLeft: Number(held.expires_s ?? 0) };
    return idle;
  }
  const action: string = last.args?.action ?? '';
  if (last.state !== 'answered') {
    switch (action) {
      case 'start': return { ...idle, phase: 'working', key: 'devices.muleDs2.asking' };
      case 'confirm': return { ...idle, phase: 'confirming', key: 'devices.muleDs2.confirming' };
      case 'forget': return { ...idle, phase: 'working', key: 'devices.muleDs2.forgetting' };
      default: return { ...idle, phase: 'working', key: 'devices.muleDs2.cancelling' };
    }
  }
  const r = last.result ?? {};
  const b = r?.result?.ds2_bond ?? {};
  if (r.ok === false || b.ok === false) {
    const token = b.error || r.error || '';
    return { ...idle, phase: 'failed', key: muleDs2ErrorKey(token), error: token };
  }
  if (action === 'start') {
    if (b.bonded && !b.bonding) return { ...idle, phase: 'paired', key: 'devices.muleDs2.done' };
    const code: string = held?.code || b.bonding?.code || '';
    const answeredAt = Number((last as any).answered_at ?? 0);
    const left = held?.code ? Number(held.expires_s ?? 0)
                            : Math.max(0, answeredAt + Number(b.bonding?.expires_s ?? 0) - now);
    if (code && left > 0) return { ...idle, phase: 'confirm', key: 'devices.muleDs2.confirmQuestion', code, secondsLeft: left };
    return { ...idle, phase: 'failed', key: 'devices.muleDs2.timeout' };
  }
  if (action === 'confirm') {
    if (b.bonded) return { ...idle, phase: 'paired', key: 'devices.muleDs2.done' };
    return { ...idle, phase: 'failed', key: muleDs2ErrorKey('pair_failed'), error: 'pair_failed' };
  }
  if (action === 'cancel') return { ...idle, phase: 'failed', key: 'devices.ds2.cancelled' };
  return idle;   // forget, answered
}

/**
 * A read is under way: one asked and not answered, or the miner busy with no
 * pairing to explain it (a pairing holds the miner busy too, code and all).
 */
export function muleDs2Reading(unit: any): boolean {
  const newest: DeviceCommand | undefined = (unit?.commands ?? []).find(
    (c: DeviceCommand) => c.cmd === 'ds2_read' || c.cmd === 'ds2_bond');
  if (newest?.cmd === 'ds2_read' && newest.state !== 'answered') return true;
  const d = unit?.state?.ds2;
  return d?.busy === true && !d?.bonding && newest?.cmd !== 'ds2_bond';
}

/** "Read now" refused (409 `result`), as its i18n key. */
export function muleDs2RefusalKey(result: string | undefined): string {
  switch (result) {
    case 'offline': return 'devices.muleDs2.err.offline';
    case 'mode_off': return 'devices.muleDs2.err.mode_off';
    case 'not_paired': return 'devices.ds2.notPaired';
    case 'busy': return 'devices.ds2.errBusy';
    default: return 'devices.muleDs2.err.other';
  }
}
