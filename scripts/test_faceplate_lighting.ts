/**
 * Faceplate LIGHTING and VFD GLASS (briefs/BRIEF-lighting-and-vfd-glass.md) — the pure rules:
 *   §1 filament wires: count by HEIGHT, even spacing, snapped to the device-pixel grid;
 *   §2 the one light angle: 104° (LEFT) reproduces today's numbers exactly — sheen, gloss, hot-spot, screws;
 *   §3 MOTION EFFECTS: follows the OS until picked, then the pick wins both ways;
 *   §4 LIGHT ANGLE: the five keys, and the row shown only where it is the light in use;
 *   §5 TILT: gravity → screen tilt in every orientation, the filters, the clamps, the write gate, the screws'
 *      settle, the synthetic source (identical every run, through the same path), the ladder and the run rule.
 *
 * Run: node --no-warnings scripts/test_faceplate_lighting.ts   (run-tests.sh does)
 */
import { filamentCount, filamentYs, devicePixel } from '../src/constants/vfdGlass.ts';
import { DEFAULT_SETTINGS, effectiveMotion, withMotion, parseSettings, MOTION_CHOICES,
  LIGHT_ANGLES, LIGHT_ANGLE_DEG, LIGHT_ANGLE_CHOICES, lightAngleRowShown, CHASSIS } from '../src/constants/faceplate.ts';
import { cssAnglePts, cssAnglePtsShifted, glossAngle, hotspotX, HOTSPOT_Y, screwHighlight, LIGHT_DEFAULT_DEG } from '../src/constants/plateLight.ts';
import { TILT, tiltFromGravity, lowPass, lightFromTilt, shouldWrite, screwStep, syntheticGravity, tiltRungFor,
  rungWriteMs, tiltShouldRun } from '../src/constants/tiltLight.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);
const near = (a: number, b: number, tol = 1e-9) => Math.abs(a - b) <= tol;

// ── §1 Filament wires ────────────────────────────────────────────────────────
eq('today\'s windows get 2 wires (40 pt)', filamentCount(40), 2);
eq('a 20 pt strip still gets 2', filamentCount(20), 2);
eq('by HEIGHT: 66 pt → 3', filamentCount(66), 3);
eq('nothing measured → no wires', filamentCount(0), 0);
eq('NaN height → no wires', filamentYs(NaN, 3), []);
for (const pr of [1, 2, 3, 2.625]) {
  for (const h of [19, 20, 29, 35, 38, 40, 46, 48, 66]) {
    const ys = filamentYs(h, pr);
    ok(`h ${h} @${pr}x: count ${ys.length} = filamentCount`, ys.length === filamentCount(h));
    ok(`h ${h} @${pr}x: every wire on a device pixel (no sub-pixel smear)`,
       ys.every(y => near(y * pr, Math.round(y * pr), 1e-6)));
    ok(`h ${h} @${pr}x: inside the glass, ascending`, ys.every((y, i) => y > 0 && y < h && (i === 0 || y > ys[i - 1])));
    ok(`h ${h} @${pr}x: within half a device pixel of i/(n+1)`,
       ys.every((y, i) => Math.abs(y - ((i + 1) * h) / (ys.length + 1)) <= 0.5 / pr + 1e-9));
  }
}
eq('40 pt @3x: ⅓ and ⅔, snapped', filamentYs(40, 3), [13.333333333333334, 26.666666666666668]);
eq('one device pixel @3x', devicePixel(3), 1 / 3);
eq('one device pixel, bad ratio → 1 pt', devicePixel(0), 1);

