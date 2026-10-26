// ★★ V12 THE OWNER'S DAB BAND SCAN ON THE SETUP PAGE — the page's real dabBandRender (vibe_setup_page.h) against stand-in
// elements. Stuart (2026-10-26): "DAB scan I will add for initial setup". AGENTS.md: drawn only where this radio can do DAB
// (never on an HF+, never with DAB blocked); progress while it runs, Cancel only then; each multiplex found a row (block,
// name, frequency, station count, quality) opening to its stations; "N stations added to bookmarks"; and, with a DAB
// landing chosen, "Use … as the landing block" puts it there with the scanned station list. The endpoint itself is
// tested end to end by test-server-dab-bandscan.mjs (E).
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };
const grab = (name) => {
  const m = src.match(new RegExp(`\\nfunction ${name}\\([^)]*\\) \\{[\\s\\S]*?\\n\\}\\n`));
  if (!m) { console.log(`   FAIL ${name} not found in the page`); process.exit(1); }
  return m[0];
};
ok(/id="dabBandWrap"/.test(src) && /id="dabBandScan"/.test(src) && /id="dabBandStop"/.test(src) && /id="dabBandList"/.test(src), 'the markup is there');

// ── stand-ins ──
class El {
  constructor(id) { this.id = id; this.cls = new Set(['hide']); this.textContent = ''; this._html = ''; this.disabled = false; this.btns = []; }
  get classList() { const c = this.cls; return { toggle: (k, on) => (on ? c.add(k) : c.delete(k)), contains: (k) => c.has(k), add: (k) => c.add(k), remove: (k) => c.delete(k) }; }
  set innerHTML(h) {
    this._html = h;
    this.btns = [...h.matchAll(/<button type="button" class="ghost use" data-ch="(\d+)">/g)].map((m) => {
      const b = { dataset: { ch: m[1] }, on: null, addEventListener: (_e, f) => { b.on = f; } }; return b;
    });
  }
  get innerHTML() { return this._html; }
  querySelectorAll() { return this.btns; }
}
const els = {};
const $ = (id) => (els[id] ||= new El(id));
const env = { HW: { dabHw: true, offline: false }, blocked: new Set(), r: { driver: 'rtlsdr', landingDabChannel: -1 }, can: true };
const dabLandStations = {};
const blocks = Array.from({ length: 41 }, (_, i) => ({ name: `B${i}`, hz: 174928000 + i * 1e6 }));
let landRenders = 0;
const render = new Function('$', 'env', 'dabLandStations', 'blocks', 'bump', `
  const esc = (t) => String(t == null ? "" : t).replace(/[&<>"]/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;","\\"":"&quot;"}[c]));
  const radio = () => env.r;
  const modeBlockSet = () => env.blocked;
  const dabLandHwCan = () => env.can;
  const dabLandBlocks = () => blocks;
  const dabLandRender = () => bump();
  let HW = env.HW;
  let dabBand = null;
  ${grab('dabBandRender')}
  return (state, hw) => { dabBand = state; HW = hw === undefined ? env.HW : hw; dabBandRender(); };
`)($, env, dabLandStations, blocks, () => landRenders++);

const hit = (block, channel, n, fib = 1) => ({ block, channel, hz: 222064000, ensemble: `${block} <MUX>`, fib, mer: 20, complete: true,
  services: Array.from({ length: n }, (_, i) => ({ sid: 0xC000 + i, label: `Station ${i}` })) });

// ── offered where DAB can run, nowhere else ──
render(null);
ok(!$('dabBandWrap').classList.contains('hide'), 'an RTL radio that can do DAB: the scan is offered');
env.can = false; render(null);
ok($('dabBandWrap').classList.contains('hide'), 'a radio that cannot do DAB (an HF+, a locked centre with own VFOs): not drawn');
env.can = true; env.blocked = new Set(['dab']); render(null);
ok($('dabBandWrap').classList.contains('hide'), 'DAB blocked by the owner: not drawn');
env.blocked = new Set();
render(null, { offline: true });
ok($('dabBandScan').disabled && /not running/.test($('dabBandMsg').textContent), 'a radio that is not running: the key is disabled and the line says why');

// ── running ──
render({ running: true, done: 12, total: 41, current: '9D', found: [hit('11D', 29, 3)], added: 0 });
ok($('dabBandScan').disabled && !$('dabBandStop').classList.contains('hide'), 'while it runs: Scan disabled, Cancel shown');
ok($('dabBandMsg').textContent === 'Scanning 12 / 41 blocks · 9D — 1 multiplex so far', `progress: "${$('dabBandMsg').textContent}"`);
ok(/<span class="b">11D<\/span>/.test($('dabBandList').innerHTML) && /11D &lt;MUX&gt;/.test($('dabBandList').innerHTML), 'a row per multiplex found, the name escaped');
ok(/222\.064 MHz · 3 stations/.test($('dabBandList').innerHTML) && /STRONG/.test($('dabBandList').innerHTML), '…with frequency, station count and quality');
ok(/Station 0<br>Station 1<br>Station 2/.test($('dabBandList').innerHTML), '…opening to its stations');
ok(!/Use 11D as the landing block/.test($('dabBandList').innerHTML), 'no landing key while the DAB landing is off');

// ── finished ──
render({ running: false, finished: true, done: 41, total: 41, found: [hit('11D', 29, 3), hit('7D', 11, 2, 0.5)], added: 5 });
ok($('dabBandStop').classList.contains('hide') && !$('dabBandScan').disabled, 'finished: Cancel gone, Scan back');
ok($('dabBandMsg').textContent === '2 multiplexes, 5 stations — 5 stations added to bookmarks.', `summary: "${$('dabBandMsg').textContent}"`);
ok($('dabBandList').innerHTML.indexOf('>7D<') < $('dabBandList').innerHTML.indexOf('>11D<'), 'rows in block order');
ok(/WEAK/.test($('dabBandList').innerHTML), 'a weak multiplex says so');
render({ running: false, finished: true, done: 41, total: 41, found: [], added: 0 });
ok(/^No DAB found on any of the 41 blocks/.test($('dabBandMsg').textContent), `nothing: "${$('dabBandMsg').textContent}"`);
render({ running: false, finished: true, cancelled: true, why: 'stopped — a listener arrived and has the radio', done: 9, total: 41, found: [], added: 0 });
ok($('dabBandMsg').textContent === 'Scan stopped at 9 / 41 (a listener arrived and has the radio) — nothing found yet.', `cancelled: "${$('dabBandMsg').textContent}"`);
render({ running: false, error: 'not while somebody is listening — 1 listener is on this radio', found: [] });
ok(/^Scan not started: not while somebody is listening/.test($('dabBandMsg').textContent), 'a refusal in the server’s words');

// ── use as the landing block ──
env.r.landingDabChannel = 32;
render({ running: false, finished: true, done: 41, total: 41, found: [hit('11D', 29, 3)], added: 3 });
const use = $('dabBandList').btns.find((b) => b.dataset.ch === '29');
ok(!!use, 'with a DAB landing on: "Use 11D as the landing block"');
use.on();
ok(env.r.landingDabChannel === 29 && env.r.landingDabSid === 0 && dabLandStations[29]?.length === 3 && landRenders > 0,
   'pressing it makes 11D the landing block, with the scanned stations to choose from');

console.log(fail ? `test-setup-dab-bandscan: ${fail} FAILED` : 'test-setup-dab-bandscan: all passed');
process.exit(fail ? 1 : 0);
