// SDD-001: event spans on the flow trace, their colour key, and the breath
// overlay that appears only once the chart is zoomed in.
// `npm test` runs it with Node's own test runner; nothing is installed for it.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  BREATH_WINDOW_SEC, breathAnnotations, eventAnnotations, eventLegend,
  isDesaturation, visibleSeconds,
} from './chart-helpers.ts';

// Ten per-minute buckets from 23:00.
const ts = Array.from({ length: 10 }, (_, i) => `2030-07-01 23:0${i}:00`);
const ev = (event_type: string, event_timestamp: string, duration_seconds: string) =>
  ({ event_type, event_timestamp, duration_seconds, details: null });

test('an event with a duration is a box from its onset to onset + duration', () => {
  const [a] = eventAnnotations([ev('Obstructive', '2030-07-01 23:02:00', '30')], [], ts);
  assert.equal(a.type, 'box');
  assert.equal(a.xMin, 2);
  assert.equal(a.xMax, 2.5);
});

test('a zero-duration event (RERA) keeps the dashed line', () => {
  const [a] = eventAnnotations([ev('RERA', '2030-07-01 23:04:00', '0')], [], ts);
  assert.equal(a.type, 'line');
  assert.equal(a.xMin, 4);
  assert.equal(a.xMax, 4);
  assert.deepEqual(a.borderDash, [2, 2]);
});

test('a short event stays wide enough to see on a minute axis', () => {
  const [a] = eventAnnotations([ev('Hypopnea', '2030-07-01 23:01:00', '5')], [], ts);
  assert.equal(a.type, 'box');
  assert.ok(a.xMax - a.xMin >= 0.35);
});

test('the colour key lists each type once, in the order it first occurs', () => {
  const key = eventLegend([
    ev('Hypopnea', '2030-07-01 23:01:00', '12'),
    ev('Obstructive', '2030-07-01 23:02:00', '15'),
    ev('Hypopnea', '2030-07-01 23:03:00', '11'),
  ]);
  assert.deepEqual(key.map(k => k.type), ['Hypopnea', 'Obstructive']);
  assert.equal(key[1].color, '#f87171');
});

test('a desaturation is told apart from the respiratory events', () => {
  assert.equal(isDesaturation(ev('Desaturation', '2030-07-01 23:01:00', '20')), true);
  assert.equal(isDesaturation(ev('Central', '2030-07-01 23:01:00', '20')), false);
});

test('the visible window is measured in seconds off the timestamps', () => {
  assert.equal(visibleSeconds(ts, 0, 9), 540);
  assert.equal(visibleSeconds(ts, 3, 4), 60);
  assert.equal(visibleSeconds(ts, 3, 3.5), 30);
});

// A breath every 4 s across the night.
const onsets = Array.from({ length: 120 }, (_, i) => {
  const s = 4 * i;
  return `2030-07-01 23:${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
});

test('zoomed out, the flow chart carries no breath overlay', () => {
  assert.deepEqual(breathAnnotations(onsets, ts, 0, 9), []);
  assert.deepEqual(breathAnnotations(onsets, ts, 2, 4), [], 'two minutes is still too wide');
});

test(`zoomed to ${BREATH_WINDOW_SEC} s, each breath gets a line and every other one is shaded`, () => {
  const anns = breathAnnotations(onsets, ts, 3, 4);
  const lines = anns.filter(a => a.type === 'line');
  const boxes = anns.filter(a => a.type === 'box');
  assert.equal(lines.length, 16, 'onsets 23:03:00 .. 23:04:00 inclusive, 4 s apart');
  assert.equal(boxes.length, 8);
  for (const l of lines) assert.ok(l.xMin >= 3 && l.xMin <= 4);
  assert.equal(boxes[0].xMin, lines[0].xMin);
  assert.equal(boxes[0].xMax, lines[1].xMin);
});

test('a night without breaths has no overlay', () => {
  assert.deepEqual(breathAnnotations([], ts, 3, 4), []);
});
