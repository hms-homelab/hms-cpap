// The Devices page's decisions, without a browser. `npm test` runs it with
// Node's own test runner and TypeScript support.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  CONFIG_KEYS, Confirmations, buildSet, commandState, configRows, costKey, errorText, groupKeys, latestConfigValues,
  recordingWarning, releaseLine, riskyGroups, switchView,
  groupCode, ds2PairView, ds2PairRunning, ds2RefusalKey, ds2Reads, ds2NextRead, isIsoDay, as11ResetRefusalKey,
  offsetSetting, fixedOffsets, clockChoiceOf, clockDraftOf, clockSettingOf, clockChanged, filterZones, clockDifference,
  hasDangerActions, learnedOffer, rereadLine, muleLimited, cloudSwitchState,
} from './devices-model.ts';
import type { ClockView, Reread } from './devices-model.ts';

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
  assert.deepEqual(riskyGroups({ pass: 'p' }), ['network'], 'a password alone is the network');
  assert.deepEqual(riskyGroups({ o2_name: 'r' }), []);
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

// ── SDD-074: the Machine clock and the Ring clock ──

const clock = (over: Partial<ClockView> = {}): ClockView => ({
  device_id: '23243570851', setting: '', kind: 'host', offset_now: -18000, offset_text: '-05:00',
  host_zone: 'America/New_York', host_offset_now: -18000, host_offset_text: '-05:00',
  learned: null, difference_s: 0, warn: false, ...over,
});

test('offsets are written the setting way, and the list runs UTC-12:00 to UTC+14:00 in 15 min steps', () => {
  assert.equal(offsetSetting(36000), '+10:00');
  assert.equal(offsetSetting(-16200), '-04:30');
  assert.equal(offsetSetting(0), '+00:00');
  assert.equal(offsetSetting(20700), '+05:45');
  const all = fixedOffsets();
  assert.equal(all[0], '-12:00');
  assert.equal(all[all.length - 1], '+14:00');
  assert.equal(all.length, 26 * 4 + 1);
  assert.ok(all.includes('+05:45') && all.includes('-09:30') && all.includes('+00:00'));
});

test('a setting is one of three: this computer, a fixed offset, a time zone', () => {
  assert.equal(clockChoiceOf(''), 'host');
  assert.equal(clockChoiceOf(null), 'host');
  assert.equal(clockChoiceOf('+10:00'), 'fixed');
  assert.equal(clockChoiceOf('-03:30'), 'fixed');
  assert.equal(clockChoiceOf('Australia/Sydney'), 'zone');
  assert.equal(clockChoiceOf('UTC'), 'zone');
});

test('the form starts on what is saved; a fixed offset starts on the learned one', () => {
  assert.deepEqual(clockDraftOf(clock({ setting: '+10:00', kind: 'fixed' })),
                   { choice: 'fixed', fixed: '+10:00', zone: '', query: '', open: false });
  assert.equal(clockDraftOf(clock({ setting: 'Australia/Sydney', kind: 'zone' })).zone, 'Australia/Sydney');
  const learned = clockDraftOf(clock({ learned: { offset: 36000, text: '+10:00', at: 1 } }));
  assert.equal(learned.choice, 'host');
  assert.equal(learned.fixed, '+10:00');
  assert.equal(clockDraftOf(clock()).fixed, '-05:00', 'nothing learned: this computer\'s offset');
});

test('Save is offered for a complete change only', () => {
  const c = clock();
  const d = clockDraftOf(c);
  assert.equal(clockChanged(d, c), false, 'unchanged');
  assert.equal(clockChanged({ ...d, choice: 'fixed', fixed: '+10:00' }, c), true);
  assert.equal(clockSettingOf({ ...d, choice: 'zone', zone: '' }), null, 'no zone picked yet');
  assert.equal(clockChanged({ ...d, choice: 'zone', zone: '' }, c), false);
  assert.equal(clockSettingOf({ ...d, choice: 'zone', zone: 'Europe/Budapest' }), 'Europe/Budapest');
  const fixed = clock({ setting: '+10:00', kind: 'fixed' });
  assert.equal(clockChanged({ ...clockDraftOf(fixed), choice: 'host' }, fixed), true, 'back to this computer');
  assert.equal(clockSettingOf({ ...d, choice: 'host' }), '');
});

