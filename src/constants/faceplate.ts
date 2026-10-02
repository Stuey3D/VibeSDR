/**
 * faceplate.ts — the FACEPLATE tokens and the ONE resolver (brief §1, §2, §12).
 *
 * "I don't want colour changes to look like a palette swap on a piece of software" — so a
 * faceplate is not a palette. It is five settings (chassis, display, controls colour, text colour,
 * signal meter) plus TRANSPARENCY EFFECTS, resolved HERE, once, into the tokens the deck draws
 * with. Components never read the colour tables below directly: ControlsBar, DrumWheel,
 * TunerKeys, VTSBar and DecoderShell take the resolved theme from FaceplateContext.
 *
 * ★★★ DEFAULT SETTINGS RENDER TODAY'S APP, PIXEL FOR PIXEL (acceptance §13.1). Every token in
 *   DEFAULT_CHASSIS below is the literal that used to sit in ControlsBar / DrumWheel / TunerKeys /
 *   VTSBar, moved, not restyled. If you change one, you have changed the default look.
 *   ★ Two places where the mockup and brief describe the default deck differently from what ships
 *     today (digits #f4f1ea, legends #f1ede4; brief §3.1) — today's values are kept, because the
 *     acceptance test for the default deck is "identical to today's build".
 *
 * ★★★ THE NIXIE RULE (§2, non-negotiable): anything drawn in Nixie One is neon orange, always. It is
 *   enforced in resolveTextColour() and in the key-legend choice below — never at a call site.
 *
 * Pure: no React, no storage — so scripts/test_faceplate.ts can check every rule directly.
 */

import type { Transparency } from './transparency';

// ── Settings ──────────────────────────────────────────────────────────────────

export type Chassis          = 'default' | 'silver' | 'black';
export type DisplayStyle     = 'hyper' | 'nixie' | 'dot' | 'seg';
export type ControlsColour   = 'green' | 'red' | 'amber' | 'blue' | 'white' | 'neon';
export type TextColour       = 'green' | 'red' | 'amber' | 'blue' | 'white' | 'teal';
export type SignalMeter      = 'bar' | 'vu' | 'edge';
/** ★ FRAME RATE (power audit, 2026-10-01): `full` = whatever the display can do (120 Hz on a
 *  ProMotion iPhone, 90 / 120 / 144 on a fast Android) — today's behaviour; `60` = capped at 60 Hz,
 *  for battery. See `frameRateChoices` for when the row is offered at all. */
export type FrameRate        = 'full' | '60';
/** ★★★ TRANSPARENCY EFFECTS (Stuart, 2026-09-30) — ONE switch for every see-through surface: the
 *  default deck, every decoder box, and (row 10, PopupShell) the menus, sheets and chat. It replaced
 *  the decoder boxes' own Transparent / Solid row. `off` = alpha 1.0 EXACTLY and no BlurView anywhere
 *  ("I thought solid would be 1.0 fully solid for max GPU savings"). */
export type { Transparency };
/** ★★ MOTION EFFECTS (lighting brief §3): `off` turns off DECORATIVE motion only — the tilt light, the LED
 *  VU's edge shimmer (steady LEDs are forced), the needle's overshoot, any future power-on effect. It NEVER
 *  touches motion that carries information: the VFD's stepped scroll, needles following the signal,
 *  dome-key presses, Nixie digit changes and afterglow. */
export type MotionEffects    = 'on' | 'off';
/** ★★ LIGHT ANGLE (lighting brief §4): where the light on the metal comes from. Never from below — hardware
 *  is never lit from below, and it reads as wrong. */
export type LightAngle       = 'left' | 'topLeft' | 'top' | 'topRight' | 'right';

export interface FaceplateSettings {
  chassis:   Chassis;
  display:   DisplayStyle;
  controls:  ControlsColour;
  text:      TextColour;
  meter:     SignalMeter;
  /** The user's choice — meaningful ONLY when `transparencyExplicit`. Otherwise the device decides
   *  (src/constants/transparency.ts `autoTransparency`) and this field is not read: see
   *  `effectiveTransparency`. ★ FaceplateContext resolves the theme with the EFFECTIVE value
   *  substituted here, so `theme.settings.transparency` is what is on screen. */
  transparency: Transparency;
  /** ★★ Has the user ever picked ON / OFF? Until they do, low-end detection decides — and the auto
   *  default is NEVER written back as if chosen, or a phone upgraded to a fast one would stay solid. */
  transparencyExplicit: boolean;
  /** §4.4 "Steady LEDs": the LED VU's edge segment solid on/off with ~1 dB hysteresis instead of the
   *  statistical partial brightness. Stored now (the CONTROL CUSTOMISATION pane's FEEL group); the
   *  VU that reads it arrives with row 5, which also ORs in the OS Reduce Motion setting. */
  steadyLeds: boolean;
  /** ★ FRAME RATE — the display-refresh cap for everything the app animates (Skia canvases,
   *  Reanimated frame callbacks, RN's own display links). ★★ DEVICE-LOCAL like the rest of the
   *  faceplate: it is about the panel in your hand, so it is never per server and never synced.
   *  Applied natively by src/services/frameRate.ts (FaceplateContext pushes it). */
  frameRate: FrameRate;
  /** ★ The user's text colour, remembered PER DISPLAY (§1: "Remember the user's choice per display
   *  if cheap to do" — it is). Leaving Hyperlegible-white for dot matrix falls back to teal; coming
   *  back restores white rather than leaving them on teal. */
  textByDisplay: Partial<Record<DisplayStyle, TextColour>>;
  /** ★★ MOTION EFFECTS — the user's pick, meaningful ONLY when `motionExplicit`. Until then the OS decides
   *  (iOS Reduce Motion / Android Remove animations): the Transparency Effects pattern exactly — see
   *  `effectiveMotion`, and the auto value is never written back as if chosen. */
  motionEffects: MotionEffects;
  motionExplicit: boolean;
  /** ★ LIGHT ANGLE — device-local like the rest of the faceplate. LEFT is today's look. With tilt lighting
   *  (brief §5, not built yet) this is the angle the tilt swings AROUND. */
  lightAngle: LightAngle;
}

export const CHASSIS:     Chassis[]        = ['default', 'silver', 'black'];
export const DISPLAYS:    DisplayStyle[]   = ['hyper', 'nixie', 'dot', 'seg'];
export const CONTROLS:    ControlsColour[] = ['green', 'red', 'amber', 'blue', 'white', 'neon'];
export const TEXTS:       TextColour[]     = ['green', 'red', 'amber', 'blue', 'white', 'teal'];
export const METERS:      SignalMeter[]    = ['bar', 'vu', 'edge'];
export const TRANSPARENCIES: Transparency[] = ['on', 'off'];
export const FRAME_RATES: FrameRate[] = ['full', '60'];
export const MOTIONS: MotionEffects[] = ['on', 'off'];
export const LIGHT_ANGLES: LightAngle[] = ['left', 'topLeft', 'top', 'topRight', 'right'];
/** The CSS gradient angle for each (0 = upward, 90 = rightward). 104 = LEFT = today exactly; RIGHT mirrors it. */
export const LIGHT_ANGLE_DEG: Record<LightAngle, number> = {
  left: 104, topLeft: 135, top: 180, topRight: 225, right: 256,
};

/** ★★★ What the real display technology came in (§1). Nixie: none — locked neon (§2). Dot and
 *  segment VFDs never came in white. The first entry is the display's default. */
export const TEXT_ALLOWED: Record<DisplayStyle, TextColour[]> = {
  hyper: ['green', 'red', 'amber', 'blue', 'white', 'teal'],
  nixie: [],
  dot:   ['teal', 'green', 'blue', 'amber', 'red'],
  seg:   ['teal', 'green', 'blue', 'amber', 'red'],
};

/** §1 defaults. `display` is overwritten by the font migration on first load (see migrate…). */
export const DEFAULT_SETTINGS: FaceplateSettings = {
  chassis: 'default', display: 'hyper', controls: 'green', text: 'green',
  meter: 'bar', transparency: 'on', transparencyExplicit: false, steadyLeds: false, frameRate: 'full',
  textByDisplay: {}, motionEffects: 'on', motionExplicit: false, lightAngle: 'left',
};

// ── Colour tokens ─────────────────────────────────────────────────────────────

/** An LED colour: the rgb TRIPLET is the truth, everything else derives from it or is the mockup's
 *  own value. `glow` is core at α .70–.80; `hot` is the white-hot centre; `dim` the unlit tint. */
