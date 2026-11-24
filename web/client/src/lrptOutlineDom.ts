/**
 * lrptOutlineDom.ts — METEOR LRPT's MAP OUTLINE drawn on the web page (V12, 2026-11-24): an SVG laid exactly over a
 * picture (viewBox = the picture's own pixels, so it follows any size the picture is shown at). The rule and the parsing
 * are src/utils/lrptOutline.ts (shared with the app); this is only the drawing. Used by the gallery viewer (main.ts) and
 * the SATELLITE box's live picture (satellites.ts).
 */
import { OUTLINE_INK, OUTLINE_KEY, outlineFromStored, outlineToStored, type Outline } from '../../../src/utils/lrptOutline';

const NS = 'http://www.w3.org/2000/svg';

/** The viewer's choice (remembered per browser once switched; OUTLINE_DEFAULT_ON until then). */
export function outlineOn(): boolean {
  try { return outlineFromStored(localStorage.getItem(OUTLINE_KEY)); } catch { return outlineFromStored(null); }
}
export function setOutlineOn(on: boolean) {
  try { localStorage.setItem(OUTLINE_KEY, outlineToStored(on)); } catch { /* private browsing: this page only */ }
}

/** Draw `o` into `svg` (cleared first). `shownWidth` = the picture's width on screen, for the labels' size. */
export function drawOutline(svg: SVGSVGElement, o: Outline, shownWidth: number) {
  while (svg.firstChild) svg.removeChild(svg.firstChild);
  svg.setAttribute('viewBox', `0 0 ${o.w} ${o.h}`);
  svg.setAttribute('preserveAspectRatio', 'none');
  const path = (d: string, colour: string, width: number, dash = '') => {
    if (!d) return;
    const p = document.createElementNS(NS, 'path');
    p.setAttribute('d', d); p.setAttribute('fill', 'none'); p.setAttribute('stroke', colour);
    p.setAttribute('stroke-width', String(width)); p.setAttribute('vector-effect', 'non-scaling-stroke');
    p.setAttribute('stroke-linejoin', 'round');
    if (dash) p.setAttribute('stroke-dasharray', dash);
    svg.appendChild(p);
  };
  path(o.grid, OUTLINE_INK.grid, 1, '4 3');
  path(o.border, OUTLINE_INK.border, 1.2);
  path(o.lake, OUTLINE_INK.lake, 1.2);
  path(o.coast, OUTLINE_INK.coast, 1.4);
  const fs = 11 * (o.w / Math.max(50, shownWidth || o.w));
  for (const l of o.labels) {
    const t = document.createElementNS(NS, 'text');
    t.setAttribute('x', String(l.x + fs * 0.3)); t.setAttribute('y', String(l.y + fs * 1.1));
    t.setAttribute('font-size', fs.toFixed(1)); t.setAttribute('fill', OUTLINE_INK.grid);
    t.setAttribute('stroke', OUTLINE_INK.shade); t.setAttribute('stroke-width', (fs * 0.25).toFixed(1));
    t.setAttribute('paint-order', 'stroke'); t.setAttribute('font-family', 'system-ui, sans-serif');
    t.textContent = l.text;
    svg.appendChild(t);
  }
}
