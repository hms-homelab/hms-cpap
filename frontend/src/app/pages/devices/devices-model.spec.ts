// The Devices page's decisions, without a browser. `npm test` runs it with
// Node's own test runner and TypeScript support.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  CONFIG_KEYS, Confirmations, buildSet, commandState, configRows, costKey, errorText, groupKeys, latestConfigValues,
  recordingWarning, releaseLine, riskyGroups, setTouchesNetwork, switchView,
  groupCode, ds2PairView, ds2PairRunning, ds2RefusalKey, ds2Reads, ds2NextRead, isIsoDay, as11ResetRefusalKey,
} from './devices-model.ts';

// ── the in-page confirm and the recording warning ──

test('a plain confirm gives back what to post, unchanged', () => {
  const c = new Confirmations();
  c.ask('mule/FE-1', 'reset', 'devices.mule.resetConfirm', { cmd: 'reset' });
  assert.equal(c.get('mule/FE-1')?.mode, 'confirm');
  const p = c.accept('mule/FE-1');
  assert.deepEqual(p?.payload, { cmd: 'reset' });
  assert.equal(c.get('mule/FE-1'), null);
});

test('the recording warning posts again with confirm, and only on yes', () => {
  const c = new Confirmations();
  c.recording('sleepbridge/B', 'reboot', costKey('reboot'), 'the machine wrote to its card 2 min ago', { cmd: 'reboot' });
  const w = c.get('sleepbridge/B');
  assert.equal(w?.mode, 'recording');
  assert.equal(w?.reason, 'the machine wrote to its card 2 min ago');
  assert.equal(w?.textKey, 'devices.confirm.costs.reboot');
  c.cancel('sleepbridge/B');
  assert.equal(c.accept('sleepbridge/B'), null, 'Not now sends nothing');
  c.recording('sleepbridge/B', 'reboot', costKey('reboot'), 'r', { cmd: 'reboot' });
  assert.deepEqual(c.accept('sleepbridge/B')?.payload, { cmd: 'reboot', confirm: true });
});

test('one pending question per card; cards do not share it', () => {
  const c = new Confirmations();
  c.ask('a', 'reset', 'k', {});
  c.ask('b', 'cloud', 'k', {});
  c.ask('a', 'move', 'k2', {});
  assert.equal(c.get('a')?.action, 'move');
  assert.equal(c.get('b')?.action, 'cloud');
});

test('the 409 warning is told apart from any other error', () => {
  assert.deepEqual(recordingWarning({ status: 409, error: { warning: 'recording', reason: 'streaming', confirm: true } }),
                   { reason: 'streaming' });
  assert.equal(recordingWarning({ status: 409, error: { error: 'the bridge is not connected' } }), null);
  assert.equal(recordingWarning({ status: 500, error: { warning: 'recording' } }), null);
  assert.equal(errorText({ error: { error: 'nope' } }), 'nope');
  assert.equal(errorText({ message: 'Http failure' }), 'Http failure');
});

test('each warned action says what it costs', () => {
  assert.equal(costKey('reset'), 'devices.confirm.costs.reset');
  assert.equal(costKey('config_reset'), 'devices.confirm.costs.reset');
  assert.equal(costKey('cloud'), 'devices.confirm.costs.stream');
  assert.equal(costKey('switch'), 'devices.confirm.costs.switch');
  assert.equal(costKey('config'), 'devices.confirm.costs.network');
  assert.equal(costKey('update'), 'devices.confirm.costs.generic');
});

// ── the bridge's settings ──

test('every key is a row, no fold, and the secrets are blank', () => {
  const rows = configRows({ ssid: 'Home', pass: '***', o2_on: true, spl_head: 41 }, ['o2_on']);
  assert.equal(rows.length, CONFIG_KEYS.length);
  const by = Object.fromEntries(rows.map((r) => [r.key, r]));
  assert.equal(by.ssid.value, 'Home');
  assert.equal(by.pass.value, '', 'the password is never shown, not even as ***');
  assert.equal(by.pass.secret, true);
  assert.equal(by.jwt.secret, true);
  assert.equal(by.pair.secret, true);
  assert.equal(by.o2_on.type, 'boolean');
  assert.equal(by.o2_on.pending, true);
  assert.equal(by.spl_head.type, 'number');
  assert.equal(by.spl_head.nextBoot, true);
  assert.equal(by.ssid.nextBoot, false);
  assert.equal(by.direct.value, '', 'a key the answer did not carry is blank');
});