// ── §2 One light angle: LEFT (104°) is today, exactly ─────────────────────────
/** ChassisPlate's cssAngle as it stood before the light became shared — the reference. */
function oldCssAngle(deg: number, w: number, h: number) {
  const a = (deg * Math.PI) / 180;
  const dx = Math.sin(a), dy = -Math.cos(a);
  const len = Math.abs(w * dx) + Math.abs(h * dy);
  const cx = w / 2, cy = h / 2;
  return { sx: cx - (dx * len) / 2, sy: cy - (dy * len) / 2, ex: cx + (dx * len) / 2, ey: cy + (dy * len) / 2 };
}
eq('default angle is LEFT, 104°', LIGHT_DEFAULT_DEG, 104);
for (const [w, h] of [[390, 210], [620, 180], [1180, 120], [300, 600]]) {
  eq(`sheen at 104°, ${w}×${h}: identical to today's cssAngle(104)`, cssAnglePts(104, w, h), oldCssAngle(104, w, h));
  eq(`gloss at 104°, ${w}×${h}: identical to today's cssAngle(112)`, cssAnglePts(glossAngle(104), w, h), oldCssAngle(112, w, h));
}
eq('hot-spot at 104°: today\'s 28 %, exactly', hotspotX(104), 0.28);
eq('hot-spot y stays at −10 %', HOTSPOT_Y, -0.10);
eq('screw highlight at 104°: today\'s 35 % 30 %, exactly', screwHighlight(104), { fx: 0.35, fy: 0.30 });
ok('hot-spot: RIGHT (256°) mirrors LEFT', near(hotspotX(256), 0.72, 1e-12));
ok('hot-spot: TOP (180°) is centred', near(hotspotX(180), 0.5, 1e-12));
ok('screw: RIGHT mirrors LEFT (65 %)', near(screwHighlight(256).fx, 0.65, 1e-12));
ok('screw: TOP centred', near(screwHighlight(180).fx, 0.5, 1e-12));
for (const d of [104, 135, 180, 225, 256]) {
  ok(`screw at ${d}°: lit from ABOVE (fy 30 %)`, screwHighlight(d).fy === 0.30);
  ok(`hot-spot at ${d}°: on the lit side`, (d < 180 ? hotspotX(d) < 0.5 : d > 180 ? hotspotX(d) > 0.5 : near(hotspotX(d), 0.5, 1e-12)));
}
ok('gloss keeps its 8° offset at every angle', [104, 135, 180, 225, 256].every(d => glossAngle(d) === d + 8));

// ── §3 MOTION EFFECTS: follows the OS until picked; the pick wins both ways ────
eq('default: not picked, ON', [DEFAULT_SETTINGS.motionEffects, DEFAULT_SETTINGS.motionExplicit], ['on', false]);
eq('unpicked + OS reduce motion OFF → on', effectiveMotion(DEFAULT_SETTINGS, false), 'on');
eq('unpicked + OS reduce motion ON → off', effectiveMotion(DEFAULT_SETTINGS, true), 'off');
const pickedOn = withMotion(DEFAULT_SETTINGS, 'on'), pickedOff = withMotion(DEFAULT_SETTINGS, 'off');
eq('a pick is explicit', [pickedOn.motionExplicit, pickedOff.motionExplicit], [true, true]);
eq('picked ON beats the OS switch', effectiveMotion(pickedOn, true), 'on');
eq('picked OFF beats the OS switch', effectiveMotion(pickedOff, false), 'off');
ok('re-picking the same value is a no-op (same object)', withMotion(pickedOff, 'off') === pickedOff);
eq('stored pick round-trips', parseSettings(JSON.stringify(pickedOff)).motionEffects, 'off');
eq('stored pick stays explicit', parseSettings(JSON.stringify(pickedOff)).motionExplicit, true);
eq('an older store (no motion keys) = not picked', [parseSettings('{"chassis":"silver"}').motionEffects,
   parseSettings('{"chassis":"silver"}').motionExplicit], ['on', false]);
eq('an unpicked "off" in storage is NOT a pick', parseSettings('{"motionEffects":"off"}').motionExplicit, false);
eq('a junk value is not a pick', parseSettings('{"motionEffects":"wobble","motionExplicit":true}').motionExplicit, false);
eq('MOTION EFFECTS keys', MOTION_CHOICES.map(c => c.label), ['ON', 'OFF']);

// ── §4 LIGHT ANGLE ───────────────────────────────────────────────────────────
eq('five keys, in order', LIGHT_ANGLE_CHOICES.map(c => c.label), ['LEFT', 'TOP-LEFT', 'TOP', 'TOP-RIGHT', 'RIGHT']);
eq('the angles', LIGHT_ANGLES.map(a => LIGHT_ANGLE_DEG[a]), [104, 135, 180, 225, 256]);
eq('default is LEFT', DEFAULT_SETTINGS.lightAngle, 'left');
eq('LEFT is today (104° = LIGHT_DEFAULT_DEG)', LIGHT_ANGLE_DEG.left, LIGHT_DEFAULT_DEG);
ok('RIGHT mirrors LEFT about TOP', LIGHT_ANGLE_DEG.left + LIGHT_ANGLE_DEG.right === 2 * LIGHT_ANGLE_DEG.top);
ok('TOP-LEFT / TOP-RIGHT mirror about TOP', LIGHT_ANGLE_DEG.topLeft + LIGHT_ANGLE_DEG.topRight === 2 * LIGHT_ANGLE_DEG.top);
ok('never lit from below (every angle between 90° and 270°)', LIGHT_ANGLES.every(a => LIGHT_ANGLE_DEG[a] > 90 && LIGHT_ANGLE_DEG[a] < 270));
eq('stored angle round-trips', parseSettings(JSON.stringify({ ...DEFAULT_SETTINGS, lightAngle: 'topRight' })).lightAngle, 'topRight');
eq('an older store → LEFT', parseSettings('{"chassis":"black"}').lightAngle, 'left');
eq('a junk angle → LEFT', parseSettings('{"lightAngle":"below"}').lightAngle, 'left');
for (const c of CHASSIS) {
  eq(`row on ${c}, no tilt`, lightAngleRowShown(c, false), c !== 'default');
  eq(`row on ${c}, tilt driving → hidden`, lightAngleRowShown(c, true), false);
}

