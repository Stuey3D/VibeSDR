// test-bench-defaults.mjs — EVERY DECODER'S DEFAULT comes from the benchmark (2026-10-31), and the owner can ALWAYS switch
// it back on (Stuart: "never hide any controls"). The server decides which switches start off (vibe_benchmark_more.h
// ruleFor → decoders.defaultOff); the setup page's REAL benchApplyDefaults applies that list against a DOM stand-in; the
// app's applyBench is checked to apply the same list. And every switch the rule can name must be on BOTH setup screens.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const app = fs.readFileSync(new URL('../src/screens/ServerModeScreen.tsx', import.meta.url), 'utf8');
const more = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_benchmark_more.h', import.meta.url), 'utf8');
const a = src.indexOf('function benchApplyDefaults()'), b = src.indexOf('async function benchLoad()');
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };
if (a < 0 || b < a) { console.log('   FAIL benchApplyDefaults was not found in the page'); process.exit(1); }

function run(bench, configured) {
  const boxes = {}, notes = [];
  const box = (m) => boxes[m] || (boxes[m] = { checked: true, disabled: false, dataset: { mode: m },
    parentElement: { appendChild: (n) => notes.push(n.textContent) }, dispatchEvent: () => {} });
  const document = {
    querySelector: (q) => { const m = /data-mode="([^"]+)"/.exec(q); return m ? box(m[1]) : null; },
    createElement: () => ({ style: {}, className: '', textContent: '' }),
  };
  new Function('$', 'document', 'BENCH', 'cfg', 'Event', `${src.slice(a, b)}\nbenchApplyDefaults();`)(() => null, document, bench, { configured }, class { constructor(t) { this.type = t; } });
  return { boxes, notes };
}
const bench = {
  rows: [{ id: 'dec_dsc', label: 'Decoder: DSC (MF/HF)', pct: 78, grade: 'amber' }, { id: 'dec_ndb', label: 'Decoder: NDB beacons', pct: 120, grade: 'red' }],
  decoders: { recommend: 2, forUsers: 4, defaultOff: [{ switch: 'dsc', row: 'dec_dsc', pct: 78 }, { switch: 'ndb', row: 'dec_ndb', pct: 120 }], flagged: [] },
};
let r = run(bench, false);
ok(!r.boxes.dsc.checked && !r.boxes.ndb.checked, 'a fresh box: each switch the server named starts OFF (dsc, ndb)');
ok(r.notes.some((n) => /DSC \(MF\/HF\) is off: it measured 78% of a core here\. Tick it to offer it anyway\./.test(n)), `…with the reason beside it: "${r.notes[0]}"`);
ok(!r.boxes.dsc.disabled && !r.boxes.ndb.disabled, '…and stays an ordinary tick-box: offered, never hidden');
r = run(bench, true);
ok(Object.values(r.boxes).every((x) => x.checked) && r.notes.length === 0, 'an already-configured box is never changed');
r = run({ rows: [] }, false);
ok(Object.values(r.boxes).every((x) => x.checked), 'an older benchmark (no defaultOff): nothing changes');

ok(/j\.decoders\?\.defaultOff/.test(app) && /BLOCKABLE\.some\(\(m\) => m\.id === sw\)/.test(app),
   'ServerModeScreen.tsx applyBench applies the same list, and only for a switch it can show');
// ★ every switch the rule can name is on both screens (the app's list mirrors the page's)
const sws = new Set([...more.matchAll(/\{ "(?:dec|spot)_[a-z0-9]+",\s*\{ "([a-z]+)",\s*(true|false)/g)].filter((m) => m[2] === 'true').map((m) => m[1]));
ok(sws.size >= 15, `the rule's primary switches were found (${[...sws].join(', ')})`);
for (const sw of sws) {
  ok(new RegExp(`\\{ id: "${sw}",`).test(src) && new RegExp(`\\{ id: '${sw}',`).test(app), `"${sw}" is on the setup page and in the app`);
}
console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
