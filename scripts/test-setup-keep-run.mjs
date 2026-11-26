// ★★★ beta96 THE LANDING MAP + KEEP-RUNNING MODES ON THE SETUP PAGE — the page's real keepGate (vibekeep::gate in JavaScript),
// keepFacts, keepSetModes and mapLandRender (vibe_setup_page.h), lifted out and run against stand-in elements. The gate runs
// the SAME cases as vibeserver/test-keep-run.cpp §3/§4 (2.048 / 2.4 / 2.56 / 8 MS/s, multi-radio), then the card is drawn
// from a server's hwinfo (mapModes / mapCombo) for an RTL at 2.4, an RSP at 8 and an HF+, and the save mirrors ADS-B.
// Silent, no browser. Run: node --max-old-space-size=512 scripts/test-setup-keep-run.mjs
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
let fail = 0, pass = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };
const grab = (name) => {
  const m = src.match(new RegExp(`\\nfunction ${name}\\([^)]*\\) \\{[\\s\\S]*?\\n\\}\\n`));
  if (!m) { console.log(`   FAIL ${name} not found in the page`); process.exit(1); }
  return m[0];
};
// ★ a const: one line, or an array literal up to its closing "];"
const constOf = (name) => {
  const m = src.match(new RegExp(`\\nconst ${name} = (\\[[\\s\\S]*?\\n\\];|[^\\n]*)`));
  if (!m) { console.log(`FAIL const ${name}`); process.exit(1); }
  return m[0] + '\n';
};

// ── stand-in DOM ──
class El {
  constructor(id) { this.id = id; this.textContent = ''; this.innerHTML = ''; this.checked = false; this.disabled = false; this.value = '';
                    this.style = {}; const set = new Set(); this.classList = { toggle: (c, on) => { (on === undefined ? !set.has(c) : on) ? set.add(c) : set.delete(c); },
                    add: (c) => set.add(c), remove: (c) => set.delete(c), contains: (c) => set.has(c) }; }
  addEventListener() {}
  querySelectorAll() { return []; }
}
const els = {};
const $ = (id) => (els[id] ||= new El(id));

const fnNames = ['keepParse', 'keepIsCombo', 'keepRateWords', 'keepGate', 'keepCostWords', 'keepFacts', 'keepSatWords',
                 'mapLandRender', 'keepSetModes', 'keepMirror', 'keepMigrate', 'adsbLandOffer'];
const body = constOf('KEEP_MODES') + constOf('KEEP_COMBO_MIN') + constOf('ADSB_MIN_RATE') + fnNames.map(grab).join('\n');
const make = new Function('env', `
  let { $, radio, HW, adsbLandForm, modeBlockSet, plainLandingWords, esc } = env;
  const setHW = (h) => { HW = h; };
  ${body}
  return { ${fnNames.join(', ')}, setHW };`);

let RADIO = {};
let FORM = { users: 1, rate: 2400000, boost: false, blocked: false };
let BLOCKS = new Set();
const P = make({ $, radio: () => RADIO, HW: null, adsbLandForm: () => FORM, modeBlockSet: () => BLOCKS,
                 plainLandingWords: () => '96.100 MHz WFM', esc: (t) => String(t) });
const all = () => ['adsb', 'vdl2', 'acars', 'vor', 'ais'].map(id => ({ id, hw: true, why: '', pct: -1 }));
const find = (v, id) => v.find(o => o.id === id);