test('the zone search matches every word, any case, an underscore as a space, starts first', () => {
  const zones = ['America/New_York', 'America/Los_Angeles', 'Australia/Sydney', 'Europe/London', 'Pacific/Auckland', 'Europe/Lisbon'];
  assert.deepEqual(filterZones(zones, ''), zones);
  assert.deepEqual(filterZones(zones, 'syd'), ['Australia/Sydney']);
  assert.deepEqual(filterZones(zones, 'new york'), ['America/New_York']);
  assert.deepEqual(filterZones(zones, 'NEW_YORK'), ['America/New_York']);
  assert.deepEqual(filterZones(zones, 'europe l'), ['Europe/London', 'Europe/Lisbon']);
  assert.deepEqual(filterZones(zones, 'land'), ['Pacific/Auckland']);
  assert.deepEqual(filterZones(zones, 'lo'), ['America/Los_Angeles', 'Europe/London']);
  assert.deepEqual(filterZones(zones, 'zzz'), []);
  assert.equal(filterZones(zones, '', 2).length, 2);
});

test('the difference is said in minutes under an hour, else in hours', () => {
  assert.deepEqual(clockDifference(900), { key: 'devices.clock.diffMinutes', params: { m: 15 } });
  assert.deepEqual(clockDifference(-2700), { key: 'devices.clock.diffMinutes', params: { m: 45 } });
  assert.deepEqual(clockDifference(3600), { key: 'devices.clock.diffHours', params: { h: 1 } });
  assert.deepEqual(clockDifference(54000), { key: 'devices.clock.diffHours', params: { h: 15 } });
  assert.deepEqual(clockDifference(5400), { key: 'devices.clock.diffHoursMinutes', params: { h: 1, m: 30 } });
});

test('the learned clock is a suggestion: Use this saves it as a fixed offset, gone once it is the setting', () => {
  assert.equal(learnedOffer(clock()), null);
  assert.equal(learnedOffer(undefined), null);
  const preview = { night_start: '2026-11-19 22:10:00', now: '07:10', would_be: '22:10' };
  const l = learnedOffer(clock({ learned: { offset: 36000, text: '+10:00', at: 1763500000, from: 'card_stamps', nights: 2,
                                            use: '+10:00', offer: true, preview } }));
  assert.deepEqual(l, { text: '+10:00', at: 1763500000, use: '+10:00', offer: true,
                        preview: { now: '07:10', would_be: '22:10' } });
  assert.equal(learnedOffer(clock({ setting: '+10:00', kind: 'fixed',
                                    learned: { offset: 36000, text: '+10:00', at: 0, from: 'card_stamps', nights: 2, offer: true } }))?.offer, false);
  assert.equal(learnedOffer(clock({ setting: 'Australia/Brisbane', kind: 'zone',
                                    learned: { offset: 36000, text: '+10:00', at: 0, from: 'card_stamps', nights: 2, offer: true } }))?.offer,
               true, 'a zone is not the fixed offset, so it is still offered');
});

test('item 10 layout B: the destructive group shows only where there is a destructive action', () => {
  assert.equal(hasDangerActions({ kind: 'sleepbridge', detail: {} }), true, 'Reset Wi-Fi, Forget this device');
  assert.equal(hasDangerActions({ kind: 'mule', detail: {} }), true);
  assert.equal(hasDangerActions({ kind: 'ds2_ble', detail: { paired: true } }), true, 'Forget pairing');
  assert.equal(hasDangerActions({ kind: 'ds2_ble', detail: { paired: false } }), false);
  assert.equal(hasDangerActions({ kind: 'as11_ble', detail: { paired: true, not_compiled: true } }), false);
  assert.equal(hasDangerActions({ kind: 'o2ring', detail: {} }), false);
});

test('issue #100: a Mule & Miner that reads a card is limited, as the server lists it', () => {
  assert.equal(muleLimited({ kind: 'mule', detail: { limited: true, limited_reason: 'this unit reads a card' } }), true);
  assert.equal(muleLimited({ kind: 'mule', detail: { limited: false } }), false);
  assert.equal(muleLimited({ kind: 'mule', detail: {} }), false, 'an older hms-cpap never says it');
  assert.equal(muleLimited({ kind: 'sleepbridge', detail: { limited: true } }), false);
  assert.equal(hasDangerActions({ kind: 'mule', detail: { limited: true } }), true, 'Forget this device stays, at the right');
});

test('issue #100: the switch to the cloud follows the answer, across a Mule & Miner\'s test reconnect', () => {
  const sent = { req_id: 7, cmd: 'config', state: 'sent' as const };
  assert.equal(cloudSwitchState('mule', 'yes', sent), 'switching');
  // A 6.0.0 unit closes the socket to test the cloud, then opens it again to answer.
  assert.equal(cloudSwitchState('mule', 'no', sent), 'switching', 'away during the test is not switched yet');
  assert.equal(cloudSwitchState('mule', 'no', null), 'switching');
  assert.equal(cloudSwitchState('mule', 'yes', { ...sent, state: 'answered', result: { ok: true, result: { hms_url: { switched: true } } } }), 'switched');
  assert.equal(cloudSwitchState('mule', 'no', { ...sent, state: 'answered', result: { ok: true } }), 'switched');
  assert.equal(cloudSwitchState('mule', 'yes', { ...sent, state: 'answered', result: { ok: false, error: 'no ack to the hello in 10 s' } }), 'failed');
  // A Sleep Bridge that leaves with the command out has switched, as before.
  assert.equal(cloudSwitchState('sleepbridge', 'no', sent), 'switched');
  assert.equal(cloudSwitchState('sleepbridge', 'yes', sent), 'switching');
});

