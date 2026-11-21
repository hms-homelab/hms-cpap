// SDD-073 D9: the web wizard's Sleep Bridge steps.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import {
  BRIDGE_ANIMATIONS, BRIDGE_PICTURES, BRIDGE_STEPS, cardOutcome, cardOutcomeKey, findBridge, formatChanged, helloBridge,
  nextBridgeStep, onlineBridges, parseWatchFrame, portNote, prefillAddress, previousBridgeStep, qrRects,
  realtimeFrame, snapshotMark, streamOfFrame, streamStateKey, validBridgeAddress, watchUrl,
} from './bridge-setup.ts';
import type { WatchFrame } from './bridge-setup.ts';

const PUBLIC = fileURLToPath(new URL('../../../../public/', import.meta.url));

test('the steps run in the SDD order, and skip walks them one by one', () => {
  assert.deepEqual([...BRIDGE_STEPS], ['card', 'assemble', 'connect', 'wait', 'machine', 'online']);
  assert.equal(nextBridgeStep('card'), 'assemble');
  assert.equal(nextBridgeStep('online'), null);
  assert.equal(previousBridgeStep('card'), null);
  assert.equal(previousBridgeStep('wait'), 'connect');
});

test('every picture the steps show is in public/', () => {
  for (const p of Object.values(BRIDGE_PICTURES)) assert.ok(existsSync(PUBLIC + p), p);
  for (const p of Object.values(BRIDGE_ANIMATIONS)) assert.ok(existsSync(PUBLIC + p!), p);
});

test('an animated step has a still of its own for reduced motion and the desktop', () => {
  for (const k of Object.keys(BRIDGE_ANIMATIONS) as (keyof typeof BRIDGE_PICTURES)[]) {
    assert.ok(BRIDGE_PICTURES[k].endsWith('.png'), k);
    assert.equal(BRIDGE_ANIMATIONS[k], BRIDGE_PICTURES[k].replace(/\.png$/, '.webp'), k);
  }
});

test('the watch socket sits under <base href>, ws for http and wss for https', () => {
  assert.equal(watchUrl('http://192.168.1.20:8893/'), 'ws://192.168.1.20:8893/api/sleepbridge/watch');
  assert.equal(watchUrl('https://ha.local/api/hassio_ingress/abc/'),
               'wss://ha.local/api/hassio_ingress/abc/api/sleepbridge/watch');
});

test('a frame is read only when it is one of ours', () => {
  assert.equal(parseWatchFrame('not json'), null);
  assert.equal(parseWatchFrame('{"t":"other"}'), null);
  assert.equal(parseWatchFrame('{"t":"cpapdash","event":"state","stream":{"state":"connected"}}')?.t, 'cpapdash');
  assert.equal(parseWatchFrame('{"t":"night","event":"live"}')?.t, 'night');
});

test('issue #98: Settings takes the stream state from the watch frames', () => {
  const f = parseWatchFrame('{"t":"cpapdash","event":"state","stream":{"state":"connected","files_acked":3}}')!;
  assert.deepEqual(streamOfFrame(f), { state: 'connected', files_acked: 3 });
  assert.equal(streamOfFrame(parseWatchFrame('{"t":"cpapdash","event":"snapshot","stream":{"state":"off"}}')!)?.state, 'off');
  assert.equal(streamOfFrame({ t: 'sleepbridge', event: 'hello' }), null);
  assert.equal(streamOfFrame({ t: 'cpapdash', event: 'state' }), null, 'no stream, nothing to show');
});

test('issue #96: the dashboard reads the live state again on a night frame and on the socket opening', () => {
  assert.equal(realtimeFrame(parseWatchFrame('{"t":"night","event":"live"}')!), true);
  assert.equal(realtimeFrame(parseWatchFrame('{"t":"night","event":"settled"}')!), true);
  assert.equal(realtimeFrame({ t: 'sleepbridge', event: 'snapshot' }), true, 'the socket just opened');
  assert.equal(realtimeFrame({ t: 'sleepbridge', event: 'hello', sender: 'B1' }), false);
  assert.equal(realtimeFrame({ t: 'cpapdash', event: 'snapshot' }), false, 'one re-read per opening, not two');
});

const snap = (bridges: any[]): WatchFrame => ({ t: 'sleepbridge', event: 'snapshot', status: { bridges } });

test('D4: a hello event names the bridge', () => {
  assert.equal(helloBridge({ t: 'sleepbridge', event: 'hello', sender: 'TAP-S3-00000041' }, 0), 'TAP-S3-00000041');
});

test('D4: a bridge streaming before the step opened is not the new one, but is offered', () => {
  const f = snap([{ sender: 'TAP-S3-00000041', online: true, session_started_at: 1000 }]);
  const opened = snapshotMark((f as any).status);
  assert.equal(opened, 1000);
  assert.equal(helloBridge(f, opened), null);
  assert.deepEqual(onlineBridges((f as any).status), ['TAP-S3-00000041']);
});

