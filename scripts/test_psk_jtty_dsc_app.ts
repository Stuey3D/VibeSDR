/**
 * The app's PSK, JTTY and DSC boxes (src/services/cwList.ts PSK rules, jtty.ts, dsc.ts) against the web client's —
 * ONE RULE, TWO READERS (V12 app parity, 2026-10-26):
 *   • JTTY and DSC: the web's own modules (web/client/src/jtty.ts, dsc.ts) run on the same inputs — dials / channels,
 *     the sanitisers, the order, the spot / call merges, where a tap or a key tunes, the attach parameters;
 *   • PSK: the web's cwTune / passbandCentreHz / pskHint / decoders.ts parse, cut out of main.ts and decoders.ts;
 *   • the hints read out of web/client/index.html, the DSC colours out of its CSS;
 *   • the reach rule (utils/tuneWindow hzReachable) that decides which channel / band keys are drawn.
 *
 * Run: npx tsx scripts/test_psk_jtty_dsc_app.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import {
  parseCwSignals, parseCwSpot, pskWidthOf, passbandCentreHz, pskTuneFor, pskHintText, kbIdleText, kbRateText, kbSpotRateText,
  PSK_WIDTHS, PSK_DEFAULT_WIDTH,
} from '../src/services/cwList';
import * as AJ from '../src/services/jtty';
import * as AD from '../src/services/dsc';
import * as WJ from '../web/client/src/jtty';
import * as WD from '../web/client/src/dsc';
import { hzReachable, parseTunable } from '../src/utils/tuneWindow';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const ok = (what: string, c: boolean) => eq(what, c, true);
const main = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
const html = readFileSync(new URL('../web/client/index.html', import.meta.url), 'utf8');
/** A hint's words from the page: tags dropped, whitespace collapsed. */
const words = (s: string) => s.replace(/<[^>]+>/g, '').replace(/\s+/g, ' ').trim();
const hintFromHtml = (id: string) => {
  const m = html.match(new RegExp(`<div class="hint" id="${id}">([\\s\\S]*?)</div>`));
  return m ? words(m[1]) : null;
};