export interface LedColour {
  rgb:    string;   // '61,255,114'
  core:   string;   // '#3dff72'
  glow:   string;   // 'rgba(61,255,114,0.70)'
  hot:    string;   // '#c9ffd6'
  /** The hot centre as a triplet, for alpha ramps (TunerKeys' lit symbol). */
  hotRgb: string;
  dim:    string;
  /** ★ Only TODAY_GREEN has these: the exact hsl the drums were drawn with, so alpha ramps come out
   *  as the same strings Skia parsed before (see ledA / hotA). */
  hsl?:    [number, number, number];
  hotHsl?: [number, number, number];
}

/**
 * ★★★ A translucent colour made OPAQUE without changing how it looked: composited over `base`
 * (black by default — the dark waterfall floor it sat on), returned at alpha 1.0 EXACTLY.
 * Transparency OFF builds every surface colour with this, so the app "looks the same as now, just
 * not see-through" — never a lighter or darker panel (scripts/test_transparency.ts measures it).
 * ★ Only rgb()/rgba()/#rrggbb in; an already-opaque colour comes back as the same rgb.
 */
export function solidOver(color: string, base: [number, number, number] = [0, 0, 0]): string {
  const c = color.trim();
  let r: number, g: number, b: number, a = 1;
  if (c.startsWith('#')) {
    const h = c.slice(1);
    const n = h.length === 3 ? h.split('').map(x => x + x).join('') : h;
    r = parseInt(n.slice(0, 2), 16); g = parseInt(n.slice(2, 4), 16); b = parseInt(n.slice(4, 6), 16);
  } else {
    const m = c.match(/^rgba?\(([^)]+)\)$/);
    if (!m) throw new Error(`solidOver: cannot parse ${color}`);
    const p = m[1].split(',').map(Number);
    [r, g, b] = p;
    if (p.length > 3) a = Math.min(1, Math.max(0, p[3]));
  }
  const mix = (v: number, u: number) => Math.round(v * a + u * (1 - a));
  return `rgb(${mix(r, base[0])},${mix(g, base[1])},${mix(b, base[2])})`;
}

/** rgba() from a triplet — the only way a token should be given an alpha. */
export const rgba = (rgb: string, a: number) => `rgba(${rgb},${Math.min(1, Math.max(0, a))})`;

/** The LED at alpha `a` — what every glow, well edge and icon in the controls colour is drawn with. */
export function ledA(c: LedColour, a: number): string {
  const al = Math.min(1, Math.max(0, a));
  return c.hsl ? `hsla(${c.hsl[0]},${c.hsl[1]}%,${c.hsl[2]}%,${al})` : `rgba(${c.rgb},${al})`;
}
/** The LED's white-hot centre at alpha `a` (TunerKeys' lit symbol core). */
export function hotA(c: LedColour, a: number): string {
  const al = Math.min(1, Math.max(0, a));
  return c.hotHsl ? `hsla(${c.hotHsl[0]},${c.hotHsl[1]}%,${c.hotHsl[2]}%,${al})` : `rgba(${c.hotRgb},${al})`;
}

/** Brief §1 table, with glow and dim from the mockup's LEDS (Deck.mockup renderVals). */
export const LED: Record<ControlsColour | TextColour, LedColour> = {
  green: { rgb: '61,255,114',  core: '#3dff72', glow: 'rgba(61,255,114,0.70)', hot: '#c9ffd6', hotRgb: '201,255,214', dim: 'rgba(61,255,114,0.45)' },
  red:   { rgb: '255,58,46',   core: '#ff3a2e', glow: 'rgba(255,58,46,0.80)',  hot: '#ffcbc6', hotRgb: '255,203,198', dim: 'rgba(255,58,46,0.50)' },
  amber: { rgb: '255,174,26',  core: '#ffae1a', glow: 'rgba(255,174,26,0.80)', hot: '#ffe4b3', hotRgb: '255,228,179', dim: 'rgba(255,174,26,0.50)' },
  blue:  { rgb: '61,155,255',  core: '#3d9bff', glow: 'rgba(61,155,255,0.80)', hot: '#d0e7ff', hotRgb: '208,231,255', dim: 'rgba(61,155,255,0.50)' },
  white: { rgb: '215,228,255', core: '#eef3ff', glow: 'rgba(215,228,255,0.70)', hot: '#ffffff', hotRgb: '255,255,255', dim: 'rgba(215,228,255,0.45)' },
  teal:  { rgb: '70,255,215',  core: '#46ffd7', glow: 'rgba(70,255,215,0.70)', hot: '#c8fff2', hotRgb: '200,255,242', dim: 'rgba(70,255,215,0.45)' },
  neon:  { rgb: '255,106,20',  core: '#ff7a26', glow: 'rgba(255,100,16,0.80)', hot: '#ffc48a', hotRgb: '255,196,138', dim: 'rgba(255,106,20,0.50)' },
};

/**
 * ★★ TODAY'S DRUM GREEN — the default chassis's `green`, and ONLY the default chassis's.
 * DrumWheel and TunerKeys drew with `hsl(GLOW_HUE=120, 100%, 45%)` = rgb(0, 229.5, 0), and the lit
 * key symbol with hsl(120, 100%, 78%) = rgb(143, 255, 143). The brief's green (61,255,114) is a
 * different, bluer LED; putting it on the default deck would change today's look, which §13.1
 * forbids. Silver and black take the brief's green.
 * ★ §6.1 TRAP: G() could not make white or neon at all, and got blue/amber at the wrong brightness
 *   — which is why every colour is a triplet now and the hue maths is gone.
 */
const TODAY_GREEN: LedColour = {
  rgb: '0,230,0', core: 'rgb(0,230,0)', glow: 'rgba(0,230,0,0.70)',
  hot: 'rgb(143,255,143)', hotRgb: '143,255,143', dim: 'rgba(0,230,0,0.45)',
  hsl: [120, 100, 45], hotHsl: [120, 100, 78],
};

/** The Nixie text colour — §2, exactly. Not an LED: a gas discharge. */
export const NEON_TEXT = {
  core:     '#ffc48a',
  /** CSS `0 0 1.5px #ff8a2e, 0 0 5px #ff6410, 0 0 11px rgba(255,80,10,0.6)` — RN takes one shadow,
   *  so the middle (the one that reads as the glow) stands in until the displays land (row 4). */
  glow:     '#ff6410',
  mode:     '#ffb37a',
  reading:  '#ff9a55',
  readingGlow: 'rgba(255,90,10,0.8)',
};

export interface ResolvedTextColour {
  /** The setting that won, or 'neon' under the Nixie rule. */
  name:   TextColour | 'neon';
  rgb:    string;
  core:   string;
  glow:   string;
  hot:    string;
  isNeon: boolean;
}

/** Is `text` one this display can show? */
export function textAllowed(display: DisplayStyle, text: TextColour): boolean {
  return TEXT_ALLOWED[display].includes(text);
}

/**
 * ★★★ THE resolver (§2). Neon for Nixie, whatever was asked for; otherwise the text colour clamped
 * to the display's allowed set (the display's first colour when it is not allowed). No other code
 * decides a text colour — and no route (picker, migration, stored prefs) can reach white on dot or
 * seg, because every one of them comes through here (acceptance §13.4).
 */
export function resolveTextColour(display: DisplayStyle, text: TextColour): ResolvedTextColour {
  if (display === 'nixie') {
    return { name: 'neon', rgb: LED.neon.rgb, core: NEON_TEXT.core, glow: NEON_TEXT.glow,
             hot: NEON_TEXT.core, isNeon: true };
  }
  const allowed = TEXT_ALLOWED[display];
  const name = allowed.includes(text) ? text : allowed[0];
  const c = LED[name];
  return { name, rgb: c.rgb, core: c.core, glow: c.glow, hot: c.hot, isNeon: false };
}

/** The controls colour's LED, with today's green on the default chassis (see TODAY_GREEN). */
export function resolveControlsColour(chassis: Chassis, controls: ControlsColour): LedColour {
  if (chassis === 'default' && controls === 'green') return TODAY_GREEN;
  return LED[controls];
}

// ── Setting changes with side effects (§1) ────────────────────────────────────

/**
 * Changing Display. ★ Choosing Nixie switches the controls to neon so drums and keys match the
 * tubes (the user may change them after); leaving Nixie with neon controls resets them to amber.
 * The text colour follows the display: the one remembered for it, else the current one if allowed,
 * else the display's first. Nixie keeps the stored text untouched — it is ignored, not replaced.
 */
export function withDisplay(s: FaceplateSettings, display: DisplayStyle): FaceplateSettings {
  if (display === s.display) return s;
  const textByDisplay = { ...s.textByDisplay };
  if (s.display !== 'nixie') textByDisplay[s.display] = s.text;
  let controls = s.controls;
  if (display === 'nixie') controls = 'neon';
  else if (s.display === 'nixie' && controls === 'neon') controls = 'amber';
  let text = s.text;
  if (display !== 'nixie') {
    const remembered = textByDisplay[display];
    text = remembered && textAllowed(display, remembered) ? remembered
         : textAllowed(display, s.text) ? s.text
         : TEXT_ALLOWED[display][0];
  }
  return { ...s, display, controls, text, textByDisplay };
}

