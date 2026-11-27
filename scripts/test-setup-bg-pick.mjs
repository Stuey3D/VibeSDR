// ★★★ b96-wholeradio-pick BACKGROUND DECODING → "A whole-radio mode": PICKING A MODE (Stuart 11-27, recorded on the Pi 500's V4L
// RTL at 2.4 MS/s). The page's own functions (bgWholeRender, bgSetWhole, bgKindInit, keepGate …) lifted out of vibe_setup_page.h
// and run in jsdom on the card's real markup, so the inputs are clicked the way a person clicks them.
//   The fault: with ADS-B ticked, unticking it flipped the card to Manual; choosing "A whole-radio mode" again brought ADS-B
//   back; every other mode stayed greyed "ADS-B is ticked" — ADS-B → ISM was impossible on a one-mode radio.
//   Now: a radio that runs one mode at a time draws RADIO BUTTONS (picking ISM switches straight to it, nothing greyed for being
//   ticked, one line above the list); a wide radio keeps checkboxes, where a new tick replaces what it cannot run beside and the
//   last tick cannot be taken off (Manual, above, is how it stops — the card says so). The stored value (bgMode) is unchanged.
// Silent, no browser. Run: node --max-old-space-size=512 scripts/test-setup-bg-pick.mjs [--html <dir>] (the card drawn, for screenshots)
import fs from 'node:fs';
import path from 'node:path';
import { JSDOM } from 'jsdom';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const htmlDir = process.argv.includes('--html') ? process.argv[process.argv.indexOf('--html') + 1] : '';
let fail = 0, pass = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };
const grab = (name) => {
  const m = src.match(new RegExp(`\\nfunction ${name}\\([^)]*\\) \\{[^\\n]*\\}\\n`)) || src.match(new RegExp(`\\nfunction ${name}\\([^)]*\\) \\{[\\s\\S]*?\\n\\}\\n`));
  if (!m) { console.log(`   FAIL ${name} not found in the page`); process.exit(1); }
  return m[0];
};
const constOf = (name) => {
  const m = src.match(new RegExp(`\\nconst ${name} = (\\[[\\s\\S]*?\\];|[^\\n]*)`));
  if (!m) { console.log(`FAIL const ${name}`); process.exit(1); }
  return m[0] + '\n';
};
// ★ the card's own markup and the page's own style
const page = src.match(/kVibeSetupPage = R"HTML\(([\s\S]*?)\)HTML"/)[1];
const style = page.match(/<style>[\s\S]*?<\/style>/)[0];
const cardStart = page.indexOf('<div class="card" id="bgCard">');
const cardEnd = page.indexOf('<div id="hwBg"', cardStart);
const cardHtml = page.slice(cardStart, cardEnd) + '<div id="hwBg"></div><p id="releaseKeepNote" class="hide"></p></div>';

const fnNames = ['esc', 'singleWords', 'keepMapOf', 'keepBase', 'keepTokName', 'keepParse', 'keepIsCombo', 'keepRateWords', 'keepGate', 'keepCostWords',
                 'keepFacts', 'keepSatWords', 'keepMirror', 'keepMigrate', 'adsbLandOffer', 'keepBgNormal', 'keepIsmPlan', 'keepIsmDefault',
                 'keepIsmSelect', 'bgWholeRender', 'bgSetWhole', 'bgKindInit'];
const body = constOf('KEEP_MODES') + constOf('KEEP_GROUP_WORDS') + constOf('KEEP_COMBO_MIN') + constOf('KEEP_MARITIME_MIN') + constOf('ADSB_MIN_RATE')
  + fnNames.map(grab).join('\n');

const hw = (rate, combo) => ({ offline: false, adsbHw: true, mapRate: rate,
  mapModes: [{ id: 'adsb', hw: true, why: '', pct: 13.2 }, { id: 'vdl2', hw: true, why: '', pct: 9.4 }, { id: 'acars', hw: true, why: '', pct: 3 },
             { id: 'vor', hw: true, why: '', pct: 20 }, { id: 'ais', hw: true, why: '', pct: 2 }, { id: 'ism', hw: true, why: '', pct: 11 }],
  mapCombo: { minRate: 6083334, ok: combo, why: '' },
  ismRegion: 'Europe', ismPlan: [{ id: '433', name: '433.92 MHz', region: 'Europe', own: true }, { id: '868', name: '868 MHz SRD', region: 'Europe', own: true },
                                 { id: '915', name: '915 MHz', region: 'Americas', own: false }] });

