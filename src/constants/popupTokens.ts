/**
 * popupTokens.ts — the popups', menus' and chat's palette, per chassis × controls colour × text
 * colour (faceplates brief §10.3; numbers from docs/faceplates/Popup.mockup.dc.html `renderVals()`).
 *
 * Every popup (FreqModal, AudioSheet, MenuSheet, ChatDrawer, RecordingsOverlay, KeyboardShortcuts,
 * PasswordModal, IdentModal, CityPickerModal, AboutOverlay, StepPicker) takes its colours from HERE,
 * through PopupShell's `usePopupTheme()`. No popup holds its own gold any more.
 *
 * ★★★ THE DEFAULT CHASSIS IS TODAY, VALUE FOR VALUE (§10.3: "Unchanged. Today's dark glass and gold
 *   selection, pixel for pixel"). The `gold` group below is the literals the files drew — 83 of them
 *   (AudioSheet 35, MenuSheet 38, ChatDrawer 9, RecordingsOverlay 1), mostly inline per button — named
 *   by the ROLE they play (a selected legend, a slider's fill, a value readout…), because on silver /
 *   black each role becomes something different: a lit legend in the controls colour, an engraved
 *   value, a readout in the text colour. scripts/test_popup.ts proves the default values are today's.
 *
 * ★★ TWO GOLDS, NOT ONE. The accessibility-skin menus (MenuSheet, AudioSheet) drew `C.gold` =
 *   #ffe566 (a pale yellow); the chat drawer and the recordings list drew the older amber #ffb833.
 *   They are different colours on screen today, so they are different tokens (`accent` / `amber`).
 *
 * ★★★ SILVER MEANS DARK TEXT ON A LIGHT PLATE (§10.3 TRAP). Anything light that sits directly on the
 *   plate vanishes on silver, so every text role here says what it sits ON: `label` / `note` /
 *   `value` are engraved into the plate; `win*` / `readout` are inside a recessed window (#070605),
 *   where light text is right on every chassis.
 *
 * ★ MEANING COLOURS ARE NOT TOKENS OF THE LOOK: chat keeps own-name blue, others amber, system grey
 *   italic (§10.3), and red for danger / recording stays red.
 *
 * Pure: type-only imports, so Node runs scripts/test_popup.ts straight from this file.
 */

import type { Chassis, DisplayStyle } from './faceplate';

export interface PopupEntryStyle {
  fontFamily: string;
  fontSize:   number;
  fontWeight: '400' | '700' | '900';
  fontStyle:  'normal' | 'italic';
  letterSpacing: number;
  color:      string;
  /** Text-shadow glow colour (radius 6), or null. */
  glow:       string | null;
  /** 14-segment: the TextInput's value is fed through toSegCells (identity for a frequency). */
  seg:        boolean;
}

export interface PopupTokens {
  chassis: Chassis;
  /** Silver / black: a brushed plate, dome keys, engraved labels, recessed windows. */
  metal:   boolean;
  silver:  boolean;

  // ── TODAY'S GOLD, BY ROLE (default = the literals; metal = what that role becomes) ──
  gold: {
    /** A selected / active key's legend (`btnTextActive`, SectionIcon when active, the colour-map
     *  and profile lists' current row). */
    sel:        string;
    /** A selected key's border (`btnActive`, `btnSelected`). */
    selBorder:  string;
    /** A slider's filled track (minimumTrackTintColor). */
    fill:       string;
    /** A slider's thumb. */
    thumb:      string;
    /** The number beside a slider (`bwVal`, `bwEdgeVal`, `stepSliderVal`). */
    value:      string;
    /** A live data readout (the audio sheet's SIGNAL, the recording timer). */
    readout:    string;
    /** An accent glyph on a key (‹ BACK, − / +, ◄ ►). */
    glyph:      string;
    /** The Full-Keyboard-Access notice (title, key names, border). */
    notice:     string;
    noticeBorder: string;
    /** The menu's CLOSE legend. */
    close:      string;
    /** ★ The OLDER amber (#ffb833): chat, the recordings list's mode, the admin unlock box. */
    amber:      string;
    /** Amber at alpha `a` (rgba(255,184,51,a)) — chat's placeholders, sync legend, room line. */
    amberA:     (a: number) => string;
  };

