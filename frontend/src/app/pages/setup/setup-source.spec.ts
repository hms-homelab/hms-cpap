// The setup wizard's source step: what each choice writes, and where a Sleep
// Bridge is told to stream.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import {
  SLEEP_BRIDGE_DEFAULT_PORT, sleepBridgePort, sleepBridgeTarget, sourceConfig
} from './setup-source.ts';
import type { SetupSourceInputs } from './setup-source.ts';

const INPUTS: SetupSourceInputs = {
  cardFormat: 'resmed', localFormat: 'resmed', ezshareUrl: 'http://192.168.4.1',
  flashairUrl: 'http://flashair.local', localDir: '/media/card', mmBaseUrl: 'http://10.0.0.7'
};

test('Sleep Bridge writes the same config as skip: no source keys at all', () => {
  assert.deepEqual(sourceConfig('sleepbridge', INPUTS), sourceConfig('skip', INPUTS));
  assert.deepEqual(sourceConfig('sleepbridge', INPUTS), {});
});

test('the other choices still write their source', () => {
  assert.deepEqual(sourceConfig('mm', INPUTS), {
    source: 'ezshare', transport: 'ezshare', format: 'resmed', ezshare_url: 'http://10.0.0.7' });
  assert.deepEqual(sourceConfig('ezshare', { ...INPUTS, cardFormat: 'sefam' }), {
    source: 'ezshare', transport: 'ezshare', format: 'sefam', ezshare_url: 'http://192.168.4.1' });
  assert.deepEqual(sourceConfig('flashair', INPUTS), {
    source: 'flashair', transport: 'flashair', format: 'resmed', flashair_url: 'http://flashair.local' });
  assert.deepEqual(sourceConfig('local', INPUTS), {
    transport: 'local', format: 'resmed', source: 'local', local_dir: '/media/card' });
  assert.deepEqual(sourceConfig('local', { ...INPUTS, localFormat: 'lowenstein' }), {
    transport: 'local', format: 'lowenstein', source: 'lowenstein', local_dir: '/media/card' });
});

test('the note shows the configured receiver port, 8894 when unknown', () => {
  assert.equal(SLEEP_BRIDGE_DEFAULT_PORT, 8894);
  assert.equal(sleepBridgePort({ port: 9000 }), 9000);
  assert.equal(sleepBridgePort({ port: 8894 }), 8894);
  assert.equal(sleepBridgePort({}), 8894);
  assert.equal(sleepBridgePort(null), 8894);
  assert.equal(sleepBridgePort({ port: 0 }), 8894);
  assert.equal(sleepBridgePort({ port: 70000 }), 8894);
});

test('address:port when the page was opened at a LAN IPv4 address', () => {
  assert.equal(sleepBridgeTarget('192.168.2.15', 8894), '192.168.2.15:8894');
  assert.equal(sleepBridgeTarget('10.0.0.4', 9000), '10.0.0.4:9000');
});

test('no address for loopback, a name or a malformed address', () => {
  for (const h of ['localhost', '127.0.0.1', '0.0.0.0', 'homeassistant.local', '::1', '300.1.1.1', ''])
    assert.equal(sleepBridgeTarget(h, 8894), null, h);
});

test('every language has the Sleep Bridge strings, with the port or target in them', () => {
  for (const lang of ['en', 'es', 'fr', 'pt', 'hu']) {
    const file = fileURLToPath(new URL(`../../i18n/pages/setup.${lang}.json`, import.meta.url));
    const s = JSON.parse(readFileSync(file, 'utf8')).setup.source;
    for (const k of ['sleepbridgeTitle', 'sleepbridgeDesc', 'sleepbridgeListening',
                     'sleepbridgeStreamTo', 'sleepbridgeStreamToPort', 'sleepbridgeNights']) {
      assert.ok(s[k], `${lang} ${k}`);
      assert.ok(!s[k].includes('—'), `${lang} ${k} has an em-dash`);
    }
    assert.ok(s.sleepbridgeStreamTo.includes('{{target}}'), lang);
    assert.ok(s.sleepbridgeStreamToPort.includes('{{port}}'), lang);
  }
});
