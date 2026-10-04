/**
 * TRANSPARENCY EFFECTS — the low-end default, the stored choice, the migration from the decoder
 * boxes' Transparent / Solid, and OFF's colours (src/constants/transparency.ts, faceplate.ts,
 * decoderTokens.ts).
 *
 * Stuart, 2026-09-30: "when app opened we detect low end hardware we default to transparency off.
 * So app looks the same as now just not see through" — and "solid would be 1.0 fully solid".
 *
 * Proves:
 *   • each detection signal on its own turns the default OFF, and a fast device stays ON;
 *   • a stored choice always wins, both ways, and the auto default is never saved as if chosen;
 *   • `decoderBg` migrates: solid → OFF (chosen), transparent → ON (NOT chosen — it was written
 *     back as a default on every first launch, so it is no evidence of a choice);
 *   • OFF is alpha 1.0 EXACTLY on every surface, and each opaque colour is today's glass composited
 *     over black — never lighter than the glass looked on ANY waterfall, and within a whisker of it
 *     (≤ 1.10:1 in luminance) on every dark colour map's floor, the default Jet's navy included;
 *   • OFF's surface contract (Stuart: "opaque panels over a live, undimmed waterfall"): no blur, no
 *     scrim, no drop shadow — and the files that draw a scrim or a shadow over the spectrum read it;
 *   • every file that draws a BlurView reads the switch (a new unguarded blur fails here).
 *
 * Run: node --no-warnings scripts/test_transparency.ts   (run-tests.sh does)
 */
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import {
  autoTransparency, effectiveTransparency, isPreA12Model, parseDeviceClass, parseOsVersion,
  LOW_MEMORY_BYTES, MIN_ANDROID_API, MIN_IPHONE_IOS, AUTO_REASON_NOTE,
  type DeviceSignals,
} from '../src/constants/transparency.ts';
import {
  DEFAULT_SETTINGS, DEFAULT_CHASSIS, SILVER_CHASSIS, BLACK_CHASSIS, parseSettings, resolveFaceplate,
  withTransparency, solidOver, surfaceTokens, NO_DROP_SHADOW, TRANSPARENCY_NOTE,
} from '../src/constants/faceplate.ts';
import { DEFAULT_SOLID_BG, decoderTokensFor } from '../src/constants/decoderTokens.ts';
import { COLORMAPS_STOPS } from '../src/assets/colormaps.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);

// ── The decision: each signal alone ──────────────────────────────────────────

/** A current, fast phone: every signal says "leave the glass on". */
const FAST: DeviceSignals = {
  os: 'ios', osVersion: 26, isPad: false, isTV: false, reduceTransparency: false,
  totalMemoryBytes: null, modelId: null, isMac: false,
};
const GB = 1024 ** 3;

eq('fast iPhone, nothing known beyond the OS → ON', autoTransparency(FAST), { transparency: 'on', reason: null });
eq('fast Android → ON', autoTransparency({ ...FAST, os: 'android', osVersion: 35 }).transparency, 'on');

eq('Reduce Transparency → OFF', autoTransparency({ ...FAST, reduceTransparency: true }),
   { transparency: 'off', reason: 'reduceTransparency' });
eq('3 GB (reports ~2.8 GiB) → OFF', autoTransparency({ ...FAST, totalMemoryBytes: 2.8 * GB }).reason, 'lowMemory');
eq('2 GB → OFF', autoTransparency({ ...FAST, totalMemoryBytes: 1.9 * GB }).reason, 'lowMemory');
eq('4 GB (reports ~3.7 GiB) → ON', autoTransparency({ ...FAST, totalMemoryBytes: 3.7 * GB }).transparency, 'on');
eq('8 GB → ON', autoTransparency({ ...FAST, totalMemoryBytes: 7.5 * GB }).transparency, 'on');
ok('the memory line sits between 3 GB and 4 GB phones', LOW_MEMORY_BYTES > 3 * GB && LOW_MEMORY_BYTES < 3.7 * GB);
eq('memory 0 / unknown decides nothing', autoTransparency({ ...FAST, totalMemoryBytes: 0 }).transparency, 'on');