test('D4: a session that started after the step opened counts; offline, rebuilt or cpapdash frames do not', () => {
  assert.equal(helloBridge(snap([{ sender: 'A', online: true, session_started_at: 1001 }]), 1000), 'A');
  assert.equal(helloBridge(snap([{ sender: 'A', online: false, session_started_at: 1001 }]), 1000), null);
  assert.equal(helloBridge({ t: 'sleepbridge', event: 'rebuilt', sender: 'A', status: { bridges: [] } }, 0), null);
  assert.equal(helloBridge({ t: 'cpapdash', event: 'state', stream: { state: 'connected' } }, 0), null);
});

test('D5: the card statement counts only when newer than at the hello', () => {
  assert.equal(cardOutcome(null, 0), 'waiting');
  assert.equal(cardOutcome({ sender: 'A', card_stated_at: 0 }, 0), 'waiting');
  assert.equal(cardOutcome({ sender: 'A', card_stated_at: 900 }, 900), 'waiting');
  assert.equal(cardOutcome({ sender: 'A', card_stated_at: 950 }, 900), 'seen');
  assert.equal(cardOutcome({ sender: 'A', card_stated_at: 950, learned: { verdict: 'latched' } }, 0), 'seen');
});

test('D5: a learned card the machine does not have is said in plain words', () => {
  const o = cardOutcome({ sender: 'A', card_stated_at: 950, learned: { verdict: 'mismatch' } }, 0);
  assert.equal(o, 'mismatch');
  assert.equal(cardOutcomeKey(o), 'setup.bridge.machine.mismatch');
  assert.equal(cardOutcomeKey('seen'), 'setup.bridge.machine.seen');
});

test('D5: the source is set again only when a known format differs', () => {
  assert.equal(formatChanged('resmed', { sender: 'A', card_format: 'bmc' }), true);
  assert.equal(formatChanged('resmed', { sender: 'A', card_format: 'resmed' }), false);
  assert.equal(formatChanged('resmed', { sender: 'A', card_format: '' }), false);
  assert.equal(formatChanged('resmed', null), false);
});

test('findBridge picks the one by name', () => {
  assert.equal(findBridge({ bridges: [{ sender: 'A' }, { sender: 'B' }] }, 'B')?.sender, 'B');
  assert.equal(findBridge(undefined, 'B'), null);
});

test('D3: the port is mentioned only when it is not 8894', () => {
  assert.equal(portNote(8894), null);
  assert.equal(portNote(9000), 9000);
});

test('D3: the address is prefilled from the page only for a LAN IPv4', () => {
  assert.equal(prefillAddress('192.168.1.20'), '192.168.1.20');
  assert.equal(prefillAddress('localhost'), '');
  assert.equal(prefillAddress('127.0.0.1'), '');
  assert.equal(prefillAddress('300.1.1.1'), '');
  assert.equal(prefillAddress('hms.local'), '');
});

test('D3: the bridge takes an address or a hostname, nothing else', () => {
  assert.ok(validBridgeAddress('192.168.1.20'));
  assert.ok(validBridgeAddress('pi.local'));
  assert.ok(!validBridgeAddress(''));
  assert.ok(!validBridgeAddress('192.168.1.20:8894'));
  assert.ok(!validBridgeAddress('ws://pi.local'));
  assert.ok(!validBridgeAddress('999.1.1.1'));
});

test('D6: each stream state has its own words, an unknown one a fallback', () => {
  assert.equal(streamStateKey('connected'), 'setup.bridge.online.state.connected');
  assert.equal(streamStateKey('weird'), 'setup.bridge.online.state.unknown');
  assert.equal(streamStateKey('unreachable'), 'setup.bridge.online.state.unreachable');
});

test('D6: the line says where the person is, "off" only once they turned it off', () => {
  const k = (s: string | undefined, off: boolean | null) => streamStateKey(s, off).split('.').pop();
  assert.equal(k(undefined, null), 'notYet');
  assert.equal(k('off', null), 'notYet', 'before a save, the switch ticked: not sending yet, not "off"');
  assert.equal(k('no_token', false), 'notYet');
  assert.equal(k('connecting', false), 'connecting');
  assert.equal(k('connected', null), 'connected');
  assert.equal(k('off', false), 'notYet');
  assert.equal(k('off', true), 'off');
});

test('the QR rows become dark modules inside a 4-module quiet zone', () => {
  const q = qrRects(['101', '010', '101']);
  assert.equal(q.side, 11);
  assert.deepEqual(q.rects.slice(0, 2), [{ x: 4, y: 4 }, { x: 6, y: 4 }]);
  assert.equal(q.rects.length, 5);
});
