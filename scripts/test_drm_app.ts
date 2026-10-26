/**
 * The app's DRM box (src/services/drm.ts drmView, drawn by DrmPanel) against the web client's (web/client/src/drm.ts
 * drmRender) — ONE RULE, TWO READERS (V12 app parity, 2026-10-26). Both run on the same reports; the web's HTML is reduced
 * to its text and must say exactly what the app's box says, in the same order. Also: the sanitiser, the lights, the SNR
 * quality, the capability (`modes` lists "drm"), and that the app offers DRM only through that.
 *
 * Run: npx tsx scripts/test_drm_app.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { drmSanitise, drmView, drmViewText, drmOffered, DRM_LIGHTS, type DrmState } from '../src/services/drm';
import { drmRender } from '../web/client/src/drm';

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
                             why: 'This server cannot play xHE-AAC: FFmpeg 7.1 or newer is needed.' }],
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

console.log(`test_drm_app: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
