// test-setup-bench-wide.mjs — the QUICK / WIDE BENCHMARK on the setup page (Stuart, 2026-11-18). The page's REAL code:
//   · the three locked-range profiles in Stuart's shape ("CPU/RAM:", then "50 DAB users @ 8 MS/s" …), DAB first, each
//     saying which bound set it — and no line for a profile that was not measured;
//   · the WIDE BENCHMARK button only where a configured radio is wide-capable (the server's wideOffer), a radio picker only
//     with several, THAT radio's rates with its current one selected;
//   · a wide result: its heading, the same lines, the USB bus verdict — and its figures marked unusable when the bus lost;
//   · the app says the same lines (ServerModeScreen.tsx benchProfileLines — ONE RULE, TWO READERS).
import fs from 'node:fs';

const rd = (p) => fs.readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const src = rd('android/app/src/main/cpp/vibe_setup_page.h');
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

const a = src.indexOf('function benchRateWords('), b = src.indexOf('/** ★ A running radio\'s own rate list');
if (a < 0 || b < a) { console.log('   FAIL the wide benchmark code was not found in the page'); process.exit(1); }

// ── a DOM just big enough ──
class El {
  constructor(tag) { this.tag = tag; this.innerHTML = ''; this.value = ''; this.dataset = {};
    const set = new Set(); this.classList = { set, toggle: (c, on) => (on ? set.add(c) : set.delete(c)), contains: (c) => set.has(c), add: (c) => set.add(c), remove: (c) => set.delete(c) }; }
}
const els = { benchWideWrap: new El('div'), benchWideRadio: new El('select'), benchWideRate: new El('select'),
              benchWideResult: new El('div'), benchProfiles: new El('div') };
for (const e of Object.values(els)) e.classList.add('hide');
// ★ A <select>'s value is one of its options, as a browser's: parsed from the innerHTML written.
for (const id of ['benchWideRadio', 'benchWideRate']) {
  const s = els[id]; let v = '', html = '';
  const opts = () => [...html.matchAll(/<option value="([^"]*)"/g)].map((m) => m[1]);
  Object.defineProperty(s, 'innerHTML', { get: () => html, set: (h) => { html = h; v = opts()[0] ?? ''; } });
  Object.defineProperty(s, 'value', { get: () => v, set: (x) => { v = opts().includes(String(x)) ? String(x) : (opts()[0] ?? ''); } });
  s.options = opts;
}
const $ = (id) => els[id];
const esc = (t) => String(t == null ? '' : t).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const api = new Function('$', 'esc', `${src.slice(a, b)}\nreturn { benchRateWords, benchProfileLines, benchWideLines, benchWideRadios,
  benchProfilesRender, benchWideRender, benchWideRates };`)($, esc);

// ── the words ──
ok(api.benchRateWords(2048000) === '2.048 MS/s' && api.benchRateWords(8e6) === '8 MS/s' && api.benchRateWords(30720000) === '30.72 MS/s'
   && api.benchRateWords(912000) === '912 kS/s', 'rates as rateWords says them (2.048 MS/s, 8 MS/s, 30.72 MS/s, 912 kS/s)');
const lu = { wfm: 20, nfm: 20,
  dab: { n: 50, rate: 8e6, by: 'cpu', blocks: 4 }, fm: { n: 25, rate: 8e6, by: 'ram' }, narrow: { n: 200, rate: 8e6, by: 'cpu', mode: 'ssb' } };
const L = api.benchProfileLines(lu);
ok(L.length === 3 && L[0] === '50 DAB users @ 8 MS/s, 4 blocks (set by CPU)' && L[1] === '25 WFM + Advanced RDS users @ 8 MS/s (set by RAM)'
   && L[2] === '200 SSB/AM/NFM users @ 8 MS/s (set by CPU)', 'Stuart\'s three lines, DAB first, each saying its bound');
ok(api.benchProfileLines({ dab: { n: 1, rate: 2048000, by: 'cpu', blocks: 1 } })[0] === '1 DAB user @ 2.048 MS/s (set by CPU)',
   'one block: no block count; one user: singular');
ok(api.benchProfileLines({ fm: { n: 0, rate: 56e6, by: 'capture' } })[0] === '0 WFM + Advanced RDS users @ 56 MS/s (the capture itself cannot keep up at this rate)',
   'the capture could not keep up: said so');
