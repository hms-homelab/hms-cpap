// SDD-078 D3: the line that says where a night's oximetry came from.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { oximetryDevices, originsOf } from './oximetry-label.ts';

test('a Garmin night is named for its watch', () => {
  assert.equal(oximetryDevices([{ source: 'garmin', device_name: 'Garmin Fenix 7X' }], 'Upload'),
               'Garmin Fenix 7X');
});

test('a ring with no device name is the O2 Ring, an unnamed upload the given word', () => {
  assert.equal(oximetryDevices([{ source: 'o2ring', device_name: '' }], 'Upload'), 'O2 Ring');
  assert.equal(oximetryDevices([{ source: 'upload', device_name: '' }], 'Subida'), 'Subida');
  assert.equal(oximetryDevices([{ source: 'garmin', device_name: '' }], 'Upload'), 'Garmin');
});

test('two devices in one night are both said, each once', () => {
  assert.equal(oximetryDevices([
    { source: 'o2ring', device_name: '' },
    { source: 'garmin', device_name: 'Garmin Fenix 7X' },
    { source: 'o2ring', device_name: '' },
  ], 'Upload'), 'O2 Ring + Garmin Fenix 7X');
});

test('nothing said when there is nothing to say', () => {
  assert.equal(oximetryDevices([], 'Upload'), '');
  assert.equal(oximetryDevices(undefined, 'Upload'), '');
  assert.deepEqual(originsOf(null), []);
  assert.deepEqual(originsOf({ source: null }), []);
});

test('an answer with only the top-level source still has its origin', () => {
  assert.deepEqual(originsOf({ source: 'garmin', device_name: 'Garmin Fenix 7X' }),
                   [{ source: 'garmin', device_name: 'Garmin Fenix 7X' }]);
});
