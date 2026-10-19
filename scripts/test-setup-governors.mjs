// ★★★ THE GOVERNOR MENU OFFERS ONLY WHAT THIS MACHINE HAS (2026-10-19) — vibe_setup_page.h govOffer().
// The Lenovo's intel_pstate offers performance and powersave; the menu offered schedutil, the save "worked",
// vibeserver-governor rightly refused it, and the page showed Schedutil over "Currently: performance".
// Runs the page's REAL govOffer against a <select> stand-in that behaves as a browser's does (setting a value no option
// has leaves it blank) — options are REMOVED, never hidden, because Safari offers a hidden <option> anyway.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const fn = src.match(/let govHw = null, govAll = null;\nfunction govOffer\(\) \{[\s\S]*?\n\}\n/);
if (!fn) { console.log('   FAIL govOffer not found in the page'); process.exit(1); }
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

const PAGE = [['performance', 'Full speed'], ['ondemand', 'On demand'], ['schedutil', 'Schedutil'],
              ['powersave', "Processor's own scaling"], ['default', 'Leave the system setting alone']];
function select(saved) {
  const sel = { options: [], _v: '' };
  const mk = (value, text) => ({ value, textContent: text, remove() { sel.options = sel.options.filter((o) => o !== this); } });
  sel.options = PAGE.map(([v, t]) => mk(v, t));
  Object.defineProperty(sel, 'innerHTML', { set() { sel.options = []; } });
  Object.defineProperty(sel, 'value', { get() { return sel._v; }, set(v) { sel._v = sel.options.some((o) => o.value === v) ? v : ''; } });
  sel.appendChild = (o) => { o.remove = function () { sel.options = sel.options.filter((x) => x !== this); }; sel.options.push(o); };
  sel.value = saved;
  return sel;
}
function run(hw, saved) {
  const sel = select(saved);
  const ctx = { $: () => sel, document: { createElement: () => ({ value: '', textContent: '' }) } };
  new Function('$', 'document', 'hw', `${fn[0]}\ngovHw = hw; govOffer();`)(ctx.$, ctx.document, hw);
  return { values: sel.options.map((o) => o.value), labels: sel.options.map((o) => o.textContent), value: sel.value };
}

const lenovo = { governors: 'performance powersave', cpuFreqDriver: 'intel_pstate' };
let r = run(lenovo, 'performance');
ok(JSON.stringify(r.values) === JSON.stringify(['performance', 'powersave', 'default']),
   `intel_pstate: full speed, the processor's own scaling, leave alone (${r.values.join(', ')})`);
r = run(lenovo, 'schedutil');
ok(r.value === 'schedutil' && r.labels.some((l) => l === 'Schedutil — not available on this processor'),
   `a saved schedutil it cannot do stays chosen and is marked (${r.labels.find((l) => l.startsWith('Schedutil'))})`);
ok(!r.values.includes('ondemand'), '…and on demand, which it does not have, is not offered');

const pi = { governors: 'conservative ondemand userspace powersave performance schedutil', cpuFreqDriver: 'cpufreq-dt' };
r = run(pi, 'performance');
ok(JSON.stringify(r.values) === JSON.stringify(['performance', 'ondemand', 'schedutil', 'default']),
   `a Pi: the three it has, and NOT powersave — on a Pi that pins the minimum clock (${r.values.join(', ')})`);

r = run({}, 'performance');
ok(!r.values.includes('powersave') && r.values.includes('schedutil'),
   `no governor list (no cpufreq): the menu as before, without the pstate-only entry (${r.values.join(', ')})`);

const amd = { governors: 'performance powersave', cpuFreqDriver: 'amd-pstate-epp' };
ok(run(amd, 'performance').values.includes('powersave'), 'amd-pstate: the processor\'s own scaling is offered too');

console.log(fail ? `\n${fail} FAILED` : '\nall good');
process.exit(fail ? 1 : 0);