function card(H, rate, radio) {
  const dom = new JSDOM(`<!doctype html><html><head>${style}</head><body style="padding:16px;max-width:560px">${cardHtml}</body></html>`);
  const { document, Event } = dom.window;
  let RADIO = radio;
  const env = { $: (id) => document.getElementById(id), radio: () => RADIO, HW: H, modeBlockSet: () => new Set(),
                adsbLandForm: () => ({ users: 1, rate, boost: false, blocked: false }), bgIsmBands: () => [], capMode: () => true };
  const P = new Function('env', `
    let { $, radio, HW, modeBlockSet, adsbLandForm, bgIsmBands, capMode } = env;
    let spRender = () => {};
    let dabLandRender = () => bgWholeRender();   // ★ the page's dabLandRender → mapLandRender → bgWholeRender
    ${body}
    return { ${fnNames.join(', ')} };`)(env);
  P.bgKindInit(); P.bgWholeRender();
  const $ = (id) => document.getElementById(id);
  const input = (id) => document.querySelector(`#bgModeList input[data-bgmode="${id}"]`);
  const click = (el) => { el.checked = el.type === 'radio' ? true : !el.checked; el.dispatchEvent(new Event('change')); };
  const rowWhy = (id) => { const i = input(id); return i ? [...i.closest('label').querySelectorAll('.hint')].pop()?.textContent || '' : ''; };
  return { P, $, input, click, rowWhy, r: () => RADIO, document, save: (name) => {
    if (!htmlDir) return;
    fs.mkdirSync(htmlDir, { recursive: true });
    fs.writeFileSync(path.join(htmlDir, name), dom.serialize());
  } };
}

console.log('── Stuart\'s recording: an RTL at 2.4 MS/s, background ADS-B, change it to ISM');
{
  const c = card(hw(2400000, false), 2400000, { driver: 'rtlsdr', bgMode: 'adsb', landingMap: '', landingModes: '' });
  c.save(`${process.env.SHOT_TAG || 'card'}-rtl-adsb.html`);
  ok(c.$('bgKindWhole').checked && c.input('adsb')?.checked, 'whole-radio mode, ADS-B chosen');
  ok(!/is ticked/.test(c.$('bgModeList').textContent), 'no row is greyed for something else being ticked');
  ok(c.input('ism') && !c.input('ism').disabled && !c.input('vdl2').disabled, 'ISM and VDL2 can be picked straight away');
  ok(c.input('adsb').type === 'radio', 'a one-mode radio draws radio buttons');
  ok(/2\.4 MS\/s: this radio runs one of these at a time/.test(c.$('bgRateNote').textContent), `one line above the list: ${c.$('bgRateNote').textContent}`);
  // the recorded flow: untick ADS-B (on the old page a checkbox; on the new a radio cannot be unticked — click ISM instead)
  if (c.input('adsb').type === 'checkbox') c.click(c.input('adsb'));
  ok(c.$('bgKindWhole').checked && !c.$('bgWholeBox').classList.contains('hide'), 'the card stays on "A whole-radio mode" (never flips to Manual)');
  ok(c.input('ism') && !c.input('ism').disabled, 'ISM can be picked');
  if (c.input('ism') && !c.input('ism').disabled) c.click(c.input('ism'));
  ok(c.r().bgMode === 'ism:433', `picking ISM stores ISM on the region's own band: "${c.r().bgMode}"`);
  ok(c.input('ism')?.checked && !c.input('adsb')?.checked, 'ISM chosen, ADS-B not');
  const band = c.document.querySelector('#bgModeList select[data-bgband]');
  ok(!!band && band.value === '433', 'ISM\'s band choice appears under ISM');
  if (band) { band.value = '868'; band.dispatchEvent(new c.document.defaultView.Event('change')); }
  ok(c.r().bgMode === 'ism:868', `the band changes the stored token: "${c.r().bgMode}"`);
  ok(/ISM 868 MHz SRD is kept running/.test(c.$('bgSatNote').textContent) && !/ADS-B/.test(c.$('bgSatNote').textContent),
     `the satellites line names the mode picked: ${c.$('bgSatNote').textContent.slice(0, 60)}…`);
  ok(/ISM 868 MHz SRD runs on this radio/.test(c.$('releaseKeepNote').textContent), 'the hand-off line names it too');
  ok(!c.r().mapKeep && !c.r().adsbKeepRunning, 'the mirrors follow (no ADS-B pair)');
  c.save(`${process.env.SHOT_TAG || 'card'}-rtl-ism.html`);
  if (c.input('adsb')) c.click(c.input('adsb'));
  ok(c.r().bgMode === 'adsb' && /ADS-B pauses for the pass/.test(c.$('bgSatNote').textContent), 'and back to ADS-B: stored, the satellites line says ADS-B');
  if (c.input('vor')) c.click(c.input('vor'));
  ok(c.r().bgMode === 'vor', `…then the VOR map: "${c.r().bgMode}"`);
  if (c.input('acars')) c.click(c.input('acars'));
  ok(c.r().bgMode === 'acars', `ACARS at 2.4 MS/s is one choice like the rest (no pair): "${c.r().bgMode}"`);
}

