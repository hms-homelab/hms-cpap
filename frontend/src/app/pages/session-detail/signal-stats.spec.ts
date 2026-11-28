// The session page's figures for each chart (min, median, max, 95%, 99.5%).
// `npm test` runs it with Node's own test runner; nothing is installed for it.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { percentile, signalStats, validFor, statsDigits } from './signal-stats.ts';

const close = (a: number, b: number) => assert.ok(Math.abs(a - b) < 1e-9, `${a} != ${b}`);

test('1..100: the figures between the ranks', () => {
  const s = signalStats(Array.from({ length: 100 }, (_, i) => i + 1))!;
  assert.equal(s.n, 100);
  assert.equal(s.min, 1);
  assert.equal(s.max, 100);
  close(s.med, 50.5);
  close(s.p95, 95.05);
  close(s.p995, 99.505);
});

test('a single value is every figure', () => {
  assert.deepEqual(signalStats([7.5]), { n: 1, min: 7.5, med: 7.5, max: 7.5, p95: 7.5, p995: 7.5 });
});

test('a percentile is interpolated between the two closest ranks', () => {
  close(percentile([10, 20], 0.5), 15);
  close(percentile([10, 20, 40], 0.75), 30);
  close(percentile([0, 10, 20, 30, 40], 0.95), 38);
  assert.ok(Number.isNaN(percentile([], 0.5)));
});

test('the order the values come in does not matter', () => {
  assert.deepEqual(signalStats([3, 1, 2]), signalStats([1, 2, 3]));
});

test('null, empty and non-numbers are skipped; numbers as text are read', () => {
  const s = signalStats([null, '', undefined, 'n/a', NaN, '4.5', 2, '1'])!;
  assert.equal(s.n, 3);
  assert.equal(s.min, 1);
  assert.equal(s.max, 4.5);
  close(s.med, 2);
});

test('SpO2 skips the 0 and 255 markers', () => {
  const s = signalStats([0, 95, 255, 97, 96], validFor('spo2'))!;
  assert.equal(s.n, 3);
  assert.equal(s.min, 95);
  assert.equal(s.max, 97);
});

test('heart rate skips 0 and 255', () => {
  const s = signalStats([0, 60, 255, 70], validFor('heart_rate'))!;
  assert.equal(s.n, 2);
  assert.equal(s.max, 70);
});

test('a leak of 0 is a reading', () => {
  assert.equal(validFor('leak_rate'), undefined);
  const s = signalStats([0, 0, 0, 12], validFor('leak_rate'))!;
  assert.equal(s.n, 4);
  assert.equal(s.min, 0);
  assert.equal(s.med, 0);
});

test('nothing to count is no figures, not zeros', () => {
  assert.equal(signalStats([]), null);
  assert.equal(signalStats([null, '']), null);
  assert.equal(signalStats([0, 255], validFor('spo2')), null);
});

test('decimals by unit', () => {
  assert.equal(statsDigits('0-1'), 2);
  assert.equal(statsDigits('ratio'), 2);
  assert.equal(statsDigits('%'), 0);
  assert.equal(statsDigits('bpm'), 0);
  assert.equal(statsDigits('L/min'), 1);
  assert.equal(statsDigits('cmH2O'), 1);
  assert.equal(statsDigits(''), 1);
});
