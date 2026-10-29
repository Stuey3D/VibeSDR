/**
 * The app's DRM box (src/services/drm.ts drmView, drawn by DrmPanel) against the web client's (web/client/src/drm.ts
 * drmRender) — ONE RULE, TWO READERS (V12 app parity, 2026-10-26). Both run on the same reports; the web's HTML is reduced
 * to its text and must say exactly what the app's box says, in the same order. Also: the sanitiser, the lights, the SNR
 * quality, the capability (`modes` lists "drm"), and that the app offers DRM only through that.
 *
 * Run: npx tsx scripts/test_drm_app.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { drmSanitise, drmView, drmViewText, drmOffered, drmSignalRows, DRM_LIGHTS, type DrmState } from '../src/services/drm';
import { drmRender, drmSignalRows as webSignalRows, drmSignalRender } from '../web/client/src/drm';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const ok = (what: string, c: boolean) => eq(what, c, true);

/** The web box's text: tags dropped, entities read, whitespace gone (the comparison is of words and their order). */
function webText(s: DrmState | null): string {
  const host = { innerHTML: '' } as unknown as HTMLElement;
  drmRender(host, s);
  return host.innerHTML.replace(/<[^>]+>/g, '').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"')
    .replace(/&amp;/g, '&').replace(/\s+/g, '');
}
const appText = (s: DrmState | null) => drmViewText(drmView(s)).join('').replace(/\s+/g, '');

const RRI: DrmState = {
  type: 'drm', sync: true, freq: true, frame: true, fac: true, sdc: true, msc: true, locked: true,
  robustness: 'B', bwKhz: 10, mscQam: 16, sdcQam: 4, interleave: 'long', codeRate: 0.62, snr: 17.4, offsetHz: -3.25,
  serviceId: '3E1C2', label: 'RRI', language: 'English', country: 'ro', programme: 'News', text: 'Radio Romania International <live>',
  utc: '2026-10-26 14:32', codec: 'AAC', audioMode: 'mono', audioRate: 12000, bitrate: 16300, playable: true, decoder: 'FAAD2',
  facOk: 40, facBad: 2,
};
const cases: Array<[string, DrmState | null]> = [
  ['nothing yet', null],
  ['no timing', { type: 'drm' }],
  ['timing, no lock', { type: 'drm', sync: true, freq: true }],
  ['timing + label, no lock', { type: 'drm', sync: true, freq: true, frame: true, fac: true, sdc: true, label: 'Funklust' }],
  ['RRI playing via FAAD2', RRI],
  ['xHE-AAC not playable', { ...RRI, codec: 'xHE-AAC', audioRate: 24000, bitrate: 22100, playable: false, decoder: undefined,
                             why: 'This station uses xHE-AAC. This server was built without an xHE-AAC decoder, so the audio cannot be played.' }],
  ['locked, fair SNR, no codec yet', { type: 'drm', sync: true, freq: true, frame: true, fac: true, locked: true, robustness: 'A', snr: 11 }],
  ['locked, poor SNR, no meta', { type: 'drm', sync: true, freq: true, frame: true, fac: true, locked: true, snr: 4.2, offsetHz: 2 }],
  ['lang3 only', { ...RRI, language: undefined, lang3: 'deu', country: undefined, programme: undefined }],
];
for (const [what, s] of cases) eq(`box text: ${what}`, appText(s), webText(s));

// The quality bands (the web's: ≥ 15 good, ≥ 9 fair, else poor) and the lights' order.
eq('SNR good', drmView(RRI).sig?.q, 'good');
eq('SNR fair', drmView({ ...RRI, snr: 9 }).sig?.q, 'fair');
eq('SNR poor', drmView({ ...RRI, snr: 8.9 }).sig?.q, 'poor');
eq('lights in the order a receiver gains them', DRM_LIGHTS.map((l) => l[1]), ['Time', 'Freq', 'Frame', 'FAC', 'SDC', 'MSC']);
const web = readFileSync(new URL('../web/client/src/drm.ts', import.meta.url), 'utf8');
for (const [k, name, tip] of DRM_LIGHTS) ok(`the web's light ${name}`, web.includes(`['${String(k)}', '${name}', '${tip.replace(/'/g, "\\'")}']`));
eq('the why only when not playable', drmView({ ...RRI, why: 'x' }).why, undefined);

