// The source pictures: every kind maps to a file that ships in public/sources/.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { SOURCE_KINDS, sourceImage } from './source-image.ts';

const PUBLIC = fileURLToPath(new URL('../../../public/', import.meta.url));

test('every kind has a plain and a glow picture, relative to <base href>', () => {
  for (const kind of SOURCE_KINDS) {
    for (const glow of [false, true]) {
      const src = sourceImage(kind, glow)!;
      assert.equal(src, `sources/${kind}${glow ? '-glow' : ''}.png`);
      assert.ok(!src.startsWith('/'), 'a root-absolute path breaks under Ingress');
      const file = PUBLIC + src;
      assert.ok(existsSync(file), `${file} is missing`);
      assert.ok(statSync(file).size < 60 * 1024, `${file} is over 60 KB`);
    }
  }
});

test('the setup wizard\'s mm is the Mule & Miner', () => {
  assert.equal(sourceImage('mm', false), 'sources/mule.png');
});

test('a kind with no picture answers null', () => {
  assert.equal(sourceImage('local'), null);
  assert.equal(sourceImage('skip'), null);
  assert.equal(sourceImage(''), null);
});