const hfOnly = api.benchProfileLines({ wfm: 3, nfm: 9, narrow: { n: 40, rate: 912000, by: 'cpu' } });
ok(hfOnly.length === 1 && /^40 SSB\/AM\/NFM users @ 912 kS\/s/.test(hfOnly[0]), 'never a line for a profile not measured (an HF+: no DAB, no FM)');
ok(api.benchProfileLines({ narrow: { n: 5, rate: 8e6, by: 'cpu' }, dabWhy: 'run the quick benchmark first: …' })[0] === 'DAB: run the quick benchmark first: …',
   'DAB not worked out: the reason in words');

// ── the quick card ──
api.benchProfilesRender({ v: 1, lockedUsers: lu });
ok(!els.benchProfiles.classList.contains('hide') && els.benchProfiles.innerHTML.startsWith('<b>Quick benchmark — CPU/RAM:</b><br>50 DAB users'),
   'the quick result: "CPU/RAM:" then the lines');
api.benchProfilesRender({ v: 1, lockedUsers: { wfm: 20, nfm: 20 } });
ok(els.benchProfiles.classList.contains('hide'), 'an older result (no profiles): no CPU/RAM block (its old sentence stays)');

// ── the WIDE BENCHMARK button ──
api.benchWideRender({ v: 1, wideOffer: [] });
ok(els.benchWideWrap.classList.contains('hide'), 'no wide-capable radio: no WIDE BENCHMARK button');
api.benchWideRender({ v: 0 });
ok(els.benchWideWrap.classList.contains('hide'), 'no offer at all (an older server): no button');
const usrp = { index: 0, serial: '30C1234', driver: 'usrp', label: 'B210', current: 30720000, rates: [8e6, 16e6, 30720000, 56e6] };
api.benchWideRender({ v: 1, wideOffer: [usrp] });
ok(!els.benchWideWrap.classList.contains('hide'), 'a USRP: the WIDE BENCHMARK button');
ok(els.benchWideRadio.classList.contains('hide'), '…one wide radio: no radio picker');
ok(els.benchWideRate.options().join(',') === '8000000,16000000,30720000,56000000' && els.benchWideRate.value === '30720000',
   '…its own rates, its current rate selected');
const rsp = { index: 1, serial: 'RSP77', driver: 'sdrplay', label: 'RSP1A', current: 6e6, rates: [2e6, 2048000, 6e6, 8e6] };
api.benchWideRender({ v: 1, wideOffer: [usrp, rsp] });
ok(!els.benchWideRadio.classList.contains('hide') && els.benchWideRadio.options().length === 2, 'two wide radios: a radio picker');
els.benchWideRadio.value = '1'; api.benchWideRates();
ok(els.benchWideRate.options().join(',') === '2000000,2048000,6000000,8000000' && els.benchWideRate.value === '6000000',
   '…picking the RSP lists ITS rates, its current one selected');
const merged = api.benchWideRadios([usrp], { '30C1234': [40e6, 56e6] });
ok(merged[0].rates.includes(40e6) && merged[0].rates.length === 5 && merged[0].key === '30C1234', 'a running radio\'s own list is merged in');
ok(api.benchWideRadios([{ index: 2, driver: 'hackrf', rates: [10e6], current: 20e6 }])[0].rates.join(',') === '10000000,20000000'
   && api.benchWideRadios([{ index: 2, driver: 'hackrf', rates: [] }])[0].key === 'radio2', 'the current rate is always offered; no serial = radio<n>');

// ── a wide result ──
const entry = { v: 1, at: 1794986586, rate: 56e6, label: 'B210', dab: { n: 50, rate: 56e6, by: 'cpu', blocks: 27 },
  fm: { n: 25, rate: 56e6, by: 'cpu' }, narrow: { n: 200, rate: 56e6, by: 'cpu' },
  usb: { ok: true, deliveredPct: 99.98, usbDrops: 0, iqDrops: 0 } };
const w = api.benchWideLines(entry, 'B210');
ok(w.head === 'Wide benchmark — B210 @ 56 MS/s' && w.lines.length === 3 && w.usb === 'USB bus: kept up (99.98 % delivered, 0 drops)' && !w.unusable,
   'the heading, the three lines, "USB bus: kept up (99.98 % delivered, 0 drops)"');