test('the set carries only what changed, typed as the bridge wants it', () => {
  const rows = configRows({ ssid: 'Home', o2_on: false, o2_name: '', spl_head: 41 });
  const { set, error } = buildSet(rows, { o2_on: 'true', o2_name: '', spl_head: '41', ssid: 'Home' });
  assert.equal(error, '');
  assert.deepEqual(set, { o2_on: true });
  const n = buildSet(rows, { spl_head: '40' });
  assert.deepEqual(n.set, { spl_head: 40 });
  assert.equal(buildSet(rows, { spl_head: '4x' }).error, 'spl_head');
  assert.equal(buildSet(rows, { o2_on: 'yes' }).error, 'o2_on');
  assert.equal(buildSet(rows, { o2_name: 'x'.repeat(65) }).error, 'o2_name');
});

test('a password goes when typed; a new network never goes without one', () => {
  const rows = configRows({ ssid: 'Home' });
  assert.deepEqual(buildSet(rows, { pass: '' }).set, {}, 'an empty write-only field sends nothing');
  assert.deepEqual(buildSet(rows, { pass: 'hunter2222' }).set, { pass: 'hunter2222' });
  assert.deepEqual(buildSet(rows, { ssid: 'Cabin' }).set, { ssid: 'Cabin', pass: '' }, 'an open network unless typed');
  assert.deepEqual(buildSet(rows, { ssid: 'Cabin', pass: 'pw-cabin-1' }).set, { ssid: 'Cabin', pass: 'pw-cabin-1' });
});

test('each risky key keeps its own confirm', () => {
  assert.deepEqual(riskyGroups({ o2_on: true }), []);
  assert.deepEqual(riskyGroups({ ssid: 'x', pass: 'y', jwt: 't', spl_head: 1 }), ['network', 'identity', 'sequence']);
  assert.deepEqual(groupKeys({ serial: 'S', boot: 2, spl_drain: 3 }, 'sequence'), ['boot', 'spl_drain']);
  assert.equal(setTouchesNetwork({ pass: 'p' }), true);
  assert.equal(setTouchesNetwork({ o2_name: 'r' }), false);
});

test('the newest config answer with values is the table', () => {
  const list = [
    { req_id: 3, cmd: 'config', state: 'sent' as const },
    { req_id: 2, cmd: 'config', state: 'answered' as const, result: { ok: true, values: { ssid: 'A' }, pending_reboot: ['o2_on'], risks: [{ key: 'serial', risk: 'identity' }] } },
    { req_id: 1, cmd: 'config', state: 'answered' as const, result: { ok: true, values: { ssid: 'old' } } },
  ];
  const v = latestConfigValues(list);
  assert.equal(v?.values.ssid, 'A');
  assert.deepEqual(v?.pending, ['o2_on']);
  assert.equal(v?.risks.length, 1);
  assert.equal(latestConfigValues([]), null);
});

// ── the release line ──

test('the release line: 403 offers the file, an update only with a known board', () => {
  assert.equal(releaseLine({ state: 'not_published' }).key, 'devices.sleepbridge.release.notPublished');
  assert.equal(releaseLine({ state: 'not_published' }).button, false);
  assert.equal(releaseLine({ state: 'failed', error: 'x' }).key, 'devices.sleepbridge.release.failed');
  assert.equal(releaseLine({ state: 'ok', variant: null, available: false, version: '1.0.2' }).key,
               'devices.sleepbridge.release.noVariant');
  const a = releaseLine({ state: 'ok', variant: '', available: true, version: '1.0.2' });
  assert.equal(a.button, true);
  assert.deepEqual(a.params, { version: '1.0.2' });
  assert.equal(releaseLine({ state: 'ok', variant: '', available: false, version: '1.0.2' }).key,
               'devices.sleepbridge.release.upToDate');
  assert.equal(releaseLine({ state: 'off' }).key, '');
  assert.equal(releaseLine(null).button, false);
});

// ── commands and switches ──