  // ── Silver / black (unused on default, where every popup keeps its own look) ──
  /** Engraved label / section header on the plate (§10.3). */
  label:      string;
  /** The engraving's lip as a hard 1 pt text shadow: dy +1 white (silver), -1 black (black). */
  engrave:    { color: string; dy: number } | null;
  /** An own-name / other-name engraving on the plate (chat's user list, mockup `engraveName`). */
  engraveName: { color: string; dy: number } | null;
  /** A secondary sentence engraved on the plate (hints, reasons) — contrast-lifted (test_popup). */
  note:       string;
  /** An engraved value beside a fader (mockup `value`). */
  value:      string;
  /** Unlit key legend, engraved into the cap. */
  legend:     string;
  legendShadow: { color: string; dy: number } | null;
  /** A LIT legend (selected, or the primary action) — the controls colour, with its glow. */
  legendLit:  string;
  legendLitGlow: string;
  /** Silver's dark copy under a lit legend (the deck keys' `keyLegend.shade`), or null. */
  legendLitShade: string | null;
  /** The 4 × 4 pt LED pip: lit (controls colour + glow) and unlit (a dark inset dot). */
  pipOn:      string;
  pipGlow:    string;
  pipOff:     string;
  pipOffInset: string;
  /** Recessed data window: #070605, an inner shadow at the top, a 1 pt light lip below. */
  window:     { bg: string; lip: string; border: string; shade: string; radius: number };
  /** Text in a window: the TEXT colour (neon under Nixie), its dim companion, a rule. */
  winText:    string;
  winDim:     string;
  winRule:    string;
  readout:    string;
  readoutGlow: string;
  /** Chat meaning colours (kept on every chassis — only their shade changes for the surface). */
  chatOwn:    string;   // in the window
  chatOther:  string;
  chatText:   string;
  chatSys:    string;
  chatOwnPlate:   string;   // engraved on the plate (the user list)
  chatOtherPlate: string;
  /** The slide fader (§10.3): recessed slot, fill in the controls colour, live level behind it,
   *  a brushed cap with a lit index line. */
  fader: {
    slot:      string;
    slotLip:   string;
    fill:      string;
    fillGlow:  string;
    level:     string;
    capColors: [string, string, string];
    capBorder: string;
    capHi:     string;
    lineColor: string;
    lineGlow:  string;
  };
  /** The shell: plate edge, top highlight, engraved grab-handle groove and its lip. */
  shell: { border: string; topHi: string; handle: string; handleLip: string };
  /** Drop shadow of a sheet over the spectrum (`0 -4px 18px rgba(0,0,0,.8)`) — off with Transparency OFF. */
  sheetShadow: string;
  /** A hairline rule / sub-panel edge on the plate (section dividers). */
  rule:       string;
  /** A destructive key's legend (RESET, delete) — a meaning colour, red on every chassis. */
  danger:     string;
  /** The tune entry, following the Display style (§10.3). */
  entry:      PopupEntryStyle;
}

// ── Today's literals ─────────────────────────────────────────────────────────

/** MenuSheet / AudioSheet `C.gold` and `C.goldDim` (the accessibility skin's pale yellow). */
export const TODAY_ACCENT     = '#ffe566';
export const TODAY_ACCENT_DIM = 'rgba(255,229,102,0.70)';
/** ChatDrawer / RecordingsOverlay / the admin unlock box: the older amber. */
export const TODAY_AMBER      = '#ffb833';
export const todayAmberA = (a: number) => `rgba(255,184,51,${a})`;

const FONT = 'Atkinson Hyperlegible';

/** The default chassis: every value is the literal the popup drew before row 10. */
function defaultTokens(): PopupTokens {
  return {
    chassis: 'default', metal: false, silver: false,
    gold: {
      sel: TODAY_ACCENT, selBorder: TODAY_ACCENT_DIM, fill: TODAY_ACCENT, thumb: TODAY_ACCENT,
      value: TODAY_ACCENT, readout: TODAY_ACCENT, glyph: TODAY_ACCENT,
      notice: TODAY_ACCENT, noticeBorder: TODAY_ACCENT_DIM, close: TODAY_ACCENT_DIM,
      amber: TODAY_AMBER, amberA: todayAmberA,
    },
    // Not drawn on default (each popup keeps today's own look); the mockup's `isDef` values, so a
    // stray reader gets something sensible rather than undefined.
    label: 'rgba(255,255,255,0.62)', engrave: null, engraveName: null, note: 'rgba(255,255,255,0.45)',
    value: TODAY_ACCENT,
    legend: 'rgba(255,255,255,0.88)', legendShadow: null,
    legendLit: TODAY_ACCENT, legendLitGlow: 'rgba(0,0,0,0)', legendLitShade: null,
    pipOn: TODAY_ACCENT, pipGlow: 'rgba(0,0,0,0)', pipOff: 'transparent', pipOffInset: 'transparent',
    window: { bg: 'rgba(255,255,255,0.03)', lip: 'transparent', border: 'rgba(255,255,255,0.12)',
              shade: 'transparent', radius: 6 },
    winText: '#ffffff', winDim: 'rgba(255,255,255,0.5)', winRule: 'rgba(255,255,255,0.10)',
    readout: TODAY_ACCENT, readoutGlow: 'rgba(0,0,0,0)',
    chatOwn: '#7fd1ff', chatOther: '#ffd27f', chatText: 'rgba(255,240,210,0.92)', chatSys: 'rgba(200,200,200,0.55)',
    chatOwnPlate: '#7fd1ff', chatOtherPlate: '#ffd27f',
    fader: {
      slot: 'rgba(255,255,255,0.14)', slotLip: 'transparent', fill: 'rgba(255,224,102,0.9)',
      fillGlow: 'rgba(0,0,0,0)', level: 'rgba(255,255,255,0.85)',
      capColors: ['#ffe066', '#ffe066', '#ffe066'], capBorder: '#ffe066', capHi: 'transparent',
      lineColor: 'transparent', lineGlow: 'transparent',
    },
    shell: { border: 'rgba(255,255,255,0.10)', topHi: 'transparent', handle: 'rgba(255,255,255,0.22)', handleLip: 'transparent' },
    sheetShadow: 'rgba(0,0,0,0.8)',
    rule: 'rgba(255,255,255,0.12)', danger: '#ff6666',
    entry: { fontFamily: FONT, fontSize: 32, fontWeight: '400', fontStyle: 'normal', letterSpacing: 3,
             color: '#ffffff', glow: null, seg: false },
  };
}

