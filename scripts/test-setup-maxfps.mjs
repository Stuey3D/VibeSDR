// ★★ THE WATERFALL RATE MENU SHOWS THE RATE THE SERVER RUNS (11.0.9) — vibe_setup_page.h fpsShow().
// The Pi 2 was saved at 15; the menu (5 / 10 / 20) showed "Full · 20 fps", and an older page wrote that 20 back on save,
// which broke DAB audio. An odd value is now SHOWN as "(set by hand)" and saved untouched unless the owner picks another.
// Runs the page's REAL fpsShow against a <select> stand-in that behaves as a browser's (a value with no option = blank).
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const fn = src.match(/\nfunction fpsShow\(r\) \{[\s\S]*?\n\}\n/);
// ★★★ beta96 b96-setupux the helpers fpsShow calls (the server's frame rate, follow or own), lifted from the page too
const helpers = ['fpsServerNow', 'fpsFollows', 'fpsEffWords'].map((n) => (src.match(new RegExp(`\\nfunction ${n}\\([^)]*\\) \\{[\\s\\S]*?\\n\\}\\n`)) || [''])[0]).join('')
  + (src.match(/\nconst FPS_CHOSEN = new WeakSet\(\);\n/) || [''])[0];
if (!fn) { console.log('   FAIL fpsShow not found in the page'); process.exit(1); }
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

// ★ The menu's options as the PAGE writes them (not a copy that could drift): the 10-fps default (2026-10-20)
//   renamed Full/Half/Quarter to Smoother · 20 / Default · 10 / Half · 5 — same VALUES, so saved configs read as before.
const sel0 = src.match(/<select id="maxFps">([\s\S]*?)<\/select>/);
ok(/<option value="server" id="maxFpsServer">Use the server&rsquo;s frame rate<\/option>/.test(sel0 ? sel0[1] : ''), 'beta96: the menu offers "Use the server\'s frame rate"');
const PAGE = sel0 ? [...sel0[1].matchAll(/<option value="(\d+)">([^<]*)<\/option>/g)].map((m) => [m[1], m[2].replace(/&middot;/g, '·')]) : [];
ok(JSON.stringify(PAGE) === JSON.stringify([['20', 'Smoother · 20 fps'], ['10', 'Default · 10 fps'], ['5', 'Half · 5 fps']]),
   `the menu reads Smoother · 20 / Default · 10 / Half · 5 (${JSON.stringify(PAGE)})`);
ok(/10 fps is the default\. 20 is smoother, but sends about\s+twice the data and costs more processor time/.test(src),
   'the hint says 10 is the default, and that 20 is smoother but costs more data and CPU');
function makeSelect() {
  const sel = { options: [], _v: '' };
  const mk = (value, textContent) => ({ value, textContent, remove() { sel.options = sel.options.filter((o) => o !== this); } });
  sel.options = PAGE.map(([v, t]) => mk(v, t));
  Object.defineProperty(sel, 'value', { get() { return sel._v; }, set(v) { sel._v = sel.options.some((o) => o.value === v) ? v : ''; } });
  sel.appendChild = (o) => { o.remove = function () { sel.options = sel.options.filter((x) => x !== this); }; sel.options.push(o); };
  return sel;
}
const show = new Function('$', 'document', 'r', 'cfg', 'ANDROID', 'radioList', `${helpers}${fn[0]}\nfpsShow(r);`);
let CFG = { fpsDefault: 10 }, ANDR = false;
const EFF = { textContent: '' };
function run(sel, r) {
  const stub = (id) => id === 'maxFps' ? sel : id === 'maxFpsEff' ? EFF : null;
  // ★ an existing (configured) radio unless the case says otherwise — exactly the old behaviour
  if (r.configured === undefined) r.configured = true;
  show(stub, { createElement: () => ({ value: '', textContent: '' }) }, r, CFG, ANDR, () => [r]);
  const o = sel.options.find((x) => x.value === sel.value);
  return { value: sel.value, label: o ? o.textContent : '(blank)', n: sel.options.length };
}

