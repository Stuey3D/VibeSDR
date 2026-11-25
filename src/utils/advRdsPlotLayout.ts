/**
 * ★★ THE ADVANCED RDS PLOTS AT A GIVEN WIDTH (iPhone Duo, half-open BIG: the lower panel is ~296 pt of content — Stuart:
 * "labels getting cut off in advanced rds mode"). The plots row was laid out for a box: the constellation column
 * (≥ 120) beside the MPX / scope / figures column (a 200 pt basis that never shrinks), 10 apart = 330 pt, and the
 * symbol trace 310 pt — wider than the panel, so its right edge was clipped mid-word.
 * `null` width = the box as it always was (every layout but the split). Pure, so scripts/test_decoder_shell_layout.ts
 * can prove nothing is wider than the panel.
 */
/** The figures' block (deviation key 132 + gap 6 + value 62; the L/R rows; the verdict) — fixed, as before. */
export const ADVRDS_FIGURES_W = 200;
/** The longest fixed-width row in the figures: "MPX power:" (132) + gap 6 + "+12.6 dB (24 s)" (~96). */
export const ADVRDS_POWER_ROW_W = 132 + 6 + 96;
/** "SCOPE VIEW · PILOT · STEREO · RDS" at 10 pt, letter-spacing 1 (measured ~226 pt; with headroom). */
export const ADVRDS_SCOPE_LABEL_W = 236;

export interface AdvRdsPlotLayout {
  /** Constellation above the MPX column instead of beside it. */
  stacked: boolean;
  constSize: number;
  /** MPX, scope views and the deviation bar. */
  plotW: number;
  eyeW: number;
}

export function advRdsPlotLayout(contentW: number | null): AdvRdsPlotLayout {
  if (contentW == null) return { stacked: false, constSize: 120, plotW: 180, eyeW: 310 };
  // Side by side needs the constellation, the gap and the MPX column at its widest fixed line (the scope label).
  const side = 120 + 10 + Math.max(ADVRDS_FIGURES_W, ADVRDS_POWER_ROW_W, ADVRDS_SCOPE_LABEL_W);
  const stacked = contentW < side;
  return {
    stacked,
    constSize: Math.min(120, Math.floor(contentW)),
    plotW: Math.min(stacked ? 240 : 180, Math.floor(contentW)),
    eyeW: Math.min(310, Math.floor(contentW)),
  };
}

/** The widest thing the plots block draws at this layout (for the test: never wider than the content). */
export function advRdsPlotsWidth(l: AdvRdsPlotLayout): number {
  const mpxCol = Math.max(l.plotW, ADVRDS_FIGURES_W, ADVRDS_POWER_ROW_W, ADVRDS_SCOPE_LABEL_W);
  const row = l.stacked ? Math.max(l.constSize, mpxCol) : l.constSize + 10 + mpxCol;
  return Math.max(row, l.eyeW);
}
