// The firmware update's view, without a browser. `npm test` runs it with
// Node's own test runner and TypeScript support.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Dismissed, adviceKey, agoParts, barFraction, failKey, otaView, sizeParts } from './ota-progress-model.ts';
import type { OtaJob } from './ota-progress-model.ts';

function job(over: Partial<OtaJob> = {}, steps: Partial<OtaJob['steps_detail'][0]>[] = [{}]): OtaJob {
  const detail = steps.map((s) => ({
    target: 'tapper', phase: 'queued', bytes: 0, total: 0, bytes_meaning: 'sent',
    version_before: '1.0.1', version_target: '1.0.2', version_after: '', ...s,
  }));
  return {
    job_id: '7', serial: 'BRIDGE-A', kind: 'tapper', source: 'file x.bin', confirmed_while_recording: false,
    phase: detail[0].phase, step: 1, steps: detail.length, steps_detail: detail, hold_reason: null,
    held_since: null, reason: null, detail: null, retryable: false, last_heard: 1000, started_at: 900,
    updated_at: 1000, ended_at: null, ...over,
  };
}

test('no job is the offer, not a progress', () => {
  const v = otaView(null);
  assert.equal(v.state, 'none');
  assert.equal(v.ended, true);
  assert.equal(v.pct, null);
});

test('held, queued, preparing, verifying: words, never a bar', () => {
  for (const p of ['held', 'queued', 'preparing', 'verifying']) {
    const v = otaView(job({ phase: p }, [{ phase: p, bytes: 10, total: 100 }]));
    assert.equal(v.state, p);
    assert.equal(v.pct, null, p + ' shows no percent it was not told');
    assert.equal(v.ended, false);
  }
});

test('transferring: a bar from counted bytes, and the step of a two-step job', () => {
  const j = job({ phase: 'transferring', step: 2, kind: 'mule_miner' }, [
    { target: 'mule', phase: 'done', version_after: '6.2.0' },
    { target: 'miner', phase: 'transferring', bytes: 412000, total: 1048576 },
  ]);
  const v = otaView(j);
  assert.equal(v.state, 'transferring');
  assert.equal(v.pct, 39);
  assert.deepEqual(v.step, { n: 2, m: 2, target: 'miner' });
  assert.equal(v.bytes, 412000);
});

test('the bar is a scale of a fixed track, 0 to 1', () => {
  assert.equal(barFraction(otaView(job({ phase: 'transferring' }, [{ phase: 'transferring', bytes: 2048, total: 8192 }]))), 0.25);
  assert.equal(barFraction(otaView(job({ phase: 'verifying' }, [{ phase: 'verifying', bytes: 8192, total: 8192 }]))), 0);
  assert.equal(barFraction(otaView(null)), 0);
});

test('a one-step job names no step; bytes never exceed the total', () => {
  const v = otaView(job({ phase: 'transferring' }, [{ phase: 'transferring', bytes: 5000, total: 4096 }]));
  assert.equal(v.step, null);
  assert.equal(v.pct, 100);
});

test('restarting: the last word of the unit is a fact, not a failure', () => {
  const v = otaView(job({ phase: 'restarting', last_heard: 950, updated_at: 1000 }, [{ phase: 'restarting' }]));
  assert.equal(v.state, 'restarting');
  assert.equal(v.ended, false);
  assert.equal(v.notHeardSince, 950);
  assert.equal(v.target, '1.0.2');
  // Heard after the restart began: nothing to say about silence.
  assert.equal(otaView(job({ phase: 'restarting', last_heard: 1010, updated_at: 1000 }, [{ phase: 'restarting' }])).notHeardSince, null);
});

test('done reads before and after across the steps', () => {
  const v = otaView(job({ phase: 'done', step: 2, ended_at: 1100 }, [
    { target: 'mule', phase: 'done', version_before: '6.1.0', version_after: '6.2.0' },
    { target: 'miner', phase: 'done', version_before: '6.1.0', version_after: '6.2.0' },
  ]));
  assert.equal(v.state, 'done');
  assert.equal(v.ended, true);
  assert.equal(v.after, '6.2.0');
});

test('failed: the fixed reason picks the sentence and the advice; retry only when it allows', () => {
  const v = otaView(job({ phase: 'failed', reason: 'connection_lost', detail: 'gone after 4096 B', retryable: true },
                        [{ phase: 'failed' }]));
  assert.equal(v.state, 'failed');
  assert.equal(v.retryable, true);
  assert.equal(failKey(v.reason), 'devices.ota.fail.connection_lost');
  assert.equal(adviceKey(v.reason), 'devices.ota.advice.checkPower');
  assert.equal(failKey('something new'), 'devices.ota.fail.other');
  assert.equal(adviceKey('superseded'), '');
  const off = otaView(job({ phase: 'failed', reason: 'offline', retryable: false }, [{ phase: 'failed' }]));
  assert.equal(off.retryable, false);
});

test('cancelled is ended and says what it still runs', () => {
  const v = otaView(job({ phase: 'cancelled' }, [{ phase: 'cancelled' }]));
  assert.equal(v.ended, true);
  assert.equal(v.before, '1.0.1');
});

test('a page opened mid-update shows the job at once', () => {
  // Nothing about the job lives in the page: the first read renders it.
  const v = otaView(job({ phase: 'transferring' }, [{ phase: 'transferring', bytes: 2048, total: 8192 }]));
  assert.equal(v.pct, 25);
});

test('a dismissed job stays dismissed; a running one always shows', () => {
  const d = new Dismissed();
  const done = job({ phase: 'done' }, [{ phase: 'done', version_after: '1.0.2' }]);
  assert.equal(d.shown(done), true);
  d.dismiss(done);
  assert.equal(d.shown({ ...done }), false, 'the next poll brings the same job back: still hidden');
  const next = job({ job_id: '8', phase: 'transferring' }, [{ phase: 'transferring' }]);
  assert.equal(d.shown(next), true);
  d.dismiss(next);
  assert.equal(d.shown(next), true, 'a running job cannot be hidden');
  assert.equal(d.shown(null), false);
});

test('sizes and ages', () => {
  assert.deepEqual(sizeParts(512), { key: 'devices.ota.units.b', n: '512' });
  assert.deepEqual(sizeParts(412000), { key: 'devices.ota.units.kb', n: '402.3' });
  assert.deepEqual(sizeParts(1136528), { key: 'devices.ota.units.mb', n: '1.1' });
  assert.deepEqual(agoParts(100, 130), { key: 'devices.ota.ago.seconds', n: 30 });
  assert.deepEqual(agoParts(100, 400), { key: 'devices.ota.ago.minutes', n: 5 });
  assert.deepEqual(agoParts(0, 7200), { key: 'devices.ota.ago.hours', n: 2 });
  assert.deepEqual(agoParts(0, 200000), { key: 'devices.ota.ago.days', n: 2 });
});