eq('Android 9 (API 28) → OFF', autoTransparency({ ...FAST, os: 'android', osVersion: 28 }).reason, 'oldAndroid');
eq('Android 10 (API 29) → ON', autoTransparency({ ...FAST, os: 'android', osVersion: MIN_ANDROID_API }).transparency, 'on');
eq('Android, API unknown → ON', autoTransparency({ ...FAST, os: 'android', osVersion: null }).transparency, 'on');

eq('iPhone on iOS 16 (may be an 8 / X) → OFF', autoTransparency({ ...FAST, osVersion: 16 }).reason, 'oldIos');
eq('iPhone on iOS 17 (XS/XR or newer) → ON', autoTransparency({ ...FAST, osVersion: MIN_IPHONE_IOS }).transparency, 'on');
// ★★ The Mac runs the iPad app, and reports a macOS version: it must never read as an old iPad.
eq('iPad idiom on "15" (a Mac on macOS 15) → ON', autoTransparency({ ...FAST, isPad: true, osVersion: 15 }).transparency, 'on');
eq('Apple TV is not an old iPhone', autoTransparency({ ...FAST, isTV: true, osVersion: 16 }).transparency, 'on');
eq('the iOS rule never reaches Android numbers', autoTransparency({ ...FAST, os: 'android', osVersion: 34 }).transparency, 'on');

for (const [id, old] of [['iPhone10,6', true], ['iPhone10,1', true], ['iPhone9,3', true], ['iPhone11,8', false],
                         ['iPhone11,2', false], ['iPhone17,1', false], ['iPad7,11', true], ['iPad8,1', false],
                         ['iPad13,18', false], ['iPod9,1', true], ['arm64', false], ['', false]] as const) {
  eq(`model ${id || '(empty)'} pre-A12: ${old}`, isPreA12Model(id), old);
}
eq('model iPhone10,3 (X) → OFF', autoTransparency({ ...FAST, modelId: 'iPhone10,3' }).reason, 'oldModel');
eq('model iPhone11,8 (XR) → ON', autoTransparency({ ...FAST, modelId: 'iPhone11,8' }).transparency, 'on');
eq('an unreadable model decides nothing', autoTransparency({ ...FAST, modelId: 'x86_64' }).transparency, 'on');

eq('several signals: the OS setting is the reason given', autoTransparency({
  ...FAST, reduceTransparency: true, totalMemoryBytes: 2 * GB, osVersion: 16 }).reason, 'reduceTransparency');
for (const r of ['reduceTransparency', 'lowMemory', 'oldModel', 'oldAndroid', 'oldIos'] as const) {
  ok(`reason ${r} has a note for the pane`, typeof AUTO_REASON_NOTE[r] === 'string' && AUTO_REASON_NOTE[r].length > 0);
}
eq('iOS version string → major', parseOsVersion('16.7.10'), 16);
eq('Android API number passes through', parseOsVersion(33), 33);
eq('garbage version → null', parseOsVersion('beta'), null);

// ── Fed from the native deviceClass() getter (src/services/deviceClass.ts) ────
// What VibeLocalSDR.deviceClass() returns, through parseDeviceClass, into the rules — the path
// FaceplateContext.baseSignals() takes.
const fed = (os: DeviceSignals['os'], osVersion: number, raw: unknown, extra: Partial<DeviceSignals> = {}) =>
  autoTransparency({ ...FAST, os, osVersion, ...parseDeviceClass(raw), ...extra });

eq('iPhone XR "iPhone11,8", 3 GB (reports ~2.8 GiB), iOS 18 → OFF (memory)',
   fed('ios', 18, { totalMemoryBytes: 2.8 * GB, model: 'iPhone11,8', isMac: false }), { transparency: 'off', reason: 'lowMemory' });
eq('iPhone 15 "iPhone15,4", 6 GB (reports ~5.6 GiB), iOS 26 → ON',
   fed('ios', 26, { totalMemoryBytes: 5.6 * GB, model: 'iPhone15,4', isMac: false }), { transparency: 'on', reason: null });