/** Changing the text colour. Refused (unchanged) when the display cannot show it — the picker only
 *  offers allowed colours, but a stale caller must not be able to smuggle white onto a VFD. */
export function withText(s: FaceplateSettings, text: TextColour): FaceplateSettings {
  if (s.display === 'nixie' || !textAllowed(s.display, text)) return s;
  return { ...s, text, textByDisplay: { ...s.textByDisplay, [s.display]: text } };
}

// ── Storage: parse, validate, migrate ─────────────────────────────────────────

export const FACEPLATE_STORAGE_KEY = 'lsv_faceplate';

function pick<T extends string>(v: unknown, allowed: readonly T[], fallback: T): T {
  return typeof v === 'string' && (allowed as readonly string[]).includes(v) ? v as T : fallback;
}

/**
 * ★★ MIGRATION: Display REPLACES today's font setting (ThemeContext: Nixie One / Atkinson), it does
 * not sit beside it (§1: "don't add a second font switch"). Atkinson → `hyper`, Nixie One → `nixie`.
 * ThemeContext has never persisted its choice and has defaulted to Atkinson since 2026-06-11, so in
 * practice every existing user migrates to `hyper` — but the mapping is the rule, not the accident.
 */
export function migrateLegacyFont(legacyThemeName: string | null | undefined): DisplayStyle {
  return legacyThemeName === 'amber' ? 'nixie' : 'hyper';
}

/**
 * Stored JSON → settings. Anything unknown or missing takes the §1 default; a text colour the
 * display cannot show is clamped (the same rule as resolveTextColour, applied to what is SAVED so
 * the picker never opens on an impossible selection). `legacyThemeName` seeds Display when nothing
 * was ever stored.
 */
export function parseSettings(json: string | null, legacyThemeName?: string | null): FaceplateSettings {
  let raw: any = null;
  if (json) { try { raw = JSON.parse(json); } catch { raw = null; } }
  if (!raw || typeof raw !== 'object') {
    const display = migrateLegacyFont(legacyThemeName);
    return withDisplay({ ...DEFAULT_SETTINGS }, display);
  }
  const display = pick(raw.display, DISPLAYS, 'hyper');
  const textByDisplay: FaceplateSettings['textByDisplay'] = {};
  if (raw.textByDisplay && typeof raw.textByDisplay === 'object') {
    for (const d of DISPLAYS) {
      const t = raw.textByDisplay[d];
      if (d !== 'nixie' && typeof t === 'string' && textAllowed(d, t as TextColour)) textByDisplay[d] = t as TextColour;
    }
  }
  let text = pick(raw.text, TEXTS, 'green');
  if (display !== 'nixie' && !textAllowed(display, text)) text = TEXT_ALLOWED[display][0];
  return {
    chassis:   pick(raw.chassis, CHASSIS, 'default'),
    display,
    controls:  pick(raw.controls, CONTROLS, 'green'),
    text,
    meter:     pick(raw.meter, METERS, 'bar'),
    ...parseTransparency(raw),
    steadyLeds: raw.steadyLeds === true,
    frameRate: pick(raw.frameRate, FRAME_RATES, 'full'),
    textByDisplay,
    ...parseMotion(raw),
    lightAngle: pick(raw.lightAngle, LIGHT_ANGLES, 'left'),
  };
}

/** MOTION EFFECTS from storage: a pick only counts when it was explicitly made (older stores have none). */
function parseMotion(raw: any): Pick<FaceplateSettings, 'motionEffects' | 'motionExplicit'> {
  const explicit = raw.motionExplicit === true && MOTIONS.includes(raw.motionEffects);
  return { motionEffects: explicit ? raw.motionEffects : 'on', motionExplicit: explicit };
}

/** The user's pick from the pane: ON / OFF, and from now on it is theirs (explicit). */
export function withMotion(s: FaceplateSettings, m: MotionEffects): FaceplateSettings {
  return s.motionExplicit && s.motionEffects === m ? s : { ...s, motionEffects: m, motionExplicit: true };
}

/**
 * ★★★ THE ONE RESOLVER for MOTION EFFECTS (lighting brief §3) — beside effectiveTransparency's pattern:
 * the user's pick once made (it wins in BOTH directions), otherwise the OS's reduce-motion switch.
 * ★★ TRAP: LedVu and EdgeMeter used to call useReduceMotion() directly, which would bypass the pick. They
 *   read this (via FaceplateContext); useReduceMotion() is now only the OS INPUT here.
 */
export function effectiveMotion(s: Pick<FaceplateSettings, 'motionEffects' | 'motionExplicit'>,
                                osReduceMotion: boolean): MotionEffects {
  if (s.motionExplicit) return s.motionEffects;
  return osReduceMotion ? 'off' : 'on';
}

/**
 * ★★ MIGRATION from the decoder boxes' Transparent / Solid (`decoderBg`, row 8) to TRANSPARENCY
 * EFFECTS. `solid` → OFF, and it WAS a choice: `transparent` was the default. But a stored
 * `transparent` is NOT evidence of a choice — FaceplateProvider writes the parsed defaults back on
 * first launch, so every install that never opened the pane has `decoderBg: 'transparent'` saved.
 * It maps to ON *unchosen*, which lets low-end detection still switch a slow phone off.
 */
function parseTransparency(raw: any): Pick<FaceplateSettings, 'transparency' | 'transparencyExplicit'> {
  if (typeof raw.transparencyExplicit === 'boolean') {
    return { transparency: pick(raw.transparency, TRANSPARENCIES, 'on'),
             transparencyExplicit: raw.transparencyExplicit && TRANSPARENCIES.includes(raw.transparency) };
  }
  if (raw.decoderBg === 'solid') return { transparency: 'off', transparencyExplicit: true };
  return { transparency: 'on', transparencyExplicit: false };
}

/** The user's choice from the pane: ON / OFF, and from now on it is theirs (explicit). */
export function withTransparency(s: FaceplateSettings, t: Transparency): FaceplateSettings {
  return s.transparencyExplicit && s.transparency === t ? s : { ...s, transparency: t, transparencyExplicit: true };
}

// ── The CONTROL CUSTOMISATION pane (§1) ───────────────────────────────────────
//
// ★ What the pane offers is decided HERE, not in MenuSheet, so the rules (which text colours a
//   display may show, when the TEXT row is a note, when HAPTICS is hidden) are testable and there
//   is one copy of them. Order and labels are the Popup mockup's (`kind: 'custom'`), which wins over
//   the brief: DISPLAY reads NIXIE · HYPER · DOT · VCR, and the meters BAR · LED VU · ANALOGUE.

export interface PaneChoice<T extends string> { value: T; label: string }

export const CHASSIS_CHOICES: PaneChoice<Chassis>[] = [
  { value: 'default', label: 'DEFAULT' }, { value: 'silver', label: 'SILVER' }, { value: 'black', label: 'BLACK' },
];
export const DISPLAY_CHOICES: PaneChoice<DisplayStyle>[] = [
  { value: 'nixie', label: 'NIXIE' }, { value: 'hyper', label: 'HYPER' },
  { value: 'dot',   label: 'DOT' },   { value: 'seg',   label: 'VCR' },
];
export const METER_CHOICES: PaneChoice<SignalMeter>[] = [
  { value: 'bar', label: 'BAR' }, { value: 'vu', label: 'LED VU' }, { value: 'edge', label: 'ANALOGUE' },
];
export const TRANSPARENCY_CHOICES: PaneChoice<Transparency>[] = [
  { value: 'on', label: 'ON' }, { value: 'off', label: 'OFF' },
];
/** The TRANSPARENCY EFFECTS row's subtitle (§1 group layout; UK English): what OFF buys and costs
 *  nothing to read — it is the one faceplate choice that is also a performance setting. */
export const TRANSPARENCY_NOTE = 'Off · solid panels, easier to read and lighter on older devices';
export const MOTION_CHOICES: PaneChoice<MotionEffects>[] = [
  { value: 'on', label: 'ON' }, { value: 'off', label: 'OFF' },
];
export const LIGHT_ANGLE_CHOICES: PaneChoice<LightAngle>[] = [
  { value: 'left', label: 'LEFT' }, { value: 'topLeft', label: 'TOP-LEFT' }, { value: 'top', label: 'TOP' },
  { value: 'topRight', label: 'TOP-RIGHT' }, { value: 'right', label: 'RIGHT' },
];