console.log('── a row greyed for a REAL reason stays greyed, with it');
{
  const H = hw(2400000, false); H.mapModes.find((m) => m.id === 'vor').why = 'not enough CPU for the VOR map on this server';
  const c = card(H, 2400000, { driver: 'rtlsdr', bgMode: 'adsb' });
  ok(c.input('vor')?.disabled && /not enough CPU/.test(c.rowWhy('vor')), `VOR greyed: ${c.rowWhy('vor')}`);
  ok(!c.input('ism')?.disabled, 'ISM still free');
}

console.log('── a wide radio (8 MS/s, ACARS + VDL2 together): checkboxes');
{
  const c = card(hw(8000000, true), 8000000, { driver: 'sdrplay', bgMode: 'vdl2' });
  c.save(`${process.env.SHOT_TAG || 'card'}-rsp-vdl2.html`);
  ok(c.input('vdl2')?.type === 'checkbox' && c.input('vdl2').checked, 'checkboxes, VDL2 ticked');
  c.click(c.input('acars'));
  ok(c.r().bgMode === 'vdl2,acars' && c.input('acars').checked && c.input('vdl2').checked, `ticking ACARS adds it: "${c.r().bgMode}"`);
  ok(c.input('ism') && !c.input('ism').disabled, 'ISM can still be ticked beside the pair');
  if (c.input('ism') && !c.input('ism').disabled) c.click(c.input('ism'));
  ok(c.r().bgMode === 'ism:433' && c.$('bgKindWhole').checked, `ticking ISM replaces the pair (what fits): "${c.r().bgMode}"`);
  c.click(c.input('ism'));   // untick the last one
  ok(c.$('bgKindWhole').checked && !c.$('bgWholeBox').classList.contains('hide') && c.r().bgMode === 'ism:433',
     'unticking the last mode does not flip to Manual — it stays ticked');
  ok(/Manual/.test(c.$('bgRateNote').textContent), `…and the card says Manual (above) is how it stops: ${c.$('bgRateNote').textContent}`);
}

console.log('── Manual and back');
{
  const c = card(hw(2400000, false), 2400000, { driver: 'rtlsdr', bgMode: 'ais' });
  c.click(c.$('bgKindManual'));
  ok(c.r().bgMode === '' && c.$('bgWholeBox').classList.contains('hide'), 'Manual: stored as ""');
  c.click(c.$('bgKindWhole'));
  ok(c.r().bgMode !== '' && c.$('bgKindWhole').checked, `whole-radio again: the first mode it can run ("${c.r().bgMode}"), and it can be changed`);
  c.click(c.input('ism'));
  ok(c.r().bgMode === 'ism:433', 'changed to ISM');
}

// ★ the save writes bgMode as before (the same field, the same normalised value)
ok(/bgMode: keepBgNormal\(radio\(\)\.bgMode\)/.test(src), 'the save payload: bgMode: keepBgNormal(radio().bgMode), unchanged');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
