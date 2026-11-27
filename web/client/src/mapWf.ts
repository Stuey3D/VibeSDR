/**
 * mapWf.ts — ★★★ V12 THE MAP DRAWER'S LIVE WATERFALL (Stuart, 2026-11-27). The bottom-left drawer of a live map over a
 * radio shows EITHER the list OR this (the folder tab at its top-left swaps them). It is TOTALLY PASSIVE: its own small
 * canvas with no pointer handlers — no tap-to-tune, no drag, no zoom. It only shows what the radio is hearing.
 *
 *  ★ THE SPAN FITS THE MODE, not the page's view (main.ts mapWfModel decides it): a narrow channel is a slice a few times
 *    its filter wide, centred on the dial; a multichannel decoder (VDL2, ACARS) spans all its channels with one labelled
 *    marker per channel; a scan (the VOR / ILS map) is the radio's whole window for that dwell, its stations marked and
 *    their idents STAMPED into the history at the row they were decoded on (they scroll down with it). Stuart reversed a
 *    band-wide composite: a 2 MHz step would only leave narrow smears — each new window starts FRESH.
 *  ★★ CROPPED CLIENT-SIDE from the FFT this listener already receives. The per-listener zoom request was NOT used: on a
 *    shared dial a view change can move the shared IF filter for everybody (memory: shared_dial_joiner_imposes_view), and
 *    a crop disturbs nobody. The price: a narrow slice of a wide view has coarse bins — drawn honestly, bins stretched.
 *  ★ A RETUNE BREAKS THE HISTORY: when the received frame's centre / span or the model's own key changes (a scan stepping
 *    to its next window), the old rows are cleared — they would be drawn at the wrong frequencies otherwise.
 *  ★ Cost: nothing between frames (no rAF); one crop, one row blit and a short trace per received frame, only while the
 *    drawer shows it (the spectrum is off at the server otherwise — main.ts mapWfShow).
 */
import { getColorLUT } from '../../../src/assets/colormapUtils';

import type { MapWfModel } from '../../../src/utils/mapWfModel';
export type { MapWfModel, MapWfMark } from '../../../src/utils/mapWfModel';

const SPEC_FRAC = 0.3;      // the trace's share of the height
const RANGE_DB = 48;        // the colour range above the floor
const STAMP_FONT = '600 10px ui-monospace,Menlo,monospace';

export class MapWf {
  readonly el: HTMLDivElement;
  private cv: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private marksEl: HTMLDivElement;
  private ro: ResizeObserver | null = null;
  private lut: Uint8Array;
  private row: ImageData | null = null;
  private floor = NaN;
  private lastKey = '';
  private marksSig = '';
  private w = 0; private h = 0; private dpr = 1;
  private crop: Float32Array = new Float32Array(0);
  /** recent stamps (x range, row counter) so a new one never lands on another */
  private stampsAt: Array<{ x0: number; x1: number; row: number }> = [];
  private rowN = 0;
  /** for the drivers: rows drawn, history breaks, stamps written, the span shown */
  stats = { rows: 0, breaks: 0, stamps: 0, lo: 0, hi: 0, marks: 0, labels: [] as string[], stampLog: [] as Array<{ text: string; hz: number; row: number }> };

  constructor(palette: string) {
    this.lut = getColorLUT(palette || 'gqrx');
    const el = document.createElement('div');
    el.className = 'mapWf';
    el.setAttribute('aria-hidden', 'true');   // ★ a picture: the bar under it reads the dial in words
    el.innerHTML = '<canvas></canvas><div class="mwMarks"></div>';
    this.el = el;
    this.cv = el.querySelector('canvas') as HTMLCanvasElement;
    this.ctx = this.cv.getContext('2d', { alpha: false }) as CanvasRenderingContext2D;
    this.marksEl = el.querySelector('.mwMarks') as HTMLDivElement;
    if (typeof ResizeObserver !== 'undefined') { this.ro = new ResizeObserver(() => this.size()); this.ro.observe(el); }
  }
  setPalette(p: string) { this.lut = getColorLUT(p || 'gqrx'); }
  destroy() { this.ro?.disconnect(); this.ro = null; this.el.remove(); }