const bad = api.benchWideLines({ ...entry, usb: { ok: false, deliveredPct: 97.2, why: 'only 97.20 % of the samples arrived; 3 USB drops reported by the radio' } }, 'B210');
ok(bad.unusable && bad.usb === 'USB bus: could not keep up (only 97.20 % of the samples arrived; 3 USB drops reported by the radio)',
   'the bus lost samples: "could not keep up (…)" and the figures marked unusable');
ok(api.benchWideLines({ ...entry, usb: { ok: false, notRun: 'the radio was not running, so the USB bus could not be tested' } }).usb
   === 'USB bus: not tested — the radio was not running, so the USB bus could not be tested', 'not tested: says why');
api.benchWideRender({ v: 1, wideOffer: [usrp], wide: { '30C1234': { ...entry, usb: bad && { ok: false, why: 'x' } } } });
const html = els.benchWideResult.innerHTML;
ok(!els.benchWideResult.classList.contains('hide') && html.includes('Wide benchmark — B210 @ 56 MS/s') && html.includes('CPU/RAM:')
   && html.includes('not usable at this rate') && html.includes('<s>50 DAB users'), 'drawn: heading, CPU/RAM, the lines struck through when unusable');
api.benchWideRender({ v: 0, wideOffer: [usrp], wide: { '30C1234': entry } });
ok(els.benchWideResult.innerHTML.includes('USB bus: kept up') && !els.benchWideResult.innerHTML.includes('<s>'), 'a wide result shows even with no quick result saved');
// ★★★ 2026-11-19: the capture verdict is the WIDE benchmark's now (the quick run is an RTL's) — drawn under its entry.
api.benchWideRender({ v: 1, wideOffer: [usrp], wide: { '30C1234': { ...entry, capture: { lockedMaxHz: 8e6, fullMaxHz: 8e6, headroomPct: 70,
  rates: [{ hz: 8e6, lockedOk: true, lockedHottestPct: 31, perListenerPct: 2, lockedUsers: 10, fullOk: true, fullHottestPct: 12 }] } } } });
const ch = els.benchWideResult.innerHTML;
ok(ch.includes('Wide capture rates:') && ch.includes('<b>8 MS/s</b>: this machine keeps up as a locked range (31% of a core')
   && ch.includes('keeps up as a full radio (12%)') && ch.includes('offered rates up to <b>8 MS/s</b> as a locked range'),
   'the wide entry\'s capture verdict is drawn under it');
api.benchWideRender({ v: 1, wideOffer: [usrp], wide: { '30C1234': entry } });
ok(!els.benchWideResult.innerHTML.includes('Wide capture rates:'), '…and nothing where the entry has none');

// ── wired in ──
ok(/<button id="benchRun" type="button">Run quick benchmark<\/button>/.test(src) && /btn\.textContent = "Run quick benchmark"/.test(src),
   'the quick button is named QUICK, and keeps the name after a run');
ok(src.includes('High sample rate SDR detected. As well as the quick benchmark, run the wide benchmark at the\n          sample rate you intend to use.'),
   'the WIDE button\'s subtext, Stuart\'s words');
ok(/\$\("benchWideRun"\)\.addEventListener\("click"/.test(src) && /benchWideRender\(j\);/.test(src), 'the button and the render are wired');
ok(/\/vibeserver\/benchmark\?wide=\$\{hz\}&radio=/.test(src) && /benchFollow\(pre\)/.test(src),
   'the request names the rate and the radio, and its progress is followed at that radio\'s process');

// ── the app says the same lines ──
const app = rd('src/screens/ServerModeScreen.tsx');
const fa = app.indexOf('function benchRateWords('), fb = app.indexOf('/* END benchProfileLines */');
if (fa < 0 || fb < fa) ok(false, 'the app\'s benchProfileLines was found');
else {
  const js = app.slice(fa, fb).replace(/: any\b/g, '').replace(/: string\[\]/g, '').replace(/: string\b/g, '').replace(/: number\b/g, '');
  const appApi = new Function(`${js}\nreturn { benchProfileLines, benchRateWords };`)();
  ok(JSON.stringify(appApi.benchProfileLines(lu)) === JSON.stringify(L), 'the app\'s three lines are the page\'s');
  ok(appApi.benchRateWords(30720000) === '30.72 MS/s' && appApi.benchProfileLines({ dabWhy: 'x' })[0] === 'DAB: x', '…and its words for a rate and a missing DAB');
}

console.log(fail ? `setup bench wide: ${fail} FAILED` : 'ok  setup bench wide');
process.exit(fail ? 1 : 0);
