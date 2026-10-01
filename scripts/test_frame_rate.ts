/**
 * FRAME RATE — the CONTROL CUSTOMISATION pane's 60 Hz cap (src/constants/faceplate.ts,
 * src/services/frameRate.ts, the native halves in VibeLocalSDR.mm / VibeLocalSdrModule.kt).
 *
 * Power audit, 2026-10-01: a ProMotion iPhone ran its panel at 120 Hz the whole time the radio
 * streamed. Stuart: "add a 60Hz option".
 *
 * Proves:
 *   • the row is HIDDEN on a 60 Hz panel and whenever the binary cannot say (AGENTS.md: never offer
 *     a control whose every use is a no-op) — and shown on 90 / 120 / 144 Hz panels;
 *   • the top key carries the panel's REAL rate, rounded (a 90 Hz phone never reads "120 Hz";
 *     Android's 119.99 / 60.000004 do not leak into a label);
 *   • the default is today's behaviour ('full', no cap), a stored 60 survives, garbage falls back;
 *   • what native is told: 60 for the cap, 0 for none;
 *   • the crash-safety reset never touches it (it is not a risky faceplate, and the safe faceplate
 *     keeps it);
 *   • the wiring is real: the pane reads frameRateChoices, the provider pushes frameRateCapHz, and
 *     both native modules implement both methods under the names JS calls.
 *
 * Run: node --no-warnings scripts/test_frame_rate.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import {
  DEFAULT_SETTINGS, frameRateCapHz, frameRateChoices, isRiskyFaceplate, normaliseHz, parseSettings,
  safeFaceplate, FRAME_RATE_NOTE, type FaceplateSettings,
} from '../src/constants/faceplate.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);

// ── When the row shows ────────────────────────────────────────────────────────
eq('unknown (old binary / web / Expo Go) → hidden', frameRateChoices(null), null);
eq('undefined → hidden', frameRateChoices(undefined), null);
eq('NaN → hidden', frameRateChoices(NaN), null);
eq('0 → hidden', frameRateChoices(0), null);
eq('60 Hz iPhone → hidden', frameRateChoices(60), null);
eq('Android 60.000004 → hidden', frameRateChoices(60.000004), null);
eq('Android 59.94 → hidden', frameRateChoices(59.94), null);
eq('ProMotion 120 → 120 Hz / 60 Hz', frameRateChoices(120),
   [{ value: 'full', label: '120 Hz' }, { value: '60', label: '60 Hz' }]);
eq('Android 119.99 → labelled 120 Hz', frameRateChoices(119.99)?.[0].label, '120 Hz');
eq('90 Hz phone → labelled 90 Hz, never 120', frameRateChoices(90.0)?.[0].label, '90 Hz');
eq('144 Hz phone → 144 Hz', frameRateChoices(144)?.[0].label, '144 Hz');
eq('normaliseHz rounds', normaliseHz(89.6), 90);
eq('normaliseHz refuses a string', normaliseHz('120' as unknown as number), null);

// ── Stored value and default ─────────────────────────────────────────────────
eq('default is today: no cap', DEFAULT_SETTINGS.frameRate, 'full');
eq('nothing stored → full', parseSettings(null, 'white').frameRate, 'full');
eq('an old store without the field → full', parseSettings(JSON.stringify({ chassis: 'silver' })).frameRate, 'full');
eq('stored 60 survives', parseSettings(JSON.stringify({ frameRate: '60' })).frameRate, '60');
eq('stored garbage → full', parseSettings(JSON.stringify({ frameRate: 30 })).frameRate, 'full');
eq('round trip', parseSettings(JSON.stringify({ ...DEFAULT_SETTINGS, frameRate: '60' })).frameRate, '60');

// ── What native is told ──────────────────────────────────────────────────────
eq('60 → cap 60', frameRateCapHz({ frameRate: '60' }), 60);
eq('full → 0 (no cap)', frameRateCapHz({ frameRate: 'full' }), 0);

// ── Crash safety leaves it alone ─────────────────────────────────────────────
const capped: FaceplateSettings = { ...DEFAULT_SETTINGS, frameRate: '60' };
ok('a frame-rate cap alone is not a risky faceplate', !isRiskyFaceplate(capped));
eq('the safe faceplate keeps the cap', safeFaceplate({ ...capped, chassis: 'black', display: 'nixie' }).frameRate, '60');

ok('the note is UK English and says what it costs', /battery/.test(FRAME_RATE_NOTE) && /Hz/.test(FRAME_RATE_NOTE));

// ── Wiring ───────────────────────────────────────────────────────────────────
const read = (p: string) => readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');
const menu = read('src/components/MenuSheet.tsx');
ok('the pane decides the row with frameRateChoices', /frameRateChoices\(maxRefreshHz\)/.test(menu));
ok('the pane labels it FRAME RATE', /label="FRAME RATE"/.test(menu));
const ctx = read('src/contexts/FaceplateContext.tsx');
ok('the provider pushes the cap', /applyFrameRateCap\(capHz\)/.test(ctx) && /frameRateCapHz\(settings\)/.test(ctx));
ok('the provider waits for the stored copy before pushing', /capKnown/.test(ctx));
const svc = read('src/services/frameRate.ts');
const ios = read('modules/vibe-local-sdr/VibeLocalSDR.mm');
const android = read('android/app/src/main/java/com/vibesdr/app/VibeLocalSdrModule.kt');
for (const m of ['setFrameRateCap', 'maxRefreshRate']) {
  ok(`JS calls ${m}`, svc.includes(`${m}?.`) || svc.includes(`${m}?:`) || svc.includes(`.${m}`));
  ok(`iOS exports ${m}`, new RegExp(`RCT_EXPORT_METHOD\\(${m}:`).test(ios));
  ok(`Android exports ${m}`, new RegExp(`@ReactMethod\\s+fun ${m}\\(`).test(android));
}
ok('iOS wraps all three display-link entry points',
   ['displayLinkWithTarget:selector:', 'setPreferredFrameRateRange:', 'setPreferredFramesPerSecond:']
     .every(s => ios.includes(`@selector(${s})`)));
ok('Android re-applies on resume', /onHostResume\(\)\s*\{\s*applyFrameRateCap\(\)/.test(android));

console.log(`frame rate: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
