// Issue #74: the dashboard's verdict on "Regenerate AI summary".
// `npm test` runs it with Node's own test runner; nothing is installed for it.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { summaryRefreshOutcome } from './summary-refresh.ts';

test('a started summary is queued', () => {
  assert.deepEqual(summaryRefreshOutcome({ status: 'started', date: '2026-11-14' }), { queued: true });
});

test('a failed reply is a failure, not "queued"', () => {
  // A myAir-only night: the server finds no session and answers failed.
  // The old page said "Summary queued" here.
  assert.deepEqual(summaryRefreshOutcome({ status: 'failed', date: '2026-11-14' }),
                   { queued: false, reason: '' });
});

test('the reason the reply gives is shown', () => {
  assert.deepEqual(summaryRefreshOutcome({ status: 'failed', error: 'no session found' }),
                   { queued: false, reason: 'no session found' });
  assert.deepEqual(summaryRefreshOutcome({ status: 'failed', message: 'LLM not enabled' }),
                   { queued: false, reason: 'LLM not enabled' });
  assert.deepEqual(summaryRefreshOutcome({ status: 'failed', reason: '  no data  ' }),
                   { queued: false, reason: 'no data' });
});

test('a message on a started reply does not make it a failure', () => {
  assert.deepEqual(summaryRefreshOutcome({ status: 'started', message: 'working' }), { queued: true });
});

test('an empty or older reply keeps the old behaviour', () => {
  assert.deepEqual(summaryRefreshOutcome(null), { queued: true });
  assert.deepEqual(summaryRefreshOutcome({}), { queued: true });
});
