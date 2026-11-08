// Issue #56, the session page: its Pressure chart plotted the mask pressure.
// `npm test` runs it with Node's own test runner; nothing is installed for it.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { hasAnyValue, sessionPressurePlan, THERAPY_COLOR, MASK_COLOR } from './session-pressure.ts';

// The reporter's case: an AirSense 11 on a minimum of 7 with EPR 3. The machine
// delivers 7 and up; the mask reads about 2 lower on the breath out.
const epr3 = {
  timestamps: ['2026-11-03 23:00:00', '2026-11-03 23:01:00', '2026-11-03 23:02:00'],
  therapy_pressure: ['7.0', '7.4', '8.1'],
  pressure_avg: ['5.1', '5.4', '6.0'],
  pressure_min: ['4.0', '4.2', '4.9'],
  pressure_max: ['7.0', '7.5', '8.2'],
};

test('the chart is the delivered pressure, with the mask as its own dashed line', () => {
  const p = sessionPressurePlan(epr3)!;
  assert.equal(p.source, 'therapy');
  assert.equal(p.mainKey, 'therapy_pressure');
  assert.equal(p.titleKey, 'signal.therapyPressure');
  assert.deepEqual(p.lines.map(l => [l.key, l.dashed]), [
    ['therapy_pressure', false],
    ['pressure_avg', true],
  ]);
  assert.equal(p.lines[0].labelKey, 'sessionDetail.pressure.legendTherapy');
  assert.equal(p.lines[1].labelKey, 'sessionDetail.pressure.legendMask');
  assert.notEqual(p.lines[0].color, p.lines[1].color, 'two lines, two colours');
  assert.equal(p.band, null, 'no mask band around the therapy line');
  assert.equal(p.noteKey, 'sessionDetail.pressure.noteTherapy');
});

test('a night parsed before the per-minute delivered pressure was kept shows the mask, titled as the mask', () => {
  const p = sessionPressurePlan({ ...epr3, therapy_pressure: [null, null, null] })!;
  assert.equal(p.source, 'mask');
  assert.equal(p.mainKey, 'pressure_avg');
  assert.equal(p.titleKey, 'signal.maskPressure', 'never a mask figure titled as therapy pressure');
  assert.deepEqual(p.lines.map(l => l.key), ['pressure_avg']);
  assert.deepEqual(p.band, { minKey: 'pressure_min', maxKey: 'pressure_max' });
  assert.equal(p.noteKey, 'sessionDetail.pressure.noteMask');
});

test('an older backend without the series at all is the same fallback', () => {
  const { therapy_pressure, ...old } = epr3;
  assert.equal(sessionPressurePlan(old)!.source, 'mask');
});

test('zeros are no reading: a series of 0 is not a therapy pressure', () => {
  const p = sessionPressurePlan({ ...epr3, therapy_pressure: [0, '0', '0.0'] })!;
  assert.equal(p.source, 'mask');
});

test('a few delivered minutes are enough to plot the therapy pressure', () => {
  const p = sessionPressurePlan({ ...epr3, therapy_pressure: [null, '7.0', null] })!;
  assert.equal(p.source, 'therapy');
});

test('a delivered pressure with no mask waveform plots the one line, and says so', () => {
  const p = sessionPressurePlan({ timestamps: epr3.timestamps, therapy_pressure: epr3.therapy_pressure })!;
  assert.equal(p.source, 'therapy');
  assert.deepEqual(p.lines.map(l => l.key), ['therapy_pressure']);
  assert.equal(p.noteKey, 'sessionDetail.pressure.noteTherapyOnly');
});

test('no pressure at all is no chart', () => {
  assert.equal(sessionPressurePlan({ timestamps: [], pressure_avg: [] }), null);
  assert.equal(sessionPressurePlan(null), null);
  assert.equal(sessionPressurePlan(undefined), null);
});

// A BMC / React Health Luna: IPAP per minute as the therapy pressure, and the
// machine's own pressure reading, which the card does not place at the mask.
const luna = {
  machine_format: 'bmc',
  timestamps: epr3.timestamps,
  therapy_pressure: ['6.0', '6.0', '6.5'],
  pressure_avg: ['5.9', '6.1', null],
  pressure_min: ['4.6', '4.8', null],
  pressure_max: ['7.4', '7.6', null],
};