/**
 * ★★ LIGHT ANGLE row: shown ONLY when it is the light in use (lighting brief §4) — on silver or black (the
 * default chassis has no metal, so a light angle there would be a key that does nothing: AGENTS.md), AND
 * while tilt is not driving the light. `tiltDriving` is false on every device today (tilt lighting, §5, is
 * not built) — so on silver / black the row is there everywhere, which is exactly the brief's Mac rule
 * ("no motion sensor"). ▶ Item 5 passes true where tilt is live, and the row hides there.
 */
export function lightAngleRowShown(chassis: Chassis, tiltDriving: boolean): boolean {
  return chassis !== 'default' && !tiltDriving;
}

/** The MOTION EFFECTS row's subtitle: what OFF changes — and, as plainly, what it never does. */
export const MOTION_NOTE = 'Off · no decorative movement — signal, tuning and keys still move';

/** The note the TEXT row shows instead of colours under Nixie (§1; the mockup's exact words). */
export const TEXT_LOCKED_NOTE = 'Locked to neon by the Nixie display';

/** Spoken names for the colour keys, which carry no legend — only a lit LED dot. */
export const COLOUR_NAMES: Record<ControlsColour | TextColour, string> = {
  green: 'Green', red: 'Red', amber: 'Amber', blue: 'Blue', white: 'White', teal: 'Teal', neon: 'Neon',
};

/** The TEXT row's colour keys for a display — ONLY the ones it may show — or null when the row is
 *  the locked-to-neon note (Nixie). Reads TEXT_ALLOWED, so it can never offer white on dot/seg. */
export function textChoices(display: DisplayStyle): TextColour[] | null {
  const allowed = TEXT_ALLOWED[display];
  return allowed.length ? allowed : null;
}

/** The lit LED dot a CONTROLS key shows: the colour as the deck will draw it on this chassis
 *  (today's drum green on the default chassis, so the dot matches the wells it lights). */
export function controlsDot(chassis: Chassis, c: ControlsColour): string {
  return resolveControlsColour(chassis, c).core;
}

/** FEEL rows, in order. HAPTICS is hidden on a device with no haptic motor (§1: "as today") — a
 *  switch whose every use is a no-op. STEADY LEDS is always offered, and MOTION EFFECTS beside it
 *  (lighting brief §3) — it always does something (the needle's overshoot at least, on every device). */
export function feelRows(hapticsHardware: boolean): Array<'haptics' | 'steadyLeds' | 'motion'> {
  return hapticsHardware ? ['haptics', 'steadyLeds', 'motion'] : ['steadyLeds', 'motion'];
}

/**
 * ★★★ FRAME RATE row: the keys to offer, or null when the row must be HIDDEN. `maxHz` is the
 * display's top refresh rate as the native side reports it (iOS `UIScreen.maximumFramesPerSecond`;
 * Android the fastest mode at the CURRENT resolution) — null / not a number = unknown (old binary,
 * web, Expo Go). ★ AGENTS.md: never offer a control whose every use is a no-op — on a 60 Hz panel
 * (every non-ProMotion iPhone, most Android phones, all Android before 6) a 60 Hz cap changes
 * nothing, so there is no row. The top key is labelled with the panel's REAL rate (a 90 Hz phone
 * reads "90 Hz", never "120 Hz"), rounded: Android reports 119.99 / 60.000004.
 */
export function frameRateChoices(maxHz: number | null | undefined): PaneChoice<FrameRate>[] | null {
  const hz = normaliseHz(maxHz);
  if (hz == null || hz <= 60) return null;
  return [{ value: 'full', label: `${hz} Hz` }, { value: '60', label: '60 Hz' }];
}

/** A reported refresh rate as a whole number of Hz, or null when it says nothing usable. */
export function normaliseHz(maxHz: number | null | undefined): number | null {
  if (typeof maxHz !== 'number' || !Number.isFinite(maxHz) || maxHz <= 0) return null;
  return Math.round(maxHz);
}

/** What the native side is told: the cap in Hz, or 0 = no cap (the display's own maximum).
 *  ★ Sent whatever the panel — on a 60 Hz panel a 60 Hz cap is simply a no-op natively. */
export function frameRateCapHz(s: Pick<FaceplateSettings, 'frameRate'>): number {
  return s.frameRate === '60' ? 60 : 0;
}

/** The FRAME RATE row's subtitle (UK English): what 60 Hz buys and costs. Live — no restart. */
export const FRAME_RATE_NOTE = '60 Hz · easier on the battery; scrolling and animation a touch less smooth';

// ── Chassis tokens ────────────────────────────────────────────────────────────

/**
 * Everything the deck draws with that is not a text or controls colour. ★ Silver and black spread
 * DEFAULT_CHASSIS and override the plate and keys (row 3) and the drum wells (row 6).
 */
export interface ChassisTokens {
  // Keys (today's outline keys)
  keyBg:          string;
  keyBorder:      string;
  keyBorderRec:   string;
  keyBorderChat:  string;
  keyPulseRec:    string;
  keyPulseChat:   string;
  keyLegend:      string;
  // Meter (bar)
  meterTrack:     string;
  /** The bar fill's stops by level: < 0.20, < 0.58, above. Positions are ControlsBar's. */
  meterGradLow:   string[];
  meterGradMid:   string[];
  meterGradHigh:  string[];
  peakLine:       string;
  sqlHalo:        string;
  sqlLine:        string;
  // Frequency + mode pill
  pillBg:         string;
  modeDivider:    string;
  modeGlow:       string;
  sharedBorder:   string;
  // Deck glass (the island behind everything)
  barBorder:      string;
  deckTint:       string;
  /** ★ Transparency OFF (default chassis only — silver / black are already an opaque plate): the
   *  glass island's tint and ring at alpha 1.0, composited over black (`solidOver`). The deck draws
   *  its tint ON the shadowed root view in this case — see ControlsBar for why that matters on iOS. */
  deckSolid:      string;
  barBorderSolid: string;
  // Status row
  clock:          string;
  srvClock:       string;
  linkDim:        string;
  linkRate:       string;
  linkUnlit:      string;
  /** Connection bars — good / fair / poor (and the ✕ when disconnected). */
  linkGood:       string;
  linkFair:       string;
  linkBad:        string;
  recRed:         string;
  recordDot:      string;
  dspTagText:     string;
  dspTagBorder:   string;
  dspTagBg:       string;
  handbackBg:     string;
  // Drum well (DrumWheel, and TunerKeys' well) — §6.1 / §6.2
  /** The panel face's vertical gradient (stops at 0 / 40 / 100 %). Under a texture it is the base
   *  the grain is laid over — a flat colour, so the well never flashes a gradient while it loads. */
  wellFace:       string[];
  /** Brushed grain on the face (silver / black), or null for today's machined dark metal. */
  wellTexture:    'silver' | 'black' | null;
  /** The DRUM's cut-out: the face's 0.9 pt dark gap (metal), or null = none (default).
   *  ★★★ NO RING (Stuart, 2026-10-01: "try removing the outline ring that surrounds both buttons and
   *  replace it with a glow coming up in the panel gap around each button"). The controls-colour ring,
   *  its 8 pt glow, the default's lit border and its inner glow are GONE on every chassis, lit or not —
   *  the tokens with them, so nothing can draw them back. Each control is lit on its own instead
   *  (constants/keyLight.ts). The keys well draws no edge at all; the drum keeps this dark cut. */
  wellBorder:     string | null;
  /** The face's top lip (`inset 0 1px 0 …`), or null. */
  wellTopLip:     string | null;
  drumBody:       string[];
  /** The drum gradient's stop positions (silver's crown light sits at 26 %, the dark drums' at 28 %). */
  drumPos:        number[];
  drumShadeTop:   string[];
  drumShadeBot:   string[];
  rimLine:        string;
  ridgeShadow:    string;
  ridgeHighlight: string;
  glint:          string[];
  /** ★ §6.1 TRAP — the notch PAIR, colour AND draw order. On dark rubber the notch is a light line
   *  with its shadow beside it; on aluminium it inverts: a DARK cut with a white highlight beside it.
   *  `notchPair` is the second line (shadow / highlight), offset `notchPairDx`, drawn UNDER the
   *  notches when `notchPairUnder` (so the notch's own colour wins where the two overlap). */
  notchPair:      string;
  notchPairDx:    number;
  notchPairUnder: boolean;
  notchMinor:     string;
  notchMed:       string;
  notchMajor:     string;
  sheen:          string[];
  sideShade:      string[];
  trapFill:       string;
  // (★★ §6.1: the red index needle is GONE on every chassis, and its tokens with it. In its place the
  //   LED pool in the controls colour — constants/drumWell.ts POOL.)
  // Tuner-keys mode (TunerKeys) — §6.2
  /** The well's face behind the keys (Deck.mockup `tk.bg`); metal lays its grain over it. */
  keysFace:       string;
  /** The dark slot each key sits in, where the key draws none itself (the default chassis's outline
   *  key); null on metal, whose DomeKey cap already sits in its own slot. */
  keysSlot:       string | null;
  // VTS strip
  vtsBg:          string;
  vtsBorder:      string;
  /** The deck's plate (§3.2/§3.3). null = the default chassis: today's glass island (BlurView +
   *  tint + ring). Anything else is OPAQUE — no BlurView behind it (§3.4). */
  plate:          PlateTokens | null;
  /** The dome keys' cap and slot (§5). */
  dome:           DomeTokens;
}