eq('iPhone X "iPhone10,3" on iOS 16 → OFF (memory first: it has 3 GB)',
   fed('ios', 16, { totalMemoryBytes: 2.8 * GB, model: 'iPhone10,3', isMac: false }).reason, 'lowMemory');
eq('iPhone 8 Plus "iPhone10,5" with 3 GB-plus reported → OFF by model',
   fed('ios', 16, { totalMemoryBytes: 3.6 * GB, model: 'iPhone10,5', isMac: false }).reason, 'oldModel');
eq('2 GB Android (reports ~1.8 GiB), Android 11 → OFF',
   fed('android', 30, { totalMemoryBytes: 1.8 * GB, model: 'samsung SM-A125F', isMac: false }), { transparency: 'off', reason: 'lowMemory' });
eq('8 GB Android 14 (reports ~7.4 GiB) → ON',
   fed('android', 34, { totalMemoryBytes: 7.4 * GB, model: 'Google Pixel 8', isMac: false }), { transparency: 'on', reason: null });
eq('an Android "model" never trips the iOS model rule',
   fed('android', 34, { totalMemoryBytes: 7.4 * GB, model: 'iPhone10,3', isMac: false }).transparency, 'on');
// ★★ A Mac is never downgraded by memory, model or version — only by its own Reduce Transparency.
eq('Mac "Mac14,2", 8 GB → ON', fed('ios', 15, { totalMemoryBytes: 8 * GB, model: 'Mac14,2', isMac: true }, { isPad: true }).transparency, 'on');
eq('Mac with a (spoofed) pre-A12 identifier and little memory → still ON',
   fed('ios', 14, { totalMemoryBytes: 2 * GB, model: 'iPad7,11', isMac: true }).transparency, 'on');
eq('Mac on an iPhone-idiom version below 17 → still ON',
   fed('ios', 15, { totalMemoryBytes: 16 * GB, model: 'MacBookPro18,3', isMac: true }).transparency, 'on');
eq('Mac with Reduce Transparency → OFF (the user\'s own words)',
   fed('ios', 15, { totalMemoryBytes: 16 * GB, model: 'Mac14,2', isMac: true }, { reduceTransparency: true }).reason, 'reduceTransparency');

// The guard: no getter (old binary, Expo Go, web, tests) or garbage decides nothing.
eq('no getter → nothing decided', parseDeviceClass(undefined), { totalMemoryBytes: null, modelId: null, isMac: false });
eq('null → nothing decided', parseDeviceClass(null), { totalMemoryBytes: null, modelId: null, isMac: false });
eq('garbage fields → nothing decided',
   parseDeviceClass({ totalMemoryBytes: 'lots', model: 42, isMac: 'yes' }), { totalMemoryBytes: null, modelId: null, isMac: false });
eq('0 / negative / NaN / Infinity memory → null',
   [0, -1, NaN, Infinity].map(m => parseDeviceClass({ totalMemoryBytes: m }).totalMemoryBytes), [null, null, null, null]);
eq('blank model → null', parseDeviceClass({ model: '  ' }).modelId, null);
eq('no getter on an iPhone left on iOS 16 still falls back to the version rule', fed('ios', 16, undefined).reason, 'oldIos');
eq('no getter on a current iPhone → ON', fed('ios', 26, undefined).transparency, 'on');
{
  const src = readFileSync(new URL('../src/contexts/FaceplateContext.tsx', import.meta.url), 'utf8');
  ok('FaceplateContext feeds the getter into the signals', /parseDeviceClass\(readNativeDeviceClass\(\)\)/.test(src));
  const svc = readFileSync(new URL('../src/services/deviceClass.ts', import.meta.url), 'utf8');
  ok('deviceClass.ts guards a missing method and a throwing call',
     /typeof fn === 'function'/.test(svc) && /catch/.test(svc));
  const mm = readFileSync(new URL('../modules/vibe-local-sdr/VibeLocalSDR.mm', import.meta.url), 'utf8');
  ok('iOS exports deviceClass synchronously', /RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD\(deviceClass\)/.test(mm));
  const kt = readFileSync(new URL('../android/app/src/main/java/com/vibesdr/app/VibeLocalSdrModule.kt', import.meta.url), 'utf8');
  ok('Android exports deviceClass synchronously', /isBlockingSynchronousMethod = true\)\s*fun deviceClass\(\)/.test(kt));
}

