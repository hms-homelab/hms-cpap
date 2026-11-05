// The Devices page's decisions, without a browser. `npm test` runs it with
// Node's own test runner and TypeScript support.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  CONFIG_KEYS, Confirmations, buildSet, commandState, configRows, costKey, errorText, groupKeys, latestConfigValues,
  recordingWarning, releaseLine, riskyGroups, setTouchesNetwork, switchView,
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
