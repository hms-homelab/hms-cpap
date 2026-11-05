// Issue #56: the dashboard's therapy pressure, without a browser. `npm test`
// runs it with Node's own test runner and TypeScript support.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { nightPressure, pressureTrendPlan } from './pressure-source.ts';

// The reporter's machine: AirSense 11 AutoSet, minimum 7, EPR 3. The machine
// delivers 7 and up all night; the mask reads about 2 lower. The database
// hands numbers back as strings on some engines.
const eprNight = {
  therapy_press_50: '7.2', therapy_press_95: '8.9', therapy_press_max: '9.6',
  mask_press_50: '5.1', mask_press_95: '6.9', mask_press_max: '8.0',
};

test('an EPR night shows the delivered pressure, never below the minimum', () => {
  const p = nightPressure(eprNight);
  assert.ok(p);
  assert.equal(p.source, 'therapy');
  assert.equal(p.p50, 7.2);
  assert.equal(p.p95, 8.9);
  assert.equal(p.max, 9.6);
  assert.ok(p.p50 >= 7, 'the therapy median is at or above the prescribed minimum');
});

test('the mask median stays, as its own value, beside the therapy pressure', () => {
  const p = nightPressure(eprNight);
  assert.ok(p);
  assert.equal(p.maskP50, 5.1);
  assert.ok((p.maskP50 ?? 0) < 7, 'the mask dips under the minimum with EPR');
});

test('a night without therapy pressure falls back to the mask, and says so', () => {
  const p = nightPressure({
    therapy_press_50: null, therapy_press_95: null, therapy_press_max: null,
    mask_press_50: '9.1', mask_press_95: '11.2', mask_press_max: '12.0',
  });
  assert.ok(p);
  assert.equal(p.source, 'mask');
  assert.equal(p.p50, 9.1);
  assert.equal(p.p95, 11.2);
  assert.equal(p.maskP50, null, 'no second copy of the value already shown');
});

test('an older server without the columns falls back to the mask', () => {
  const p = nightPressure({ mask_press_50: 9.1, mask_press_95: 11.2, mask_press_max: 12 });
  assert.equal(p?.source, 'mask');
});

test('a machine with no pressure channel has no panel, not a 0.0 reading', () => {
  assert.equal(nightPressure({ mask_press_50: 0, mask_press_95: '0', therapy_press_50: '' }), null);
  assert.equal(nightPressure(null), null);
});

test('a month with therapy pressure plots it, with the mask median beside it', () => {
  const plan = pressureTrendPlan([
    { record_date: '2026-11-01', ...eprNight },
    // An older night with no PLD minutes: no therapy figure, the mask line covers it.
    { record_date: '2026-11-02', therapy_press_50: null, therapy_press_95: null,
      mask_press_50: '5.0', mask_press_95: '6.8' },
  ]);
  assert.ok(plan);
  assert.equal(plan.source, 'therapy');
  assert.equal(plan.titleKey, 'dashboard.chart.pressureTrend');
  assert.deepEqual(plan.series.map(s => s.key),
    ['therapy_press_50', 'therapy_press_95', 'mask_press_50']);
  const mask = plan.series.find(s => s.key === 'mask_press_50');
  assert.equal(mask?.labelKey, 'dashboard.pressure.trendMaskP50');
  assert.equal(mask?.dashed, true);
  // A 3-minute night stored mask P50 0.0: a gap, not a plunge to zero.
  assert.ok(plan.series.every(s => s.zeroIsGap));
});

test('a month without therapy pressure plots the mask, titled as the mask', () => {
  const plan = pressureTrendPlan([
    { mask_press_50: '9.1', mask_press_95: '11.2' },
  ]);
  assert.ok(plan);
  assert.equal(plan.source, 'mask');
  assert.equal(plan.titleKey, 'dashboard.chart.maskPressureTrend');
  assert.deepEqual(plan.series.map(s => s.key), ['mask_press_50', 'mask_press_95']);
});

test('a month with no pressure at all has no chart', () => {
  assert.equal(pressureTrendPlan([{ mask_press_50: 0, mask_press_95: 0 }]), null);
  assert.equal(pressureTrendPlan([]), null);
});
