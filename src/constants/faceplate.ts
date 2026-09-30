/**
 * faceplate.ts — the FACEPLATE tokens and the ONE resolver (brief §1, §2, §12).
 *
 * "I don't want colour changes to look like a palette swap on a piece of software" — so a
 * faceplate is not a palette. It is five settings (chassis, display, controls colour, text colour,
 * signal meter) plus the decoder background, resolved HERE, once, into the tokens the deck draws
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

// ── Settings ──────────────────────────────────────────────────────────────────

export type Chassis          = 'default' | 'silver' | 'black';
export type DisplayStyle     = 'hyper' | 'nixie' | 'dot' | 'seg';
export type ControlsColour   = 'green' | 'red' | 'amber' | 'blue' | 'white' | 'neon';
export type TextColour       = 'green' | 'red' | 'amber' | 'blue' | 'white' | 'teal';
export type SignalMeter      = 'bar' | 'vu' | 'edge';
export type DecoderBackground = 'transparent' | 'solid';

export interface FaceplateSettings {
  chassis:   Chassis;
  display:   DisplayStyle;
  controls:  ControlsColour;
  text:      TextColour;
  meter:     SignalMeter;
  decoderBg: DecoderBackground;
  /** §4.4 "Steady LEDs": the LED VU's edge segment solid on/off with ~1 dB hysteresis instead of the
   *  statistical partial brightness. Stored now (the CONTROL CUSTOMISATION pane's FEEL group); the
   *  VU that reads it arrives with row 5, which also ORs in the OS Reduce Motion setting. */
  steadyLeds: boolean;
  /** ★ The user's text colour, remembered PER DISPLAY (§1: "Remember the user's choice per display
   *  if cheap to do" — it is). Leaving Hyperlegible-white for dot matrix falls back to teal; coming
   *  back restores white rather than leaving them on teal. */
  textByDisplay: Partial<Record<DisplayStyle, TextColour>>;
}

export const CHASSIS:     Chassis[]        = ['default', 'silver', 'black'];
export const DISPLAYS:    DisplayStyle[]   = ['hyper', 'nixie', 'dot', 'seg'];
export const CONTROLS:    ControlsColour[] = ['green', 'red', 'amber', 'blue', 'white', 'neon'];
export const TEXTS:       TextColour[]     = ['green', 'red', 'amber', 'blue', 'white', 'teal'];
export const METERS:      SignalMeter[]    = ['bar', 'vu', 'edge'];
export const DECODER_BGS: DecoderBackground[] = ['transparent', 'solid'];

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
  meter: 'bar', decoderBg: 'transparent', steadyLeds: false, textByDisplay: {},
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
    decoderBg: pick(raw.decoderBg, DECODER_BGS, 'transparent'),
    steadyLeds: raw.steadyLeds === true,
    textByDisplay,
  };
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
export const DECODER_BG_CHOICES: PaneChoice<DecoderBackground>[] = [
  { value: 'transparent', label: 'TRANSPARENT' }, { value: 'solid', label: 'SOLID' },
];

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
 *  switch whose every use is a no-op. STEADY LEDS is always offered. */
export function feelRows(hapticsHardware: boolean): Array<'haptics' | 'steadyLeds'> {
  return hapticsHardware ? ['haptics', 'steadyLeds'] : ['steadyLeds'];
}

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
  /** The face's 0.9 pt border: a fixed colour (metal: the dark gap), or null = the controls colour
   *  at .70 (default: today's lit edge). */
  wellBorder:     string | null;
  /** Today's inner glow along the border (G(.10), 5 pt, blur 6); 0 = none. */
  wellInnerGlowA: number;
  /** Metal: the controls-colour ring OUTSIDE the dark gap (`0 0 0 1px L(a)`) and the glow beyond it
   *  (`0 0 8px L(a)`). 0 on the default well, whose edge is its lit border. */
  wellRingA:      number;
  wellGlowA:      number;
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
  // Today's tuner keys (TunerKeys, until §6.2 makes them dome keys)
  tkSlotEdge:     string;
  tkSlotLip:      string[];
  tkCapDown:      string[];
  tkCapUp:        string[];
  tkShadeDown:    string;
  tkShadeUp:      string;
  tkCapSheen:     string[];
  tkGrainA:       string;
  tkGrainB:       string;
  tkRimDown:      string;
  tkRimUp:        string;
  tkWellRing:     string;
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
  wellInnerGlowA: 0.10,
  wellRingA:     0,
  wellGlowA:     0,
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
  tkSlotEdge:    'rgba(0,0,0,0.9)',
  tkSlotLip:     ['rgba(255,255,255,0.05)', 'rgba(255,255,255,0.22)', 'rgba(255,255,255,0.05)'],
  tkCapDown:     ['#050605', '#090a09', '#0d0f0d'],
  tkCapUp:       ['#0a0b0a', '#141614', '#1b1e1b'],
  tkShadeDown:   'rgba(0,0,0,0.75)',
  tkShadeUp:     'rgba(0,0,0,0.6)',
  tkCapSheen:    ['rgba(255,255,255,0)', 'rgba(255,255,255,0.05)'],
  tkGrainA:      'rgba(255,255,255,0.030)',
  tkGrainB:      'rgba(255,255,255,0.045)',
  tkRimDown:     'rgba(255,255,255,0.03)',
  tkRimUp:       'rgba(255,255,255,0.07)',
  tkWellRing:    'rgba(255,255,255,0.10)',
  vtsBg:         'rgba(8,10,14,0.94)',
  vtsBorder:     'rgba(255,255,255,0.22)',
  plate:         null,
  // ★ The default key keeps today's look AT REST (outline + tint); it still snaps and clicks
  //   (§5: "every dome key clicks, on every chassis, including default"). Only pressDim is read.
  dome: {
    look: 'outline', slotBg: 'transparent', slotShade: 'transparent', slotLip: 'transparent',
    capBase: 'transparent', capBorder: 'transparent',
    capSheen: ['rgba(0,0,0,0)', 'rgba(0,0,0,0)', 'rgba(0,0,0,0)'],
    chamfer: 'rgba(255,255,255,0.03)', rim: 'rgba(255,255,255,0.07)', bottomEdge: 'rgba(0,0,0,0.5)',
    cast: 'rgba(0,0,0,0.85)', pressDim: 0.84,
  },
};