// The sanitiser: plain fields of the types read, control characters out, strings cut short.
const dirty = drmSanitise({ type: 'drm', sync: 1, locked: true, snr: 'x', label: 'R\u0007RI'.padEnd(200, 'x'), country: 'gbr', playable: 'yes',
                            bwKhz: 10, text: 7, why: 'w' });
eq('sanitised', dirty, { type: 'drm', locked: true, bwKhz: 10, label: ('R RI'.padEnd(200, 'x')).slice(0, 64), country: 'gb', why: 'w' });
eq('not a report', drmSanitise(null), null);

// The capability: only where the radio's vibeserver.json `modes` lists "drm".
ok('offered where listed', drmOffered(['nfm', 'am', 'drm']));
ok('not on an older server', !drmOffered(['nfm', 'am']) && !drmOffered(undefined));
const scr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
ok('the app offers DRM only through the capability', /const vibeModes = drmCapable \? withDrm\(LOCAL_MODES\) : LOCAL_MODES;/.test(scr));
ok('the width row goes in DRM', /onFilterBoth=\{status\.mode === 'drm' \? undefined : onFilterBoth\}/.test(scr));
ok('the shim lists drm in its modes', readFileSync(new URL('../android/app/src/main/cpp/local_sdr_shim.cpp', import.meta.url), 'utf8').includes('"jtty", "drm" }'));

// ── ★★ V12 THE SIGNAL PANE (2026-10-26): the app's rows ARE the web's — same headings, labels, values, tones, tooltips ──
const NEW: DrmState = {
  ...RRI, so: 3, sdcQam: 16, codeRateA: 0.5, sdcRate: 0.5, mscMer: 13.1, sdcMer: 14.2, clockPpm: -1.5, delayMs: 0.46, dopplerHz: 0.29,
  sbr: false, ps: false, audioUnits: 1500, audioBad: 12, concealed: 3, underruns: 1, sdcOk: 40, sdcBad: 1, superFrames: 41,
  lockS: [0.9, 0.9, 0.9, 1.6, 2.8, 4.0], reacq: 1, lostWhy: 'fac', lostAtS: 31.2, blankedPct: 0.4, iqGaps: 2, dropped: 0,
};
const sigCases: Array<[string, DrmState | null]> = [
  ['nothing yet', null],
  ['searching, new server', { type: 'drm', reacq: 0, dropped: 0 }],
  ['re-acquiring after a loss', { type: 'drm', sync: true, freq: true, lockS: [0.8, 1.2, -1, -1, -1, -1], reacq: 2, lostWhy: 'fac', dropped: 3 }],
  ['RRI on an OLDER server (none of the new fields)', RRI],
  ['RRI, every field', NEW],
  ['xHE-AAC not playable', { ...NEW, codec: 'xHE-AAC', sbr: undefined, ps: undefined, playable: false, decoder: undefined, why: 'This server was built without an xHE-AAC decoder.' }],
  ['bad channel', { ...NEW, snr: 7, mscMer: 6, sdcMer: 3, delayMs: 5.1, dopplerHz: 2.4, facBad: 30, blankedPct: 12, mscQam: 64, robustness: 'B' }],
  ['mode D, not measured yet', { ...NEW, robustness: 'D', delayMs: undefined, dopplerHz: undefined }],
];
for (const [what, st] of sigCases) eq(`signal rows: ${what}`, drmSignalRows(st), webSignalRows(st));
const rowsOf = (st: DrmState | null) => drmSignalRows(st).filter((r): r is { k: string; v: string; tone?: 'ok' | 'warn' | 'bad'; tip: string } => !('h' in r));
const get = (st: DrmState | null, k: string) => rowsOf(st).find((r) => r.k === k);
eq('the groups, in order', drmSignalRows(NEW).filter((r) => 'h' in r).map((r) => (r as { h: string }).h), ['LOCK', 'SIGNAL', 'QUALITY', 'ERRORS', 'AUDIO', 'STATION']);
ok('every row has a plain-English tooltip', rowsOf(NEW).every((r) => r.tip.length > 20 && !/undefined/.test(r.tip + r.v)));
eq('the six lock times, as the lights', rowsOf(NEW).filter((r) => / after$/.test(r.k)).map((r) => `${r.k}=${r.v}`),
   ['Time after=0.9 s', 'Freq after=0.9 s', 'Frame after=0.9 s', 'FAC after=1.6 s', 'SDC after=2.8 s', 'MSC after=4.0 s']);