/** An opaque brushed plate. Numbers are Deck.mockup's `T.silver` / `T.black`, verbatim. */
export interface PlateTokens {
  base:        string;
  border:      string;
  radius:      number;
  texture:     'silver' | 'black';
  /** Lighting, drawn OVER the texture and never baked into it (§3.4 TRAP). CSS `linear 104°`. */
  lightColors: string[];
  lightPos:    number[];
  /** `radial-gradient(140% 70% at 28% -10%, c → 0 at 60%)`. */
  radialColor: string;
  /** Bottom 30% darkening to this, or null (black has none in the mockup). */
  bottomShade: string | null;
  /** The plate's inset box-shadows: the 1 pt top lip, the soft 2 pt below it, the bottom edge. */
  lipTop:      string;
  lipTop2:     string;
  lipBottom:   string;
  screws:      boolean;
  /** Black's gloss acrylic panel behind the frequency and meter, with its aluminium trim (§3.3). */
  gloss:       boolean;
  /** The 1 pt light lip under a recessed window (Deck.mockup `lip`). */
  windowLip:   string;
  /** A black veil over the GRAIN, under the lighting (0 = none). The texture, not the base colour, is
   *  what shows once it decodes, so this is what darkens a plate — deck, popups, decoder boxes and the
   *  drum wells all read it. */
  textureDim:  number;
}

export interface DomeTokens {
  /** `outline` = today's key (border + tint, no slot); `cap` = a brushed cap in a recessed slot. */
  look:        'outline' | 'cap';
  slotBg:      string;
  /** The slot's inner shadow at its top edge (`inset 0 3px 5px`). */
  slotShade:   string;
  /** The machined lip catching light below the slot (`0 1px 0`). */
  slotLip:     string;
  capBase:     string;
  capBorder:   string;
  /** A black veil over the cap's grain (the plate's `textureDim`), 0 = none. */
  textureDim:  number;
  /** `.bz-silver` / `.bz-black`: the sheen over the cap's texture — top, clear at 42%, bottom. */
  capSheen:    [string, string, string];
  /** Chamfer: `inset 0 1px 0 rgba(255,255,255, hi × 0.3)`. */
  chamfer:     string;
  rim:         string;
  bottomEdge:  string;
  /** The cast shadow at rest (`0 1.5px 1px`), gone when clicked. */
  cast:        string;
  /** Clicked: brightness .84 / .82 — drawn as black at 1 − pressDim. */
  pressDim:    number;
}

/** ★★★ Today's literals, moved from ControlsBar / DrumWheel / TunerKeys / VTSBar. */
export const DEFAULT_CHASSIS: ChassisTokens = {
  keyBg:         'rgba(20,10,0,0.75)',
  keyBorder:     'rgba(255,255,255,0.35)',
  keyBorderRec:  'rgba(220,40,40,0.90)',
  keyBorderChat: 'rgba(40,140,255,0.85)',
  keyPulseRec:   'rgba(255,60,60,1)',
  keyPulseChat:  'rgba(100,180,255,1)',
  keyLegend:     '#ffffff',
  meterTrack:    'rgba(105,98,82,0.30)',
  meterGradLow:  ['#bb1100', '#ff4400'],
  meterGradMid:  ['#bb1100', '#ff4400', '#ffaa00'],
  meterGradHigh: ['#bb1100', '#ff4400', '#ffaa00', '#00dd44'],
  peakLine:      'rgba(255,245,200,0.92)',
  sqlHalo:       'rgba(255,255,255,0.90)',
  sqlLine:       'rgba(255,50,50,1)',
  pillBg:        'rgb(20,10,0)',
  modeDivider:   'rgba(70,60,45,0.45)',
  modeGlow:      'rgba(255,160,0,0.6)',
  sharedBorder:  'rgba(255,255,255,0.30)',
  barBorder:     'rgba(255,255,255,0.30)',
  deckTint:      'rgba(8,6,2,0.55)',
  deckSolid:     solidOver('rgba(8,6,2,0.55)'),
  barBorderSolid: solidOver('rgba(255,255,255,0.30)'),
  clock:         'rgba(255,255,255,0.30)',
  srvClock:      '#9fd0ff',
  linkDim:       'rgba(255,255,255,0.40)',
  linkRate:      'rgba(255,255,255,0.55)',
  linkUnlit:     'rgba(255,255,255,0.18)',
  linkGood:      '#33cc44',
  linkFair:      '#e0b020',
  linkBad:       '#e04040',
  recRed:        '#e05050',
  recordDot:     '#e23b3b',
  dspTagText:    '#ffb833',
  dspTagBorder:  'rgba(255,184,51,0.55)',
  dspTagBg:      'rgba(255,184,51,0.16)',
  handbackBg:    'rgba(124,255,155,0.10)',
  wellFace:      ['#101410', '#0a0c0a', '#060706'],
  wellTexture:   null,
  wellBorder:    null,
  wellTopLip:    null,
  drumBody:      ['#070807', '#191a18', '#232422', '#181917', '#050505'],
  drumPos:       [0, 0.28, 0.50, 0.74, 1],
  drumShadeTop:  ['rgba(0,0,0,0.62)', 'rgba(0,0,0,0)'],
  drumShadeBot:  ['rgba(0,0,0,0)', 'rgba(0,0,0,0.58)'],
  rimLine:       'rgba(180,185,175,0.14)',
  ridgeShadow:   'rgba(0,0,0,0.45)',
  ridgeHighlight:'rgba(160,160,150,0.10)',
  glint:         ['#262626', '#6b6b6b', '#a5a5a5', '#d6d6d6', '#ffffff',
                  '#d6d6d6', '#a5a5a5', '#6b6b6b', '#262626'],
  notchPair:     'rgba(0,0,0,0.5)',
  notchPairDx:   0.9,
  notchPairUnder: true,
  notchMinor:    'rgba(168,166,158,0.22)',
  notchMed:      'rgba(168,166,158,0.36)',
  notchMajor:    'rgba(168,166,158,0.55)',
  sheen:         ['rgba(255,255,255,0)', 'rgba(255,255,255,0.07)', 'rgba(255,255,255,0)'],
  sideShade:     ['rgba(0,0,0,0.55)', 'rgba(0,0,0,0)'],
  trapFill:      'rgba(3,4,3,0.96)',
  keysFace:      '#0b0a08',
  keysSlot:      '#050403',
  vtsBg:         'rgba(8,10,14,0.94)',
  vtsBorder:     'rgba(255,255,255,0.22)',
  plate:         null,
  // ★ The default key keeps today's look AT REST (outline + tint); it still snaps and clicks
  //   (§5: "every dome key clicks, on every chassis, including default"). Only pressDim is read.
  dome: {
    look: 'outline', slotBg: 'transparent', slotShade: 'transparent', slotLip: 'transparent',
    capBase: 'transparent', capBorder: 'transparent', textureDim: 0,
    capSheen: ['rgba(0,0,0,0)', 'rgba(0,0,0,0)', 'rgba(0,0,0,0)'],
    chamfer: 'rgba(255,255,255,0.03)', rim: 'rgba(255,255,255,0.07)', bottomEdge: 'rgba(0,0,0,0.5)',
    cast: 'rgba(0,0,0,0.85)', pressDim: 0.84,
  },
};

