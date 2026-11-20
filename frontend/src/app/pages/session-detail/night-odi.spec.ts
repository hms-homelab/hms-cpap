// SDD-001 F2: the night's ODI on the session page.
// `npm test` runs it with Node's own test runner; nothing is installed for it.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { nightDesaturations } from './night-odi.ts';

test('one session: its desaturations over its hours', () => {
  const r = nightDesaturations([{ avg_spo2: '96.1', spo2_drops: '6', duration_seconds: '10800' }]);
  assert.deepEqual(r, { drops: 6, odi: 2 });
});

test('a session without SpO2 adds neither desaturations nor hours', () => {
  const r = nightDesaturations([
    { avg_spo2: '95.0', spo2_drops: 4, duration_seconds: 7200 },
    { avg_spo2: '0', spo2_drops: 0, duration_seconds: 7200 },
    { avg_spo2: null, spo2_drops: null, duration_seconds: 3600 },
  ]);
  assert.deepEqual(r, { drops: 4, odi: 2 });
});

test('a night without SpO2 has no ODI, not an ODI of zero', () => {
  assert.equal(nightDesaturations([{ avg_spo2: null, spo2_drops: null, duration_seconds: 3600 }]), null);
  assert.equal(nightDesaturations([]), null);
});

test('the hours fall back to duration_hours, as the AHI does', () => {
  const r = nightDesaturations([{ avg_spo2: 97, spo2_drops: 3, duration_hours: '1.50' }]);
  assert.deepEqual(r, { drops: 3, odi: 2 });
});
