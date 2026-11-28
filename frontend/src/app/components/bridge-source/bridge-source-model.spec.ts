// SDD-073 D8 / D10: whether a bridge's folder is the source.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { bridgeInUse, formatLabel, sameFolder } from './bridge-source-model.ts';

test('the same folder, with or without its trailing separator', () => {
  assert.ok(sameFolder('/data/sleepbridge/TAP-S3-00000041', '/data/sleepbridge/TAP-S3-00000041/'));
  assert.ok(!sameFolder('/data/sleepbridge/TAP-S3-00000041', '/data/sleepbridge/TAP-S3-00000042'));
  assert.ok(!sameFolder('', '/x'));
  assert.ok(!sameFolder('/x', undefined));
});

test('a Windows folder matches in any case and either slash', () => {
  assert.ok(sameFolder('C:\\Users\\a\\.hms-cpap\\sleepbridge\\TAP', 'c:/users/a/.hms-cpap/sleepbridge/TAP'));
});

test('in use only as a local folder source at that folder', () => {
  assert.ok(bridgeInUse({ transport: 'local', local_dir: '/d/sb/A' }, '/d/sb/A'));
  assert.ok(!bridgeInUse({ transport: 'ezshare', local_dir: '/d/sb/A' }, '/d/sb/A'));
  assert.ok(!bridgeInUse(null, '/d/sb/A'));
});

test('the card format is named, or not known yet', () => {
  assert.equal(formatLabel('resmed'), 'ResMed');
  assert.equal(formatLabel('bmc'), 'BMC / React Health Luna');
  assert.equal(formatLabel('sefam'), 'Sefam S.Box');
  assert.equal(formatLabel(''), null);
});
