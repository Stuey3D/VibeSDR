// globePaint.ts — ★★★ beta97 the SATELLITE box's half-globe painter (web canvas), lifted out of satellites.ts drawGlobe so
// the pass preview (satPreviewWeb.ts) paints with the same code. The geometry is src/services/satellites.ts globeScene.
import { GLOBE_ARROW_PX, GLOBE_GLYPH, GLOBE_RX_DOT, globeArrowPath, globeGlyph, globeLabels, globeLandInk, globeLayers, globeScene } from '../../../src/services/satellites';

/** ★★★ beta97 THE HALF-GLOBE's PAINTER — drawGlobe's own drawing, lifted out so the pass preview paints with the same
 *  code: 'all' = the live box (in its order); 'back' = what never moves in a preview (dome, graticule, land, the dotted
 *  orbit with its tick and chevrons, the receiver) — cached once as a picture; 'front' = what moves (the footprint, the
 *  beam, the bird, the labels) over it every frame. */
export interface GlobeInk { sc: number; ink: string; amber: string; ground: string; ly: ReturnType<typeof globeLayers>;
  labels: { rx: ReturnType<typeof globeLabels>['rx']; sat: ReturnType<typeof globeLabels>['sat'] }; font: number }
export function paintGlobe(ctx: CanvasRenderingContext2D, g: ReturnType<typeof globeScene>, k: GlobeInk, part: 'all' | 'back' | 'front') {
  const { sc, ink, amber, ground, ly } = k;
  const back = part !== 'front', front = part !== 'back';
  ctx.save();
  ctx.beginPath(); ctx.rect(0, 0, g.w, g.cy); ctx.clip();
  if (back) {
    ctx.beginPath(); ctx.arc(g.cx, g.cy, g.r, Math.PI, 2 * Math.PI);
    ctx.globalAlpha = 0.12; ctx.fillStyle = ink; ctx.fill();
    ctx.globalAlpha = 0.55; ctx.strokeStyle = ink; ctx.lineWidth = 2; ctx.stroke();
    if (g.grid) { ctx.globalAlpha = 0.12; ctx.lineWidth = 1; ctx.stroke(new Path2D(g.grid)); }   // ★ fainter than the land
    // ★ rough land (globeCoast.ts) — beta91 neutral, never the faceplate's hue, so it reads through the beam (globeLandInk)
    if (g.coast) { ctx.globalAlpha = 0.7; ctx.lineWidth = 1; ctx.strokeStyle = globeLandInk(ground); ctx.stroke(new Path2D(g.coast)); ctx.strokeStyle = ink; }
  }
  if (front) {
    if (g.foot) { ctx.globalAlpha = 0.28; ctx.fillStyle = amber; ctx.fill(new Path2D(g.foot)); }
    if (ly.beam && g.beam) { ctx.globalAlpha = 0.35; ctx.fillStyle = amber; ctx.fill(new Path2D(g.beam)); }
  }
  // ★ 1.7 the orbit, dotted, and its highest point
  if (back && g.orbit) {
    ctx.globalAlpha = 0.85; ctx.strokeStyle = amber; ctx.lineWidth = 1.5 * sc; ctx.lineCap = 'round';
    ctx.setLineDash([1.5 * sc, 4 * sc]); ctx.stroke(new Path2D(g.orbit)); ctx.setLineDash([]);
    if (ly.tick && g.tcaTick) { ctx.lineWidth = 1.5 * sc; ctx.stroke(new Path2D(g.tcaTick)); }
    // ★ beta91 which way it travels: open chevrons along increasing time, haloed in the ground so they read over the land
    if (ly.arrows && g.arrows.length) {
      const ar = new Path2D(globeArrowPath(g.arrows, GLOBE_ARROW_PX * sc));
      ctx.lineJoin = 'round';
      ctx.globalAlpha = 0.8; ctx.strokeStyle = ground; ctx.lineWidth = 3.6 * sc; ctx.stroke(ar);
      ctx.globalAlpha = 1; ctx.strokeStyle = amber; ctx.lineWidth = 1.6 * sc; ctx.stroke(ar);
    }
  }
  ctx.globalAlpha = 1;
  // ★ the receiver: green with a dark ring on every faceplate (never the dome's ink) — a preview draws it over the footprint
  if (g.rx) {
    ctx.beginPath(); ctx.arc(g.rx.x, g.rx.y, GLOBE_RX_DOT.r * sc, 0, 2 * Math.PI);
    ctx.fillStyle = GLOBE_RX_DOT.fill; ctx.fill();
    ctx.lineWidth = GLOBE_RX_DOT.ringW * sc; ctx.strokeStyle = GLOBE_RX_DOT.ring; ctx.stroke();
  }
  if (front) {
    // ★ 1.8 the bird: tube, dish towards the Earth, panels along its heading (cells ruled)
    // ★★ beta96 SMALL: the satellite as a glyph — a square body, a line either side, on the BIG bird's heading, whole pixels
    if (ly.glyph && g.bird && g.sat) {
      const gl = globeGlyph(g.sat, g.bird.u, sc), bird = globeLandInk(ground);
      ctx.globalAlpha = 1; ctx.lineCap = 'butt'; ctx.strokeStyle = bird; ctx.lineWidth = GLOBE_GLYPH.line * sc; ctx.stroke(new Path2D(gl.panels));
      ctx.fillStyle = bird; ctx.fill(new Path2D(gl.body));
    }
    if (ly.bird && g.bird) {
      const b = g.bird;
      const bird = globeLandInk(ground);   // ★ beta91 the bird is neutral too, so it stands off the beam (Stuart 11-22)
      ctx.lineJoin = 'round';
      ctx.globalAlpha = 0.45; ctx.fillStyle = bird; ctx.fill(new Path2D(b.panels));
      ctx.globalAlpha = 1; ctx.strokeStyle = bird; ctx.lineWidth = 1.2 * sc; ctx.stroke(new Path2D(b.panels));
      ctx.lineWidth = 0.8 * sc; ctx.globalAlpha = 0.9; ctx.stroke(new Path2D(b.cells));
      ctx.globalAlpha = 1; ctx.lineWidth = 1.6 * sc; ctx.stroke(new Path2D(b.boom));
      ctx.fillStyle = bird; ctx.fill(new Path2D(b.body));
      ctx.lineWidth = 1 * sc; ctx.strokeStyle = ground; ctx.stroke(new Path2D(b.body));
      ctx.fillStyle = bird; ctx.fill(new Path2D(b.dish));
    }
    ctx.font = `${k.font.toFixed(1)}px system-ui,sans-serif`; ctx.textBaseline = 'alphabetic';
    for (const lb of [k.labels.rx, k.labels.sat]) {
      if (!lb) continue;
      ctx.textAlign = lb.anchor === 'start' ? 'left' : lb.anchor === 'end' ? 'right' : 'center';
      ctx.globalAlpha = 0.9; ctx.lineWidth = 3 * sc; ctx.strokeStyle = ground; ctx.strokeText(lb.text, lb.x, lb.y);
      ctx.globalAlpha = 1; ctx.fillStyle = ink; ctx.fillText(lb.text, lb.x, lb.y);
    }
  }
  ctx.restore();
}