test('a command reads queued, sent, answered, refused, or never answered', () => {
  assert.equal(commandState({ req_id: 1, cmd: 'logs', state: 'queued' }).key, 'devices.commands.state.queued');
  assert.equal(commandState({ req_id: 1, cmd: 'logs', state: 'sent' }).key, 'devices.commands.state.sent');
  assert.equal(commandState({ req_id: 1, cmd: 'reboot', state: 'sent', answered_by_bridge: false }).key,
               'devices.commands.state.unanswered');
  const r = commandState({ req_id: 1, cmd: 'config', state: 'answered', result: { ok: false, error: 'use stream' } });
  assert.equal(r.key, 'devices.commands.state.refused');
  assert.deepEqual(r.params, { error: 'use stream' });
  assert.equal(commandState({ req_id: 1, cmd: 'logs', state: 'answered', result: { ok: true } }).key,
               'devices.commands.state.answered');
});

test('a Mule & Miner switch shows the unit state and the last ask', () => {
  const unit = {
    state: { as11: { enabled: true }, o2_enabled: false },
    commands: [{ req_id: 2, cmd: 'config', state: 'sent', args: { config: { o2_enabled: true } } }],
  };
  assert.deepEqual(switchView(unit, 'as11_enabled'), { on: true, pending: '' });
  assert.deepEqual(switchView(unit, 'o2_enabled'), { on: false, pending: 'devices.mule.switchSent' });
  assert.equal(switchView({}, 'o2_enabled').on, null);
});

// ── SDD-064: the DreamStation 2 pairing ──

test('the six-digit code is grouped in threes, anything else left as digits', () => {
  assert.equal(groupCode('042517'), '042 517');
  assert.equal(groupCode('42517'), '42517');
  assert.equal(groupCode(''), '');
  assert.equal(groupCode(undefined), '');
});

test('each pairing state has its phase and its line', () => {
  const s = (state: string, extra: any = {}) => ({ pairing: { state }, ...extra });
  assert.deepEqual(ds2PairView(s('idle')), { phase: 'idle', key: '' });
  assert.equal(ds2PairView(s('scanning')).phase, 'working');
  assert.equal(ds2PairView(s('pairing')).phase, 'working');
  assert.equal(ds2PairView(s('pairing', { os_dialog: true })).phase, 'os_dialog');
  assert.deepEqual(ds2PairView(s('confirm')), { phase: 'confirm', key: 'devices.ds2.confirmQuestion' });
  assert.equal(ds2PairView(s('os_dialog')).key, 'devices.ds2.osDialog');
  assert.equal(ds2PairView(s('paired')).phase, 'paired');
  assert.equal(ds2PairView(s('rejected')).key, 'devices.ds2.errRejected');
  assert.equal(ds2PairView(s('timeout')).key, 'devices.ds2.errTimeout');
  assert.equal(ds2PairView(s('not_found')).key, 'devices.ds2.errNotFound');
  assert.equal(ds2PairView(s('unsupported')).key, 'devices.ds2.errUnsupported');
  assert.equal(ds2PairView(s('cancelled')).key, 'devices.ds2.cancelled');
  assert.equal(ds2PairView(s('failed')).key, 'devices.ds2.errGeneric');
  assert.deepEqual(ds2PairView(null), { phase: 'idle', key: '' });
});

test('radio off is worded per OS', () => {
  const s = (backend: string) => ({ backend, pairing: { state: 'radio_off' } });
  assert.equal(ds2PairView(s('bluez')).key, 'devices.ds2.errRadioOff');
  assert.equal(ds2PairView(s('corebluetooth')).key, 'devices.ds2.errRadioOffMac');
  assert.equal(ds2PairView(s('winrt')).key, 'devices.ds2.errRadioOffWin');
  assert.equal(ds2RefusalKey('radio_off', 'winrt'), 'devices.ds2.errRadioOffWin');
});

test('the page polls only while a pairing runs', () => {
  for (const st of ['scanning', 'pairing', 'confirm', 'os_dialog'])
    assert.equal(ds2PairRunning({ pairing: { state: st } }), true, st);
  for (const st of ['idle', 'paired', 'rejected', 'timeout', 'failed', 'cancelled'])
    assert.equal(ds2PairRunning({ pairing: { state: st } }), false, st);
});