let sel = makeSelect();
let r = run(sel, { fftRate: 15, maxFps: 15 });
ok(r.value === '15' && r.label === '15 fps (set by hand)', `15 saved → "${r.label}", not Smoother · 20`);
// What the save would write: the change handler is the ONLY writer, and it reads the selected value.
const handler = src.match(/\$\("maxFps"\)\.addEventListener\("change", \(\) => \{[\s\S]{0,400}?const v = parseFloat\(\$\("maxFps"\)\.value\) \|\| 10;/);
ok(!!handler && parseFloat(sel.value) === 15, 'the only writer is the change handler, and it would read 15 — never 20 or 10');
ok(!/stashRadio[\s\S]{0,400}maxFps/.test(src.slice(src.indexOf('function stashRadio'), src.indexOf('function stashRadio') + 400)),
   'stashRadio (every save) does not write the rate');

r = run(sel, { fftRate: 10, maxFps: 10 });
ok(r.value === '10' && r.n === 3, `switching to a radio at 10: Default · 10, and the hand-set 15 is gone from the menu (${r.n} options)`);

sel = makeSelect();
ok(run(sel, { fftRate: 5, maxFps: 5 }).value === '5', 'Half · 5 shows as 5');
ok(run(makeSelect(), { fftRate: 20, maxFps: 20 }).value === '20', 'Smoother · 20 shows as 20');
ok(run(makeSelect(), { fftRate: 20, maxFps: 0 }).value === '20', 'an existing server saved at 20 with no ceiling shows Smoother · 20 (no migration)');
ok(run(makeSelect(), {}).value === '10', 'nothing saved (server default) shows Default · 10');
r = run(makeSelect(), { fftRate: 20, maxFps: 8 });
ok(r.value === '8' && r.label === '8 fps (set by hand)', `fftRate 20 under a maxFps of 8 runs at 8 → "${r.label}"`);
r = run(makeSelect(), { fftRate: 12.5, maxFps: 0 });
ok(r.value === '12.5', `no ceiling, fftRate 12.5 → "${r.label}"`);
sel = makeSelect();
run(sel, { fftRate: 15 }); r = run(sel, { fftRate: 15 });
ok(r.n === 4, `shown twice (a re-render) → one hand-set entry, not two (${r.n} options)`);

// ★★★ beta96 b96-setupux THE SERVER'S FRAME RATE (Stuart 11-26). The menu's "server" entry is a VALUE the select holds.
function makeSelectS() {
  const s2 = makeSelect();
  const o = { value: 'server', textContent: "Use the server's frame rate" };
  o.remove = function () { s2.options = s2.options.filter((x) => x !== this); };
  s2.options.unshift(o);
  return s2;
}
CFG = { fpsDefault: 20 };
r = run(makeSelectS(), { fftRate: 5, maxFps: 5, fpsFollow: true });
ok(r.value === 'server', `a radio following the server shows "Use the server's frame rate" (${r.value})`);
ok(/server\u2019s 20 fps/.test(EFF.textContent), `…and says the rate it runs at: "${EFF.textContent}"`);
r = run(makeSelectS(), { fftRate: 5, maxFps: 5, fpsFollow: false });
ok(r.value === '5' && EFF.textContent === '', 'an override (fpsFollow false) shows its own 5, no server words');
r = run(makeSelectS(), { fftRate: 10, maxFps: 10 });
ok(r.value === '10', 'an EXISTING radio saved before beta96 (no fpsFollow) keeps its own 10 — identical behaviour');
const fresh = { fftRate: 10, maxFps: 10, configured: false };
r = run(makeSelectS(), fresh);
ok(r.value === 'server' && fresh.fpsFollow === true, 'a NEW radio (never set up) follows the server by default');
ANDR = true;
const ph = { fftRate: 10, maxFps: 10, configured: false };
r = run(makeSelectS(), ph);
ok(r.value === '10' && ph.fpsFollow !== true, "a phone's page never follows (one radio: its rate IS the server's)");
ANDR = false;
// the change handler: "server" sets the word and keeps the radio's own numbers; a value clears it
const hsrc = src.match(/\$\("maxFps"\)\.addEventListener\("change", \(\) => \{[\s\S]*?\}\);/)[0];
ok(/=== "server"\) \{ radio\(\)\.fpsFollow = true;[^}]*return; \}/.test(hsrc) && /radio\(\)\.fpsFollow = false;/.test(hsrc),
   'the handler: "server" → fpsFollow true (own numbers untouched); a value → fpsFollow false + the value');
ok(/fpsDefault: fpsServerNow\(\)/.test(src) && /cfg\.fpsDefault = fpsServerNow\(\)/.test(src), 'the Server tab\'s Frame rate is saved (collect + stashServer)');
ok(/<div class="card" id="fpsDefaultCard">\s*<h2>Frame rate<\/h2>/.test(src), 'the Server tab has a Frame rate card');

console.log(fail ? `\n${fail} FAILED` : '\nall good');
process.exit(fail ? 1 : 0);