console.log('── the gate (vibekeep::gate\'s cases)');
{
  const a = all(); a[0].why = 'ADS-B needs a capture of 2.4 MS/s';
  let g = P.keepGate(a, false, '', 2048000, [], 0);
  ok(!find(g, 'adsb').enabled && /2\.4 MS\/s/.test(find(g, 'adsb').why) && find(g, 'adsb').drawn, '2.048: ADS-B greyed, the server\'s reason');
  ok(find(g, 'vdl2').enabled && find(g, 'acars').enabled && find(g, 'ais').enabled, '2.048: the rest may be ticked');
  g = P.keepGate(all(), false, '', 2400000, ['vdl2'], 0);
  ok(find(g, 'vdl2').on, 'VDL2 ticked');
  ok(find(g, 'adsb').why === '2.4 MS/s: one mode at a time on this radio — VDL2 is ticked', `2.4: ADS-B greyed: ${find(g, 'adsb').why}`);
  ok(!find(g, 'acars').enabled && /6\.1 MS\/s/.test(find(g, 'acars').why), `2.4: ACARS says 6.1 MS/s: ${find(g, 'acars').why}`);
  g = P.keepGate(all(), false, '', 2560000, ['ais'], 0);
  ok(/^2\.56 MS\/s: one mode at a time/.test(find(g, 'vdl2').why), `2.56: ${find(g, 'vdl2').why}`);
  g = P.keepGate(all(), true, '', 8000000, ['vdl2'], 0);
  ok(find(g, 'acars').enabled && !find(g, 'adsb').enabled, '8: VDL2 leaves ACARS open');
  g = P.keepGate(all(), true, '', 8000000, ['acars', 'vdl2'], 0);
  ok(find(g, 'acars').on && find(g, 'vdl2').on && find(g, 'ais').why === "ACARS + VDL2 already take this radio's capture", '8: both, the rest greyed');
  g = P.keepGate(all(), false, '', 2400000, ['acars', 'vdl2'], 0);
  ok(find(g, 'vdl2').on && !find(g, 'acars').on, '2.4: a stored pair keeps its first (VDL2, table order)');
  const hf = all(); hf[0].hw = false;
  g = P.keepGate(hf, false, '', 912000, [], 0);
  ok(!find(g, 'adsb').drawn && find(g, 'vdl2').enabled, 'HF+: ADS-B not drawn, VDL2 offered');
  ok(P.keepRateWords(2048000) === '2.048 MS/s' && P.keepRateWords(2400000) === '2.4 MS/s' && P.keepRateWords(2560000) === '2.56 MS/s'
     && P.keepRateWords(8000000) === '8 MS/s', 'rate words as the server says them');
  ok(P.keepParse(' VDL2 , acars,bogus') .join() === 'vdl2,acars' && P.keepParse('acars+vdl2').join() === 'vdl2,acars', 'the CSV as the server reads it');
  ok(P.keepCostWords(9.4) === 'about 9 % of one core (setup benchmark)' && P.keepCostWords(-1) === '', 'the cost words');
}

const hwRtl = (rate) => ({ offline: false, adsbHw: true, mapRate: rate,
  mapModes: [{ id: 'adsb', hw: true, why: '', pct: 13.2 }, { id: 'vdl2', hw: true, why: '', pct: 9.4 }, { id: 'acars', hw: true, why: '', pct: 3 },
             { id: 'vor', hw: true, why: '', pct: 20 }, { id: 'ais', hw: true, why: '', pct: 2 }],
  mapCombo: { minRate: 6083334, ok: rate >= 6083334, why: rate >= 6083334 ? '' : `ACARS + VDL2 needs a capture of 6.1 MS/s or more — this receiver runs at ${(rate / 1e6).toFixed(3)} MS/s` } });