// ── Contrast (shared with scripts/test_popup.ts) ─────────────────────────────

/**
 * ★ Engraved text that carries information must clear 4.5:1 on the plate (§10.3; "lighten the same
 * hue if not" — on SILVER the text is dark, so the same rule means DARKEN the same hue). Measured by
 * scripts/test_popup.ts against the plate at the point of its lighting that is WORST for the text
 * (silver's .16 black end, black's .11 white band — the decoder test's rule). The mockup's values
 * that failed, and what they became (smallest 0.05 step toward black / white that clears 4.5:1):
 *   silver note  #55524b 3.2:1 → #3c3935 4.7:1     black note  #7d8086 3.1:1 → #9ea0a4 4.7:1
 *   silver chat own-name   #1d4f7a 3.5:1 → #173f62 4.5:1
 *   silver chat other-name #6a4a0a 3.3:1 → #503808 4.5:1
 * Labels (#35332e 5.2 / #a3a6ac 5.0), values and unlit legends cleared as drawn.
 */
export const CONTRAST_FIX = {
  silverNote: '#3c3935', blackNote: '#9ea0a4', silverOwn: '#173f62', silverOther: '#503808',
} as const;

/**
 * Silver / black. `controlsRgb` = the controls colour's triplet (LED[controls].rgb);
 * `textRgb` = the RESOLVED text colour's triplet (neon under Nixie — resolveTextColour).
 */