test('a Luna\'s measured pressure is never called the mask\'s', () => {
  const p = sessionPressurePlan(luna)!;
  assert.equal(p.source, 'therapy');
  assert.deepEqual(p.lines.map(l => l.key), ['therapy_pressure', 'pressure_avg']);
  assert.equal(p.lines[1].labelKey, 'signal.pressure');
  assert.equal(p.lines[1].dashed, true);
  assert.equal(p.noteKey, 'sessionDetail.pressure.noteTherapyMeasured');
  for (const l of p.lines) assert.ok(!/mask/i.test(l.labelKey), l.labelKey);
});

test('a Luna night with only the measured pressure is titled Pressure', () => {
  const p = sessionPressurePlan({ ...luna, therapy_pressure: [null, null, null] })!;
  assert.equal(p.mainKey, 'pressure_avg');
  assert.equal(p.titleKey, 'signal.pressure');
  assert.equal(p.lines[0].labelKey, 'signal.pressure');
  assert.equal(p.noteKey, 'sessionDetail.pressure.noteMeasured');
  assert.deepEqual(p.band, { minKey: 'pressure_min', maxKey: 'pressure_max' });
});

test('every other format keeps the mask wording', () => {
  for (const machine_format of ['resmed', undefined]) {
    const p = sessionPressurePlan({ ...epr3, machine_format })!;
    assert.equal(p.lines[1].labelKey, 'sessionDetail.pressure.legendMask');
    assert.equal(p.noteKey, 'sessionDetail.pressure.noteTherapy');
  }
});

// A Philips DreamStation card: flow only while the session is open; at
// mask-off the machine writes the pressure it delivered, every 2 minutes.
const ds2Open = {
  machine_format: 'philips',
  timestamps: epr3.timestamps,
  flow_avg: ['0.4', '-0.2', '0.1'],
  pressure_avg: [null, null, null],
  therapy_pressure: [null, null, null],
  epr_pressure: [null, null, null],
};

test('a live Philips session says its pressure arrives at mask-off, no chart', () => {
  const p = sessionPressurePlan(ds2Open, true)!;
  assert.equal(p.source, 'pending');
  assert.equal(p.pending, true);
  assert.equal(p.titleKey, 'signal.pressure');
  assert.equal(p.noteKey, 'sessionDetail.pressureLive.pendingPhilips');
  assert.deepEqual(p.lines, []);
});

test('a Philips session no longer live with no pressure has no pressure chart', () => {
  assert.equal(sessionPressurePlan(ds2Open, false), null);
});

test('after mask-off a Philips night plots the pressure it delivered, gaps spanned', () => {
  const p = sessionPressurePlan({ ...ds2Open, epr_pressure: ['6.2', null, '6.4'] }, false)!;
  assert.equal(p.source, 'delivered');
  assert.equal(p.mainKey, 'epr_pressure');
  assert.equal(p.titleKey, 'signal.pressureDelivered');
  assert.equal(p.noteKey, 'sessionDetail.pressureLive.deliveredPhilips');
  assert.equal(p.spanGaps, true);
  assert.ok(!p.pending);
  // Still the delivered pressure if the page was opened while live.
  assert.equal(sessionPressurePlan({ ...ds2Open, epr_pressure: ['6.2', null, '6.4'] }, true)!.source,
    'delivered');
});

test('another machine live with no pressure is not told it arrives at mask-off', () => {
  assert.equal(sessionPressurePlan({ ...ds2Open, machine_format: 'bmc' }, true), null);
});

test('a channel empty in every minute has no value; one at 0 all night does', () => {
  const night = {
    flow_limitation: [null, null, null],    // a Luna records none
    snore_index: ['0', '0', '0'],           // a ResMed night with no snoring
    leak_rate: [null, '2.5', null],
  };
  assert.equal(hasAnyValue(night, 'flow_limitation'), false);
  assert.equal(hasAnyValue(night, 'snore_index'), true);
  assert.equal(hasAnyValue(night, 'leak_rate'), true);
  assert.equal(hasAnyValue(night, 'not_in_the_response'), false);
  assert.equal(hasAnyValue(null, 'leak_rate'), false);
});

test('the colours are the ones the trend chart uses for the same two pressures', () => {
  assert.equal(THERAPY_COLOR, '#ce93d8');
  assert.equal(MASK_COLOR, '#64b5f6');
});
