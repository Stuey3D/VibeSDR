// check-icom-app-gates.mjs — the APP hides every IQ-only control on an Icom and draws the radio's own instead
// (docs/ICOM-SUPPORT-PLAN.md "Client controls that do NOT apply on an Icom", AGENTS.md "a control that only works
// on one radio should not be there"). A static check of the gates in the source: each one is a single expression a
// later edit could quietly drop, and the app cannot be driven headless here. Pure logic is tested by
// scripts/test_icom_radio.ts; the web page by scripts/test-web-icom.mjs.
import fs from 'node:fs';
const read = (p) => fs.readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');
const S = read('src/screens/SDRScreen.tsx');
const L = read('src/components/LocalHardwarePanel.tsx');
const M = read('src/components/MenuSheet.tsx');
const W = read('src/services/VibeServerWsClient.ts');
let pass = 0, fail = 0;
const has = (src, needle, what) => { const ok = src.includes(needle); ok ? pass++ : fail++; console.log(`   ${ok ? 'ok  ' : 'FAIL'} ${what}`); };
const count = (src, needle, n, what) => { const c = src.split(needle).length - 1; const ok = c === n; ok ? pass++ : fail++; console.log(`   ${ok ? 'ok  ' : 'FAIL'} ${what} (${c}/${n})`); };

console.log('Icom gates in the app source');
// ── hidden ──
has(S, 'onLocalSquelch={isLocal && !icom ? onLocalSquelch', 'our squelch (audio sheet)');
has(S, 'onLocalSquelch={isLocal && !icom ? onLocalSquelch : undefined}\n', 'our squelch (menu)');
count(S, '={vibeFmDsp && !icom ? askWrap(', 6, 'FM DSP: NR, IMS, auto bandwidth, CEQ, our NB, NBX');
has(S, 'onNfmVoice={nfmVoiceKnown && !icom', 'NFM voice');
has(S, 'onDeemph={icom ? undefined', 'de-emphasis');
has(S, 'onStereo={icom ? undefined', 'stereo');
has(S, '&& !dabOn && !icom ? onBinaural', 'binaural');
has(S, "rawIqMode !== 'off' && !sharedDial && !icom ?", 'raw IQ out');
has(S, 'onLinkMode={icom ? undefined', 'link mode / fps');
has(S, 'onToggleVfoLock={icom ? undefined', 'VFO lock');
has(M, '{!!onToggleVfoLock && <BtnRow>', 'the menu honours an absent VFO lock');
has(S, '!controlsHidden && !icom', 'centre-on-VFO button');
has(S, "icomModes.has(String(m.id).toLowerCase())", 'mode list = what the radio has (no WFM)');
has(S, "wfScroll={icom ? 'sharp'", 'no interpolated waterfall rows');
has(S, 'specSmoothing={icom ? 1', 'no temporal spectrum smoothing');
has(S, 'spatialSmooth={icom ? false', 'no spatial smoothing');
has(S, 'onSpecSmoothing={icom ? undefined', 'smoothing control not offered');
// ── the panel: an Icom branch FIRST, and the whole IQ body behind it ──
const iIcom = L.indexOf('{p.icom ? (');
const iGain = L.indexOf('GAIN IS NOT ONE CONTROL ACROSS RADIOS', iIcom > 0 ? iIcom : 0);
const iPpm = L.indexOf('FREQUENCY CORRECTION (PPM)');
const ok = iIcom > 0 && iGain > iIcom && iPpm > iIcom; ok ? pass++ : fail++;
console.log(`   ${ok ? 'ok  ' : 'FAIL'} the panel's Icom branch comes before the gain table, ppm, bias-T, DS, rate (else means dongle)`);
has(S, 'icom={icom}', 'SDRScreen hands the panel the Icom');
// ── drawn ──
for (const k of ['send({ mode: m })', 'send({ fil: f })', 'send({ span: sp })', 'send({ scopeMode: m })', 'send({ preamp: v })',
                 'send({ att: v })', 'send({ agc: v })', 'send({ rfGain:', 'send({ scopeSpeed: v })'])
  has(L, k, `the panel sends ${k.replace(/send\(\{ |:.*$/g, '')}`);
has(L, "['nb', 'NB', icom.nb, icom.nbNow], ['nr', 'NR', icom.nr, icom.nrNow], ['anf', 'ANF'", "the radio's NB/NR/ANF");
has(S, '{!!icomBanner(icom) && (', 'the TX / link-down banner');
has(S, 'icomFollowNote(icom, adminOk)', 'the follow note for a listener');
has(S, '(icomRef.current ? icomDbmRef.current : null)', "the S-meter from the radio's status line");
// ── the wire ──
has(W, "msg.type === 'icom'", 'the status line is read');
has(W, '!this.sharedDial && !icomNow', 'no tune re-assert on an Icom');
has(W, 'nearestSpanIndex(spans, binBandwidth * n)', 'zoom lands on a span rung');
// ★★★ receive only: nothing in the app builds a transmit message.
for (const [name, src] of [['SDRScreen', S], ['panel', L], ['ws client', W]])
  { const bad = /type:\s*['"](ptt|tx|transmit)['"]|\bptt\s*:/i.test(src); !bad ? pass++ : fail++;
    console.log(`   ${!bad ? 'ok  ' : 'FAIL'} ${name}: no transmit message anywhere`); }
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
