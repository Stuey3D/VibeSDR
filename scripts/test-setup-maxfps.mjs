// ★★ THE WATERFALL RATE MENU SHOWS THE RATE THE SERVER RUNS (11.0.9) — vibe_setup_page.h fpsShow().
// The Pi 2 was saved at 15; the menu (5 / 10 / 20) showed "Full · 20 fps", and an older page wrote that 20 back on save,
// which broke DAB audio. An odd value is now SHOWN as "(set by hand)" and saved untouched unless the owner picks another.
// Runs the page's REAL fpsShow against a <select> stand-in that behaves as a browser's (a value with no option = blank).
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const fn = src.match(/\nfunction fpsShow\(r\) \{[\s\S]*?\n\}\n/);
if (!fn) { console.log('   FAIL fpsShow not found in the page'); process.exit(1); }
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

const PAGE = [['20', 'Full · 20 fps'], ['10', 'Half · 10 fps'], ['5', 'Quarter · 5 fps']];
function makeSelect() {
  const sel = { options: [], _v: '' };
  const mk = (value, textContent) => ({ value, textContent, remove() { sel.options = sel.options.filter((o) => o !== this); } });
  sel.options = PAGE.map(([v, t]) => mk(v, t));
  Object.defineProperty(sel, 'value', { get() { return sel._v; }, set(v) { sel._v = sel.options.some((o) => o.value === v) ? v : ''; } });
  sel.appendChild = (o) => { o.remove = function () { sel.options = sel.options.filter((x) => x !== this); }; sel.options.push(o); };
  return sel;
}
const show = new Function('$', 'document', 'r', `${fn[0]}\nfpsShow(r);`);
function run(sel, r) {
  show(() => sel, { createElement: () => ({ value: '', textContent: '' }) }, r);
  const o = sel.options.find((x) => x.value === sel.value);
  return { value: sel.value, label: o ? o.textContent : '(blank)', n: sel.options.length };
}

let sel = makeSelect();
let r = run(sel, { fftRate: 15, maxFps: 15 });
ok(r.value === '15' && r.label === '15 fps (set by hand)', `15 saved → "${r.label}", not Full · 20`);
// What the save would write: the change handler is the ONLY writer, and it reads the selected value.
const handler = src.match(/\$\("maxFps"\)\.addEventListener\("change", \(\) => \{\n\s*const v = parseFloat\(\$\("maxFps"\)\.value\) \|\| 20;/);
ok(!!handler && parseFloat(sel.value) === 15, 'the only writer is the change handler, and it would read 15 — never 20');
ok(!/stashRadio[\s\S]{0,400}maxFps/.test(src.slice(src.indexOf('function stashRadio'), src.indexOf('function stashRadio') + 400)),
   'stashRadio (every save) does not write the rate');

r = run(sel, { fftRate: 10, maxFps: 10 });
ok(r.value === '10' && r.n === 3, `switching to a radio at 10: Half · 10, and the hand-set 15 is gone from the menu (${r.n} options)`);

sel = makeSelect();
ok(run(sel, { fftRate: 5, maxFps: 5 }).value === '5', 'Quarter · 5 shows as 5');
ok(run(makeSelect(), { fftRate: 20, maxFps: 20 }).value === '20', 'Full · 20 shows as 20');
ok(run(makeSelect(), {}).value === '20', 'nothing saved (server default) shows Full · 20');
r = run(makeSelect(), { fftRate: 20, maxFps: 8 });
ok(r.value === '8' && r.label === '8 fps (set by hand)', `fftRate 20 under a maxFps of 8 runs at 8 → "${r.label}"`);
r = run(makeSelect(), { fftRate: 12.5, maxFps: 0 });
ok(r.value === '12.5', `no ceiling, fftRate 12.5 → "${r.label}"`);
sel = makeSelect();
run(sel, { fftRate: 15 }); r = run(sel, { fftRate: 15 });
ok(r.n === 4, `shown twice (a re-render) → one hand-set entry, not two (${r.n} options)`);

console.log(fail ? `\n${fail} FAILED` : '\nall good');
process.exit(fail ? 1 : 0);
