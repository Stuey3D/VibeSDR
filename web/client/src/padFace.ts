/**
 * ★★★ FACE-BUTTON GLYPHS, NINTENDO SWITCH STYLE (Stuart 2026-11-06): a diamond of four circles, three HOLLOW and the one
 * meant SOLID — never a letter and never a PlayStation shape. ABXY layouts differ (the R36S has A on the right and B at
 * the bottom; an Xbox pad is the reverse), and the controls go BY POSITION (bottom = enter/select, right = back), so the
 * glyph says "the bottom one" on every pad.
 * ★ Inline SVG in currentColor, 1em tall, sitting on the text baseline — drop it into any hint with innerHTML.
 */
export type FacePos = 'top' | 'left' | 'right' | 'bottom';

const AT: Record<FacePos, [number, number]> = { top: [8, 3.2], left: [3.2, 8], right: [12.8, 8], bottom: [8, 12.8] };

export function padFace(solid: FacePos, label?: string): string {
  const dots = (Object.keys(AT) as FacePos[]).map((p) => {
    const [x, y] = AT[p];
    return `<circle cx="${x}" cy="${y}" r="2.3" fill="${p === solid ? 'currentColor' : 'none'}" stroke="currentColor" stroke-width="1.1"/>`;
  }).join('');
  const name = label ?? `the ${solid} button`;
  return `<svg class="padFace" viewBox="0 0 16 16" width="1.15em" height="1.15em" role="img" aria-label="${name}"`
    + ` style="vertical-align:-0.22em;display:inline-block">${dots}</svg>`;
}