function metalTokens(chassis: 'silver' | 'black', controlsRgb: string, textRgb: string,
                     display: DisplayStyle): PopupTokens {
  const silver = chassis === 'silver';
  const nixie = display === 'nixie';
  const L = (a: number) => `rgba(${controlsRgb},${a})`;
  // ★ Under Nixie the mockup's readouts use the neon READING colour (255,154,85 = #ff9a55), not the
  //   tubes' pale core — the same split the deck makes between the tubes and the S-reading (§2).
  const txRgb = nixie ? '255,154,85' : textRgb;
  const T = (a: number) => `rgba(${txRgb},${a})`;
  const label = silver ? '#35332e' : '#a3a6ac';
  const engrave = silver ? { color: 'rgba(255,255,255,0.60)', dy: 1 } : { color: 'rgba(0,0,0,0.90)', dy: -1 };
  const legend = silver ? '#2a2824' : '#cfd2d7';
  const valueC = silver ? '#2a2824' : '#d8dade';

  const E: Record<DisplayStyle, Omit<PopupEntryStyle, 'color' | 'glow'>> = {
    hyper: { fontFamily: FONT,             fontSize: 30, fontWeight: '700', fontStyle: 'normal', letterSpacing: 2, seg: false },
    nixie: { fontFamily: 'Nixie One',      fontSize: 32, fontWeight: '400', fontStyle: 'normal', letterSpacing: 1, seg: false },
    dot:   { fontFamily: 'Doto',           fontSize: 30, fontWeight: '900', fontStyle: 'normal', letterSpacing: 1, seg: false },
    // ★ DSEG14 is the Bold Italic cut already (the file IS that face) — no synthetic italic on top.
    seg:   { fontFamily: 'DSEG14 Classic', fontSize: 24, fontWeight: '400', fontStyle: 'normal', letterSpacing: 2, seg: true },
  };
  const entry: PopupEntryStyle = nixie
    // ★★★ The Nixie rule (§2): Nixie One is ALWAYS neon — #ffc48a with the orange glow.
    ? { ...E.nixie, color: '#ffc48a', glow: '#ff6410' }
    : { ...E[display], color: T(1), glow: T(0.6) };

  return {
    chassis, metal: true, silver,
    gold: {
      sel: L(1), selBorder: 'transparent', fill: L(0.85), thumb: L(0.95),
      value: valueC, readout: T(1), glyph: legend,
      // ★ The notice sits in a recessed window on metal (light text), so it is lit in the text colour.
      notice: T(1), noticeBorder: 'rgba(0,0,0,0.80)', close: legend,
      amber: label, amberA: () => label,
    },
    label, engrave,
    engraveName: silver ? { color: 'rgba(255,255,255,0.50)', dy: 1 } : { color: 'rgba(0,0,0,0.90)', dy: -1 },
    note: silver ? CONTRAST_FIX.silverNote : CONTRAST_FIX.blackNote,
    value: valueC,
    legend,
    legendShadow: silver ? { color: 'rgba(255,255,255,0.55)', dy: 1 } : { color: 'rgba(0,0,0,0.80)', dy: -1 },
    legendLit: L(1), legendLitGlow: L(0.6),
    legendLitShade: silver ? 'rgba(0,0,0,0.5)' : null,
    pipOn: L(1), pipGlow: L(0.95),
    pipOff: silver ? 'rgba(0,0,0,0.30)' : 'rgba(255,255,255,0.07)',
    pipOffInset: silver ? 'rgba(0,0,0,0.5)' : 'rgba(0,0,0,0.8)',
    window: { bg: '#070605', lip: silver ? 'rgba(255,255,255,0.80)' : 'rgba(255,255,255,0.14)',
              border: 'rgba(0,0,0,0.80)', shade: 'rgba(0,0,0,0.60)', radius: 7 },
    // ★ winDim .45 → .50: the mockup's .45 measured 4.47:1 on the #070605 window (test_popup).
    winText: T(1), winDim: 'rgba(255,255,255,0.50)', winRule: 'rgba(255,255,255,0.08)',
    readout: T(1), readoutGlow: T(0.6),
    chatOwn: '#7fd1ff', chatOther: '#ffd27f', chatText: '#eeeae2', chatSys: 'rgba(200,200,200,0.55)',
    chatOwnPlate: silver ? CONTRAST_FIX.silverOwn : '#8fc8ff',
    chatOtherPlate: silver ? CONTRAST_FIX.silverOther : '#e6c27a',
    fader: {
      slot: '#0a0908', slotLip: silver ? 'rgba(255,255,255,0.80)' : 'rgba(255,255,255,0.14)',
      fill: L(0.85), fillGlow: L(0.6), level: 'rgba(255,255,255,0.22)',
      capColors: silver ? ['#f1efea', '#bdbab3', '#9f9c95'] : ['#34363b', '#16171a', '#0c0c0e'],
      capBorder: silver ? '#7d7a73' : '#050505',
      capHi: silver ? '#ffffff' : 'rgba(255,255,255,0.25)',
      lineColor: L(0.95), lineGlow: L(0.8),
    },
    shell: silver
      ? { border: '#8b8983', topHi: 'rgba(255,255,255,0.95)', handle: 'rgba(0,0,0,0.38)', handleLip: 'rgba(255,255,255,0.75)' }
      : { border: '#3a3c40', topHi: 'rgba(255,255,255,0.22)', handle: 'rgba(0,0,0,0.70)', handleLip: 'rgba(255,255,255,0.12)' },
    sheetShadow: 'rgba(0,0,0,0.8)',
    rule: silver ? 'rgba(0,0,0,0.22)' : 'rgba(255,255,255,0.10)', danger: silver ? '#9e1b14' : '#ff6b62',
    entry,
  };
}

/** Unmemoised — the test sweeps it. */
export function buildPopupTokens(chassis: Chassis, controlsRgb: string, textRgb: string,
                                 display: DisplayStyle): PopupTokens {
  return chassis === 'default' ? defaultTokens() : metalTokens(chassis, controlsRgb, textRgb, display);
}

const cache = new Map<string, PopupTokens>();

/**
 * ★ The resolver, memoised on its inputs, so every popup on screen shares ONE object per setting and
 * a style sheet built from it (PopupShell `usePopupStyles`) is built once, not per render — a meter
 * update never touches it. The default chassis is one object whatever the colours: it does not
 * read them.
 */
export function popupTokensFor(chassis: Chassis = 'default', controlsRgb = '61,255,114',
                               textRgb = '61,255,114', display: DisplayStyle = 'hyper'): PopupTokens {
  const key = chassis === 'default' ? 'default' : `${chassis}|${controlsRgb}|${textRgb}|${display}`;
  let t = cache.get(key);
  if (!t) { t = buildPopupTokens(chassis, controlsRgb, textRgb, display); cache.set(key, t); }
  return t;
}
