/**
 * Faceplate LIGHTING and VFD GLASS (briefs/BRIEF-lighting-and-vfd-glass.md) — the pure rules:
 *   §1 filament wires: count by HEIGHT, even spacing, snapped to the device-pixel grid;
 *   §2 the one light angle: 104° (LEFT) reproduces today's numbers exactly — sheen, gloss, hot-spot, screws;
 *   §3 MOTION EFFECTS: follows the OS until picked, then the pick wins both ways;
 *   §4 LIGHT ANGLE: the five keys, and the row shown only where it is the light in use.
 *
 * Run: node --no-warnings scripts/test_faceplate_lighting.ts   (run-tests.sh does)
 */
import { filamentCount, filamentYs, devicePixel } from '../src/constants/vfdGlass.ts';
import { DEFAULT_SETTINGS, effectiveMotion, withMotion, parseSettings, MOTION_CHOICES,
  LIGHT_ANGLES, LIGHT_ANGLE_DEG, LIGHT_ANGLE_CHOICES, lightAngleRowShown, CHASSIS } from '../src/constants/faceplate.ts';
import { cssAnglePts, glossAngle, hotspotX, HOTSPOT_Y, screwHighlight, LIGHT_DEFAULT_DEG } from '../src/constants/plateLight.ts';

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

console.log(`faceplate lighting: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