// ── A stored choice always wins; the auto default is never saved as if chosen ─

const AUTO_OFF = autoTransparency({ ...FAST, reduceTransparency: true });
const AUTO_ON  = autoTransparency(FAST);
eq('never chosen + low-end → OFF on screen', effectiveTransparency(DEFAULT_SETTINGS, AUTO_OFF), 'off');
eq('never chosen + fast → ON on screen', effectiveTransparency(DEFAULT_SETTINGS, AUTO_ON), 'on');
const choseOn  = withTransparency(DEFAULT_SETTINGS, 'on');
const choseOff = withTransparency(DEFAULT_SETTINGS, 'off');
eq('chose ON on a low-end device → ON', effectiveTransparency(choseOn, AUTO_OFF), 'on');
eq('chose OFF on a fast device → OFF', effectiveTransparency(choseOff, AUTO_ON), 'off');
ok('a pick is explicit', choseOn.transparencyExplicit && choseOff.transparencyExplicit);
ok('re-picking the same value is a no-op (no write)', withTransparency(choseOff, 'off') === choseOff);
// The stored object while the device decides: saving and reloading it must NOT turn the device's OFF
// into the user's — the same JSON on a fast phone (a restored backup) is ON again.
const storedWhileAuto = parseSettings(JSON.stringify(DEFAULT_SETTINGS));
ok('the auto default is never saved as chosen', storedWhileAuto.transparencyExplicit === false);
eq('…so the same stored JSON on a fast phone is ON', effectiveTransparency(storedWhileAuto, AUTO_ON), 'on');
eq('…and on a slow one OFF', effectiveTransparency(storedWhileAuto, AUTO_OFF), 'off');
eq('a pick round-trips through storage', parseSettings(JSON.stringify(choseOff)).transparency, 'off');
ok('…still explicit', parseSettings(JSON.stringify(choseOff)).transparencyExplicit);
// The theme is resolved from the EFFECTIVE value (FaceplateContext substitutes it).
ok('theme.opaque follows transparency off', resolveFaceplate({ ...DEFAULT_SETTINGS, transparency: 'off' }).opaque);
ok('theme.opaque false when on', !resolveFaceplate({ ...DEFAULT_SETTINGS, transparency: 'on' }).opaque);

// ── Migration from row 8's decoderBg ─────────────────────────────────────────

const migSolid = parseSettings(JSON.stringify({ chassis: 'silver', decoderBg: 'solid' }));
eq('decoderBg solid → OFF, chosen', [migSolid.transparency, migSolid.transparencyExplicit], ['off', true]);
const migTrans = parseSettings(JSON.stringify({ chassis: 'default', decoderBg: 'transparent' }));
eq('decoderBg transparent → ON, NOT chosen (it was the written-back default)',
   [migTrans.transparency, migTrans.transparencyExplicit], ['on', false]);
ok('the migrated object no longer carries decoderBg', !('decoderBg' in migSolid));
eq('garbage transparency with explicit → not a choice',
   parseSettings(JSON.stringify({ transparency: 'maybe', transparencyExplicit: true })).transparencyExplicit, false);
eq('the new keys win over a stale decoderBg',
   parseSettings(JSON.stringify({ decoderBg: 'solid', transparency: 'on', transparencyExplicit: true })).transparency, 'on');

// ── OFF's colours: alpha 1.0, and "the same as now, just not see-through" ──────

