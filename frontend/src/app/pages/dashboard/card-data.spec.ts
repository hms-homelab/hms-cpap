// An empty dashboard card is not shown; a populated one is.
// `npm test` runs it with Node's own test runner; nothing is installed for it.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { hasCardData, CARD_FIELDS } from './card-data.ts';

// What /api/dashboard sends for latest_night on a myAir-only install (no daily
// summary row: only the 30-day compliance, which defaults to "0").
const myairOnlyNight = { compliance_pct: '0' };
const fullNight = { date: '2026-11-15', ahi: '1.74', usage_hours: '5.17', leak_avg: '22.8',
                    compliance_pct: '100', therapy_mode: '1' };

test('key metrics: a myAir-only night is empty, a recorded night is not', () => {
  assert.equal(hasCardData(myairOnlyNight, CARD_FIELDS.keyMetrics), false);
  assert.equal(hasCardData(fullNight, CARD_FIELDS.keyMetrics), true);
});

test('a recorded zero is data: a night with no events still shows its card', () => {
  assert.equal(hasCardData({ ahi: '0.00', usage_hours: '6.1' }, CARD_FIELDS.keyMetrics), true);
  assert.equal(hasCardData({ oai: '0', cai: '0', hi: '0', rin: '0', duration_minutes: '402' },
                           CARD_FIELDS.events), true);
});

test('absent, null, empty, NaN and placeholder dashes are not data', () => {
  for (const v of [null, undefined, '', '  ', '--', '-', NaN, [], {}, [null, ''], { a: null, b: '' }]) {
    assert.equal(hasCardData(v), false, `${JSON.stringify(v)} should be empty`);
  }
  for (const v of ['A summary.', 0, '0', [{ x: 1 }], { spo2: 95 }]) {
    assert.equal(hasCardData(v), true, `${JSON.stringify(v)} should be data`);
  }
});

test('lists: no myAir rows hides the panel, one row shows it', () => {
  assert.equal(hasCardData([]), false);
  assert.equal(hasCardData([{ record_date: '2026-11-15', myair_ahi: '3.6' }]), true);
});

test('the real-time card is not shown for its status alone', () => {
  const statusOnly = { sessionStatus: 'completed', sessionDuration: '', lastSessionTime: '',
                       currentPressure: 0, minPressure: 0, maxPressure: 0 };
  assert.equal(hasCardData(statusOnly, CARD_FIELDS.realtime, { zeroIsEmpty: true }), false);
  assert.equal(hasCardData({ ...statusOnly, lastSessionTime: '2026-11-15' },
                           CARD_FIELDS.realtime, { zeroIsEmpty: true }), true);
  assert.equal(hasCardData({ ...statusOnly, currentPressure: 8.4 },
                           CARD_FIELDS.realtime, { zeroIsEmpty: true }), true);
});

test('the "Not enough data" insight is a placeholder, a real one is not', () => {
  const placeholder = { title: 'Not enough data', body: 'Need at least 7 therapy days for insights.',
                        category: 'info', metric: '', value: 0 };
  const real = { title: 'Average AHI: 1.74', category: 'positive', metric: 'AHI', value: 1.74 };
  assert.equal(hasCardData([placeholder], CARD_FIELDS.insight), false);
  assert.equal(hasCardData([placeholder, real], CARD_FIELDS.insight), true);
});

test('trends: an empty month hides the chart, one night with a value shows it', () => {
  assert.equal(hasCardData([], CARD_FIELDS.ahiTrend), false);
  assert.equal(hasCardData([{ date: '2026-11-15', value: '1.74' }], CARD_FIELDS.ahiTrend), true);
  assert.equal(hasCardData([{ record_date: '2026-11-15', leak_50: null, leak_95: null }],
                           CARD_FIELDS.leakTrend), false);
});

test('a channel the machine fills with zeros is empty when zeros mean "no channel"', () => {
  const csrMonth = [{ record_date: '2026-11-14', csr: '0' }, { record_date: '2026-11-15', csr: 0 }];
  assert.equal(hasCardData(csrMonth, CARD_FIELDS.csrTrend, { zeroIsEmpty: true }), false);
  assert.equal(hasCardData([...csrMonth, { record_date: '2026-11-13', csr: '12.5' }],
                           CARD_FIELDS.csrTrend, { zeroIsEmpty: true }), true);
  // Without the option a zero counts: EPR level 0 is "EPR off", a setting.
  assert.equal(hasCardData([{ epr_level: 0 }], CARD_FIELDS.eprTrend), true);
});
