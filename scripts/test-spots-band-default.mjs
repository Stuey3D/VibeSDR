// test-spots-band-default.mjs — V12 DIGITAL SPOTS: BAND's DEFAULT comes from the benchmark, and the owner can ALWAYS
// switch it back on (Stuart: "never hide any controls"). The setup page's REAL benchApplyDefaults against a DOM stand-in:
// a fresh box whose Band row measured amber/red gets "spotsband" switched off with the reason beside it; a green box keeps
// it; an already-configured box is never changed; and the switch stays an ordinary, enabled tick-box the owner can tick.
// The app's twin (ServerModeScreen.tsx applyBench) is checked to carry the same rule (ONE RULE, TWO READERS).
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const a = src.indexOf('function benchApplyDefaults()'), b = src.indexOf('async function benchLoad()');
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };
if (a < 0 || b < a) { console.log('   FAIL benchApplyDefaults was not found in the page'); process.exit(1); }

function run(rows, configured) {
  const blocked = new Set();
  const notes = [];
  const box = { checked: true, disabled: false, dataset: { mode: 'spotsband' },
    parentElement: { appendChild: (n) => notes.push(n.textContent) },
    dispatchEvent: () => { if (box.checked) blocked.delete('spotsband'); else blocked.add('spotsband'); } };
  const document = {
    querySelector: (q) => (q === 'input[data-mode="spotsband"]' ? box : null),
    createElement: () => ({ style: {}, className: '', textContent: '' }),
  };
  const $ = () => null;
  const BENCH = { rows };
  const cfg = { configured };
  new Function('$', 'document', 'BENCH', 'cfg', 'Event', `${src.slice(a, b)}\nbenchApplyDefaults();`)($, document, BENCH, cfg, class { constructor(t) { this.type = t; } });
  return { box, blocked, notes };
}
const row = (grade, pct) => [{ id: 'dec_ft8band', label: 'Decoder: digital spots, Band (80 m)', pct, grade }];

let r = run(row('red', 140), false);
ok(!r.box.checked && r.blocked.has('spotsband'), 'a fresh box that measured Band RED: "spotsband" is switched off');
ok(r.notes.length === 1 && /140% of a core/.test(r.notes[0]) && /Tick it to offer it anyway/.test(r.notes[0]), `…with the reason beside the switch: "${r.notes[0]}"`);
ok(!r.box.disabled, '…and the switch stays an ordinary tick-box: the owner can always switch Band back on');
r = run(row('amber', 78), false);
ok(!r.box.checked && r.blocked.has('spotsband'), 'AMBER is off too (only a green box offers Band by default)');
r = run(row('green', 12), false);
ok(r.box.checked && !r.blocked.has('spotsband') && r.notes.length === 0, 'a GREEN box keeps Band on, no note');
r = run(row('red', 140), true);
ok(r.box.checked && r.notes.length === 0, 'an already-configured box is never changed by re-running the benchmark');
r = run([], false);
ok(r.box.checked, 'no Band row (an older benchmark): nothing changes');

// ★ The app's server screen: the same rule, the same id, the same grades.
const app = fs.readFileSync(new URL('../src/screens/ServerModeScreen.tsx', import.meta.url), 'utf8');
ok(/row\('dec_ft8band'\)/.test(app) && /band\.grade !== 'green' && band\.grade !== 'none'/.test(app) && /spotsband/.test(app),
   'ServerModeScreen.tsx applyBench carries the same rule (dec_ft8band, not green → spotsband off)');
// ★ e95282d89 (modes and decoders as a tree): Band is a child of "Digital spots" now, labelled for its place under it.
ok(/\{ id: "spots", +label: "[^"]+" \}/.test(src) && /\{ id: 'spots', label: '[^']+' \}/.test(app) &&
   /\{ id: "spotsband", label: "[^"]*Band[^"]*", parent: "spots" \}/.test(src) && /\{ id: 'spotsband', label: '[^']*Band[^']*', parent: 'spots' \}/.test(app),
   'both setup screens list the switch, so the owner can always turn it on');
console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