type RGB = [number, number, number];
function rgbOf(c: string): { rgb: RGB; a: number } {
  if (c.startsWith('#')) return { rgb: [1, 3, 5].map(i => parseInt(c.slice(i, i + 2), 16)) as RGB, a: 1 };
  const p = c.match(/rgba?\(([^)]+)\)/)![1].split(',').map(Number);
  return { rgb: [p[0], p[1], p[2]], a: p.length > 3 ? p[3] : 1 };
}
function over(c: string, bg: RGB): RGB {
  const { rgb, a } = rgbOf(c);
  return rgb.map((v, i) => v * a + bg[i] * (1 - a)) as RGB;
}
function lum(c: RGB): number {
  const f = (v: number) => { const x = v / 255; return x <= 0.04045 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4; };
  return 0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2]);
}
const ratio = (a: RGB, b: RGB) => (Math.max(lum(a), lum(b)) + 0.05) / (Math.min(lum(a), lum(b)) + 0.05);

eq('solidOver: 50 % white over black', solidOver('rgba(255,255,255,0.5)'), 'rgb(128,128,128)');
eq('solidOver: opaque stays itself', solidOver('#102030'), 'rgb(16,32,48)');
eq('solidOver: over a base', solidOver('rgba(0,0,0,0.5)', [200, 100, 0]), 'rgb(100,50,0)');
eq('decoderTokens and faceplate agree on the default box', DEFAULT_SOLID_BG, solidOver('rgba(10,8,4,0.72)'));

/** Every surface OFF turns opaque: what it was (the glass), and what OFF draws. */
/** `max`: the luminance ratio OFF may differ from the glass by on a dark floor. 1.10 for the panels.
 *  ★ The deck's 1 pt RING gets 1.25: at 30 % white it is the one token bright enough for Jet's navy
 *  floor (#00008f) to show through it as a lighter blue edge (1.21:1). That navy is exactly what "not
 *  see-through" removes; over every black floor it is identical. */
const SURFACES: Array<{ name: string; glass: string; solid: string; max: number }> = [
  { name: 'deck island tint', glass: DEFAULT_CHASSIS.deckTint, solid: DEFAULT_CHASSIS.deckSolid, max: 1.10 },
  { name: 'deck ring',        glass: DEFAULT_CHASSIS.barBorder, solid: DEFAULT_CHASSIS.barBorderSolid, max: 1.25 },
  { name: 'decoder box',      glass: 'rgba(10,8,4,0.72)', solid: decoderTokensFor('default', '61,255,114', 'off').solidBg!, max: 1.10 },
  { name: 'menu sheet',       glass: 'rgba(6,4,2,0.60)',  solid: solidOver('rgba(6,4,2,0.60)'), max: 1.10 },
];
// The waterfall floors: the first stop of every colour map. "Dark" = the ones a see-through panel
// was designed over (all but Black Hot's white and Kiwi Linear's green).
const floors = Object.entries(COLORMAPS_STOPS).map(([name, stops]) => ({ name, rgb: rgbOf(stops[0]).rgb }));
const dark = floors.filter(f => lum(f.rgb) < 0.02);
ok('Jet (the default map) is among the dark floors', dark.some(f => f.name === 'Jet'));
for (const s of SURFACES) {
  ok(`${s.name}: OFF is alpha 1.0 exactly (${s.solid})`, rgbOf(s.solid).a === 1 && s.solid.startsWith('rgb('));
  const solid = rgbOf(s.solid).rgb;
  let worst = 1, worstOn = '';
  for (const f of floors) {
    // (Rounded to 8-bit, as the screen shows it — OFF's own channels are rounded the same way.)
    const glass = over(s.glass, f.rgb).map(Math.round) as RGB;
    // Composited over black, OFF can never be LIGHTER than the glass was over anything.
    ok(`${s.name} OFF never lighter than the glass over ${f.name}`, lum(solid) <= lum(glass) + 1e-9);
    if (dark.includes(f)) {
      const r = ratio(solid, glass);
      if (r > worst) { worst = r; worstOn = f.name; }
    }
  }
  ok(`${s.name}: OFF within ${s.max}:1 of the glass on every dark floor (worst ${worst.toFixed(3)} on ${worstOn})`, worst <= s.max);
  console.log(`  note: ${s.name} OFF ${s.solid} — worst ${worst.toFixed(3)}:1 against the glass (${worstOn})`);
}
ok('silver / black decks are opaque plates already (OFF changes nothing there)',
   SILVER_CHASSIS.plate != null && BLACK_CHASSIS.plate != null);