eq('re-acquisitions say why', get(NEW, 'Re-acquisitions')?.v, '1 · last: the FAC failed 16 frames running');
eq('delay spread against the guard', [get(NEW, 'Delay spread')?.v, get(NEW, 'Delay spread')?.tone], ['0.46 ms of 5.33 ms guard', 'ok']);
eq('Doppler', [get(NEW, 'Doppler spread')?.v, get(NEW, 'Doppler spread')?.tone], ['0.29 Hz', 'ok']);
eq('a bad channel is coloured bad', ['SNR', 'MSC MER', 'Delay spread', 'Doppler spread', 'FAC', 'Pulse blanker'].map((k) => get(sigCases[6][1], k)?.tone),
   ['bad', 'bad', 'bad', 'bad', 'bad', 'bad']);
eq('errors, not passes', get(NEW, 'FAC')?.v, '2 bad of 42 (4.8 %)');
eq('MSC MER judged by its QAM (16-QAM needs ~12 dB)', get(NEW, 'MSC MER')?.tone, 'ok');
eq('not measured yet says so', get(sigCases[7][1], 'Delay spread')?.v, 'measuring…');
ok('an OLDER server: no row it cannot fill (no lock times, delay, Doppler, SDC MER, gaps)',
   !rowsOf(RRI).some((r) => / after$|Re-acq|Delay|Doppler|SDC MER|IQ gaps|Audio gaps/.test(r.k)));
ok('no codec yet: no Decoder row', !get({ type: 'drm', sync: true, locked: true, reacq: 0 }, 'Decoder'));
// the web renderer: rows patched in place on the same skeleton (DOM-free check of the built HTML)
{
  const host = { innerHTML: '', dataset: {} as Record<string, string>, querySelectorAll: () => [] } as unknown as HTMLElement;
  drmSignalRender(host, NEW);
  const h = host.innerHTML;
  ok('web pane: headings, rows with tooltips, traffic lights', /<h4>LOCK<\/h4>/.test(h) && /class="row" title="[^"]{20,}"/.test(h) && /class="tl ok"/.test(h));
  ok('web pane: values escaped', !/<live>/.test(h) && /&lt;live&gt;/.test(h));
}
// sanitiser: the new fields kept when well formed, dropped when not
eq('sanitised new fields', drmSanitise({ type: 'drm', reacq: 2, lockS: [1, 2, 3, 4, 5, 6], lostWhy: 'fac\u0007', delayMs: 0.5, sbr: true, ps: 'no', iqGaps: 1 }),
   { type: 'drm', delayMs: 0.5, sbr: true, lockS: [1, 2, 3, 4, 5, 6], reacq: 2, lostWhy: 'fac ', iqGaps: 1 });
eq('a malformed lockS is dropped', drmSanitise({ type: 'drm', lockS: [1, 'x', 3, 4, 5, 6] })?.lockS, undefined);
// the panels: the pane key in the header, reachable by pad and keyboard
const panel = readFileSync(new URL('../src/components/DrmPanel.tsx', import.meta.url), 'utf8');
ok('app: SIGNAL / STATION key in the header', /label=\{pane === 'signal' \? 'STATION' : 'SIGNAL'\}/.test(panel));
ok('app: the screen highlight can land on the box, Enter takes the keys', /screenNavRef\('drmBox', \(\) => setGrab\(true\)\)/.test(panel) && /useListNav\(/.test(panel));
ok('app: the pane draws drmSignalRows', /drmSignalRows\(state\)/.test(panel));
const main = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
ok('web: the box is one pad stop (data-navbox) with its pane key', /box\.setAttribute\('data-navbox', ''\)/.test(main) && /id="drmPane"/.test(main));
ok('the app passes the box its height limit', /<DrmPanel state=\{drmState\} topLimit=\{boxTopLimit\}/.test(scr));

console.log(`test_drm_app: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
