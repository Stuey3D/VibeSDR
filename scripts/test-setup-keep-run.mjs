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
  // ★ a one-line function first (its closing brace on the same line), else up to the first top-level "}"
  const m = src.match(new RegExp(`\\nfunction ${name}\\([^)]*\\) \\{[^\\n]*\\}\\n`)) || src.match(new RegExp(`\\nfunction ${name}\\([^)]*\\) \\{[\\s\\S]*?\\n\\}\\n`));
  if (!m) { console.log(`   FAIL ${name} not found in the page`); process.exit(1); }
  return m[0];
};
// ★ a const: one line, or an array literal up to its closing "];"
const constOf = (name) => {
  const m = src.match(new RegExp(`\\nconst ${name} = (\\[[\\s\\S]*?\\];|[^\\n]*)`));
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

const fnNames = ['singleBlockOf', 'singleWords', 'singleOptions', 'singleFromTicks', 'singleRender', 'singleKeepable', 'modeLandingId', 'keepMapOf', 'keepBase', 'keepTokName', 'keepParse', 'keepIsCombo', 'keepRateWords', 'keepGate', 'keepCostWords', 'keepFacts', 'keepSatWords',
                 'mapLandRender', 'keepSetModes', 'keepMirror', 'keepMigrate', 'adsbLandOffer'];
const body = constOf('LISTEN_DEMODS') + constOf('WHOLE_MODES') + constOf('KEEP_MODES') + constOf('KEEP_GROUP_WORDS') + constOf('KEEP_COMBO_MIN') + constOf('KEEP_MARITIME_MIN') + constOf('ADSB_MIN_RATE') + fnNames.map(grab).join('\n');
const make = new Function('env', `
  let { $, radio, HW, adsbLandForm, modeBlockSet, plainLandingWords, esc, bgIsmBands, modeTree, modeCapShown, dabLandHwCan, DAB_DECODER } = env;
  const setHW = (h) => { HW = h; };
  ${body}
  return { ${fnNames.join(', ')}, setHW };`);

let RADIO = {};
let FORM = { users: 1, rate: 2400000, boost: false, blocked: false };
let BLOCKS = new Set();
const P = make({ $, radio: () => RADIO, HW: null, adsbLandForm: () => FORM, modeBlockSet: () => BLOCKS,
                 plainLandingWords: () => '96.100 MHz WFM', esc: (t) => String(t),
                 bgIsmBands: () => [{ id: '433', label: '433.92 MHz' }, { id: '868', label: '868 MHz SRD' }],
                 modeTree: () => ['wfm', 'nfm', 'am', 'usb', 'lsb', 'drm', 'sondes'].map((id) => ({ id })), modeCapShown: (t, m) => m.id !== 'drm',
                 dabLandHwCan: () => true, DAB_DECODER: true });
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
             { id: 'vor', hw: true, why: '', pct: 20 }, { id: 'ais', hw: true, why: '', pct: 2 }, { id: 'ism', hw: true, why: '', pct: 11 }],
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