/** §3.2 brushed silver (Sony HCD-SE1, Panasonic stacking hi-fi). */
export const SILVER_CHASSIS: ChassisTokens = {
  ...DEFAULT_CHASSIS,
  // §6.1, Deck.mockup `W_FACE.silver`: a brushed-silver face and a dark 1 pt gap (the mockup's ring and
  // glow beyond it are gone — see wellBorder); a polished-aluminium drum with DARK cuts (the pair
  // inverts — TRAP).
  wellFace:      ['#c9c6bf', '#c9c6bf', '#c9c6bf'],
  wellTexture:   'silver',
  wellBorder:    'rgba(0,0,0,0.55)',
  wellTopLip:    'rgba(255,255,255,0.80)',
  drumBody:      ['#4d4b46', '#a9a69f', '#e4e2dc', '#a3a09a', '#393834'],
  drumPos:       [0, 0.26, 0.50, 0.74, 1],
  rimLine:       'rgba(255,255,255,0.55)',
  ridgeHighlight:'rgba(255,255,255,0.35)',
  notchPair:     'rgba(255,255,255,0.45)',
  notchMinor:    'rgba(58,56,50,0.40)',
  notchMed:      'rgba(48,46,41,0.55)',
  notchMajor:    'rgba(38,36,32,0.70)',
  sheen:         ['rgba(255,255,255,0)', 'rgba(255,255,255,0.40)', 'rgba(255,255,255,0)'],
  keysFace:      '#c9c6bf',
  keysSlot:      null,
  plate: {
    base: '#c9c6bf', border: '#8b8983', radius: 16, texture: 'silver',
    lightColors: ['rgba(255,255,255,0)', 'rgba(255,255,255,0.10)', 'rgba(255,255,255,0.34)',
                  'rgba(255,255,255,0.08)', 'rgba(0,0,0,0.06)', 'rgba(0,0,0,0.16)'],
    lightPos:    [0, 0.18, 0.36, 0.52, 0.74, 1],
    radialColor: 'rgba(255,250,240,0.22)',
    bottomShade: 'rgba(0,0,0,0.10)',
    lipTop: 'rgba(255,255,255,0.95)', lipTop2: 'rgba(255,255,255,0.35)', lipBottom: 'rgba(0,0,0,0.20)',
    screws: true, gloss: false, windowLip: 'rgba(255,255,255,0.80)', textureDim: 0,
  },
  dome: {
    look: 'cap', slotBg: '#141414', slotShade: 'rgba(0,0,0,0.95)', slotLip: 'rgba(255,255,255,0.85)',
    capBase: '#c9c6bf', capBorder: '#8d8a83', textureDim: 0,
    capSheen: ['rgba(255,255,255,0.60)', 'rgba(255,255,255,0)', 'rgba(0,0,0,0.22)'],
    chamfer: rgba('255,255,255', 0.95 * 0.3), rim: 'rgba(255,255,255,0.07)', bottomEdge: 'rgba(0,0,0,0.5)',
    cast: 'rgba(0,0,0,0.85)', pressDim: 0.84,
  },
};

/** §3.3 brushed black (Stuart's Yamaha RX-V583): no screws, a gloss acrylic display panel. */
export const BLACK_CHASSIS: ChassisTokens = {
  ...DEFAULT_CHASSIS,
  // §6.1 — ★ THE MOCKUP WINS over the brief's table here: `W_FACE.black` is NOT the default well. It
  // has a brushed-black face (#000 gap, top lip .14; its ring and glow are gone) and a NEUTRAL grey drum
  // (#070707 … #050505) where the default's is faintly green. Notches stay light on dark.
  /* ★ BLACK IS A TOUCH DARKER than the mockup (Stuart, build 356: "can be darkened ever so slightly"):
   *  base #1b1c1e → #161719, a .14 veil over the grain (its mean #2a2a2a → ~#242424), the lighting's
   *  white bands at ~.8 of the mockup's, the caps #1d1e20 → #18191b. One set of tokens, so the deck,
   *  every popup, the decoder boxes and the wells move together. */
  wellFace:      ['#161719', '#161719', '#161719'],
  wellTexture:   'black',
  wellBorder:    '#000000',
  wellTopLip:    'rgba(255,255,255,0.14)',
  drumBody:      ['#070707', '#181818', '#222222', '#171717', '#050505'],
  rimLine:       'rgba(180,182,186,0.14)',
  ridgeHighlight:'rgba(160,162,166,0.10)',
  notchMinor:    'rgba(168,168,170,0.22)',
  notchMed:      'rgba(169,169,172,0.36)',
  notchMajor:    'rgba(170,170,174,0.55)',
  keysFace:      '#161719',
  keysSlot:      null,
  plate: {
    base: '#161719', border: '#3a3c40', radius: 16, texture: 'black',
    // ★ The mockup's own black lighting, not "silver at a third" (the brief's paraphrase of it) —
    //   its white bands at ~.8 (.03 / .11 / .02 / radial .07 → .025 / .09 / .015 / .06).
    lightColors: ['rgba(255,255,255,0)', 'rgba(255,255,255,0.025)', 'rgba(255,255,255,0.09)',
                  'rgba(255,255,255,0.015)', 'rgba(0,0,0,0.10)', 'rgba(0,0,0,0.25)'],
    lightPos:    [0, 0.18, 0.36, 0.52, 0.74, 1],
    radialColor: 'rgba(255,255,255,0.06)',
    textureDim:  0.14,
    bottomShade: null,
    lipTop: 'rgba(255,255,255,0.22)', lipTop2: 'rgba(255,255,255,0.06)', lipBottom: 'rgba(0,0,0,0.60)',
    screws: false, gloss: true, windowLip: 'rgba(255,255,255,0.14)',
  },
  dome: {
    look: 'cap', slotBg: '#030303', slotShade: 'rgba(0,0,0,0.95)', slotLip: 'rgba(255,255,255,0.14)',
    capBase: '#18191b', capBorder: '#050505', textureDim: 0.14,
    capSheen: ['rgba(255,255,255,0.20)', 'rgba(255,255,255,0)', 'rgba(0,0,0,0.45)'],
    chamfer: rgba('255,255,255', 0.22 * 0.3), rim: 'rgba(255,255,255,0.07)', bottomEdge: 'rgba(0,0,0,0.5)',
    cast: 'rgba(0,0,0,0.85)', pressDim: 0.82,
  },
};

export function chassisTokens(chassis: Chassis): ChassisTokens {
  return chassis === 'silver' ? SILVER_CHASSIS : chassis === 'black' ? BLACK_CHASSIS : DEFAULT_CHASSIS;
}

// ── The resolved faceplate ────────────────────────────────────────────────────

export const FONT_HYPER = 'Atkinson Hyperlegible';
export const FONT_NIXIE = 'Nixie One';
/** Doto 900 (assets/fonts/Doto-Black.ttf — the Black cut IS the file, so no fontWeight is needed).
 *  Registered under this name in App.tsx's useFonts. */
export const FONT_DOTO  = 'Doto';
/** DSEG14 Classic Bold Italic (assets/fonts/DSEG14Classic-BoldItalic.ttf), registered in App.tsx.
 *  ★ Only ever fed through toSegCells() (constants/displayText.ts): its space is not a cell. */
export const FONT_SEG14 = 'DSEG14 Classic';

/** The deck's TEXT roles, already under the Nixie rule. Default chassis + hyper = today's WHITE
 *  theme values (ThemeContext), so the frequency pill is unchanged. */
export interface DeckText {
  /** Which frequency window to draw (§7): Atkinson text, tubes, Doto over a ghost grid, drawn segments. */
  style:        DisplayStyle;
  freqFont:     string;
  freq:         string;
  freqGlow:     string;
  /** Letter-spacing of a TEXT frequency (hyper / dot). §7 says 2.5 for Hyperlegible; the default
   *  chassis keeps today's 1.5 (§13.1 — identical to today's build). */
  freqSpacing:  number;
  unit:         string;
  /** The unit label's font — never Nixie One: beside the tubes it is the mockup's plain grey sans. */
  unitFont:     string;
  modeFont:     string;
  mode:         string;
  reading:      string;
  /** The mode label's glow (textShadowColor). Default hyper: today's amber glow. */
  modeGlow:     string;
  /** The breathing "SQL" in the mode box (§4.6): red, or neon under Nixie (the rule outranks red). */
  sqlClosed:    string;
  /** Its glow (Deck.mockup `md.g` while closed): `0 0 4px rgba(255,64,64,.6)`, neon
   *  `rgba(255,90,10,.8)` under Nixie, or null — the default deck's SQL has none today (§13.1). */
  sqlGlow:      string | null;
  bannerFont:   string;
  /** The SHARED TUNER banner's two states — "free to tune" green and "ask" grey today. */
  bannerFree:   string;
  bannerAsk:    string;
  /** The lit colour as a triplet / core / glow, for the ghost layers and glow sprites (dot, seg). */
  rgb:          string;
  core:         string;
  glow:         string;
}

/** The four main keys' legends (§2): white on the default chassis, neon when the controls are neon;
 *  Nixie One only when BOTH the display is Nixie and the legend is neon (never white Nixie One). */
export interface KeyLegend {
  color:   string;
  font:    string;
  /** Text glow for the step legend, or null for none. */
  glow:    string | null;
  /** The legend while the key is clicked (§5 "legend flare"): today's white stays white; an LED
   *  legend goes to its white-hot centre. */
  hot:     string;
  /** The engraving's dark shadow on a light cap (silver: `0 -0.5px 0 rgba(0,0,0,.6)`), or null. */
  shade:   string | null;
}

/** §5: the legend glow's radius at rest and clicked ("legend glow 4 → 7 pt"). */
export const LEGEND_GLOW_REST = 4;
export const LEGEND_GLOW_DOWN = 7;

