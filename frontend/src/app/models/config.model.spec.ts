// #108: Settings never sends the paired AirSense 11 back. hms-cpap saves the
// machine itself after the pairing; a page opened before that holds the old
// blank address and would blank the saved one on its next save.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { withoutAs11Machine } from './config.model.ts';

test('the paired machine is not in what Settings saves', () => {
  const cfg: any = {
    device_id: 'x',
    airsense11_ble: { enabled: true, address: '', paired_serial: '', capture: true, strict_stream_gaps: false },
  };
  const out: any = withoutAs11Machine(cfg);
  assert.equal('address' in out.airsense11_ble, false);
  assert.equal('paired_serial' in out.airsense11_ble, false);
  assert.equal(out.airsense11_ble.enabled, true);
  assert.equal(out.airsense11_ble.capture, true);
  assert.equal(out.device_id, 'x');
  // The page's own copy is left as it was.
  assert.equal(cfg.airsense11_ble.address, '');
});

test('a config without the section is saved as it is', () => {
  const out: any = withoutAs11Machine({ device_id: 'y' } as any);
  assert.deepEqual(out, { device_id: 'y' });
});
