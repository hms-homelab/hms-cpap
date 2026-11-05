// Sending a Sleep Bridge back to the cloud, without a browser. `npm test` runs
// it with Node's own test runner and TypeScript support.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { BridgeCloud, cloudCommand, cloudError } from './sleepbridge-cloud.ts';

const online = { sender: 'BRIDGE-A', online: true };
const offline = { sender: 'BRIDGE-B', online: false };

test('the command is a stream with an empty url, to the one bridge named', () => {
  assert.deepEqual(cloudCommand('BRIDGE-A'), { sender: 'BRIDGE-A', cmd: 'stream', url: '' });
});

test('a fresh bridge is idle and its button is pressable only when online', () => {
  const c = new BridgeCloud();
  assert.equal(c.phase('BRIDGE-A'), 'idle');
  assert.equal(c.canAsk(online), true);
  assert.equal(c.canAsk(offline), false);
});

test('an offline bridge never opens the confirmation', () => {
  const c = new BridgeCloud();
  c.ask(offline);
  assert.equal(c.phase('BRIDGE-B'), 'idle');
  assert.equal(c.confirm(offline), null);
});

test('nothing is sent without the confirmation', () => {
  const c = new BridgeCloud();
  assert.equal(c.confirm(online), null);
  assert.equal(c.phase('BRIDGE-A'), 'idle');
});

test('ask, then cancel, leaves the bridge as it was', () => {
  const c = new BridgeCloud();
  c.ask(online);
  assert.equal(c.phase('BRIDGE-A'), 'confirming');
  assert.equal(c.canAsk(online), false);
  c.cancel('BRIDGE-A');
  assert.equal(c.phase('BRIDGE-A'), 'idle');
});

test('ask, then confirm, gives the request body and marks it sending', () => {
  const c = new BridgeCloud();
  c.ask(online);
  assert.deepEqual(c.confirm(online), { sender: 'BRIDGE-A', cmd: 'stream', url: '' });
  assert.equal(c.phase('BRIDGE-A'), 'sending');
  assert.equal(c.canAsk(online), false);
  // A second confirm while one is in flight sends nothing.
  assert.equal(c.confirm(online), null);
});

test('a bridge that went offline while the question was open is not sent', () => {
  const c = new BridgeCloud();
  c.ask(online);
  assert.equal(c.confirm({ sender: 'BRIDGE-A', online: false }), null);
  assert.equal(c.phase('BRIDGE-A'), 'idle');
});

test('the outcome is kept per bridge: sent, or failed with its error', () => {
  const c = new BridgeCloud();
  c.ask(online);
  c.confirm(online);
  c.sent('BRIDGE-A');
  assert.deepEqual(c.state('BRIDGE-A'), { phase: 'sent', error: '' });
  c.failed('BRIDGE-C', 'no Sleep Bridge named "BRIDGE-C" has connected here');
  assert.equal(c.phase('BRIDGE-C'), 'failed');
  assert.match(c.state('BRIDGE-C').error, /BRIDGE-C/);
  assert.equal(c.phase('BRIDGE-A'), 'sent');
});

test('after an outcome the button can be pressed again', () => {
  const c = new BridgeCloud();
  c.failed('BRIDGE-A', 'boom');
  assert.equal(c.canAsk(online), true);
  c.ask(online);
  assert.equal(c.phase('BRIDGE-A'), 'confirming');
  assert.equal(c.state('BRIDGE-A').error, '');
});

test('the error shown is the server\'s own words, else the transport message', () => {
  assert.equal(cloudError({ error: { ok: false, error: 'busy' }, message: 'Http failure' }), 'busy');
  assert.equal(cloudError({ message: 'Http failure response' }), 'Http failure response');
  assert.equal(cloudError(null), '');
});
