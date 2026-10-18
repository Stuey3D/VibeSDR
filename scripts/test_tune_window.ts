/**
 * The locked-window tune clamp (src/utils/tuneWindow.ts).
 *
 * Stuart, 2026-10-18: the app let a listener tune into the black past the walls of a VibeServer whose
 * owner had locked the centre; the server clamped silently and the readout went on claiming the
 * frequency asked for. Every tune path in SDRScreen now clamps through tuneBounds(); this pins the
 * rule to the server's own (local_sdr_shim.cpp retune: lockedCentre ± (Fs/2 − min(50 kHz, 0.15·Fs))).
 *
 * ★ The rule clamps the DIAL, not the passband, and does not depend on the mode — the server's
 *   doesn't either (it is the same `limit` whatever rxMode is). The functions take no mode and no
 *   filter on purpose; the checks below that vary them are there to say so out loud.
 *
 * Run: node --no-warnings scripts/test_tune_window.ts
 */
import { lockedGuardHz, lockedWindow, tuneBounds, clampHz, rangeMessage } from '../src/utils/tuneWindow.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}

const RTL: [number, number] = [100_000, 1_766_000_000];     // LOCAL_CAPS.freqRange
const HFP: [number, number] = [500, 260_000_000];           // an Airspy HF+'s reported coverage

// ── The guard band is the server's ────────────────────────────────────────────────────────────
eq('guard 2.4 MS/s = 50 kHz',      lockedGuardHz(2_400_000), 50_000);
eq('guard 8 MS/s = 50 kHz',        lockedGuardHz(8_000_000), 50_000);
eq('guard 250 kS/s = 15 % = 37.5 kHz', lockedGuardHz(250_000), 37_500);

// ── Unlocked: nothing narrows ─────────────────────────────────────────────────────────────────
eq('no lock → no window',          lockedWindow(0, 2_400_000), null);
eq('negative lock → no window',    lockedWindow(-1, 2_400_000), null);
eq('fs unknown → no window',       lockedWindow(100_000_000, 0), null);
eq('unlocked bounds = radio range', tuneBounds(RTL, null), RTL);
eq('unlocked 14 MHz passes',       clampHz(14_000_000, tuneBounds(RTL, null)), 14_000_000);
eq('unlocked clamps to radio floor', clampHz(10_000, tuneBounds(RTL, null)), 100_000);

// ── Locked: RTL at 2.4 MS/s on 98 MHz ─────────────────────────────────────────────────────────
const w98 = lockedWindow(98_000_000, 2_400_000);
eq('window 98 MHz ±1.15 MHz',      w98, [96_850_000, 99_150_000]);
const b98 = tuneBounds(RTL, w98);
eq('bounds = window inside radio', b98, [96_850_000, 99_150_000]);
eq('inside passes untouched',      clampHz(97_700_000, b98), 97_700_000);
eq('low edge exact stays',         clampHz(96_850_000, b98), 96_850_000);
eq('high edge exact stays',        clampHz(99_150_000, b98), 99_150_000);
eq('1 Hz below → low edge',        clampHz(96_849_999, b98), 96_850_000);
eq('1 Hz above → high edge',       clampHz(99_150_001, b98), 99_150_000);
eq('the black past the wall → edge', clampHz(99_190_000, b98), 99_150_000);
eq('another band (14 MHz) → edge', clampHz(14_000_000, b98), 96_850_000);
eq('typed 121.5 MHz → edge',       clampHz(121_500_000, b98), 99_150_000);

// ── Locked HF+ window on 6.5 MHz at 8 MS/s (Stuart's 2.5–10.5 example) ────────────────────────
const w65 = lockedWindow(6_500_000, 8_000_000);
eq('window 6.5 MHz ±3.95 MHz',     w65, [2_550_000, 10_450_000]);
eq('14 MHz → 10.45 (the edge, not silence)', clampHz(14_000_000, tuneBounds(HFP, w65)), 10_450_000);

// ── A window pinned near 0 Hz never offers a frequency below the radio's floor ────────────────
const w0 = lockedWindow(1_000_000, 2_400_000);
eq('window below 0 Hz',            w0, [-150_000, 2_150_000]);
eq('…intersected with the radio',  tuneBounds(RTL, w0), [100_000, 2_150_000]);

// ── A stale freqRange that misses the window entirely: the window is what the server honours ──
eq('no overlap → window whole',    tuneBounds([0, 30_000_000], w98), w98);

// ── Mode and filter do not enter it (the server's limit is the same in every rxMode) ──────────
for (const [mode, halfBw] of [['wfm', 100_000], ['am', 5_000], ['usb', 1_500], ['nfm', 6_250]] as const) {
  eq(`${mode} ±${halfBw}: dial clamps to the edge, passband not pulled in`,
     clampHz(99_400_000, tuneBounds(RTL, w98)), 99_150_000);
}

// ── What the frequency card says ──────────────────────────────────────────────────────────────
eq('range message',                rangeMessage(b98), 'This receiver covers 96.850–99.150 MHz');

console.log(`tune window: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
