// SDD-065: the DreamStation 2 controls on a Mule & Miner's card, without a
// browser. Made-up units only.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  muleDs2ErrorKey, muleDs2ModePayload, muleDs2On, muleDs2Pairing, muleDs2Reading, muleDs2RefusalKey, muleDs2Switch,
} from './mule-ds2-model.ts';

const NOW = 2_000_000_000;

function unit(ds2: any, commands: any[] = []): any {
  return { state: ds2 === null ? {} : { ds2 }, commands };
}
function bond(action: string, state: string, result?: any, answered_at = NOW - 5): any {
  return { req_id: 1, cmd: 'ds2_bond', args: { action }, state, result, answered_at };
}

test('the mode is the unit\'s own report, and its switch follows the last config', () => {
  assert.equal(muleDs2On(unit(null)), false);
  assert.equal(muleDs2On(unit({ enabled: true })), true);
  assert.deepEqual(muleDs2ModePayload(true), { cmd: 'config', config: { ble_machine: 'ds2' } });
  assert.deepEqual(muleDs2ModePayload(false), { cmd: 'config', config: { ble_machine: '' } });

  const asked = (state: string, value: string, result?: any) =>
    ({ req_id: 2, cmd: 'config', args: { config: { ble_machine: value } }, state, result });
  assert.deepEqual(muleDs2Switch(unit(null)), { on: false, pending: '' });
  assert.equal(muleDs2Switch(unit(null, [asked('queued', 'ds2')])).pending, 'devices.mule.switchQueued');
  assert.equal(muleDs2Switch(unit(null, [asked('sent', 'ds2')])).pending, 'devices.mule.switchSent');
  assert.equal(muleDs2Switch(unit(null, [asked('answered', 'ds2', { ok: false, error: 'x' })])).pending,
               'devices.mule.switchRefused');
  // Taken, the miner restarting: until its state says so.
  assert.equal(muleDs2Switch(unit(null, [asked('answered', 'ds2', { ok: true })])).pending,
               'devices.mule.switchAnswered');
  assert.equal(muleDs2Switch(unit({ enabled: true }, [asked('answered', 'ds2', { ok: true })])).pending, '');
  // Another switch's config is not this one's.
  const other = { req_id: 3, cmd: 'config', args: { config: { o2_enabled: true } }, state: 'queued' };
  assert.equal(muleDs2Switch(unit({ enabled: true }, [other])).pending, '');
});

test('pairing: asked, the code held, confirmed', () => {
  assert.equal(muleDs2Pairing(unit({ enabled: true }), NOW).phase, 'idle');
  assert.equal(muleDs2Pairing(unit({ enabled: true }, [bond('start', 'sent')]), NOW).phase, 'working');

  const held = { ok: true, result: { ok: true, ds2_bond: { ok: true, bonded: false, bonding: { code: '042517', expires_s: 25 } } } };
  const p = muleDs2Pairing(unit({ enabled: true }, [bond('start', 'answered', held)]), NOW);
  assert.equal(p.phase, 'confirm');
  assert.equal(p.code, '042517');
  assert.equal(p.secondsLeft, 20);
  assert.equal(p.key, 'devices.muleDs2.confirmQuestion');

  // The unit's state, fresher, wins for the seconds left.
  const fresh = muleDs2Pairing(unit({ enabled: true, bonding: { code: '042517', expires_s: 9 } },
                                    [bond('start', 'answered', held)]), NOW);
  assert.equal(fresh.secondsLeft, 9);

  // The window closed without an answer.
  assert.equal(muleDs2Pairing(unit({ enabled: true }, [bond('start', 'answered', held, NOW - 60)]), NOW).key,
               'devices.muleDs2.timeout');

  assert.equal(muleDs2Pairing(unit({ enabled: true }, [bond('confirm', 'sent')]), NOW).phase, 'confirming');
  const bonded = { ok: true, result: { ok: true, ds2_bond: { ok: true, bonded: true, peer: 'AA:BB:CC:00:00:01' } } };
  assert.equal(muleDs2Pairing(unit({ enabled: true }, [bond('confirm', 'answered', bonded)]), NOW).phase, 'paired');
  // Just Works answers bonded at once.
  assert.equal(muleDs2Pairing(unit({ enabled: true }, [bond('start', 'answered', bonded)]), NOW).phase, 'paired');
});

