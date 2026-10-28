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
  autoFrameRungs, autoWindowLate, DEFAULT_SETTINGS, frameRateCapHz, frameRateChoices, isRiskyFaceplate, normaliseHz, parseSettings,
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

// ── When the row shows, and what it offers ─────────────────────────────────────
eq('unknown (old binary / web / Expo Go) → hidden', frameRateChoices(null), null);
eq('undefined → hidden', frameRateChoices(undefined), null);
eq('NaN → hidden', frameRateChoices(NaN), null);
eq('0 → hidden', frameRateChoices(0), null);
const keys = (hz: number) => frameRateChoices(hz)?.map(c => c.label);
eq('60 Hz panel → AUTO / 60 / 30 (no key twice)', keys(60), ['AUTO', '60 Hz', '30 Hz']);
eq('Android 60.000004 → AUTO / 60 / 30', keys(60.000004), ['AUTO', '60 Hz', '30 Hz']);
eq('Android 59.94 → AUTO / 60 / 30', keys(59.94), ['AUTO', '60 Hz', '30 Hz']);
eq('ProMotion 120 → AUTO / 120 / 60 / 30', frameRateChoices(120),
   [{ value: 'auto', label: 'AUTO' }, { value: 'max', label: '120 Hz' }, { value: '60', label: '60 Hz' }, { value: '30', label: '30 Hz' }]);
eq('Android 119.99 → labelled 120 Hz', frameRateChoices(119.99)?.[1].label, '120 Hz');
eq('90 Hz phone → labelled 90 Hz, never 120', frameRateChoices(90.0)?.[1].label, '90 Hz');
eq('144 Hz phone → 144 Hz', frameRateChoices(144)?.[1].label, '144 Hz');
eq('normaliseHz rounds', normaliseHz(89.6), 90);
eq('normaliseHz refuses a string', normaliseHz('120' as unknown as number), null);

// ── Stored value and default ─────────────────────────────────────────────────
eq('default is AUTO', DEFAULT_SETTINGS.frameRate, 'auto');
eq('nothing stored → auto', parseSettings(null, 'white').frameRate, 'auto');
eq('an old store without the field → auto', parseSettings(JSON.stringify({ chassis: 'silver' })).frameRate, 'auto');
eq("the OLD default 'full' (saved for everyone) → auto", parseSettings(JSON.stringify({ frameRate: 'full' })).frameRate, 'auto');
eq('stored 60 survives', parseSettings(JSON.stringify({ frameRate: '60' })).frameRate, '60');
eq('stored 30 survives', parseSettings(JSON.stringify({ frameRate: '30' })).frameRate, '30');
eq('a deliberate max survives', parseSettings(JSON.stringify({ frameRate: 'max' })).frameRate, 'max');
eq('stored garbage → auto', parseSettings(JSON.stringify({ frameRate: 30 })).frameRate, 'auto');
eq('round trip', parseSettings(JSON.stringify({ ...DEFAULT_SETTINGS, frameRate: '30' })).frameRate, '30');

// ── What native is told ──────────────────────────────────────────────────────
eq('60 → cap 60', frameRateCapHz({ frameRate: '60' }), 60);
eq('30 → cap 30', frameRateCapHz({ frameRate: '30' }), 30);
eq('max → 0 (no cap)', frameRateCapHz({ frameRate: 'max' }), 0);
eq('auto, nothing learnt → 0', frameRateCapHz({ frameRate: 'auto' }), 0);
eq('auto follows its rung', frameRateCapHz({ frameRate: 'auto' }, 30), 30);
eq('a pick ignores the auto rung', frameRateCapHz({ frameRate: '60' }, 30), 60);

// ── AUTO's rule ──────────────────────────────────────────────────────────────
eq('rungs on 120 Hz: none → 60 → 30', autoFrameRungs(120), [0, 60, 30]);
eq('rungs on 60 Hz: none → 30', autoFrameRungs(60), [0, 30]);
eq('Android AUTO on a 120 Hz panel: 60, then 30', autoFrameRungs(120, 'android'), [60, 30]);
eq('Android AUTO on a 60 Hz panel: the panel, then 30', autoFrameRungs(60, 'android'), [0, 30]);
eq('Android AUTO with the panel unknown: no cap, then 30', autoFrameRungs(null, 'android'), [0, 30]);
eq('iOS ProMotion keeps its maximum', autoFrameRungs(120, 'ios'), [0, 60, 30]);
ok('a still screen teaches nothing', !autoWindowLate(20, 20));
ok('a fast phone (1 % late) stays', !autoWindowLate(300, 3));
ok('the Tab at 60 (≈ 5 % late) stays', !autoWindowLate(300, 15));
ok('a struggling phone (> 10 % late) steps down', autoWindowLate(300, 36));
ok('the XCover before the engine (59 % janky) steps down', autoWindowLate(250, 148));

