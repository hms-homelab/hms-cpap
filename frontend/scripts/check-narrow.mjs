/**
 * SDD-080 section 9.5: does any page overflow a 375px phone in any language?
 *
 * Portuguese runs longer than English and Hungarian compounds run longer still
 * ("Bajttartomany-keresekkel valo letoltes" is one word for what English says in
 * four), so a label that fits in English is not evidence that the same label
 * fits in Hungarian. The Flutter app got this check and it found a real English
 * overflow; the two web frontends never had one.
 *
 * WHAT COUNTS AS A FAILURE. Only horizontal overflow of the viewport, and only
 * from elements that are supposed to wrap. Deliberate horizontal scrollers (the
 * chart strips, the myAir table, anything with overflow-x auto or scroll) are
 * SUPPOSED to be wider than the screen, so they and their descendants are
 * excluded rather than reported as bugs -- otherwise the check cries wolf on
 * the one pattern that is already handling narrow screens correctly.
 *
 * The API is stubbed, not mocked away: every route returns a fixture with
 * plausible worst-case values (long device names, two-decimal indices, big
 * counts) because an empty dashboard proves nothing about layout.
 *
 *   node scripts/check-narrow.mjs            # all languages, exit 1 on overflow
 *   node scripts/check-narrow.mjs --shots    # also write PNGs to .narrow-shots/
 */
// Playwright is not a dependency of this app: it is a dev-machine tool, and
// adding it to package.json would put a browser download in every `npm ci`
// including the ones that only build. Resolved from wherever it is installed.
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const { chromium } = require(process.env.PLAYWRIGHT_PATH
  ?? '/opt/homebrew/lib/node_modules/@playwright/test');

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { existsSync, mkdirSync } from 'node:fs';
import { join, extname } from 'node:path';

const DIST = new URL('../dist/frontend/browser/', import.meta.url).pathname;
const WIDTH = 375;
const HEIGHT = 667;
const LANGS = ['en', 'es', 'fr', 'pt', 'hu'];
const SHOTS = process.argv.includes('--shots');

const ROUTES = [
  ['/dashboard', 'dashboard'],
  ['/sessions', 'sessions'],
  ['/events', 'events'],
  ['/reports', 'reports'],
  ['/equipment', 'equipment'],
  ['/upload', 'upload'],
  ['/logs', 'logs'],
  ['/settings', 'settings'],
  ['/setup', 'setup'],
];

const MIME = {
  '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css',
  '.json': 'application/json', '.ico': 'image/x-icon', '.svg': 'image/svg+xml',
  '.woff2': 'font/woff2', '.png': 'image/png',
};

/** Static server for dist, falling back to index.html for client routes. */
function serve() {
  const server = createServer(async (req, res) => {
    const path = req.url.split('?')[0];
    let file = join(DIST, path === '/' ? 'index.html' : path);
    if (!existsSync(file) || !extname(file)) file = join(DIST, 'index.html');
    try {
      const body = await readFile(file);
      res.writeHead(200, { 'Content-Type': MIME[extname(file)] ?? 'text/plain' });
      res.end(body);
    } catch {
      res.writeHead(404).end('not found');
    }
  });
  return new Promise((ok) => server.listen(0, () => ok(server)));
}

/**
 * Worst-case-but-plausible API fixtures.
 *
 * Long strings on purpose: a device called "ResMed AirCurve 10 VAuto" is a real
 * product name and is the longest one the equipment page has to hold.
 */
const LONG_DEVICE = 'ResMed AirCurve 10 VAuto';
/**
 * Field names match what the templates actually read, not what seemed likely.
 *
 * This bit me: the first version set record_date and the sessions table reads
 * sleep_day, so every date cell rendered empty and the column measured narrower
 * than it ever will in production. A fixture that does not populate a column is
 * a fixture that cannot prove the column fits.
 */
const night = (d) => ({
  sleep_day: d, session_start: `${d}T23:14:00`, record_date: d, session_date: d,
  ahi: 12.34, duration_hours: 7.45, duration_minutes: 447,
  obstructive_apneas: 14, central_apneas: 9, hypopneas: 31, reras: 7,
  total_events: 61, avg_spo2: 94.3, avg_heart_rate: 61.7,
  leak_95: 24.6, pressure_95: 14.2, spo2_avg: 94.3, hr_avg: 61.7,
  events: { OA: 14, CA: 9, H: 31, RERA: 7 }, sleep_score: 82,
  myair_present: true, ours_present: true, myair_ahi: 11.98,
  leak_percentile: 22.1, total_usage_min: 441, mask_pair_count: 3,
  usage_delta_min: 6, ahi_delta: 0.36, leak_delta: 2.5,
  // The events table's own row shape.
  event_timestamp: `${d}T02:41:17`, event_type: 'ClearAirway',
  duration_seconds: 18.4, details: 'Apnea-Central',
});
const NIGHTS = ['2026-09-01', '2026-09-02', '2026-09-03'].map(night);

