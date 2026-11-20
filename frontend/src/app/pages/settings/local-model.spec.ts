// SDD-013: the "Local Sleep Model" section is hidden while no model is
// published, and shown once the server says one is.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { showLocalModelSection } from './local-model.ts';

test('hidden while no model is published', () => {
  assert.equal(showLocalModelSection({ published: false }), false);
});

test('hidden when the server has no local model (an older server, or no answer yet)', () => {
  assert.equal(showLocalModelSection(null), false);
  assert.equal(showLocalModelSection(undefined), false);
  assert.equal(showLocalModelSection({}), false);
});

test('shown once a model is published, whatever its state', () => {
  assert.equal(showLocalModelSection({ published: true }), true);
});