// ── PSK ──────────────────────────────────────────────────────────────────────
{
  eq('PSK widths are the web page\'s', [...PSK_WIDTHS], [...html.matchAll(/data-pskwidth="(\d+)"/g)].map((m) => Number(m[1])));
  ok('PSK default width is the web\'s', new RegExp(`let pskWidth = ${PSK_DEFAULT_WIDTH};`).test(main));
  eq('pskWidthOf keeps a known width', pskWidthOf('24000'), 24000);
  eq('pskWidthOf refuses an unknown one', pskWidthOf(48000), 12000);

  // The web's passbandCentreHz and cwTune (PSK branch), cut out of main.ts and run against the app's on a grid of cases.
  const pc = main.match(/function passbandCentreHz\(\): number \{\n  if \(!spec\) return 1500;\n([\s\S]*?)\n\}/);
  const tu = main.match(/  if \(activeDec === 'psk'\) \{\n    const lsb = spec\.mode === 'lsb', c = (lsb \|\| spec\.mode === 'usb' \? passbandCentreHz\(\) : 1500);\n    spec\.tune\(clampTune\(Math\.round\((lsb \? hz \+ c : hz - c)\)\), (lsb \? 'lsb' : 'usb')/);
  if (!pc || !tu) { fails++; console.error('FAIL the web\'s passbandCentreHz / PSK cwTune were not found in main.ts'); }
  else {
    const webCentre = new Function('spec', pc[1].replace(/\n/g, ' ')) as (spec: { bandwidthLow: number; bandwidthHigh: number }) => number;
    const cases: Array<[number, number]> = [[300, 2700], [200, 2800], [-2700, -300], [50, 450], [-4000, 4000], [0, 3000], [1000, 2000]];
    for (const [lo, hi] of cases) eq(`passband centre ${lo}..${hi}`, passbandCentreHz(lo, hi), webCentre({ bandwidthLow: lo, bandwidthHigh: hi }));
    for (const mode of ['usb', 'lsb', 'cwu', 'am']) for (const [lo, hi] of cases.slice(0, 3)) {
      const spec = { mode, bandwidthLow: lo, bandwidthHigh: hi };
      const passbandCentre = () => webCentre(spec);
      const c = new Function('spec', 'passbandCentreHz', `const lsb = spec.mode === 'lsb'; return ${tu[1]};`)(spec, passbandCentre) as number;
      const hz = 14_070_850;
      const lsb = mode === 'lsb';
      const want = { hz: Math.round(lsb ? hz + c : hz - c), mode: lsb ? 'lsb' : 'usb' };
      eq(`PSK tap in ${mode} ${lo}..${hi}`, pskTuneFor(hz, mode, lo, hi), want);
    }
  }
  // The hints — the web's pskHint (bold dropped).
  const ph = main.match(/\$\('pskHint'\)\.innerHTML = pskSkim\n      \? (`[\s\S]*?`\n        \+ '[\s\S]*?')\n      : ('[\s\S]*?'\n        \+ '[\s\S]*?');/);
  if (!ph) { fails++; console.error('FAIL the web\'s pskHint was not found in main.ts'); }
  else for (const w of PSK_WIDTHS) {
    const pskWidth = w;
    const skim = words(new Function('pskWidth', `return ${ph[1]};`)(pskWidth) as string);
    const one = words(new Function(`return ${ph[2]};`)() as string);
    eq(`PSK skimmer hint at ${w}`, pskHintText(true, w), skim);
    eq('PSK one-signal hint', pskHintText(false, w), one);
  }
  // The idle lines (renderCw).
  eq('idle: PSK skimmer', kbIdleText(true, true), 'listening across the segment — each PSK signal appears here as it is decoded');
  eq('idle: PSK one signal', kbIdleText(true, false), 'listening — every PSK signal in your passband appears here; the one in the middle is decoded above');
  eq('idle: CW one signal', kbIdleText(false, false), 'listening — tune a CW signal into the passband');
  ok('the idle lines are the web\'s', main.includes("'listening — every PSK signal in your passband appears here; the one in the middle is decoded above'"));

  // The wire: decoders.ts reads baud > 40 as 63 and keeps IMD; a spot's mode BPSK63 or BPSK31.
  const l = parseCwSignals({ skimmer: true, signals: [
    { id: 1, hz: 14_071_000, audio: 0, baud: 62.5, imd: -24, snr: 9, q: 'high', active: true, last: 1, first: 1, call: 'G4ABC', country: 'England', text: 'cq cq' },
    { id: 2, hz: 14_071_300, audio: 0, baud: 31.25, snr: 4, q: 'weird', active: false, text: 7 },
  ] }, true)!;
  eq('PSK63 read as 63', l.signals[0].baud, 63);
  eq('PSK31 read as 31', l.signals[1].baud, 31);
  eq('IMD kept', l.signals[0].imd, -24);
  eq('an unknown quality is poor', l.signals[1].q, 'poor');
  eq('a non-string text is empty', l.signals[1].text, '');
  eq('a CW list carries no baud', parseCwSignals({ signals: [{ id: 1 }] })!.signals[0].baud, undefined);
  eq('rate column PSK', kbRateText(l.signals[0]), 'PSK63');
  eq('rate column CW', kbRateText({ wpm: 18.4 }), '18 wpm');
  eq('spot BPSK63', parseCwSpot({ callsign: 'G4ABC', mode: 'BPSK63' }, 5, true)!.mode, 'BPSK63');
  eq('spot of an unknown mode is BPSK31', parseCwSpot({ callsign: 'G4ABC', mode: 'QPSK31' }, 5, true)!.mode, 'BPSK31');
  eq('spot rate column', kbSpotRateText({ mode: 'BPSK31', wpm: 0 }), 'PSK31');
  eq('a CW spot has no mode', parseCwSpot({ callsign: 'G4ABC' }, 5)!.mode, undefined);
  // The web's decoders.ts: the same rule (baud > 40 → 63), cut out and checked to be there.
  const dec = readFileSync(new URL('../web/client/src/decoders.ts', import.meta.url), 'utf8');
  ok('decoders.ts reads baud > 40 as 63', dec.includes("baud: num(x?.baud) > 40 ? 63 : 31, imd: num(x?.imd)"));
  ok('decoders.ts spot mode rule', dec.includes("mode: d.mode === 'BPSK63' ? 'BPSK63' : 'BPSK31'"));
  ok('the web attaches PSK with centre / width', main.includes("if (mode === 'psk') return pskSkim ? { width: pskWidth } : { centre: passbandCentreHz() };"));
}

// ── JTTY: the web's module and the app's on the same inputs ───────────────────
{
  eq('JTTY dials', AJ.JTTY_DIALS, WJ.JTTY_DIALS);
  const raw = { signals: [
    { id: 4, hz: 14_091_600, audio: 1600, snr: -3, q: 'medium', active: true, last: 10, first: 1, frames: 4, call: 'G4ABC', country: 'England', text: 'CQ G4ABC IO92' },
    { id: 2, hz: 14_090_900, audio: 900, snr: 7, q: 'nope', active: false, last: 9, first: 2, frames: 1, call: '', country: '', text: 'x'.repeat(700) },
    null, 'junk', { id: 9, hz: 14_091_600, audio: 1600, snr: 0, q: 'high' },
  ] };
  eq('JTTY signals parse', AJ.parseJttySignals(raw), WJ.jttySignalsFromJson(raw));
  eq('JTTY not a list', AJ.parseJttySignals({}), WJ.jttySignalsFromJson({}));
  const list = AJ.parseJttySignals(raw)!;
  eq('JTTY order', AJ.sortJtty(list).map((s) => s.id), WJ.sortJtty(list as any).map((s) => s.id));
  for (const d of [{ callsign: 'G4ABC', frequency: 14091600, how: 'CQ', timestamp: 5 }, { callsign: 'EA1X', how: 'QSO', snr: 'x' }, { callsign: '' }, null])
    eq(`JTTY spot ${JSON.stringify(d)}`, AJ.parseJttySpot(d, 77), d && (d as any).timestamp ? WJ.jttySpotFromJson(d) : (() => {
      const w = WJ.jttySpotFromJson(d); return w ? { ...w, timestamp: (d as any).timestamp || 77 } : w; })());
  // The spot merge: a sequence through the web's JttySpots class and the app's mergeJttySpot.
  const seq = [
    { callsign: 'G4ABC', frequency: 1, audio: 0, snr: 0, how: 'CQ' as const, country: '', timestamp: 1 },
    { callsign: 'EA1X', frequency: 2, audio: 0, snr: 0, how: 'QSO' as const, country: '', timestamp: 2 },
    { callsign: 'G4ABC', frequency: 3, audio: 0, snr: 0, how: 'QSO' as const, country: '', timestamp: 3 },
    { callsign: 'EA1X', frequency: 4, audio: 0, snr: 0, how: 'CQ' as const, country: '', timestamp: 4 },
  ];
  const web = new WJ.JttySpots();
  let app: AJ.JttySpot[] = [];
  for (const s of seq) { web.push({ ...s }); app = AJ.mergeJttySpot(app, { ...s }); }
  eq('JTTY spot merge (a CQ stays a CQ)', app, web.list);
  for (const hz of [14_091_600, 7_091_500.4, 0]) eq(`JTTY tune ${hz}`, AJ.jttyTuneFor(hz), WJ.jttyTuneFor(hz));
  eq('JTTY hint is the page\'s', AJ.JTTY_HINT, hintFromHtml('jttyHint'));
  ok('JTTY idle rows are the web\'s', readFileSync(new URL('../web/client/src/jtty.ts', import.meta.url), 'utf8').includes(`'${AJ.JTTY_IDLE_ROWS}'`));
  ok('JTTY idle spots are the web\'s', readFileSync(new URL('../web/client/src/jtty.ts', import.meta.url), 'utf8').includes(`'${AJ.JTTY_IDLE_SPOTS}'`));
  // Reach: an HF+ (to 31 MHz) draws no 6 m / 2 m key; an RTL in direct sampling draws them all.
  const hf = (hz: number) => hzReachable(hz, [10_000, 31_000_000], parseTunable([[10_000, 31_000_000]]));
  eq('JTTY dials on an HF radio', AJ.jttyDialsReachable(hf).map((d) => d.label), ['160m', '80m', '40m', '30m', '20m', '17m', '15m', '12m', '10m']);
  eq('JTTY dials on a wide radio', AJ.jttyDialsReachable((hz) => hzReachable(hz, [500_000, 1_766_000_000], [])).length, 11);
}

// ── DSC: the web's module and the app's on the same inputs ────────────────────
{
  eq('DSC channels', AD.DSC_CHANNELS, WD.DSC_CHANNELS);
  for (const ch of WD.DSC_CHANNELS) eq(`DSC tune ${ch.label}`, AD.dscTuneFor(ch), WD.dscTuneFor(ch));
  const dials: Array<[number, string]> = [
    [2_185_800, 'usb'], [2_186_000, 'usb'], [8_412_800, 'usb'], [8_414_500, 'usb'], [8_416_200, 'lsb'], [156_525_000, 'nfm'],
    [156_530_000, 'usb'], [14_070_000, 'usb'], [4_205_800, 'USB'], [16_802_000, 'usb'], [12_575_300, ''], [2_187_500, 'fm'],
  ];
  for (const [hz, m] of dials) eq(`DSC params ${hz} ${m}`, AD.dscParams(hz, m), WD.dscParams(hz, m));
  ok('the web attaches DSC with dscParams', main.includes("if (mode === 'dsc') return dscParams(spec?.frequency ?? 0, String(spec?.mode ?? ''));"));
  const call = { id: 7, utc: 1_700_000_000_000, rf: 8_414_500, repeat: 3, format: 'Distress alert', category: 'distress', level: 3,
    self: '235012345678', selfCountry: 'United Kingdom', lat: 50.2, lon: -4.16, posText: '50°12\'N 004°10\'W', check: 'corrected',
    quality: 0.93, band: 'hf', line: 'DISTRESS · MMSI 235012345 (United Kingdom) · Sinking', freqs: ['8414.5', 1, 'x', 'y', 'z'] };
  eq('DSC call parse', AD.parseDscCall(call, 0), WD.dscFromJson(call));
  const bad = { ...call, lat: 95, lon: 'x', level: 9, repeat: 0, check: 'odd', band: 'vhf' };
  eq('DSC call parse (bad fields)', AD.parseDscCall(bad, 0), WD.dscFromJson(bad));
  eq('DSC not a call', AD.parseDscCall({ id: 1 }, 0), WD.dscFromJson({ id: 1 }));
  // The merge: a repeat updates its row and moves it to the top, as the web's DscCalls.
  const web = new WD.DscCalls();
  let app: AD.DscCall[] = [];
  for (const [id, rf, rep] of [[1, 8414500, 1], [2, 2187500, 1], [1, 8414500, 2], [1, 2187500, 1], [2, 2187500, 5]] as const) {
    const c = AD.parseDscCall({ ...call, id, rf, repeat: rep }, 0)!;
    web.push({ ...c } as any); app = AD.mergeDscCall(app, { ...c });
  }
  eq('DSC call merge', app.map((c) => [c.id, c.rf, c.repeat]), web.list.map((c) => [c.id, c.rf, c.repeat]));
  for (const hz of [8_414_500, 156_525_000, 0, 2_187_500]) eq(`DSC freq text ${hz}`, AD.dscFreqText(hz), WD.dscFreqText(hz));
  eq('DSC hint is the page\'s', AD.DSC_HINT, hintFromHtml('dscHint'));
  ok('DSC idle is the web\'s', main.includes(AD.DSC_IDLE.slice(0, 60)));
  // The colours, out of the page's CSS.
  const css = (sel: string, prop: string) => {
    const m = html.match(new RegExp(`#dscList \\.dscr\\.${sel}( \\.l)? \\{[^}]*?${prop}: ([^;]+);`));
    return m ? m[2].trim() : null;
  };
  eq('DSC distress rule', AD.DSC_LEVEL_BORDER[3], html.match(/#dscList \.dscr\.distress \{ border-left-color: (#[0-9A-F]+)/i)?.[1]);
  eq('DSC urgency rule', AD.DSC_LEVEL_BORDER[2], html.match(/#dscList \.dscr\.urgency \{ border-left-color: (#[0-9A-F]+)/i)?.[1]);
  eq('DSC safety rule', AD.DSC_LEVEL_BORDER[1], html.match(/#dscList \.dscr\.safety \{ border-left-color: (#[0-9A-F]+)/i)?.[1]);
  eq('DSC distress text', AD.DSC_LEVEL_TEXT[3], html.match(/#dscList \.dscr\.distress \.l \{ color: (#[0-9A-F]+)/i)?.[1]);
  eq('DSC urgency text', AD.DSC_LEVEL_TEXT[2], html.match(/#dscList \.dscr\.urgency \.l \{ color: (#[0-9A-F]+)/i)?.[1]);
  eq('DSC distress wash', AD.DSC_LEVEL_BG[3], html.match(/#dscList \.dscr\.distress \{[^}]*background: (rgba\([^)]+\))/)?.[1]?.replace(/\s/g, ''));
  eq('DSC urgency wash', AD.DSC_LEVEL_BG[2], html.match(/#dscList \.dscr\.urgency \{[^}]*background: (rgba\([^)]+\))/)?.[1]?.replace(/\s/g, ''));
  void css;
  // Reach: an HF+ draws the six HF channels, never Ch 70; a V4 locked to 8.0–8.8 MHz only 8414.5.
  eq('DSC channels on an HF radio', AD.dscChannelsReachable((hz) => hzReachable(hz, [10_000, 31_000_000], [])).map((c) => c.label),
     ['2187.5', '4207.5', '6312', '8414.5', '12577', '16804.5']);
  eq('DSC channels in a locked window', AD.dscChannelsReachable((hz) => hzReachable(hz, [8_000_000, 8_800_000], [])).map((c) => c.label), ['8414.5']);
  eq('DSC channels with the owner\'s gaps', AD.dscChannelsReachable((hz) => hzReachable(hz, [0, 2e9], [[0, 5e6], [150e6, 160e6]])).map((c) => c.label),
     ['2187.5', '4207.5', 'Ch 70']);
}

// ── The reach rule's parser ───────────────────────────────────────────────────
eq('tunable parsed', parseTunable([[500000, 1766000000], [5, 1], ['a', 3], [1, 2, 3], null]), [[500000, 1766000000], [1, 2]]);
eq('tunable absent', parseTunable(undefined), []);

console.log(`test_psk_jtty_dsc_app: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