// ── §5 Tilt ───────────────────────────────────────────────────────────────────
{
  const G = 9.80665, d2r = Math.PI / 180;
  // A phone tilted 10° right-edge-down in portrait.
  const g = { x: G * Math.sin(10 * d2r), y: 0, z: -G * Math.cos(10 * d2r) };
  const t0 = tiltFromGravity(g, 0, 1, 1);
  ok('portrait: right edge down = +roll', near(t0.roll, 10, 1e-6) && near(t0.pitch, 0, 1e-6));
  // The same device-frame tilt read in landscape is a PITCH on screen, and in 180 it is mirrored.
  const t90 = tiltFromGravity(g, 90, 1, 1), t180 = tiltFromGravity(g, 180, 1, 1), tm90 = tiltFromGravity(g, -90, 1, 1);
  ok('landscape 90: device roll → screen pitch', near(t90.roll, 0, 1e-6) && near(Math.abs(t90.pitch), 10, 1e-6));
  ok('landscape -90 is the opposite of 90', near(tm90.pitch, -t90.pitch, 1e-6));
  ok('upside down mirrors roll', near(t180.roll, -10, 1e-6));
  ok('flat on a desk = no tilt', (() => { const t = tiltFromGravity({ x: 0, y: 0, z: -G }, 0, 1, 1); return near(t.roll, 0) && near(t.pitch, 0); })());
  ok('gravity-only input is all it needs (no gyro)', Number.isFinite(t0.roll));

  // Filters: frame-rate independent, and the time constants are the brief's.
  ok('low-pass: one tau reaches 63 %', near(lowPass(0, 1, TILT.lpTauMs, TILT.lpTauMs), 1 - Math.exp(-1), 1e-12));
  ok('low-pass: two half-steps = one step', near(lowPass(lowPass(0, 1, 75, 150), 1, 75, 150), lowPass(0, 1, 150, 150), 1e-12));
  ok('low-pass: dt 0 changes nothing', lowPass(0.3, 1, 0, 150) === 0.3);
  eq('brief numbers: 30 Hz, 150 ms, 4 s', [TILT.intervalMs, TILT.lpTauMs, TILT.baseTauMs], [33, 150, 4000]);

  // Mapping and clamps.
  eq('no tilt = the stored angle, no shift', lightFromTilt(0, 0, 104), { deg: 104, shift: 0 });
  eq('roll +20 → +30°', lightFromTilt(20, 0, 104).deg, 134);
  eq('roll clamps at ±20', lightFromTilt(45, 0, 104).deg, 134);
  eq('pitch +20 → +10 % shift', lightFromTilt(0, 20, 104).shift, 0.10);
  eq('pitch clamps', lightFromTilt(0, -90, 180).shift, -0.10);
  eq('tilt swings around TOP when TOP is stored', lightFromTilt(-20, 0, 180).deg, 150);

  // The write gate: > 0.5° or > 0.005 shift, and not faster than the rung allows.
  ok('a 0.4° move is not written', !shouldWrite(104, 0, 104.4, 0, 0, 1000, 33));
  ok('a 0.6° move is written', shouldWrite(104, 0, 104.6, 0, 0, 1000, 33));
  ok('…but not 20 ms after the last write', !shouldWrite(104, 0, 110, 0, 990, 1010, 33));
  ok('a shift move alone is written', shouldWrite(104, 0, 104, 0.01, 0, 1000, 33));
  eq('rung 0/1 write at 30 Hz, rung 2 at 20 Hz', [rungWriteMs(0), rungWriteMs(1), rungWriteMs(2)], [33, 33, 50]);
  ok('shift 0 = today\'s sheen points exactly', JSON.stringify(cssAnglePtsShifted(104, 380, 220, 0)) === JSON.stringify(cssAnglePts(104, 380, 220)));
  {
    const a = cssAnglePts(104, 380, 220), b = cssAnglePtsShifted(104, 380, 220, 0.1);
    ok('shift slides BOTH ends by 10 % of the gradient', near(b.sx - a.sx, (a.ex - a.sx) * 0.1, 1e-9) && near(b.ey - a.ey, (a.ey - a.sy) * 0.1, 1e-9));
  }

  // The screws: follow only a change of ≥ 3° held for 200 ms.
  let st = { shown: 104, cand: 104, since: 0 };
  let r = screwStep(st, 106, 0); st = r.st;
  ok('a 2° move never moves the screws', r.emit === null);
  r = screwStep(st, 110, 100); st = r.st;
  ok('a 6° move starts the hold, no emit yet', r.emit === null);
  r = screwStep(st, 111, 250); st = r.st;
  ok('still within 3° of the candidate at 150 ms: wait', r.emit === null);
  r = screwStep(st, 111, 320); st = r.st;
  eq('held 220 ms: the screws follow', r.emit, 111);
  r = screwStep(st, 115, 330); r = screwStep(r.st, 120, 400); r = screwStep(r.st, 126, 650);
  ok('a light still MOVING never settles the screws', r.emit === null);

  // The synthetic source: deterministic, inside the clamps, and through the same gravity path.
  const s1 = syntheticGravity(1234), s2 = syntheticGravity(1234);
  ok('synthetic is identical every run', JSON.stringify(s1) === JSON.stringify(s2));
  ok('synthetic is a real gravity vector (|g| = g)', near(Math.hypot(s1.x, s1.y, s1.z), 9.80665, 1e-6));
  {
    let maxR = 0, maxP = 0;
    for (let t = 0; t < 18000; t += 33) { const tt = tiltFromGravity(syntheticGravity(t), 0, 1, 1); maxR = Math.max(maxR, Math.abs(tt.roll)); maxP = Math.max(maxP, Math.abs(tt.pitch)); }
    ok('synthetic roll reaches ±15° (inside the ±20 clamp)', near(maxR, 15, 0.05));
    ok('synthetic pitch reaches ±10°', near(maxP, 10, 0.05));
  }

  // The ladder and the run rule.
  eq('Mac → rung 3 (no sensor: the LIGHT ANGLE row)', tiltRungFor({ isMac: true }), 3);
  eq('TV → rung 3', tiltRungFor({ isTV: true }), 3);
  eq('a phone starts on rung 0 (until measured)', tiltRungFor({}), 0);
  eq('an iPhone is rung 0', tiltRungFor({ os: 'ios' }), 0);
  eq('Android held at rung 3 until the XCover is measured (B19)', tiltRungFor({ os: 'android' }), 3);
  const base = { chassis: 'silver', motionOn: true, sensor: true, active: true, deckVisible: true, rung: 0 as const };
  ok('runs: silver, motion on, sensor, active, deck shown', tiltShouldRun(base));
  ok('black runs too', tiltShouldRun({ ...base, chassis: 'black' }));
  ok('NEVER on the default chassis', !tiltShouldRun({ ...base, chassis: 'default' }));
  ok('MOTION EFFECTS off → no sensor', !tiltShouldRun({ ...base, motionOn: false }));
  ok('no sensor → off (the row shows)', !tiltShouldRun({ ...base, sensor: false }));
  ok('background / screen locked → off', !tiltShouldRun({ ...base, active: false }));
  ok('controls hidden → off', !tiltShouldRun({ ...base, deckVisible: false }));
  ok('rung 3 → off', !tiltShouldRun({ ...base, rung: 3 }));
}


// ── The B19 perf overlay's text (src/constants/perfOverlay.ts) ──────────────────────────────────────
{
  const { perfLines } = await import('../src/constants/perfOverlay.ts');
  const l = perfLines({ cpuPct: 12.4, footprintMB: 412.6, uiFps: 59.8, uiP50Ms: 16.7, uiP90Ms: 18.04 },
                      { running: 'sensor', writesPerSec: 12.2, rendersPerSec: 0 });
  eq('perf: CPU and RAM line', l[0], 'CPU 12%  RAM 413 MB');
  eq('perf: UI line', l[1], 'UI 60fps p50 16.7 p90 18.0 ms');
  eq('perf: tilt line', l[2], 'TILT sensor  12 w/s  0 renders/s');
  eq('perf: off when nothing drives the light', perfLines({ cpuPct: 0, footprintMB: 0, uiFps: 0, uiP50Ms: 0, uiP90Ms: 0 },
     { running: '', writesPerSec: 0, rendersPerSec: 0 })[2], 'TILT off  0 w/s  0 renders/s');
}

console.log(`faceplate lighting: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
