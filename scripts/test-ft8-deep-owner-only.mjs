// test-ft8-deep-owner-only.mjs — FT8 / FT4 "dig deeper" (V12 beta91) is AUTOMATIC with ONE owner control.
// Stuart, 2026-11-22: "should be automatic and simply enable when the cpu has headroom with a benchmark driven toggle in
// the settings with the rest of the digital modes, no user facing toggles".
//   · no listener-facing code (the web client, the app outside the owner's server screen, the handheld UI, the watch)
//     names it — listeners just get the decodes;
//   · the setup page's and the app's readings of the automatic rule are the server's (decoders/ft8_deep_auto.h,
//     test-ft8-deep-auto): the same answers for the same benchmark rows;
//   · the owner's tick / untick writes "ft8+deep" / "ft8-deep", and "Back to automatic" removes both.
import fs from 'node:fs';
import path from 'node:path';

const root = new URL('..', import.meta.url).pathname;
const rd = (p) => fs.readFileSync(path.join(root, p), 'utf8');
let fail = 0, n = 0;
const ok = (c, what) => { n++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

// ── 1. nothing a listener sees names it ──
const OWNER_ONLY = new Set(['src/screens/ServerModeScreen.tsx', 'src/components/AboutOverlay.tsx']);   // AboutOverlay: the release note
const PAT = /ft8[+-]deep|dig deeper|deepTier/i;
const walk = (d, out) => {
  let ents = [];
  try { ents = fs.readdirSync(path.join(root, d), { withFileTypes: true }); } catch { return out; }
  for (const e of ents) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) { if (!/node_modules|build|Pods|\.git/.test(e.name)) walk(p, out); }
    else if (/\.(tsx?|jsx?|mjs|html|swift|kt|java|c|cpp|h)$/.test(e.name)) out.push(p);
  }
  return out;
};
const listenerFiles = [...walk('src', []), ...walk('web/client', []), ...walk('handheld-ui/src', []), ...walk('ios/VibeSDRWatch', [])]
  .filter((p) => !OWNER_ONLY.has(p));
const hits = listenerFiles.filter((p) => PAT.test(rd(p)));
ok(listenerFiles.length > 50, `${listenerFiles.length} listener-facing source files looked at`);
ok(hits.length === 0, `no listener-facing control or message names dig deeper${hits.length ? ': ' + hits.join(', ') : ''}`);
const about = rd('src/components/AboutOverlay.tsx');
const note = (about.match(/detail: 'FT8 \/ FT4 DIG DEEPER[^']*/) || [''])[0];
ok(/automatic/i.test(note) && /server owner/i.test(note), 'the release note says it is automatic, and that only the server owner can override it');

// ── 2. the page's and the app's automatic rule = the server's ──
const page = rd('android/app/src/main/cpp/vibe_setup_page.h');
const a = page.indexOf('function ft8DeepAuto()'), b = page.indexOf('function ft8DeepOwner()');
ok(a > 0 && b > a, 'the setup page has ft8DeepAuto');
const pageAuto = (bench) => new Function('BENCH', `${page.slice(a, b)}\nreturn ft8DeepAuto();`)(bench);
const app = rd('src/screens/ServerModeScreen.tsx');
const fa = app.indexOf('function ft8DeepAutoFrom('), fb = app.indexOf('const AVDATA_TEXT_NOTICE', fa);
ok(fa > 0 && fb > fa, 'the app has ft8DeepAutoFrom');
const appSrc = app.slice(fa, fb).replace(/\(rows: any\[\]\): boolean \| null/, '(rows)');
const appAuto = (bench) => new Function(`${appSrc}\nreturn ft8DeepAutoFrom;`)()(bench ? bench.rows || [] : []);
const r = (id, pct) => ({ id, pct, grade: 'green' });
const cases = [
  ['no benchmark', null, null],
  ['beta90 benchmark, Band 0.8 %', { rows: [r('dec_ft8band', 0.8)] }, true],
  ['beta90 benchmark, Band 21 %', { rows: [r('dec_ft8band', 21)] }, false],
  ['dig-deeper row 0.97 %', { rows: [r('dec_ft8band', 0.8), r('dec_ft8deep', 0.97)] }, true],
  ['dig-deeper row 30 %', { rows: [r('dec_ft8band', 10), r('dec_ft8deep', 30)] }, false],
  ['dig-deeper not run (Band not green)', { rows: [r('dec_ft8band', 60), r('dec_ft8deep', -1)] }, false],
];
for (const [what, bench, want] of cases) {
  ok(pageAuto(bench) === want, `setup page, ${what}: ${want}`);
  ok(appAuto(bench) === want, `app, ${what}: ${want}`);
}
ok(pageAuto({ rows: [r('dec_rtty', 1)], notRun: [{ id: 'dec_ft8band', why: 'no radio makes spots' }] }) === false,
   'setup page, Band listed as not run: off');

// ── 3. the owner's word ──
ok(/set\.add\(el\.checked \? "ft8\+deep" : "ft8-deep"\)/.test(page), 'the setup page: a tick writes ft8+deep, an untick ft8-deep');
ok(/set\.delete\("ft8\+deep"\); set\.delete\("ft8-deep"\);\s*radio\(\)\.blockedModes/.test(page), 'the setup page: "Back to automatic" removes both');
ok(/setDeep\(deepOn \? 'off' : 'on'\)/.test(app) && /setDeep\('auto'\)/.test(app), 'the app: a press is the owner\'s choice; "Back to automatic" removes it');
ok(!/data-mode="ft8\+deep"[^]*?benchNote\(deepBox/.test(page) && !/deepBox\.dispatchEvent/.test(page),
   'the benchmark never writes the owner\'s word (it only shows the automatic answer)');

console.log(fail ? `${fail} failed, ${n - fail} passed` : `all passed (${n})`);
process.exit(fail ? 1 : 0);