export interface VtsText {
  /** Which display draws the strip (§7): Atkinson, Nixie One neon, Doto UPPER, DSEG14 UPPER. */
  style:   DisplayStyle;
  font:    string;
  /** dot / seg: upper-case through toUpperDisplay() (units keep their case). */
  upper:   boolean;
  onTune:  string;
  offTune: string;
  band:    string;
  dim:     string;
  sub:     string;
  offset:  string;
  /** ★ Under Nixie the per-notice colour override (band conditions) is IGNORED — it would draw
   *  Nixie One in green or red. A single-colour VFD (dot / seg) cannot show it either. */
  allowOverride: boolean;
  /** The RDS mark's colour (§7.1: the resolved text colour; neon under Nixie) and its glow, or
   *  null for none (the default chassis — Deck.mockup `vtsGlow: 'none'`). */
  mark:     string;
  markGlow: string | null;
  /** The lit colour as a triplet / core / glow (ghosts, the annunciator, the ISO code). */
  rgb:     string;
  core:    string;
  glow:    string;
}

export interface FaceplateTheme {
  settings:  FaceplateSettings;
  chassis:   ChassisTokens;
  controls:  LedColour;
  text:      ResolvedTextColour;
  deck:      DeckText;
  keyLegend: KeyLegend;
  vts:       VtsText;
  /** ★★★ Transparency OFF: every see-through surface is drawn at alpha 1.0 with no BlurView. Read
   *  through `useSurfaceOpaque()` (FaceplateContext) — the deck, DecoderShell, MenuSheet and row 10's
   *  PopupShell all take it from here, so there is one switch and one reader of it. */
  opaque:    boolean;
  /** The same switch spelled out for a surface that draws itself — `useSurface()`. */
  surface:   SurfaceTokens;
}

/**
 * ★★★ WHAT TRANSPARENCY OFF MEANS, for anything drawn over the waterfall (Stuart, 2026-09-30, the
 * same rule as the web client): OPAQUE PANELS OVER A LIVE, UNDIMMED WATERFALL.
 *   • `fill()` — a panel colour at alpha 1.0 exactly (today's colour composited over black).
 *   • `blur` false — no BlurView.
 *   • `scrimOpacity` 0 — no full-screen dim behind a sheet, modal or menu: the dim IS a full-screen
 *     blend over the live spectrum. Keep the tap-outside-to-close view; give it no background
 *     (an invisible view costs nothing to composite).
 *   • `dropShadow` false — no blurred shadow cast over the spectrum; the panel keeps its border.
 * ★★ AND THE WATERFALL NEVER STOPS DRAWING under a panel — nothing here is a licence to skip a
 *   frame. The only permitted draw-skip in the app is a start screen.
 */
export interface SurfaceTokens {
  opaque:       boolean;
  blur:         boolean;
  /** Multiply a scrim's opacity by this: 1 = today's dim, 0 = none. */
  scrimOpacity: number;
  dropShadow:   boolean;
  /** A surface colour as it must be drawn: itself when ON, `solidOver(itself)` when OFF. */
  fill:         (color: string) => string;
}

const SURFACE_ON: SurfaceTokens = { opaque: false, blur: true, scrimOpacity: 1, dropShadow: true, fill: c => c };
const SURFACE_OFF: SurfaceTokens = { opaque: true, blur: false, scrimOpacity: 0, dropShadow: false, fill: c => solidOver(c) };

export function surfaceTokens(opaque: boolean): SurfaceTokens {
  return opaque ? SURFACE_OFF : SURFACE_ON;
}

/** Style that switches a view's drop shadow off (iOS shadow + Android elevation) — for OFF. */
export const NO_DROP_SHADOW = { shadowOpacity: 0, shadowRadius: 0, elevation: 0 } as const;

/** Today's WHITE-theme text values (ThemeContext) — the default deck's text roles. */
const TODAY_TEXT = {
  freq:     '#ffffff',
  freqGlow: 'rgba(255,255,255,0.50)',
  unit:     '#b0b8c8',
  mode:     '#ffffff',
  reading:  '#b0b8c8',
  sqlRed:   '#ff4040',
  free:     '#7bd88f',
};
/** The SQL's red glow on silver / black and the VFD displays (Deck.mockup `md.g` while closed). */
const SQL_RED_GLOW = 'rgba(255,64,64,0.6)';
const TODAY_VTS = {
  onTune:  'rgba(80,220,100,0.95)',
  offTune: 'rgba(255,200,80,0.95)',
  band:    '#ffe566',
  dim:     'rgba(255,255,255,0.35)',
  sub:     'rgba(255,255,255,0.55)',
  offset:  'rgba(255,200,80,0.85)',
};

export function resolveFaceplate(s: FaceplateSettings): FaceplateTheme {
  const chassis  = chassisTokens(s.chassis);
  const controls = resolveControlsColour(s.chassis, s.controls);
  const text     = resolveTextColour(s.display, s.text);
  const nixie    = s.display === 'nixie';

  const deckDefault = s.chassis === 'default';
  const GREY_UNIT = 'rgba(255,255,255,0.55)';      // Deck.mockup's unit label, every display
  const lit = { rgb: text.rgb, core: text.core, glow: text.glow };
  let deck: DeckText;
  if (nixie) {
    // ★★★ Real tubes for the frequency (§7) — the tubes ARE Nixie One glyphs, so freqFont says so and
    //   the colour is the neon core. The unit label beside them is the mockup's grey sans (not Nixie
    //   One, so the rule does not reach it); the mode box is Barlow in the brief — not bundled, so
    //   Atkinson, in the brief's neon.
    deck = {
      style: 'nixie', freqFont: FONT_NIXIE, freq: NEON_TEXT.core, freqGlow: NEON_TEXT.glow, freqSpacing: 1.5,
      unit: GREY_UNIT, unitFont: FONT_HYPER,
      modeFont: FONT_HYPER, mode: NEON_TEXT.mode, reading: NEON_TEXT.reading, sqlClosed: NEON_TEXT.reading, sqlGlow: NEON_TEXT.readingGlow,
      modeGlow: NEON_TEXT.readingGlow,
      bannerFont: FONT_NIXIE, bannerFree: NEON_TEXT.core, bannerAsk: NEON_TEXT.reading, ...lit,
    };
  } else if (s.display === 'hyper' && deckDefault) {
    // ★ The default deck's text is today's white on EVERY text colour: on the default chassis the
    //   mockup keeps the digits and mode white (`isDefault ? '#f4f1ea'`); the text colour reaches
    //   the VTS (see `vts` below).
    deck = {
      style: 'hyper', freqFont: FONT_HYPER, freq: TODAY_TEXT.freq, freqGlow: TODAY_TEXT.freqGlow, freqSpacing: 1.5,
      unit: TODAY_TEXT.unit, unitFont: FONT_HYPER,
      modeFont: FONT_HYPER, mode: TODAY_TEXT.mode, reading: TODAY_TEXT.reading, sqlClosed: TODAY_TEXT.sqlRed, sqlGlow: null,
      modeGlow: chassis.modeGlow,
      bannerFont: FONT_HYPER, bannerFree: TODAY_TEXT.free, bannerAsk: TODAY_TEXT.reading, ...lit,
    };
  } else if (s.display === 'hyper') {
    // Silver / black Hyperlegible: Deck.mockup `digit: tx.d` (the colour's hot centre) with its glow,
    // letter-spacing 2.5, the mode in the same hot colour over a white reading, no mode glow.
    deck = {
      style: 'hyper', freqFont: FONT_HYPER, freq: text.hot, freqGlow: text.glow, freqSpacing: 2.5,
      unit: GREY_UNIT, unitFont: FONT_HYPER,
      modeFont: FONT_HYPER, mode: text.hot, reading: 'rgba(255,255,255,0.80)', sqlClosed: TODAY_TEXT.sqlRed, sqlGlow: SQL_RED_GLOW,
      modeGlow: 'rgba(0,0,0,0)',
      bannerFont: FONT_HYPER, bannerFree: text.core, bannerAsk: rgba(text.rgb, 0.75), ...lit,
    };
  } else {
    // dot / seg: the text colour lights the readouts (§2 TRAP: never Nixie One here). Dot: Doto for
    // the frequency and the mode box. Seg: the digits are DRAWN; the mode box is "sans" (Barlow in
    // the mockup, not bundled → Atkinson).
    const dot = s.display === 'dot';
    deck = {
      style: s.display, freqFont: dot ? FONT_DOTO : FONT_HYPER, freq: text.core, freqGlow: text.glow, freqSpacing: 1,
      unit: GREY_UNIT, unitFont: FONT_HYPER,
      modeFont: dot ? FONT_DOTO : FONT_HYPER, mode: text.core, reading: text.core, sqlClosed: TODAY_TEXT.sqlRed, sqlGlow: SQL_RED_GLOW,
      modeGlow: text.glow,
      bannerFont: FONT_HYPER, bannerFree: text.core, bannerAsk: rgba(text.rgb, 0.75), ...lit,
    };
  }

  // ★ Silver and black light their legends in the CONTROLS colour (§5, Deck.mockup `led.c`, clicked
  //   `led.d`), Atkinson always — the mockup's Barlow is not bundled, and never Nixie One, which
  //   would then have to be neon on every controls colour (§2). The default chassis is today's
  //   white, or neon when the controls are neon.
  const neonKeys = s.chassis === 'default' && s.controls === 'neon';
  const keyLegend: KeyLegend = s.chassis !== 'default'
    ? { color: controls.core, font: FONT_HYPER, glow: controls.glow, hot: controls.hot,
        shade: s.chassis === 'silver' ? 'rgba(0,0,0,0.6)' : null }
    : neonKeys
    ? { color: NEON_TEXT.core, font: nixie ? FONT_NIXIE : FONT_HYPER, glow: NEON_TEXT.glow,
        hot: NEON_TEXT.core, shade: null }
    : { color: chassis.keyLegend, font: FONT_HYPER, glow: null, hot: chassis.keyLegend, shade: null };

  // ★ The VTS strip (§7, §7.1). The default deck with GREEN text keeps today's strip exactly — its
  //   on-tune green / off-tune amber / band yellow palette (§13.1), the way TODAY_GREEN keeps the
  //   drums. Any other text colour, or a metal chassis, lights the strip in the text colour; a VFD
  //   (dot / seg) is one colour, so band-condition overrides cannot reach it.
  let vts: VtsText;
  if (nixie) {
    vts = { style: 'nixie', font: FONT_NIXIE, upper: false,
            onTune: NEON_TEXT.core, offTune: NEON_TEXT.core, band: NEON_TEXT.core,
            dim: rgba(LED.neon.rgb, 0.35), sub: NEON_TEXT.reading, offset: NEON_TEXT.reading, allowOverride: false,
            mark: NEON_TEXT.core, markGlow: NEON_TEXT.glow, ...lit };
  } else if (s.display === 'hyper' && deckDefault && text.name === 'green') {
    vts = { style: 'hyper', font: FONT_HYPER, upper: false, ...TODAY_VTS, allowOverride: true,
            mark: TODAY_VTS.onTune, markGlow: null, ...lit };
  } else {
    const vfd = s.display === 'dot' || s.display === 'seg';
    vts = { style: s.display, font: s.display === 'dot' ? FONT_DOTO : s.display === 'seg' ? FONT_SEG14 : FONT_HYPER,
            upper: vfd,
            onTune: text.core, offTune: text.core, band: text.core,
            dim: rgba(text.rgb, 0.30), sub: rgba(text.rgb, 0.70), offset: rgba(text.rgb, 0.85),
            allowOverride: !vfd,
            mark: text.core, markGlow: deckDefault ? null : text.glow, ...lit };
  }

  const opaque = s.transparency === 'off';
  return { settings: s, chassis, controls, text, deck, keyLegend, vts, opaque, surface: surfaceTokens(opaque) };
}