/** §3.2 brushed silver (Sony HCD-SE1, Panasonic stacking hi-fi). */
export const SILVER_CHASSIS: ChassisTokens = {
  ...DEFAULT_CHASSIS,
  // §6.1, Deck.mockup `W_FACE.silver`: a brushed-silver face, a dark 1 pt gap, then the ring and glow
  // in the controls colour; a polished-aluminium drum with DARK cuts (the pair inverts — TRAP).
  wellFace:      ['#c9c6bf', '#c9c6bf', '#c9c6bf'],
  wellTexture:   'silver',
  wellBorder:    'rgba(0,0,0,0.55)',
  wellInnerGlowA: 0,
  wellRingA:     0.35,
  wellGlowA:     0.40,
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
    screws: true, gloss: false, windowLip: 'rgba(255,255,255,0.80)',
  },
  dome: {
    look: 'cap', slotBg: '#141414', slotShade: 'rgba(0,0,0,0.95)', slotLip: 'rgba(255,255,255,0.85)',
    capBase: '#c9c6bf', capBorder: '#8d8a83',
    capSheen: ['rgba(255,255,255,0.60)', 'rgba(255,255,255,0)', 'rgba(0,0,0,0.22)'],
    chamfer: rgba('255,255,255', 0.95 * 0.3), rim: 'rgba(255,255,255,0.07)', bottomEdge: 'rgba(0,0,0,0.5)',
    cast: 'rgba(0,0,0,0.85)', pressDim: 0.84,
  },
};

/** §3.3 brushed black (Stuart's Yamaha RX-V583): no screws, a gloss acrylic display panel. */
export const BLACK_CHASSIS: ChassisTokens = {
  ...DEFAULT_CHASSIS,
  // §6.1 — ★ THE MOCKUP WINS over the brief's table here: `W_FACE.black` is NOT the default well. It
  // has a brushed-black face (#000 gap, ring .30, glow .38, top lip .14) and a NEUTRAL grey drum
  // (#070707 … #050505) where the default's is faintly green. Notches stay light on dark.
  wellFace:      ['#1b1c1e', '#1b1c1e', '#1b1c1e'],
  wellTexture:   'black',
  wellBorder:    '#000000',
  wellInnerGlowA: 0,
  wellRingA:     0.30,
  wellGlowA:     0.38,
  wellTopLip:    'rgba(255,255,255,0.14)',
  drumBody:      ['#070707', '#181818', '#222222', '#171717', '#050505'],
  rimLine:       'rgba(180,182,186,0.14)',
  ridgeHighlight:'rgba(160,162,166,0.10)',
  notchMinor:    'rgba(168,168,170,0.22)',
  notchMed:      'rgba(169,169,172,0.36)',
  notchMajor:    'rgba(170,170,174,0.55)',
  keysFace:      '#1b1c1e',
  keysSlot:      null,
  plate: {
    base: '#1b1c1e', border: '#3a3c40', radius: 16, texture: 'black',
    // ★ The mockup's own black lighting, not "silver at a third" (the brief's paraphrase of it).
    lightColors: ['rgba(255,255,255,0)', 'rgba(255,255,255,0.03)', 'rgba(255,255,255,0.11)',
                  'rgba(255,255,255,0.02)', 'rgba(0,0,0,0.10)', 'rgba(0,0,0,0.25)'],
    lightPos:    [0, 0.18, 0.36, 0.52, 0.74, 1],
    radialColor: 'rgba(255,255,255,0.07)',
    bottomShade: null,
    lipTop: 'rgba(255,255,255,0.22)', lipTop2: 'rgba(255,255,255,0.06)', lipBottom: 'rgba(0,0,0,0.60)',
    screws: false, gloss: true, windowLip: 'rgba(255,255,255,0.14)',
  },
  dome: {
    look: 'cap', slotBg: '#030303', slotShade: 'rgba(0,0,0,0.95)', slotLip: 'rgba(255,255,255,0.14)',
    capBase: '#1d1e20', capBorder: '#050505',
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
}

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

  return { settings: s, chassis, controls, text, deck, keyLegend, vts };
}