  private size() {
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    const w = Math.max(1, Math.round(this.el.clientWidth * dpr)), h = Math.max(1, Math.round(this.el.clientHeight * dpr));
    if (w === this.w && h === this.h) return;
    this.w = w; this.h = h; this.dpr = dpr;
    this.cv.width = w; this.cv.height = h;
    this.row = this.ctx.createImageData(w, 1);
    this.ctx.fillStyle = '#000'; this.ctx.fillRect(0, 0, w, h);
    this.marksSig = '';   // positions follow the width
    this.stampsAt = [];
  }

  /** One received frame: crop it to the model's span, add a row, redraw the trace, place the markers. */
  push(bins: Float32Array, centerHz: number, bwHz: number, m: MapWfModel) {
    if (!this.w) this.size();
    if (!this.w || !this.row || !bins.length || !(bwHz > 0)) return;
    const { w, h, ctx } = this;
    const specH = Math.round(h * SPEC_FRAC), wfTop = specH, wfH = h - specH;
    const lo = m.lo, hi = m.hi > m.lo ? m.hi : m.lo + 1;
    // ★ a retune (the frame's own window) or the model's (a scan's next window) breaks the history
    const key = `${Math.round(centerHz)}|${Math.round(bwHz)}|${Math.round(lo)}|${Math.round(hi)}|${m.key}`;
    if (key !== this.lastKey) {
      if (this.lastKey) this.stats.breaks++;
      this.lastKey = key;
      ctx.fillStyle = '#000'; ctx.fillRect(0, wfTop, w, wfH);
      this.stampsAt = [];
      this.floor = NaN;
    }
    // ★ crop: each pixel takes the PEAK of the bins it covers (a narrow signal is never lost), or the nearest bin when
    //   the slice is narrower than a bin per pixel
    if (this.crop.length !== w) this.crop = new Float32Array(w);
    const n = bins.length, f0 = centerHz - bwHz / 2, binHz = bwHz / n;
    const out = this.crop;
    for (let x = 0; x < w; x++) {
      const a = lo + (hi - lo) * (x / w), b = lo + (hi - lo) * ((x + 1) / w);
      let i0 = Math.floor((a - f0) / binHz), i1 = Math.floor((b - f0) / binHz);
      if (i1 < 0 || i0 >= n) { out[x] = NaN; continue; }   // outside what the radio sends: drawn dark
      i0 = Math.max(0, i0); i1 = Math.min(n - 1, Math.max(i0, i1));
      let v = bins[i0]; for (let i = i0 + 1; i <= i1; i++) if (bins[i] > v) v = bins[i];
      out[x] = v;
    }
    // ★ the floor: a low percentile of this row, followed slowly
    const s: number[] = []; for (let x = 0; x < w; x += Math.max(1, (w / 64) | 0)) if (out[x] === out[x]) s.push(out[x]);
    if (s.length) {
      s.sort((p, q) => p - q);
      const f = s[Math.floor(s.length * 0.2)];
      this.floor = this.floor === this.floor ? this.floor + (f - this.floor) * 0.08 : f;
    }
    const fl = this.floor === this.floor ? this.floor - 4 : -120;
    // the waterfall: scroll down one row, the new one on top
    if (wfH > 1) ctx.drawImage(this.cv, 0, wfTop, w, wfH - 1, 0, wfTop + 1, w, wfH - 1);
    const d = this.row.data, lut = this.lut;
    for (let x = 0; x < w; x++) {
      const v = out[x], q = x << 2;
      if (v !== v) { d[q] = d[q + 1] = d[q + 2] = 12; d[q + 3] = 255; continue; }
      const idx = Math.max(0, Math.min(255, Math.round(((v - fl) / RANGE_DB) * 255))) << 2;
      d[q] = lut[idx]; d[q + 1] = lut[idx + 1]; d[q + 2] = lut[idx + 2]; d[q + 3] = 255;
    }
    ctx.putImageData(this.row, 0, wfTop);
    this.rowN++; this.stats.rows++;
    // ★ the decoded idents stamped at this row — they scroll with the history (the page's scale-stamp idea)
    if (m.stamps?.length) for (const st of m.stamps) this.stamp(st.hz, st.text, lo, hi, wfTop);
    this.stampsAt = this.stampsAt.filter((r) => this.rowN - r.row < 40);
    // the trace
    ctx.fillStyle = '#05080b'; ctx.fillRect(0, 0, w, specH);
    ctx.beginPath();
    let started = false;
    for (let x = 0; x < w; x++) {
      const v = out[x]; if (v !== v) { started = false; continue; }
      const y = specH - 1 - Math.max(0, Math.min(1, (v - fl) / (RANGE_DB + 12))) * (specH - 3);
      if (!started) { ctx.moveTo(x, y); started = true; } else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = '#9fd3ff'; ctx.lineWidth = Math.max(1, this.dpr); ctx.stroke();
    ctx.fillStyle = '#1b2733'; ctx.fillRect(0, specH - 1, w, 1);
    this.marks(m, lo, hi);
    this.stats.lo = lo; this.stats.hi = hi;
  }

  private stamp(hz: number, text: string, lo: number, hi: number, wfTop: number) {
    if (hz < lo || hz > hi || !text) return;
    const ctx = this.ctx, dpr = this.dpr;
    ctx.font = STAMP_FONT.replace('10px', `${Math.round(10 * dpr)}px`);
    const tw = ctx.measureText(text).width + 6 * dpr;
    const cx = ((hz - lo) / (hi - lo)) * this.w;
    const x0 = Math.max(0, Math.min(this.w - tw, cx - tw / 2)), x1 = x0 + tw;
    // ★ never over another stamp: skip when one sits within the last rows at an overlapping x
    const hRows = Math.ceil(14 * dpr);
    if (this.stampsAt.some((r) => this.rowN - r.row < hRows && r.x0 < x1 && r.x1 > x0)) return;
    this.stampsAt.push({ x0, x1, row: this.rowN });
    ctx.fillStyle = 'rgba(0,0,0,.6)'; ctx.fillRect(x0, wfTop + 1, tw, 13 * dpr);
    ctx.fillStyle = '#ffd34d'; ctx.textBaseline = 'top'; ctx.fillText(text, x0 + 3 * dpr, wfTop + 2 * dpr);
    this.stats.stamps++;
    this.stats.stampLog.push({ text, hz, row: this.stats.rows });
    if (this.stats.stampLog.length > 40) this.stats.stampLog.shift();
  }

  /** The markers: DOM over the canvas (crisp text, the house label style), rebuilt only when they change. */
  private marks(m: MapWfModel, lo: number, hi: number) {
    const sig = `${Math.round(lo)}|${Math.round(hi)}|${this.w}|` + m.marks.map((k) => `${k.hz}:${k.label}:${k.active ? 1 : 0}:${k.loHz ?? ''}:${k.hiHz ?? ''}`).join(',');
    if (sig === this.marksSig) return;
    this.marksSig = sig;
    const cw = this.w / this.dpr, pct = (hz: number) => ((hz - lo) / (hi - lo)) * 100;
    const inView = m.marks.filter((k) => k.hz >= lo && k.hz <= hi);
    // ★ labels stack: sorted by x, each takes the first row (of 3) where it clears the previous label in that row
    const rows: number[] = [-1e9, -1e9, -1e9];
    const html: string[] = [];
    const labels: string[] = [];
    for (const k of inView.slice().sort((a, b) => a.hz - b.hz)) {
      const x = (pct(k.hz) / 100) * cw, lw = k.label.length * 6.2 + 8;
      let r = k.label ? rows.findIndex((end) => x - lw / 2 > end + 3) : -1;   // ★ an unlabelled channel is a faint line only
      const shown = r >= 0;
      if (shown) rows[r] = x + lw / 2; else r = 0;
      const band = k.loHz != null && k.hiHz != null && k.hiHz > k.loHz
        ? `<span class="mwBand" style="left:${pct(k.loHz).toFixed(2)}%;width:${(pct(k.hiHz) - pct(k.loHz)).toFixed(2)}%"></span>` : '';
      html.push(`${band}<span class="mwLine${k.active ? ' on' : ''}" style="left:${pct(k.hz).toFixed(2)}%"></span>`
        + (shown ? `<span class="mwLbl${k.active ? ' on' : ''}" style="left:${pct(k.hz).toFixed(2)}%;top:${2 + r * 13}px">${esc(k.label)}</span>` : ''));
      if (shown) labels.push(k.label);
    }
    this.marksEl.innerHTML = html.join('');
    this.stats.marks = inView.length; this.stats.labels = labels;
  }
}

function esc(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
}