test('issue #95: a learned clock the server does not offer is never a button', () => {
  // A Sleep Bridge night: -05:00 learned while recording, not from the card's stamps.
  const brp = learnedOffer(clock({ learned: { offset: -18000, text: '-05:00', at: 1, from: 'brp_growth', nights: 3, offer: false } }));
  assert.equal(brp?.offer, false);
  const legacy = learnedOffer(clock({ learned: { offset: 0, text: '+00:00', at: 1, from: '', nights: 0, offer: false } }));
  assert.equal(legacy?.offer, false);
  assert.equal(legacy?.preview, null);
  const one = learnedOffer(clock({ learned: { offset: 36000, text: '+10:00', at: 1, from: 'card_stamps', nights: 1, offer: false } }));
  assert.equal(one?.offer, false);
  // An answer from before 7.0.0 (no offer field) is never offered.
  assert.equal(learnedOffer(clock({ learned: { offset: 36000, text: '+10:00', at: 1 } }))?.offer, false);
});

test('a re-read says the backup, then the worker\'s progress, then done, from its own status', () => {
  const r: Reread = { backup: 9, before: '2026-11-18 10:00:00', seenRunning: false, status: null };
  assert.deepEqual(rereadLine(r), { key: 'devices.clock.rereadStarted', params: { n: 9 }, done: false, err: false });
  r.status = { status: 'complete', completed_at: '2026-11-18 10:00:00' };
  assert.equal(rereadLine(r).done, false, 'the old run\'s end is not this one');
  r.status = { status: 'running', folders_total: 0 };
  assert.equal(rereadLine(r).key, 'devices.clock.rereadStarted');
  r.status = { status: 'running', folders_total: 120, folders_done: 30 };
  assert.deepEqual(rereadLine(r), { key: 'devices.clock.rereadProgress', params: { done: 30, total: 120 }, done: false, err: false });
  r.seenRunning = true;
  r.status = { status: 'complete', sessions_saved: 118, errors: 0, completed_at: '2026-11-19 09:00:00' };
  assert.deepEqual(rereadLine(r), { key: 'devices.clock.rereadDone', params: { n: 118, errors: 0 }, done: true, err: false });
  r.status = { status: 'complete', sessions_saved: 110, errors: 2 };
  assert.equal(rereadLine(r).key, 'devices.clock.rereadDoneErrors');
  r.status = { status: 'error', error_message: 'disk full' };
  assert.deepEqual(rereadLine(r), { key: 'devices.clock.rereadFailed', params: { error: 'disk full' }, done: true, err: true });
  // Finished between two reads: never seen running, but completed_at moved.
  const quick: Reread = { backup: 9, before: '', seenRunning: false, status: { status: 'complete', completed_at: '2026-11-19 09:00:00', sessions_saved: 3 } };
  assert.equal(rereadLine(quick).key, 'devices.clock.rereadDone');
});

test('every clock line exists in every language, with no em-dash', async () => {
  const { readFileSync } = await import('node:fs');
  const en = JSON.parse(readFileSync(new URL('../../i18n/pages/devices.en.json', import.meta.url), 'utf8')).devices.clock;
  const keys = Object.keys(en);
  for (const k of ['machineLabel', 'ringLabel', 'host', 'hostNoZone', 'fixed', 'zone', 'help', 'ringHelp', 'learned', 'useThis',
                   'warn', 'diffMinutes', 'diffHours', 'diffHoursMinutes', 'reread', 'rereadHint', 'rereadStarted',
                   'rereadProgress', 'rereadDone', 'rereadDoneErrors', 'rereadFailed', 'failed', 'saved'])
    assert.ok(keys.includes(k), `en devices.clock.${k}`);
  for (const lang of ['en', 'es', 'fr', 'hu', 'pt']) {
    const j = JSON.parse(readFileSync(new URL(`../../i18n/pages/devices.${lang}.json`, import.meta.url), 'utf8')).devices.clock;
    for (const k of keys) {
      const v = j[k];
      assert.ok(typeof v === 'string' && v.length > 0, `${lang} devices.clock.${k}`);
      assert.ok(!v.includes(String.fromCharCode(0x2014)), `${lang} devices.clock.${k} has an em-dash`);
    }
  }
});
