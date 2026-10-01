/**
 * The stereo light in DAB (src/services/dabTypes.ts dabServiceStereo) — the playing SERVICE's, from
 * its own audio headers, never the FM pilot's.
 *
 * ★★★ Stuart, 2026-10-01: "the stereo icon from WFM also is stuck when in DAB mode even when on Mono
 *     stations." The server now sends `stereo` in the `dab` state (vibe_dab_stereo.h, held down by
 *     vibeserver/test-dab-stereo.cpp); a server from before that is read from the codec line it has
 *     always sent. Unknown (acquiring) = OFF. The web client imports the same function.
 *
 * Run: node --no-warnings scripts/test_dab_stereo.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { dabServiceStereo, parseDabMessage } from '../src/services/dabTypes.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};

// ── The server's own answer wins ──
eq('stereo: true → on', dabServiceStereo({ stereo: true }), true);
eq('stereo: false → off (even with a codec line that says Stereo)',
   dabServiceStereo({ stereo: false, codecDetail: 'DAB 128 kbit/s 48 kHz MPEG-1 Layer II Stereo' }), false);

// ── A server before 2026-10-01: the codec line, every channel mode it can print ──
// DAB+ (vibe_dab_service.h): "DAB+ %d kbit/s %d kHz <AAC-LC|HE-AAC v1|HE-AAC v2> <Parametric Stereo|Stereo|Mono>"
const aac: [string, boolean][] = [
  ['DAB+ 64 kbit/s 48 kHz HE-AAC v1 Stereo', true],
  ['DAB+ 32 kbit/s 32 kHz HE-AAC v2 Parametric Stereo', true],
  ['DAB+ 24 kbit/s 32 kHz HE-AAC v1 Mono', false],
  ['DAB+ 96 kbit/s 48 kHz AAC-LC Stereo', true],
  ['DAB+ 48 kbit/s 48 kHz AAC-LC Mono', false],
];
// Layer II: "DAB %d kbit/s %d kHz MPEG-<1|2 LSF> Layer II <Stereo|Joint Stereo|Dual Channel|Mono>"
const mp2: [string, boolean][] = [
  ['DAB 128 kbit/s 48 kHz MPEG-1 Layer II Stereo', true],
  ['DAB 160 kbit/s 48 kHz MPEG-1 Layer II Joint Stereo', true],
  ['DAB 192 kbit/s 48 kHz MPEG-1 Layer II Dual Channel', false],
  ['DAB 80 kbit/s 48 kHz MPEG-1 Layer II Mono', false],
  ['DAB 64 kbit/s 24 kHz MPEG-2 LSF Layer II Mono', false],
];
for (const [line, want] of [...aac, ...mp2]) eq(`old server: "${line}" → ${want ? 'on' : 'off'}`, dabServiceStereo({ codecDetail: line }), want);

// ── Not known = OFF ──
eq('no DAB state → off', dabServiceStereo(null), false);
eq('acquiring (no codec line, no stereo) → off', dabServiceStereo({}), false);
eq('a non-boolean stereo is not believed', dabServiceStereo({ stereo: 'yes' as unknown as boolean }), false);

// ── Through the parser the app uses, as the wire delivers it ──
const wire = (extra: Record<string, unknown>) => parseDabMessage({ type: 'dab', channel: '11D', services: [], ...extra });
eq('wire: DAB+ PS service', dabServiceStereo(wire({ codecDetail: 'DAB+ 32 kbit/s 32 kHz HE-AAC v2 Parametric Stereo', ps: true, audioCh: 2, stereo: true })), true);
eq('wire: Layer II mono service', dabServiceStereo(wire({ codecDetail: 'DAB 80 kbit/s 48 kHz MPEG-1 Layer II Mono', mp2Mode: 3, audioCh: 1, stereo: false })), false);
eq('wire: Layer II dual channel — two programmes, not stereo', dabServiceStereo(wire({ mp2Mode: 2, audioCh: 2, stereo: false })), false);

// ── The server's two branches emit the field (the C++ table is test-dab-stereo.cpp) ──
const svc = readFileSync(new URL('../android/app/src/main/cpp/vibe_dab_service.h', import.meta.url), 'utf8');
eq('server: the DAB+ codec line carries "stereo"', /aacEffRateHz\\":%d,\\"stereo\\":%s/.test(svc), true);
eq('server: the Layer II codec line carries "mp2Mode" and "stereo"', /\\"mp2Mode\\":%d,\\"stereo\\":%s/.test(svc), true);
// ── …and the FM pilot no longer speaks in DAB ──
const shim = readFileSync(new URL('../android/app/src/main/cpp/local_sdr_shim.cpp', import.meta.url), 'utf8');
eq('server: the rds stereo flag is false while in DAB', /const bool st = wfm && !g_dabMode\.load/.test(shim), true);
// ── Both clients drive the light from the one function ──
const web = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
eq('web: one writer for the light, DAB-aware', /dabOn \? dabServiceStereo\(dabState\) : rdsStereo/.test(web), true);
eq('web: onRds no longer writes the light directly', /onRds: \(m\) => \{\s*\$\('stereo'\)/.test(web), false);
const scr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
eq('app: the deck\'s light is the service\'s in DAB', /fmStereo=\{dabOn \? dabServiceStereo\(dabState\) : fmStereo\}/.test(scr), true);

console.log(`${fails ? 'FAIL' : 'ok'}  dab stereo light: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
