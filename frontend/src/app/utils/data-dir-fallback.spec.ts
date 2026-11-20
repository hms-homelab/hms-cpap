// SDD-067 D7: the fallback banner reads /api/capabilities' data_dir_fallback.
// `npm test` runs it with Node's own test runner.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { dataDirFallback } from './data-dir-fallback.ts';

test('a fallback run shows both directories', () => {
  assert.deepEqual(
    dataDirFallback({ data_dir: '/data', data_dir_fallback: { wanted: '/share/cpapdash', in_use: '/data' } }),
    { wanted: '/share/cpapdash', in_use: '/data' },
  );
});

test('no banner when the run is not a fallback', () => {
  assert.equal(dataDirFallback({ data_dir: '/data' }), null);
  assert.equal(dataDirFallback(null), null);
  assert.equal(dataDirFallback(undefined), null);
});

test('no banner for a half answer', () => {
  assert.equal(dataDirFallback({ data_dir_fallback: { wanted: '/share/x' } }), null);
  assert.equal(dataDirFallback({ data_dir_fallback: { wanted: '', in_use: '/data' } }), null);
  assert.equal(dataDirFallback({ data_dir_fallback: 'yes' }), null);
});

test('the banner has its text in every language, naming both directories', () => {
  for (const lang of ['en', 'es', 'fr', 'pt', 'hu']) {
    const file = fileURLToPath(new URL(`../i18n/core.${lang}.json`, import.meta.url));
    const fb = JSON.parse(readFileSync(file, 'utf8')).shell?.dataDirFallback;
    assert.ok(fb?.title, `${lang}: no title`);
    assert.ok(fb?.body?.includes('{{wanted}}'), `${lang}: body does not name the wanted directory`);
    assert.ok(fb?.body?.includes('{{in_use}}'), `${lang}: body does not name the directory in use`);
  }
});

test('the shell shows the banner on every page, setup included', () => {
  const app = readFileSync(fileURLToPath(new URL('../app.ts', import.meta.url)), 'utf8');
  assert.match(app, /@if \(dataDirFallback\) \{/);
  assert.match(app, /dataDirFallback\(caps\)/);
});