test('pairing: refused, cancelled, forgotten', () => {
  const refused = { ok: false, error: 'not_found', result: { ok: false, ds2_bond: { ok: false, error: 'not_found' } } };
  const f = muleDs2Pairing(unit({ enabled: true }, [bond('start', 'answered', refused)]), NOW);
  assert.equal(f.phase, 'failed');
  assert.equal(f.key, 'devices.muleDs2.err.not_found');
  assert.equal(f.error, 'not_found');
  const mismatch = { ok: false, result: { ok: false, ds2_bond: { ok: false, error: 'code_mismatch' } } };
  assert.equal(muleDs2Pairing(unit({ enabled: true }, [bond('confirm', 'answered', mismatch)]), NOW).key,
               'devices.muleDs2.err.code_mismatch');
  assert.equal(muleDs2Pairing(unit({ enabled: true }, [bond('cancel', 'answered', { ok: true })]), NOW).key,
               'devices.ds2.cancelled');
  assert.equal(muleDs2Pairing(unit({ enabled: true }, [bond('forget', 'sent')]), NOW).key, 'devices.muleDs2.forgetting');
  assert.equal(muleDs2Pairing(unit({ enabled: true }, [bond('forget', 'answered', { ok: true })]), NOW).phase, 'idle');
});

test('a read under way, and why Read now was refused', () => {
  const read = (state: string) => ({ req_id: 4, cmd: 'ds2_read', args: {}, state });
  assert.equal(muleDs2Reading(unit({ enabled: true })), false);
  assert.equal(muleDs2Reading(unit({ enabled: true }, [read('sent')])), true);
  assert.equal(muleDs2Reading(unit({ enabled: true }, [read('answered')])), false);
  assert.equal(muleDs2Reading(unit({ enabled: true, busy: true })), true);
  // Busy with a pairing is not a read: asked, or the code held.
  assert.equal(muleDs2Reading(unit({ enabled: true, busy: true }, [bond('start', 'sent')])), false);
  assert.equal(muleDs2Reading(unit({ enabled: true, busy: true, bonding: { code: '042517', expires_s: 9 } })), false);
  assert.equal(muleDs2Reading(unit({ enabled: true, busy: true }, [bond('start', 'answered', { ok: true })])), false);
  // A read asked after the pairing is one.
  assert.equal(muleDs2Reading(unit({ enabled: true }, [read('sent'), bond('confirm', 'answered', { ok: true })])), true);

  assert.equal(muleDs2RefusalKey('offline'), 'devices.muleDs2.err.offline');
  assert.equal(muleDs2RefusalKey('not_paired'), 'devices.ds2.notPaired');
  assert.equal(muleDs2RefusalKey('busy'), 'devices.ds2.errBusy');
  assert.equal(muleDs2RefusalKey('mode_off'), 'devices.muleDs2.err.mode_off');
  assert.equal(muleDs2RefusalKey(undefined), 'devices.muleDs2.err.other');
});

test('every token the unit or this computer says has its line', () => {
  for (const t of ['not_found', 'link_lost', 'other_machine', 'stream_broken', 'no_answer', 'storage'])
    assert.equal(muleDs2ErrorKey(t), 'devices.muleDs2.err.' + t);
  assert.equal(muleDs2ErrorKey('status_0b'), 'devices.muleDs2.err.protocol');
  assert.equal(muleDs2ErrorKey('something_new'), 'devices.muleDs2.err.other');
  assert.equal(muleDs2ErrorKey(''), 'devices.muleDs2.err.other');
});
