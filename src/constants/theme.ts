/**
 * theme.ts — VibeSDR design tokens
 *
 * Values taken directly from CSS :root and [data-theme="amber"] variables
 * in VibeSDR_Mockup_SAVE.html. This is the amber (Nixie One) theme.
 * White (Atkinson) theme values are commented for reference.
 */

// ── Amber theme (primary) ─────────────────────────────────────────────────────
export const Colors = {
  // Bar / panel
  barBg:           'rgba(10,8,4,0.84)',      // --bar-bg
  barBorder:       'rgba(255,160,0,0.22)',   // --bar-border (amber)
  barInnerGlow:    'rgba(255,160,0,0.07)',   // --bar-inner-glow
  barShadow:       'rgba(0,0,0,0.85)',       // --bar-shadow alpha

  // Buttons
  btnBg:           'rgba(20,10,0,0.75)',     // --btn-bg
  pillBg:          'rgb(20,10,0)',           // --pill-bg (solid for freq/mode)
  btnBorder:       'rgba(255,160,0,0.35)',   // --btn-border
  btnText:         '#ffb833',               // --btn-text
  btnActiveBg:     'rgba(255,200,0,0.10)',   // --btn-active-bg
  btnActiveBdr:    'rgba(255,229,102,0.55)', // --btn-active-bdr
  btnActiveText:   '#ffe566',               // --btn-active-text

  // Frequency display
  freqColor:       '#ffb833',               // --freq-color
  unitColor:       '#886600',               // --unit-color

  // Mode / SNR
  modeColor:       '#ffb833',               // --mode-color
  snrColor:        'rgba(255,160,0,0.50)',   // --snr-color

  // Signal meter
  meterTrack:      'rgba(105,98,82,0.30)',   // --meter-track
  peakLine:        'rgba(255,245,200,0.92)', // --peak-line

  // Drum
  drumBorder:      'rgba(0,200,50,0.22)',    // --drum-border
  drumGlow:        'rgba(0,185,45,0.18)',    // --drum-glow alpha

  // Clock
  clockColor:      'rgba(255,160,0,0.25)',   // --clock-color

  // Misc
  amber:           '#ffb833',
  amberGlow:       '#ffaa00',
  innerBorder:     'rgba(70,60,45,0.45)',    // mode-btn border-left
  recRed:          '#e05050',
  chatBlue:        'rgba(40,140,255,0.85)',

  // Legacy aliases used elsewhere
  bg:           '#080601',
  green:        '#28A745',
  red:          '#E05050',
  white:        '#FFFFFF',
  textDim:      'rgba(255,160,0,0.45)',
  unit:         '#886600',
  border:       'rgba(255,160,0,0.22)',
  borderBright: 'rgba(255,160,0,0.55)',
  amberDim:     'rgba(255,160,0,0.45)',
  gold:         '#ffb833',
  goldDim:      'rgba(255,184,51,0.55)',
} as const;

// ── themeFor — legacy helper used by InstancePickerScreen ─────────────────────
export function themeFor(_viewMode?: string) {
  return {
    colors: Colors,
    font:   Fonts.ui,
    scale:  1,
  };
}

export const Fonts = {
  /**
   * Primary font — Nixie One (instrument aesthetic).
   * Must be loaded via expo-font before use:
   *   useFonts({ 'Atkinson Hyperlegible': require('../assets/fonts/NixieOne-Regular.ttf') })
   * Falls back to Courier New until loaded.
   */
  ui:           'Atkinson Hyperlegible',
  mono:         'Atkinson Hyperlegible',
  monoFallback: 'Courier New',
  /**
   * ★★ Every decoder box (DecoderShell and everything inside it) — PINNED, never the display
   * style. The boxes are dense instrument read-outs (brief §10: "the text remains Hyperlegible");
   * DecoderPanel used to take `theme.font`, which would have carried Nixie One into RTTY text the
   * moment a display style could pick it.
   */
  decoder:      'Atkinson Hyperlegible',
} as const;

// ── Size tokens (from mockup CSS vars) ────────────────────────────────────────
export const Sizes = {
  freqFontSize:   26,   // --freq-size: 26px
  freqWidth:      148,  // --freq-width: 148px
  unitFontSize:   11,   // lsv-unit font-size
  modeFontSize:   14,   // --mode-size: 14px
  snrFontSize:    9,    // lsv-snr-disp font-size
  btnFontSize:    13,   // --btn-size: 13px
  clockFontSize:  8,    // lsv-clock font-size
  drumHeight:     60,   // --drum-h: 60px
  barRadius:      18,   // --bar-radius: 18px
  rowGap:         7,    // #lsv-row row-gap
  colGap:         8,    // #lsv-row column-gap
  barPadH:        12,   // #lsv-bar padding horizontal
  barPadTop:      8,    // #lsv-bar padding top
  barPadBot:      10,   // #lsv-bar padding bottom
} as const;
