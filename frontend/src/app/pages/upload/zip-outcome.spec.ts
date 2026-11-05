// Issue #55: the upload page's verdict on a CPAP zip.
// `npm test` runs it with Node's own test runner; nothing is installed for it.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { zipOutcome } from './zip-outcome.ts';

const nights = ['2026-10-01', '2026-10-02'];

test('an idle worker that has not taken the zip yet is not "done"', () => {
  // The old page stopped here and said "Imported 0 night(s)".
  assert.deepEqual(zipOutcome(7, { status: 'idle', upload: { id: 7, state: 'queued' } }, []),
                   { done: false, reading: false });
  assert.deepEqual(zipOutcome(7, { status: 'complete' }, []), { done: false, reading: false });
});

test('a previous upload\'s answer is not this one\'s', () => {
  const s = { status: 'complete', upload: { id: 6, state: 'done' as const, dates: nights } };
  assert.equal(zipOutcome(7, s, []).done, false);
});

test('while the zip is read the page says so', () => {
  assert.deepEqual(zipOutcome(7, { status: 'running', upload: { id: 7, state: 'reading' } }, []),
                   { done: false, reading: true });
});

test('the importer\'s refusal is the outcome, not a green tick', () => {
  const s = { status: 'error', error_message: 'holds no session',
              upload: { id: 7, state: 'done' as const, error: 'holds no session' } };
  assert.deepEqual(zipOutcome(7, s, []), { done: true, error: 'holds no session' });
});

test('the nights it found count once their import has finished', () => {
  const up = { id: 7, state: 'done' as const, dates: nights };
  assert.equal(zipOutcome(7, { status: 'running', upload: up }, []).done, false);
  assert.deepEqual(zipOutcome(7, { status: 'complete', upload: up }, []),
                   { done: true, error: null, dates: nights });
});

test('a failed import of those nights is reported', () => {
  const up = { id: 7, state: 'done' as const, dates: nights };
  assert.deepEqual(zipOutcome(7, { status: 'error', error_message: 'no lowenstein sessions', upload: up }, []),
                   { done: true, error: 'no lowenstein sessions' });
});

test('a server that imports inline: its reply already holds the nights', () => {
  assert.deepEqual(zipOutcome(undefined, { status: 'idle' }, nights),
                   { done: true, error: null, dates: nights });
  assert.equal(zipOutcome(undefined, { status: 'running' }, nights).done, false);
});