// ── Crash safety ──────────────────────────────────────────────────────────────

/**
 * ★★★ CRASH SAFETY — A FACEPLATE SETTING CAN NEVER LOCK SOMEONE OUT OF THE APP.
 *
 * WHY (11 B7): SIGNAL METER → LED VU crashed the app on its first frame, and because a faceplate
 * setting is STORED, every launch that reached the deck crashed again. On a phone with a default
 * server the app goes straight into it, so it could not even reach the server list to change it
 * back; the only way out was deleting the app. The LED VU bug is fixed (meters.ts, WORKLET
 * DEFAULTS); this is the guarantee that the NEXT faceplate bug cannot do the same.
 *
 *   ARM   — immediately BEFORE a non-default faceplate is drawn (the stored settings applied at
 *           launch, a change in the CONTROL CUSTOMISATION pane, the deck mounting in a receiver) a
 *           mark is written SYNCHRONOUSLY (services/faceplateGuard.ts: a file, so it is on disk before
 *           the next line runs — an abort a frame later must find it).
 *   CLEAR — after ARMED_WINDOW_MS of the app running with it, when the app goes to the BACKGROUND (a
 *           swipe-away from the switcher is not a crash), and when the settings go back to safe.
 *   CHECK — at launch, BEFORE the stored settings are applied: a mark still there means the last run
 *           died while a faceplate was being drawn. The app comes up on the SAFE faceplate (display
 *           HYPER, meter BAR, chassis DEFAULT — transparency and colours untouched), stores that, keeps
 *           what the user had chosen in a "last crashed" slot, and says so once.
 *
 * Pure (no React Native): scripts/test_faceplate_safety.ts drives it with a fake disk and clock.
 * (Here, not in its own file, because it needs withDisplay and the node tests load one module.)
 */

/** How long a faceplate must run before it is trusted (crash on mount, the first data, a retune). */
export const ARMED_WINDOW_MS = 5000;

/** The faceplate every build has drawn since before the faceplates existed. */
export const SAFE_FACEPLATE = { chassis: 'default', display: 'hyper', meter: 'bar' } as const;

/** Is anything drawn that is not the safe faceplate? (Colours and transparency are not risks: they
 *  only change the values the same components draw with.) */
export function isRiskyFaceplate(s: Pick<FaceplateSettings, 'chassis' | 'display' | 'meter'>): boolean {
  return s.chassis !== SAFE_FACEPLATE.chassis || s.display !== SAFE_FACEPLATE.display
      || s.meter !== SAFE_FACEPLATE.meter;
}

/** The user's settings with the safe faceplate — via withDisplay, so the text colour is one the
 *  display can show (leaving Nixie also takes the controls off neon). */
export function safeFaceplate(s: FaceplateSettings): FaceplateSettings {
  return { ...withDisplay(s, SAFE_FACEPLATE.display), chassis: SAFE_FACEPLATE.chassis, meter: SAFE_FACEPLATE.meter };
}

/** What the mark records: the risky choices, and when (for the diagnostics, not the decision). */
export function markOf(s: Pick<FaceplateSettings, 'chassis' | 'display' | 'meter'>, nowMs: number): string {
  return JSON.stringify({ chassis: s.chassis, display: s.display, meter: s.meter, at: nowMs });
}

export interface LaunchDecision {
  /** What to apply (and store, if it changed). */
  settings: FaceplateSettings;
  /** The settings the last run died with — for the "last crashed" slot and the notice; null when
   *  nothing is wrong. */
  crashed: FaceplateSettings | null;
}

/**
 * At launch. `mark` is whatever was on disk (null = the last run cleared it). ★ ANY mark counts, even
 * one that does not parse: a write torn by the very crash it records is still a crash. A mark with a
 * faceplate that is already safe changes nothing (the crash was not the faceplate's).
 */
export function decideLaunch(stored: FaceplateSettings, mark: string | null): LaunchDecision {
  if (mark == null || !isRiskyFaceplate(stored)) return { settings: stored, crashed: null };
  return { settings: safeFaceplate(stored), crashed: stored };
}

// ── The arm / clear state machine (disk and clock injected) ───────────────────

export interface GuardIO {
  /** Synchronous: on disk when it returns. */
  write(mark: string): void;
  read(): string | null;
  remove(): void;
  now(): number;
  setTimer(fn: () => void, ms: number): unknown;
  clearTimer(t: unknown): void;
}

export interface FaceplateGuard {
  /** Before drawing `s`: write the mark (risky) or clear it (safe). Restarts the window. */
  arm(s: Pick<FaceplateSettings, 'chassis' | 'display' | 'meter'>): void;
  /** The window ran out, or the app left the foreground. */
  clear(): void;
  /** Once, at launch, before anything arms: the previous run's mark (removed from disk), or null. */
  takeLaunchMark(): string | null;
}

export function makeFaceplateGuard(io: GuardIO, windowMs = ARMED_WINDOW_MS): FaceplateGuard {
  let timer: unknown = null;
  const stopTimer = () => { if (timer != null) { io.clearTimer(timer); timer = null; } };
  const clear = () => {
    stopTimer();
    try { io.remove(); } catch { /* nothing there, or the disk refused: nothing to undo */ }
  };
  return {
    arm(s) {
      if (!isRiskyFaceplate(s)) { clear(); return; }
      try { io.write(markOf(s, io.now())); } catch { /* a guard that cannot write must not break drawing */ }
      stopTimer();
      timer = io.setTimer(() => { timer = null; clear(); }, windowMs);
    },
    clear,
    takeLaunchMark() {
      let m: string | null = null;
      try { m = io.read(); } catch { m = null; }
      if (m != null) clear();
      return m;
    },
  };
}