ok('the pane subtitle is the brief\'s', TRANSPARENCY_NOTE === 'Off · solid panels, easier to read and lighter on older devices');

// ── OFF's surface contract (row 10's PopupShell reads the same object) ───────

{
  const on = surfaceTokens(false), off = surfaceTokens(true);
  eq('ON: today — blur, scrim, shadows, colours untouched',
     [on.opaque, on.blur, on.scrimOpacity, on.dropShadow, on.fill('rgba(6,4,2,0.6)')],
     [false, true, 1, true, 'rgba(6,4,2,0.6)']);
  eq('OFF: opaque, no blur, NO SCRIM, no drop shadow', [off.opaque, off.blur, off.scrimOpacity, off.dropShadow],
     [true, false, 0, false]);
  eq('OFF fill = solidOver (alpha 1.0)', off.fill('rgba(6,4,2,0.6)'), 'rgb(4,2,1)');
  ok('theme.surface is the OFF contract when off', resolveFaceplate({ ...DEFAULT_SETTINGS, transparency: 'off' }).surface === off);
  ok('theme.surface is today when on', resolveFaceplate({ ...DEFAULT_SETTINGS, transparency: 'on' }).surface === on);
  eq('NO_DROP_SHADOW takes both platforms\' shadows', NO_DROP_SHADOW, { shadowOpacity: 0, shadowRadius: 0, elevation: 0 });
}
// The surfaces this row owns that cast a shadow or a dim over the spectrum must switch them off.
// (Row 10 extends the list to every popup.) ★ Source checks: a render test would need a device.
{
  const src = (f: string) => readFileSync(new URL(`../src/${f}`, import.meta.url), 'utf8');
  ok('ControlsBar: the deck drops its shadow with OFF', /fpTheme\.opaque \? NO_DROP_SHADOW/.test(src('components/ControlsBar.tsx')));
  ok('DecoderShell: the boxes drop their shadow with OFF', /opaque && NO_DROP_SHADOW/.test(src('components/DecoderShell.tsx')));
  // ★ No scrim at all now, ON or OFF (2026-10-04, test_popup.ts) — the tap-to-close view stays.
  ok('MenuSheet: no scrim, ON or OFF (the tap-to-close view stays)', !/styles\.backdrop\b/.test(src('components/MenuSheet.tsx'))
     && /<TouchableWithoutFeedback onPress=\{onClose\}>/.test(src('components/MenuSheet.tsx')));
  ok('LocalHardwarePanel: no scrim with OFF', /fp\.opaque && styles\.backdropNone/.test(src('components/LocalHardwarePanel.tsx')));
}

// ── Every BlurView reads the switch ──────────────────────────────────────────
// ★ A static guard, not a render: a file that draws <BlurView must also read OFF (useSurfaceOpaque /
//   theme.opaque / a solid token), or a new blur has slipped past the one switch.
function walk(dir: string, out: string[] = []): string[] {
  for (const e of readdirSync(dir)) {
    const p = join(dir, e);
    if (statSync(p).isDirectory()) walk(p, out); else if (/\.tsx?$/.test(e)) out.push(p);
  }
  return out;
}
const blurFiles = walk(new URL('../src', import.meta.url).pathname).filter(f => /<BlurView\b/.test(readFileSync(f, 'utf8')));
ok('there are BlurViews to guard (the scan works)', blurFiles.length > 0);
for (const f of blurFiles) {
  const src = readFileSync(f, 'utf8');
  ok(`${f.replace(/.*\/src\//, 'src/')} reads Transparency OFF`, /useSurfaceOpaque\(|\.opaque\b|solidBg|solidDeck/.test(src));
}

console.log(`transparency: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
