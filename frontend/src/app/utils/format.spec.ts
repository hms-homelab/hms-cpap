// The shared display helpers, without a browser. `npm test` runs it with
// Node's own test runner and TypeScript support.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { shiftedClock } from './format.ts';

// ── SDD-074 decision 2: a stored time on this computer's clock ──

test('no shift gives back the stored hour and minute', () => {
  assert.equal(shiftedClock('2026-03-31 23:30:00', 0), '23:30');
  assert.equal(shiftedClock('2026-03-31T23:30:15', 0, true), '23:30:15');
  assert.equal(shiftedClock('2026-03-31 23:30:00', undefined), '23:30');
});

test('the shift is added, across midnight both ways', () => {
  // A machine on UTC+10, this computer on UTC+11: one hour later.
  assert.equal(shiftedClock('2026-03-31 22:00:00', 3600), '23:00');
  assert.equal(shiftedClock('2026-03-31 23:30:00', 3600), '00:30');
  assert.equal(shiftedClock('2026-04-01 00:20:00', -3600), '23:20');
  assert.equal(shiftedClock('2026-04-01 06:05:09', -5 * 3600 - 1800, true), '00:35:09');
});

test('the browser zone and its DST do not move a label', () => {
  // 02:30 on a US spring-forward day does not exist locally; as a label it does.
  assert.equal(shiftedClock('2026-03-08 02:30:00', 0), '02:30');
  assert.equal(shiftedClock('2026-03-08 01:30:00', 3600), '02:30');
});

test('unreadable text gives nothing', () => {
  assert.equal(shiftedClock('', 3600), '');
  assert.equal(shiftedClock(null, 0), '');
  assert.equal(shiftedClock('yesterday', 0), '');
});