const FIXTURES = {
  'capabilities': { features: { myair: true, o2ring: true, ml: true, llm: true } },
  'config': {
    setup_complete: true, source: 'ezshare', ezshare_url: 'http://192.168.4.1',
    ezshare_range: true, local_dir: '', archive_dir: '', burst_interval: 65,
    device_id: '23243570851', device_name: LONG_DEVICE,
    web_port: 8893, static_dir: '',
    database: { type: 'postgresql', host: 'localhost', port: 5432, name: 'cpap', user: 'cpap_user', password: '' },
    mqtt: { enabled: true, broker: '127.0.0.1', port: 1883, username: '', password: '', client_id: 'hms_cpap' },
    llm: { enabled: true, provider: 'openai', endpoint: '', model: '', api_key: '', max_tokens: 512, prompt_file: '' },
    ml_training: { enabled: true, schedule: 'weekly', min_days: 30, max_training_days: 0, model_dir: '' },
    sleephq: { enabled: true, client_id: '', client_secret: '', quiet_minutes: 5 },
    cpapdash: { enabled: true, api_url: '', token: '', auto_sync: true },
    sleep_stage: { enabled: true, model_dir: '', model_version: 'shhs-rf-v1' },
    agent: { enabled: true, embed_model: '', temperature: 0.2, max_iterations: 6 },
    o2ring: { enabled: true, mode: 'ble', mule_url: '' },
    fysetc: { enabled: true, listen_port: 3333, listen_bind: '0.0.0.0', connection_timeout_s: 30, archive_dir: '', log_dir: '' },
    logging: { enabled: true, file: '', max_mb: 10, keep: 5 },
  },
  'myair/status': { connected: true, username: 'someone@example.com', region: 'EU', poll_minutes: 60 },
  'myair/comparison': NIGHTS,
  'ml/status': { status: 'idle', last_trained: '2026-09-01 03:00', models_loaded: true, model_count: 4,
                 models: [{ name: 'ahi_forecast', primary_metric: 0.8431 }] },
  'backfill/status': { status: 'running', folders_total: 128, folders_done: 61,
                       sessions_saved: 57, sessions_parsed: 61, sessions_deleted: 4, errors: 0 },
  'equipment': [{ id: 1, name: LONG_DEVICE, category: 'machine', purchased_at: '2025-04-11',
                  replace_every_days: 1825, days_remaining: 1204 }],
  'sessions': NIGHTS,
  'events': NIGHTS,
  'ble/status': { status: 'ok' },
};

/** Longest matching fixture key, so /api/sessions/2026-09-01 still resolves. */
function fixtureFor(url) {
  const path = url.split('/api/')[1]?.split('?')[0] ?? '';
  const hit = Object.keys(FIXTURES)
    .filter((k) => path === k || path.startsWith(k + '/'))
    .sort((a, b) => b.length - a.length)[0];
  return hit ? FIXTURES[hit] : [];
}

/**
 * Report elements that stick out past the viewport.
 *
 * Runs in the page. An element is only at fault if neither it nor any ancestor
 * is a horizontal scroll container -- those are meant to be wide.
 */
const FIND_OVERFLOW = (width) => {
  const inScroller = (el) => {
    for (let n = el.parentElement; n; n = n.parentElement) {
      const o = getComputedStyle(n);
      if (o.overflowX === 'auto' || o.overflowX === 'scroll') return true;
    }
    return false;
  };
  const bad = [];
  for (const el of document.querySelectorAll('body *')) {
    const r = el.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) continue;
    if (r.right <= width + 1 && r.left >= -1) continue;
    if (inScroller(el)) continue;
    // Report the outermost offender only: a long word overflows its <label>,
    // its <div> and its <section> alike, and naming all three is noise.
    if (bad.some((b) => b.el.contains(el))) continue;
    bad.push({
      el,
      tag: el.tagName.toLowerCase(),
      cls: (el.className || '').toString().split(' ').filter(Boolean).slice(0, 2).join('.'),
      right: Math.round(r.right),
      text: (el.textContent || '').trim().slice(0, 70),
    });
  }
  return bad.map(({ el, ...rest }) => rest);
};

const run = async () => {
  const server = await serve();
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await chromium.launch();
  const failures = [];

  if (SHOTS && !existsSync('.narrow-shots')) mkdirSync('.narrow-shots');

  for (const lang of LANGS) {
    const ctx = await browser.newContext({ viewport: { width: WIDTH, height: HEIGHT } });
    await ctx.route('**/api/**', (route) =>
      route.fulfill({ status: 200, contentType: 'application/json',
                      body: JSON.stringify(fixtureFor(route.request().url())) }));
    await ctx.route('**/health', (route) => route.fulfill({ status: 200, body: 'ok' }));
    await ctx.addInitScript((l) => localStorage.setItem('hms_cpap_lang', l), lang);

    const page = await ctx.newPage();
    for (const [route, name] of ROUTES) {
      await page.goto(base + route, { waitUntil: 'networkidle' });
      // Settings hides 18 sections behind collapsed headers, and a label cannot
      // overflow while it is display:none. Open everything before measuring.
      if (name === 'settings') {
        for (const h of await page.locator('.section-header').all()) {
          await h.click().catch(() => {});
        }
        await page.waitForTimeout(150);
      }
      const bad = await page.evaluate(FIND_OVERFLOW, WIDTH);
      const doc = await page.evaluate(() => document.documentElement.scrollWidth);
      if (SHOTS) {
        await page.screenshot({ path: `.narrow-shots/${lang}-${name}.png`, fullPage: true });
      }
      if (bad.length || doc > WIDTH) {
        failures.push({ lang, name, doc, bad });
        console.log(`FAIL ${lang} ${name} (document ${doc}px)`);
        for (const b of bad) {
          console.log(`       <${b.tag} class="${b.cls}"> right=${b.right}px  ${JSON.stringify(b.text)}`);
        }
      } else {
        console.log(`ok   ${lang} ${name}`);
      }
    }
    await ctx.close();
  }

  await browser.close();
  server.close();

  console.log(failures.length
    ? `\n${failures.length} page/language combination(s) overflow ${WIDTH}px.`
    : `\nAll ${LANGS.length * ROUTES.length} page/language combinations fit ${WIDTH}px.`);
  process.exit(failures.length ? 1 : 0);
};

run();
