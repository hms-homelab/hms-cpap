// Issue #54: the session page's AHI for a night of several sessions.
// `npm test` runs it with Node's own test runner; nothing is installed for it.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { nightAhi, sessionSeconds } from './night-ahi.ts';

// The reporter's night, in two sessions: 10 apneas and hypopneas plus 230
// RERA, flow limitation and snore events over 6.5 h. The list and dashboard
// said about 1.5; this page said about 37 (all 240 events over the hours).
const night = [
  { duration_seconds: '12600', duration_hours: '3.50', obstructive_apneas: '2',
    central_apneas: '1', clear_airway_apneas: null, unclassified_apneas: '0',
    hypopneas: '3', total_events: '130' },
  { duration_seconds: '10800', duration_hours: '3.00', obstructive_apneas: '1',
    central_apneas: '0', clear_airway_apneas: null, unclassified_apneas: '0',
    hypopneas: '3', total_events: '110' },
];

test('only apneas and hypopneas count, over the summed hours', () => {
  assert.equal(nightAhi(night).toFixed(2), (10 / 6.5).toFixed(2));
});

test('unclassified and clear-airway apneas count, as in the night aggregate', () => {
  const s = [{ duration_seconds: 3600, unclassified_apneas: '4', clear_airway_apneas: '2' }];
  assert.equal(nightAhi(s), 6);
});

test('a missing count reads as zero, and no time reads as an AHI of zero', () => {
  assert.equal(nightAhi([{ duration_seconds: 7200, hypopneas: '4', obstructive_apneas: null }]), 2);
  assert.equal(nightAhi([{ duration_seconds: 0, hypopneas: '4' }]), 0);
  assert.equal(nightAhi([]), 0);
});

test('a row without seconds falls back to its hours', () => {
  assert.equal(sessionSeconds({ duration_hours: '1.50' }), 5400);
  assert.equal(sessionSeconds({ duration_seconds: '5401', duration_hours: '1.50' }), 5401);
});
