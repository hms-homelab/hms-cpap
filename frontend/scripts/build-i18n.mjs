/**
 * Assembles the runtime translation dictionaries.
 *
 * SDD-080. This file is a deliberate copy of the same script in
 * hms-cpapdash-api/frontend, not a variant: the two frontends now use one i18n
 * mechanism so a translator moving between them meets the same fragment layout
 * and the same parity rule. Keep them in step.
 *
 * Sources (committed):
 *   src/app/i18n/core.<lang>.json            -- shared chrome (nav, common)
 *   src/app/i18n/pages/<name>.<lang>.json    -- one namespace per page/area
 *
 * Output (gitignored, imported by bundled-loader.ts):
 *   src/app/i18n/<lang>.json
 *
 * Each page owns its own fragment files, so many can be authored in parallel
 * without touching a shared file. This script deep-merges them (core first,
 * then pages alphabetically) into the dictionaries the app bundles.
 *
 * It also enforces key parity: every language must carry exactly English's
 * keys, so a missing translation FAILS THE BUILD instead of silently falling
 * back to English. That gate is the only reason the sibling frontend's
 * dictionaries have never drifted, while the Flutter app — which has no
 * equivalent — quietly went 25 and 28 keys behind in es and fr. Run standalone
 * (`node scripts/build-i18n.mjs`) or via the prebuild/prestart npm hooks.
 */
import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const I18N_DIR = join(HERE, '..', 'src', 'app', 'i18n');
const PAGES_DIR = join(I18N_DIR, 'pages');
// English is the reference; every other language must match its key set exactly.
// SDD-080 decision 2: the engine and the string extraction are the expensive
// part and are paid once, so this frontend ships every language the product has
// rather than only the one that prompted the work.
const BASE = 'en';
const LANGS = [BASE, 'es', 'fr', 'pt', 'hu'];

function isObj(v) {
  return v && typeof v === 'object' && !Array.isArray(v);
}

// Deep-merge src into dst. Throws on a key collision that would silently
// overwrite an existing leaf with a different value (two fragments claiming the
// same key) so namespace clashes are caught at build time.
function deepMerge(dst, src, path = '') {
  for (const [k, v] of Object.entries(src)) {
    const p = path ? `${path}.${k}` : k;
    if (isObj(v)) {
      if (dst[k] !== undefined && !isObj(dst[k])) {
        throw new Error(`i18n merge conflict at "${p}": object vs scalar`);
      }
      dst[k] = dst[k] ?? {};
      deepMerge(dst[k], v, p);
    } else {
      if (dst[k] !== undefined && dst[k] !== v) {
        throw new Error(`i18n merge conflict at "${p}": "${dst[k]}" vs "${v}"`);
      }
      dst[k] = v;
    }
  }
  return dst;
}

function read(file) {
  return JSON.parse(readFileSync(file, 'utf8'));
}

// Collect the sorted set of leaf key-paths in an object.
function leafKeys(obj, path = '', out = new Set()) {
  for (const [k, v] of Object.entries(obj)) {
    const p = path ? `${path}.${k}` : k;
    if (isObj(v)) leafKeys(v, p, out);
    else out.add(p);
  }
  return out;
}

const dicts = {};
for (const lang of LANGS) {
  const dict = {};
  const core = join(I18N_DIR, `core.${lang}.json`);
  if (existsSync(core)) deepMerge(dict, read(core));

  const fragments = existsSync(PAGES_DIR)
    ? readdirSync(PAGES_DIR).filter((f) => f.endsWith(`.${lang}.json`)).sort()
    : [];
  for (const f of fragments) deepMerge(dict, read(join(PAGES_DIR, f)));

  dicts[lang] = dict;
}

// Parity check: every language must have exactly English's leaf keys. A missing
// translation fails the build rather than silently falling back to English,
// which is the only reason the dictionaries have never drifted. Adding a
// language means adding it to LANGS above and nothing else here.
const baseKeys = leafKeys(dicts[BASE]);
const failures = [];
for (const lang of LANGS) {
  if (lang === BASE) continue;
  const keys = leafKeys(dicts[lang]);
  const missing = [...baseKeys].filter((k) => !keys.has(k));
  const orphan = [...keys].filter((k) => !baseKeys.has(k));
  if (missing.length) failures.push(`  missing in ${lang}: ${missing.join(', ')}`);
  if (orphan.length) failures.push(`  in ${lang} but not in ${BASE}: ${orphan.join(', ')}`);
}
if (failures.length) {
  throw new Error(`i18n key parity failed:\n${failures.join('\n')}`);
}

for (const lang of LANGS) {
  writeFileSync(join(I18N_DIR, `${lang}.json`), JSON.stringify(dicts[lang], null, 2) + '\n');
}

const fragmentCount = readdirSync(PAGES_DIR).filter((f) => f.endsWith(`.${BASE}.json`)).length;
console.log(
  `i18n: built ${LANGS.map((l) => `${l}.json`).join(' + ')} ` +
    `(${baseKeys.size} keys each) from core + ${fragmentCount} page fragments`,
);