// ── Crash safety leaves it alone ─────────────────────────────────────────────
const capped: FaceplateSettings = { ...DEFAULT_SETTINGS, frameRate: '30' };
ok('a frame-rate cap alone is not a risky faceplate', !isRiskyFaceplate(capped));
eq('the safe faceplate keeps the cap', safeFaceplate({ ...capped, chassis: 'black', display: 'nixie' }).frameRate, '30');

ok("the note is Stuart's wording", /Auto monitors system performance/.test(FRAME_RATE_NOTE) && /battery/.test(FRAME_RATE_NOTE));

// ── Wiring ───────────────────────────────────────────────────────────────────
const read = (p: string) => readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');
const menu = read('src/components/MenuSheet.tsx');
ok('the pane decides the row with frameRateChoices', /frameRateChoices\(maxRefreshHz\)/.test(menu));
ok('the pane labels it FRAME RATE', /label="FRAME RATE"/.test(menu));
const ctx = read('src/contexts/FaceplateContext.tsx');
ok('the provider pushes the cap — a 30 as 60 to the display', /applyFrameRateCap\(capHz === 30 \? 60 : capHz\)/.test(ctx) && /frameRateCapHz\(settings, saver \? 30 : autoHz\)/.test(ctx));
ok('battery saver puts AUTO on 30 (Android)', /powerSaveOn\(\)/.test(ctx) && /saver \? 30 : autoHz/.test(ctx));
ok('Android exports isPowerSaveMode', /fun isPowerSaveMode\(/.test(read('android/app/src/main/java/com/vibesdr/app/VibeLocalSdrModule.kt')));
ok('the provider sets the app-side pacing', /uiFrameCapHz\.value = capHz/.test(ctx));
for (const f of ['src/components/WaterfallView.tsx', 'src/components/LedVu.tsx', 'src/components/EdgeMeter.tsx'])
  ok(`${f} honours the cap`, /uiCadence/.test(read(f)));
ok('the provider waits for the stored copy before pushing', /capKnown/.test(ctx));
const svc = read('src/services/frameRate.ts');
const ios = read('modules/vibe-local-sdr/VibeLocalSDR.mm');
const android = read('android/app/src/main/java/com/vibesdr/app/VibeLocalSdrModule.kt');
for (const m of ['startFrameWatch', 'stopFrameWatch', 'takeFrameWatch'])
  ok(`Android exports ${m}`, new RegExp(`@ReactMethod(\\([^)]*\\))?\\s+fun ${m}\\(`).test(android));
for (const m of ['setFrameRateCap', 'maxRefreshRate']) {
  ok(`JS calls ${m}`, svc.includes(`${m}?.`) || svc.includes(`${m}?:`) || svc.includes(`.${m}`));
  ok(`iOS exports ${m}`, new RegExp(`RCT_EXPORT_METHOD\\(${m}:`).test(ios));
  ok(`Android exports ${m}`, new RegExp(`@ReactMethod\\s+fun ${m}\\(`).test(android));
}
ok('iOS wraps all three display-link entry points',
   ['displayLinkWithTarget:selector:', 'setPreferredFrameRateRange:', 'setPreferredFramesPerSecond:']
     .every(s => ios.includes(`@selector(${s})`)));
// ★ NO REFRESH PIN from the waterfall (Stuart 2026-10-28: ProMotion varies its rate to suit; a fixed screen has vsync)
// ★ HELD AT 120 while rows flow, the same on a touch (Stuart 2026-10-28: 507 held — "judder seems to be gone"; free — "stuttering")
{ const w = read('src/components/WaterfallView.tsx');
  ok('the waterfall holds ONE fixed 60 while rows flow (a multiple of every row rate), the same on a touch', /setSteadyRefresh\(60\)/.test(w) && !/lastInteractAt/.test(w.split('const noteRowsFlowing')[1].slice(0, 900)));
  ok('…released 1 s after the last row, on backgrounding and on unmount', /setSteadyRefresh\(0\); \}, 1000\)/.test(w) && /if \(bg\) \{[\s\S]{0,120}setSteadyRefresh\(0\)/.test(w) && /useEffect\(\(\) => \(\) => \{[\s\S]{0,120}setSteadyRefresh\(0\)/.test(w)); }
ok('iOS still exports setSteadyRefresh (unused, kept for a screen that needs a pin)', /RCT_EXPORT_METHOD\(setSteadyRefresh:/.test(ios));
ok('Android re-applies on resume', /onHostResume\(\)\s*\{\s*applyFrameRateCap\(\)/.test(android));

console.log(`frame rate: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