console.log('── ★★★ beta96 the MARITIME row (AIS + DSC channel 70)');
{
  const withMar = (rate, hw, why = '') => { const h = hwRtl(rate); h.mapModes.push({ id: 'aisdsc', hw, why, minRate: 6166667, tunes: true }); return h; };
  P.setHW(withMar(2400000, false)); FORM = { users: 1, rate: 2400000, boost: false, blocked: false }; BLOCKS = new Set();
  RADIO = { driver: 'rtlsdr', landingMap: 'avmar', landingModes: '', mapKeep: false };
  P.mapLandRender();
  ok(!/data-keep="aisdsc"/.test($('mapModeList').innerHTML) && /data-keep="ais"/.test($('mapModeList').innerHTML), 'RTL at 2.4: AIS drawn, AIS + DSC not drawn at all');
  ok(!P.singleOptions(RADIO).some((o) => o.tok === 'aisdsc'), '…nor in the single-mode list');
  P.setHW(withMar(8000000, true)); FORM = { users: 1, rate: 8000000, boost: false, blocked: false };
  RADIO = { driver: 'sdrplay', landingMap: 'avmar', landingModes: '', mapKeep: false };
  P.mapLandRender();
  ok(/data-keep="aisdsc"/.test($('mapModeList').innerHTML) && /AIS \+ DSC \(channel 70\)/.test($('mapModeList').innerHTML), 'RSP at 8: "AIS + DSC (channel 70)" drawn under MARITIME');
  P.keepSetModes(['aisdsc']); P.mapLandRender();
  ok(RADIO.landingModes === 'aisdsc' && /data-keep="ais"[^>]* disabled/.test($('mapModeList').innerHTML), 'ticked and stored as aisdsc; AIS greyed beside it (one choice)');
  ok($('mapKeepLbl').textContent === 'Keep AIS + DSC (channel 70) running when nobody is connected', `keep names it: ${$('mapKeepLbl').textContent}`);
  ok(P.singleOptions(RADIO).some((o) => o.tok === 'aisdsc' && /MARITIME/.test(o.label)), 'single mode offers MARITIME: AIS + DSC');
  BLOCKS = new Set(['dsc']); P.mapLandRender();
  ok(/data-keep="aisdsc"[^>]* disabled/.test($('mapModeList').innerHTML) && /DSC is switched off/.test($('mapModeList').innerHTML), 'DSC switched off: greyed with the reason');
  BLOCKS = new Set();
  // the owner raises an RSP from 2 to 8 MS/s in the form before saving: the row appears (the server's minRate)
  P.setHW(withMar(2000000, false)); FORM = { users: 1, rate: 8000000, boost: false, blocked: false };
  RADIO = { driver: 'sdrplay', landingMap: 'avmar', landingModes: '', mapKeep: false };
  P.mapLandRender();
  ok(/data-keep="aisdsc"/.test($('mapModeList').innerHTML), 'the form raised to 8 MS/s: drawn before the save');
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

console.log('── ALL OFF keeps the kept-running mode on (b96-newmodes modeLandingId)');
{
  RADIO = { landingMap: 'avmar', landingModes: 'vdl2', mapKeep: true, demodMode: 'wfm' };
  ok(P.modeLandingId() === 'avdata', 'kept VDL2 → the aircraft datalink switch stays on');
  RADIO = { landingMap: 'amateur', landingModes: 'ism:868', mapKeep: true, demodMode: 'wfm' };
  ok(P.modeLandingId() === 'ism', 'kept ISM 868 → the ISM switch stays on');
  RADIO = { landingMap: 'avmar', landingModes: 'vor', demodMode: 'wfm' };
  ok(P.modeLandingId() === 'vormap', 'the VOR map landing → vormap');
  RADIO = { landingAdsb: true, demodMode: 'wfm' };
  ok(P.modeLandingId() === 'adsb', 'beta95 ADS-B as before');
  RADIO = { demodMode: 'wfm' };
  ok(P.modeLandingId() === 'wfm', 'no map: the demodulator landing');
}

console.log('── the SINGLE-MODE RADIO (Stuart 11-26)');
{
  // Paul's ADS-B-only radio: every demodulator and every other whole mode off by its ticks → shown as Single mode: ADS-B
  const paul = 'wfm,nfm,am,sam,sau,sal,ecssu,ecssl,amst,usb,lsb,cwu,cwl,drm,freedv,m17,dvsig,dab,ism,ndb,ais,avdata,sondes,vormap,sat';
  P.setHW(hwRtl(2400000)); FORM = { users: 1, rate: 2400000, boost: false, blocked: false }; BLOCKS = new Set();
  RADIO = { driver: 'rtlsdr', blockedModes: paul, singleMode: '' };
  ok(P.singleFromTicks(RADIO) === 'adsb', "★ Paul's ticks read as ADS-B");
  P.singleRender();
  ok(RADIO.singleMode === 'adsb' && $('radioUseSingle').checked && /single mode: ADS-B/.test($('singleNote').textContent)
     && /saved when you save/.test($('singleNote').textContent), '★ shown as Single mode: ADS-B on first open (derived, saved on save)');
  ok($('modeBlockList').style.opacity === '.4' && $('modeAllRow').style.display === 'none', 'the tick boxes greyed, ALL ON / OFF hidden');
  ok(RADIO.blockedModes === paul, 'the ticks themselves untouched underneath');
  ok(P.modeLandingId() === 'adsb', 'ALL OFF / the landing id follow the single mode');
  // the dropdown: only what THIS radio can do
  const opts = P.singleOptions(RADIO).map((o) => o.tok);
  ok(['adsb', 'vdl2', 'acars', 'ais', 'vor', 'ism:433', 'ism:868', 'dab', 'sondes', 'wfm', 'nfm', 'am', 'usb', 'lsb'].every((t) => opts.includes(t)),
     `the list for an RTL at 2.4: ${opts.join(' ')}`);
  ok(!opts.includes('drm') && !opts.includes('sat') && !opts.includes('ndb'), 'nothing it cannot do (DRM not in its caps, no NDB reach, no satellite location)');
  P.setHW({ ...hwRtl(2400000), ndbHw: true, singleSat: { ok: true } });
  ok(P.singleOptions(RADIO).some((o) => o.tok === 'sat') && P.singleOptions(RADIO).some((o) => o.tok === 'ndb'), 'SATELLITES and NDB where the server says so');
  P.setHW({ offline: false, mapRate: 912000, mapModes: [{ id: 'adsb', hw: false }, { id: 'vdl2', hw: true }, { id: 'acars', hw: true }, { id: 'vor', hw: false }, { id: 'ais', hw: true }, { id: 'ism', hw: false }] });
  const hf = P.singleOptions({ driver: 'airspyhf' }).map((o) => o.tok);
  ok(!hf.includes('adsb') && !hf.includes('vor') && hf.includes('vdl2'), 'an HF+: no ADS-B or VOR entry');
  // back to General: the ticks are what they were
  P.setHW(hwRtl(2400000));
  RADIO = { driver: 'rtlsdr', blockedModes: 'dab', singleMode: 'general' };
  P.singleRender();
  ok($('radioUseGeneral').checked && $('modeBlockList').style.opacity === '' && RADIO.blockedModes === 'dab', '★ General: the owner\'s ticks, live again');
  RADIO = { driver: 'rtlsdr', blockedModes: paul, singleMode: 'general' }; P.singleRender();
  ok(RADIO.singleMode === 'general', 'a chosen General is never re-derived from the ticks');
  RADIO = { driver: 'rtlsdr', singleMode: 'sat' }; P.setHW({ ...hwRtl(2400000), singleSat: { ok: true } }); P.singleRender();
  ok(/follows the satellite passes itself/.test($('singleNote').textContent), 'SATELLITES: the note says listeners watch');
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
ok(/landingMap: radio\(\)\.landingMap === "avmar" \|\| radio\(\)\.landingMap === "amateur" \? radio\(\)\.landingMap : ""/.test(src) && /landingModes: /.test(src) && /mapKeep: \(radio\(\)\.landingMap === "avmar"/.test(src), 'the save writes landingMap / landingModes / mapKeep (either map)');

console.log('── the AMATEUR map: ISM on 868 for MeshCore (Stuart 11-26)');
{
  P.setHW(hwRtl(2400000)); FORM = { users: 1, rate: 2400000, boost: false, blocked: false }; BLOCKS = new Set();
  RADIO = { driver: 'rtlsdr', landingMap: 'amateur' };
  P.keepSetModes(['ism']);
  ok(RADIO.landingModes === '' , 'ISM without a band is not stored');
  P.keepSetModes(['ism:868']);
  ok(RADIO.landingModes === 'ism:868' && !RADIO.landingAdsb, `ISM 868 stored as its token: ${RADIO.landingModes}`);
  RADIO.mapKeep = true; P.keepMirror(RADIO); P.mapLandRender();
  const h = $('mapModeList').innerHTML;
  ok(/data-keep="ism" checked/.test(h) && !/data-keep="vdl2"/.test(h), 'the Amateur map draws ISM only (ticked)');
  ok(/select data-keepband="ism"/.test(h) && /value="868" selected/.test(h) && /value="433"/.test(h), "the band from ISM's own plan (this radio's), 868 selected");
  ok(/MeshCore, Meshtastic, LoRaWAN/.test(h) && /about 11 % of one core/.test(h), 'grouped with the LoRa words, priced by dec_ism');
  ok($('mapKeepLbl').textContent === 'Keep ISM 868 MHz SRD running when nobody is connected', $('mapKeepLbl').textContent);
  ok(/ISM 868 MHz SRD pauses for the pass/.test(P.keepSatWords(RADIO)), 'the satellites words: ISM pauses for a pass');
  BLOCKS = new Set(['ism']); P.mapLandRender();
  ok(/data-keep="ism"[^>]* disabled/.test($('mapModeList').innerHTML), 'ISM switched off: greyed, said');
  BLOCKS = new Set();
  RADIO = { driver: 'rtlsdr', landingMap: 'avmar' }; P.keepSetModes(['ism:868']);
  ok(RADIO.landingModes === '', 'ISM is not an Aviation & Maritime mode');
  ok(P.keepParse('ism:868,ism:433,vdl2').join() === 'vdl2,ism:868', 'tokens in table order, one ISM band');
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