console.log('── the card: an RTL at 2.4 MS/s (the Sony)');
{
  P.setHW(hwRtl(2400000)); FORM = { users: 1, rate: 2400000, boost: false, blocked: false }; BLOCKS = new Set();
  RADIO = { driver: 'rtlsdr', landingAdsb: false, adsbKeepRunning: false };
  P.mapLandRender();
  ok(!$('mapLandWrap').classList.contains('hide') && !$('mapLandOn').checked && $('mapLandBox').classList.contains('hide'), 'offered, off by default, nothing beneath');
  RADIO.landingMap = 'avmar'; P.keepMirror(RADIO);
  P.keepSetModes(['vdl2']);
  ok(RADIO.landingModes === 'vdl2' && !RADIO.landingAdsb, 'ticking VDL2 stores it; the ADS-B pair stays off');
  P.mapLandRender();
  const h = $('mapModeList').innerHTML;
  ok(/data-keep="vdl2" checked/.test(h) && /data-keep="adsb" disabled/.test(h) && /2\.4 MS\/s: one mode at a time on this radio/.test(h),
     'VDL2 ticked, ADS-B greyed with the rate and the reason');
  ok(/data-keep="acars" disabled/.test(h) && /6\.1 MS\/s/.test(h), 'ACARS greyed: the combination needs 6.1 MS/s');
  ok(/AVIATION/.test(h) && /MARITIME/.test(h) && /about 9 % of one core/.test(h), 'grouped, with the benchmark cost');
  ok($('mapRateNote').textContent === '2.4 MS/s detected: you can choose one of these modes.', `the rate line: ${$('mapRateNote').textContent}`);
  ok(!$('mapKeep').checked && !$('mapKeep').disabled && $('mapKeepLbl').textContent === 'Keep VDL2 running when nobody is connected', 'keep-running: offered, never ticked by default, named');
  ok(/share VDL2's 137 MHz capture/.test($('mapKeepNote').innerHTML) && /costs CPU all the time/.test($('mapKeepNote').innerHTML), 'the note: CPU and the shared 137 MHz capture');
  RADIO.mapKeep = true; P.keepMirror(RADIO); P.mapLandRender();
  ok($('mapKeep').checked && !$('bgKeepNote').classList.contains('hide') && /VDL2 is kept running/.test($('bgKeepNote').textContent), 'kept: the Background Decoding card says VDL2 owns the idle radio');
  ok(/SHARES its 137 MHz capture/.test(P.keepSatWords(RADIO)), 'the satellites line: VDL2 shares the capture');
  ok(!RADIO.adsbKeepRunning && !RADIO.landingAdsb, 'still no ADS-B pair (an older server idles the radio, never lands in ADS-B)');
  // switching to ADS-B (the newest tick wins where only one fits)
  P.keepSetModes(['vdl2', 'adsb']);
  ok(RADIO.landingModes === 'adsb' && RADIO.landingAdsb && RADIO.adsbKeepRunning, 'ticking ADS-B replaces VDL2 and mirrors the ADS-B pair (kept)');
  ok(/ADS-B pauses for the pass/.test(P.keepSatWords(RADIO)), 'the satellites line for ADS-B: pauses');
  // the owner lowers the rate in the form: ADS-B greyed by its own rule
  FORM = { users: 1, rate: 2048000, boost: false, blocked: false }; P.mapLandRender();
  ok(/data-keep="adsb"[^>]* disabled/.test($('mapModeList').innerHTML) && /This radio cannot run ADS-B/.test($('mapLandNote').textContent),
     'the form at 2.048: ADS-B greyed and the card says the plain landing applies');
  FORM = { users: 1, rate: 2400000, boost: false, blocked: false };
  BLOCKS = new Set(['avdata']); P.keepSetModes(['vdl2']); P.mapLandRender();
  ok(/data-keep="vdl2"[^>]* disabled/.test($('mapModeList').innerHTML) && /switched off for this radio/.test($('mapModeList').innerHTML), 'the datalink blocked: VDL2 greyed, said');
  BLOCKS = new Set();
}

console.log('── the card: an RSP1B at 8 MS/s');
{
  P.setHW(hwRtl(8000000)); FORM = { users: 1, rate: 8000000, boost: false, blocked: false };
  RADIO = { driver: 'sdrplay', landingMap: 'avmar', landingModes: 'vdl2', mapKeep: true };
  P.keepSetModes(['vdl2', 'acars']);
  ok(RADIO.landingModes === 'vdl2,acars', `ACARS + VDL2 stored together: ${RADIO.landingModes}`);
  P.mapLandRender();
  ok(/8 MS\/s detected: one mode at a time, or ACARS & VDL2 together/.test($('mapRateNote').textContent), `the rate line: ${$('mapRateNote').textContent}`);
  ok($('mapKeepLbl').textContent === 'Keep ACARS + VDL2 running when nobody is connected', 'keep names the pair');
  ok(/data-keep="ais"[^>]* disabled/.test($('mapModeList').innerHTML), 'AIS greyed beside the pair');
}

console.log('── an Airspy HF+ and an Icom');
{
  P.setHW({ offline: false, adsbHw: false, mapRate: 912000, mapModes: [{ id: 'adsb', hw: false, why: 'this radio cannot tune to 1090 MHz' },
    { id: 'vdl2', hw: true, why: '' }, { id: 'acars', hw: true, why: '' }, { id: 'vor', hw: false, why: 'x' }, { id: 'ais', hw: true, why: '' }],
    mapCombo: { minRate: 6083334, ok: false, why: 'x' } });
  FORM = { users: 1, rate: 912000, boost: false, blocked: false };
  RADIO = { driver: 'airspyhf', landingMap: 'avmar', landingModes: 'vdl2' };
  P.mapLandRender();
  ok(!/data-keep="adsb"/.test($('mapModeList').innerHTML) && !/data-keep="vor"/.test($('mapModeList').innerHTML) && /data-keep="vdl2" checked/.test($('mapModeList').innerHTML),
     'HF+: no ADS-B or VOR row at all (AGENTS.md), VDL2 offered');
  ok(/Airspy HF\+ \u2014 pauses VDL2 for the pass/.test(P.keepSatWords({ ...RADIO, mapKeep: true })), 'the satellites line names the HF+ pause');
  P.setHW(null); RADIO = { driver: 'icom' }; P.mapLandRender();
  ok($('mapLandWrap').classList.contains('hide'), 'an Icom: the card is not drawn');
}

console.log('── migration of a beta95 radio');
{
  const r = { landingAdsb: true, adsbKeepRunning: true };
  P.keepMigrate(r);
  ok(r.landingMap === 'avmar' && r.landingModes === 'adsb' && r.mapKeep, 'beta95 ADS-B + keep → the map, ADS-B, kept (as the server reads it)');
  const n = { landingAdsb: false }; P.keepMigrate(n);
  ok(!n.landingMap, 'nothing set stays nothing');
}

// ★ the save carries the three (and the mirrored pair)
ok(/landingMap: radio\(\)\.landingMap === "avmar" \? "avmar" : ""/.test(src) && /mapKeep: radio\(\)\.landingMap === "avmar" && !!radio\(\)\.mapKeep/.test(src), 'the save writes landingMap / landingModes / mapKeep');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