test('daily or only when asked, plus the night end other sources report', () => {
  assert.deepEqual(ds2Reads({ read_mode: 'daily', read_at: '11:00', night_sources: ['o2ring', 'sleep_bridge', 'bogus'] }), {
    key: 'devices.ds2.readsDailyNight',
    sources: ['devices.ds2.source.sleep_bridge', 'devices.ds2.source.o2ring'],
    time: '11:00',
  });
  assert.equal(ds2Reads({ read_mode: 'daily', read_at: '09:30', night_sources: [] }).key, 'devices.ds2.readsDaily');
  assert.equal(ds2Reads({ read_mode: 'daily', read_at: '09:30' }).time, '09:30');
  assert.equal(ds2Reads({ read_mode: 'manual', night_sources: ['as11_ble'] }).key, 'devices.ds2.readsManualNight');
  assert.equal(ds2Reads({ read_mode: 'manual', night_sources: [] }).key, 'devices.ds2.readsManualOnly');
  // An older server says nothing about the mode: the default, daily at 11:00.
  assert.deepEqual(ds2Reads(null), { key: 'devices.ds2.readsDaily', sources: [], time: '11:00' });
});

test('the next read shows only for the daily mode with a time planned', () => {
  assert.equal(ds2NextRead({ read_mode: 'daily', next_read_at: 1893456000 }), 1893456000);
  assert.equal(ds2NextRead({ read_mode: 'daily', next_read_at: 0 }), 0);
  assert.equal(ds2NextRead({ read_mode: 'manual', next_read_at: 1893456000 }), 0);
  assert.equal(ds2NextRead(null), 0);
});

test('the import date is a real day', () => {
  assert.equal(isIsoDay('2026-11-03'), true);
  assert.equal(isIsoDay('2026-02-30'), false);
  assert.equal(isIsoDay('2026-11-3'), false);
  assert.equal(isIsoDay(''), false);
  assert.equal(isIsoDay(undefined), false);
});

test('a refused request names why', () => {
  assert.equal(ds2RefusalKey('busy'), 'devices.ds2.errBusy');
  assert.equal(ds2RefusalKey('not_paired'), 'devices.ds2.notPaired');
  assert.equal(ds2RefusalKey('disabled'), 'devices.ds2.errDisabled');
  assert.equal(ds2RefusalKey('not_compiled'), 'devices.ds2.notCompiled');
  assert.equal(ds2RefusalKey(undefined), 'devices.ds2.errGeneric');
});

test('a Reset Bluetooth that was not done says why in the page\'s own words (#67)', () => {
  assert.equal(as11ResetRefusalKey('streaming'), 'devices.as11.resetNotStreaming');
  assert.equal(as11ResetRefusalKey('o2ring_transfer'), 'devices.as11.resetNotO2Ring');
  assert.equal(as11ResetRefusalKey('pairing_open'), 'devices.as11.resetNotPairing');
  // Anything else, or nothing, is the generic line: never the service's English.
  for (const r of ['reset_failed', 'no_adapter', 'disabled', 'something_new', '', null, undefined])
    assert.equal(as11ResetRefusalKey(r), 'devices.as11.resetNotGeneric');
});

test('every Reset Bluetooth line exists in every language (#67)', async () => {
  const { readFileSync } = await import('node:fs');
  const keys = ['resetNotStreaming', 'resetNotO2Ring', 'resetNotPairing', 'resetNotGeneric', 'resetDone',
                'resetOthers', 'adapterStuck', 'adapterStuckHint', 'resetBluetooth'];
  for (const lang of ['en', 'es', 'fr', 'hu', 'pt']) {
    const j = JSON.parse(readFileSync(new URL(`../../i18n/pages/devices.${lang}.json`, import.meta.url), 'utf8'));
    for (const k of keys) {
      const v = j.devices.as11[k];
      assert.ok(typeof v === 'string' && v.length > 0, `${lang} devices.as11.${k}`);
      assert.ok(!v.includes(String.fromCharCode(0x2014)), `${lang} devices.as11.${k} has an em-dash`);
    }
    assert.equal(j.devices.as11.resetFailed, undefined, `${lang}: the raw-error line is gone`);
  }
});
