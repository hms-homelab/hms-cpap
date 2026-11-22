// SDD-082: the Reports page's form rules.
// `npm test` runs it with Node's own test runner; nothing is installed for it.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { hoursLabel, nightOf, requestBody, ruleInRange, spanDates } from './report-form.ts';

test('a rule is 0.5 to 24 hours and a whole 1 to 100 percent', () => {
  assert.equal(ruleInRange(4, 70), true);
  assert.equal(ruleInRange(0.5, 1), true);
  assert.equal(ruleInRange(24, 100), true);
  assert.equal(ruleInRange(0, 70), false);
  assert.equal(ruleInRange(24.5, 70), false);
  assert.equal(ruleInRange(4, 0), false);
  assert.equal(ruleInRange(4, 101), false);
  assert.equal(ruleInRange(4, 70.5), false);
  assert.equal(ruleInRange(Number.NaN, 70), false);
});

test('30, 60 or 90 nights end at the latest night, both ends included', () => {
  assert.deepEqual(spanDates(30, '2026-09-30'), { start: '2026-09-01', end: '2026-09-30' });
  assert.deepEqual(spanDates(60, '2026-03-01'), { start: '2026-01-01', end: '2026-03-01' });
  assert.deepEqual(spanDates(90, '2026-12-31'), { start: '2026-10-03', end: '2026-12-31' });
});

test('the latest night reads either date shape the dashboard sends', () => {
  assert.equal(nightOf('20261121'), '2026-11-21');
  assert.equal(nightOf('2026-11-21'), '2026-11-21');
  assert.equal(nightOf('2026-11-21T00:00:00'), '2026-11-21');
  assert.equal(nightOf(''), '');
  assert.equal(nightOf(undefined), '');
});

test('the hours read as typed', () => {
  assert.equal(hoursLabel(4), '4');
  assert.equal(hoursLabel(4.5), '4.5');
  assert.equal(hoursLabel(Number.NaN), '');
});

test('only a compliance report sends a rule, and the standard report no kind', () => {
  assert.deepEqual(requestBody('standard', 4, 70, 'es'), { language: 'es' });
  assert.deepEqual(requestBody('clinician', 4, 70, 'en'), { kind: 'clinician', language: 'en' });
  assert.deepEqual(requestBody('compliance', 6, 90, 'fr'),
                   { kind: 'compliance', rule: { hours: 6, share: 90 }, language: 'fr' });
});
