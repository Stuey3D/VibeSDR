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

import type { MapWfModel } from '../../../src/utils/mapWfModel';
export type { MapWfModel, MapWfMark } from '../../../src/utils/mapWfModel';
/** A frame as the page's waterfall processed it (Waterfall.onProcessed). */
export interface MapWfFrame { row: Uint8Array; spec: Float32Array; peak: Float32Array | null; centerHz: number; bwHz: number }
/** The page's look (Waterfall.mapLook): its LUT, the trace's fill, outline and peak colour. */
export interface MapWfLook { lut: Uint8Array; specAlpha: number; peakColour: string; peakLine: string | null; gradient: (ctx: CanvasRenderingContext2D, H: number) => CanvasGradient }

/** The trace's share: ~28 % of the box, never more than 90 px (a maximised drawer gave it 200 px, squashed — Stuart 16:17). */
const specHOf = (h: number) => Math.min(90, Math.round(h * 0.28));
const MIN_ROW_MS = 95;
/** The palette index the slice's noise floor lands on: the first clearly visible colour (the page's trace gradient starts
 *  its fill at 90 for the same reason — black-based palettes are invisible below it); lower, the floor reads as black. */
const LV_BASE = 70;
/** ★★ Stuart 11-27 (small → big kept only the small view's rows): the history is a RING OF PALETTE INDICES, independent of
 *  the drawer's size — COLS fixed columns (peak-binned; drawn to whatever width the box has) × the tallest the drawer could
 *  ever be on this device (the screen's longer side: maximised either way up, or the landscape side panel). 1 byte a cell:
 *  1024 × ~900 ≈ 1 MB, freed with the map. Every resize re-renders the newest rows that fit from it. */
const COLS = 1024;
const ringRowsFor = () => Math.max(400, Math.min(1600, Math.round(Math.max(screen.width || 0, screen.height || 0, innerHeight || 0))));
const GAP = 1, DATA = 2;   // a ring row's kind
const STAMP_FONT = '600 10px ui-monospace,Menlo,monospace';

export class MapWf {
  readonly el: HTMLDivElement;
  private cv: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private marksEl: HTMLDivElement;
  private ro: ResizeObserver | null = null;
  private row: ImageData | null = null;
  private lastKey = '';
  private marksSig = '';
  private w = 0; private h = 0; private dpr = 1;
  private crop: Float32Array = new Float32Array(0);
  private cropS: Float32Array = new Float32Array(0);
  private cropP: Float32Array = new Float32Array(0);
  /** recent stamps (x range, row counter) so a new one never lands on another */
  private stampsAt: Array<{ x0: number; x1: number; row: number }> = [];
  private rowN = 0;
  private ring: Uint8Array;
  private kind: Uint8Array;
  private readonly R: number;
  private head = -1;           // the newest row's slot
  private count = 0;
  private seq = 0;             // rows ever written (a stamp's row number)
  private lut: Uint8Array | null = null;
  private stamps: Array<{ seq: number; hz: number; text: string }> = [];
  private spanLo = 0; private spanHi = 1;
  private lastRowAt = 0;
  /** the slice's own levels (palette index space) and the trace's (normalised), followed smoothly; −1 = not yet */
  private lvFloor = -1; private lvTop = -1; private trLo = -1; private trHi = -1;
  /** for the drivers: rows drawn, history breaks, stamps written, the span shown */
  stats = { lvLo: 0, lvSpan: 0, skipped: 0, gaps: 0, inside: true, idxMin: 0, idxMax: 0, lut0: '', binsIn: 0, cHz: 0, bwHz: 0, rows: 0, breaks: 0, stamps: 0, lo: 0, hi: 0, marks: 0, labels: [] as string[], stampLog: [] as Array<{ text: string; hz: number; row: number }> };

  constructor() {
    this.R = ringRowsFor();
    this.ring = new Uint8Array(COLS * this.R);
    this.kind = new Uint8Array(this.R);
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
  destroy() { this.ro?.disconnect(); this.ro = null; this.el.remove(); this.ring = new Uint8Array(0); this.kind = new Uint8Array(0); this.count = 0; }
  /** for the drivers: the history's size */
  get history(): { rows: number; cap: number; cols: number } { return { rows: this.count, cap: this.R, cols: COLS }; }

  /** ★★ A RESIZE KEEPS THE HISTORY (small ↔ maximised, rotation, the side panel): the canvas is sized to the box and the
   *  newest rows that fit are drawn again from the ring. Only a retune clears it. */
  private size() {
    if (!this.el.isConnected || !this.el.clientWidth || !this.el.clientHeight) return;   // detached (the list shows): keep all
    // ★ Stuart 11-27 ("a tiny throwaway view, keep it LIGHT"): one column per CSS px, at DPR 1 — never Retina-doubled
    const dpr = 1;
    const w = Math.max(1, Math.round(this.el.clientWidth * dpr)), h = Math.max(1, Math.round(this.el.clientHeight * dpr));
    if (w === this.w && h === this.h) return;
    this.w = w; this.h = h; this.dpr = dpr;
    this.cv.width = w; this.cv.height = h;
    this.row = this.ctx.createImageData(w, 1);
    this.ctx.fillStyle = '#000'; this.ctx.fillRect(0, 0, w, h);
    this.renderAll();
    this.marksSig = '';   // positions follow the width
    this.stampsAt = [];
  }
  /** One ring row into an ImageData row of the box's width: each pixel the PEAK of the columns it covers. */
  private rowInto(slot: number, d: Uint8ClampedArray, off: number, w: number, lut: Uint8Array) {
    const k = this.kind[slot];
    if (k !== DATA) { const g = k === GAP ? [0x3a, 0x46, 0x54] : [0, 0, 0]; for (let x = 0; x < w; x++) { const q = off + (x << 2); d[q] = g[0]; d[q + 1] = g[1]; d[q + 2] = g[2]; d[q + 3] = 255; } return; }
    const base = slot * COLS;
    for (let x = 0; x < w; x++) {
      const c0 = Math.floor((x * COLS) / w), c1 = Math.max(c0 + 1, Math.floor(((x + 1) * COLS) / w));
      let v = 0; for (let c = c0; c < c1; c++) { const u = this.ring[base + c]; if (u > v) v = u; }
      const q = off + (x << 2), o = v << 2;
      if (v === 0) { d[q] = d[q + 1] = d[q + 2] = 12; } else { d[q] = lut[o]; d[q + 1] = lut[o + 1]; d[q + 2] = lut[o + 2]; }
      d[q + 3] = 255;
    }
  }
  /** The whole visible history from the ring (after a resize), and the idents stamped in it. */
  private renderAll() {
    if (!this.lut || !this.w || !this.h || !this.count) return;
    const top = specHOf(this.h), n = Math.min(this.count, this.h - top);
    if (n <= 0) return;
    const img = this.ctx.createImageData(this.w, n);
    for (let r = 0; r < n; r++) this.rowInto((this.head - r + this.R) % this.R, img.data, r * this.w * 4, this.w, this.lut);
    this.ctx.putImageData(img, 0, top);
    for (const st of this.stamps) { const y = this.seq - 1 - st.seq; if (y >= 0 && y < n) this.drawStamp(st.hz, st.text, top + y, false); }
  }
  /** A row into the ring (and its kind); returns its slot. */
  private ringPush(kind: number, idx: Float32Array | null): number {
    if (!this.ring.length) return -1;
    this.head = (this.head + 1) % this.R;
    this.count = Math.min(this.R, this.count + 1);
    this.seq++;
    this.kind[this.head] = kind;
    const base = this.head * COLS;
    if (idx) for (let c = 0; c < COLS; c++) { const v = idx[c]; this.ring[base + c] = v === v ? Math.max(1, Math.min(255, Math.round(v))) : 0; }
    return this.head;
  }
  /** The newest ring row drawn at the top of the history, the rest scrolled down one. */
  private drawNewest(slot: number) {
    if (!this.row || !this.lut || slot < 0) return;
    const top = specHOf(this.h), wfH = this.h - top;
    if (wfH > 1) this.ctx.drawImage(this.cv, 0, top, this.w, wfH - 1, 0, top + 1, this.w, wfH - 1);
    this.rowInto(slot, this.row.data, 0, this.w, this.lut);
    this.ctx.putImageData(this.row, 0, top);
  }
  /** Shown again after the list had the drawer: no rows came meanwhile — a thin grey line marks the gap in the history. */
  resumeGap() {
    if (!this.count) return;
    for (let i = 0; i < 2; i++) this.drawNewest(this.ringPush(GAP, null));
    this.stats.gaps++;
  }
  /** A strip of the history, y rows down from its top (for the drivers: the same rows before and after a resize). */
  strip(y: number): number[] {
    const top = specHOf(this.h);
    const d = this.ctx.getImageData(0, top + y, this.w, 1).data, out: number[] = [];
    for (let i = 0; i < 64; i++) { const x = Math.floor((i + 0.5) * this.w / 64) * 4; out.push(d[x] + d[x + 1] + d[x + 2]); }
    return out;
  }

  /** One frame AS THE PAGE PROCESSED IT (Waterfall.onProcessed): its row's palette indices — the page's own level mapping
   *  (brightness, auto range, clip) — and its smoothed trace and peak hold, cropped to the model's span and drawn in the
   *  page's look (its LUT, the trace's palette gradient, outline and peak colour). Nothing of our own: a miniature. */
  push(f: MapWfFrame, look: MapWfLook, m: MapWfModel) {
    if (!this.w) this.size();
    if (!this.w || !this.row || !f.row.length || !(f.bwHz > 0)) return;
    // ★ light: at most ~10 rows a second (the trace with them) — a faster spectrum is skipped, not queued
    const tNow = performance.now();
    if (tNow - this.lastRowAt < MIN_ROW_MS) { this.stats.skipped++; return; }
    this.lastRowAt = tNow;
    const { w, h, ctx } = this;
    const { centerHz, bwHz } = f;
    const specH = specHOf(h), wfTop = specH, wfH = h - specH;
    const lo = m.lo, hi = m.hi > m.lo ? m.hi : m.lo + 1;
    // ★ a retune (the frame's own window) or the model's (a scan's next window) breaks the history
    const key = `${Math.round(centerHz)}|${Math.round(bwHz)}|${Math.round(lo)}|${Math.round(hi)}|${m.key}`;
    if (key !== this.lastKey) {
      if (this.lastKey) this.stats.breaks++;
      this.lastKey = key;
      ctx.fillStyle = '#000'; ctx.fillRect(0, wfTop, w, wfH);
      this.stampsAt = []; this.stamps = [];
      this.count = 0; this.head = -1; this.kind.fill(0);   // ★ only a retune clears the history
      this.lvFloor = this.lvTop = this.trLo = this.trHi = -1;
    }
    if (this.crop.length !== COLS) this.crop = new Float32Array(COLS);
    if (this.cropS.length !== w) { this.cropS = new Float32Array(w); this.cropP = new Float32Array(w); }
    // ★ crop: each pixel takes the PEAK of the bins it covers (a narrow signal is never lost) — or the nearest bin when the
    //   slice is narrower than a bin per pixel; outside what the radio sends: NaN, drawn dark
    const f0 = centerHz - bwHz / 2;
    const cropInto = (src: ArrayLike<number>, out: Float32Array) => {
      const n = src.length, binHz = bwHz / n, W = out.length;
      for (let x = 0; x < W; x++) {
        const a = lo + (hi - lo) * (x / W), b = lo + (hi - lo) * ((x + 1) / W);
        let i0 = Math.floor((a - f0) / binHz), i1 = Math.floor((b - f0) / binHz);
        if (i1 < 0 || i0 >= n) { out[x] = NaN; continue; }
        i0 = Math.max(0, i0); i1 = Math.min(n - 1, Math.max(i0, i1));
        let v = src[i0]; for (let i = i0 + 1; i <= i1; i++) if (src[i] > v) v = src[i];
        out[x] = v;
      }
    };
    cropInto(f.row, this.crop);
    cropInto(f.spec, this.cropS);
    if (f.peak) cropInto(f.peak, this.cropP);
    /* ★★ Stuart 16:19 ("very dark"): the page's indices are levelled for the PAGE's whole view (its floor clips to 0), so a
     *  slice of it sat at the bottom of the palette. The mini view levels ITS OWN slice the way the page's auto levels do —
     *  the slice's noise floor to the palette's first visible colour, its strongest recent signal near the top — in the
     *  page's palette. Followed smoothly (up fast, down slowly) so a burst does not flash the whole view. */
    const out = this.crop;
    { const sm: number[] = []; let pk = 0;
      for (let x = 0; x < COLS; x += 8) { const v = out[x]; if (v === v) { sm.push(v); } }
      for (let x = 0; x < COLS; x++) { const v = out[x]; if (v === v && v > pk) pk = v; }
      if (sm.length) {
        sm.sort((a, b) => a - b);
        const fl = sm[Math.floor(sm.length * 0.3)];
        this.lvFloor = this.lvFloor < 0 ? fl : this.lvFloor + (fl - this.lvFloor) * 0.1;
        const want = Math.max(this.lvFloor + 24, pk);
        this.lvTop = this.lvTop < 0 ? want : want > this.lvTop ? this.lvTop + (want - this.lvTop) * 0.5 : this.lvTop + (want - this.lvTop) * 0.02;
      } }
    const lvLo = this.lvFloor < 0 ? 0 : this.lvFloor, lvSpan = Math.max(24, (this.lvTop < 0 ? 255 : this.lvTop) - lvLo);
    // the waterfall: the row levelled into palette indices, into the ring, drawn on top in the page's palette
    for (let c = 0; c < COLS; c++) { const v = out[c]; if (v === v) out[c] = LV_BASE + ((v - lvLo) / lvSpan) * (250 - LV_BASE); }
    this.lut = look.lut;
    const lut = look.lut;
    this.spanLo = lo; this.spanHi = hi;
    this.drawNewest(this.ringPush(DATA, out));
    this.rowN++; this.stats.rows++;
    // ★ the decoded idents stamped at this row — they scroll with the history (the page's scale-stamp idea)
    if (m.stamps?.length) for (const st of m.stamps) this.stamp(st.hz, st.text, lo, hi, wfTop);
    this.stampsAt = this.stampsAt.filter((r) => this.rowN - r.row < 40);
    // the trace — the page's: filled with the palette gradient, outlined in the palette's top colour, its peak hold
    ctx.fillStyle = '#000'; ctx.fillRect(0, 0, w, specH);
    const H = specH - 1, S = this.cropS;
    // ★ the trace scaled to the slice the same way: its floor a little above the bottom, its peak near the top
    let sLo = 1, sHi = 0; for (let x = 0; x < w; x++) { const v = S[x]; if (v === v) { if (v < sLo) sLo = v; if (v > sHi) sHi = v; } }
    if (sHi > sLo) { this.trLo = this.trLo < 0 ? sLo : this.trLo + (sLo - this.trLo) * 0.1; this.trHi = this.trHi < 0 ? sHi : sHi > this.trHi ? this.trHi + (sHi - this.trHi) * 0.5 : this.trHi + (sHi - this.trHi) * 0.03; }
    const tLo = this.trLo < 0 ? 0 : this.trLo, tSp = Math.max(0.08, (this.trHi < 0 ? 1 : this.trHi) - tLo);
    const ty = (v: number) => Math.max(0, Math.min(1, 0.08 + ((v - tLo) / tSp) * 0.84));
    ctx.beginPath(); ctx.moveTo(0, H);
    for (let x = 0; x < w; x++) { const v = S[x] === S[x] ? ty(S[x]) : 0; ctx.lineTo(x, H - v * H); }
    ctx.lineTo(w, H); ctx.closePath();
    ctx.globalAlpha = look.specAlpha; ctx.fillStyle = look.gradient(ctx, H); ctx.fill(); ctx.globalAlpha = 1;
    ctx.strokeStyle = look.peakColour; ctx.lineWidth = Math.max(1, this.dpr); ctx.stroke();
    if (f.peak && look.peakLine) {
      const P = this.cropP; ctx.beginPath();
      for (let x = 0; x < w; x++) { const v = P[x] === P[x] ? ty(P[x]) : 0; if (x) ctx.lineTo(x, H - v * H); else ctx.moveTo(x, H - v * H); }
      ctx.strokeStyle = look.peakLine; ctx.lineWidth = 1; ctx.stroke();
    }
    ctx.fillStyle = '#1b2733'; ctx.fillRect(0, specH - 1, w, 1);
    this.marks(m, lo, hi);
    this.stats.lo = lo; this.stats.hi = hi; this.stats.lut0 = `${lut[1020]},${lut[1021]},${lut[1022]}`;
    let mn = 255, mx = 0; for (let x = 0; x < COLS; x++) { const v = out[x]; if (v === v) { if (v < mn) mn = v; if (v > mx) mx = v; } }
    this.stats.idxMin = mn; this.stats.idxMax = mx; this.stats.lvLo = Math.round(lvLo); this.stats.lvSpan = Math.round(lvSpan); this.stats.cHz = centerHz; this.stats.bwHz = bwHz; this.stats.binsIn = f.row.length;
  }

  private stamp(hz: number, text: string, lo: number, hi: number, wfTop: number) {
    if (hz < lo || hz > hi || !text) return;
    if (this.drawStamp(hz, text, wfTop, true)) {
      this.stamps.push({ seq: this.seq - 1, hz, text });
      this.stamps = this.stamps.filter((st) => this.seq - st.seq <= this.R);
      this.stats.stamps++;
      this.stats.stampLog.push({ text, hz, row: this.stats.rows });
      if (this.stats.stampLog.length > 40) this.stats.stampLog.shift();
    }
  }
  /** An ident written into the history at row y (its frequency's column); `check` = not over another recent one. */
  private drawStamp(hz: number, text: string, y: number, check: boolean): boolean {
    const ctx = this.ctx, dpr = this.dpr, lo = this.spanLo, hi = this.spanHi;
    ctx.font = STAMP_FONT.replace('10px', `${Math.round(10 * dpr)}px`);
    const tw = ctx.measureText(text).width + 6 * dpr;
    const cx = ((hz - lo) / (hi - lo)) * this.w;
    const x0 = Math.max(0, Math.min(this.w - tw, cx - tw / 2)), x1 = x0 + tw;
    // ★ never over another stamp: skip when one sits within the last rows at an overlapping x
    if (check) {
      const hRows = Math.ceil(14 * dpr);
      if (this.stampsAt.some((r) => this.rowN - r.row < hRows && r.x0 < x1 && r.x1 > x0)) return false;
      this.stampsAt.push({ x0, x1, row: this.rowN });
    }
    ctx.fillStyle = 'rgba(0,0,0,.6)'; ctx.fillRect(x0, y + 1, tw, 13 * dpr);
    ctx.fillStyle = '#ffd34d'; ctx.textBaseline = 'top'; ctx.fillText(text, x0 + 3 * dpr, y + 2 * dpr);
    return true;
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
      const x = (pct(k.hz) / 100) * cw;
      /* ★ Stuart 16:4x (Ch12 had no label at a 316 px panel): every labelled channel KEEPS a label — the full one where it
       *  fits in one of the three rows, else its short form ("12", "DTY"), else the short form in the roomiest row */
      let text = k.label, lw = text.length * 6.2 + 8;
      let r = text ? rows.findIndex((end) => x - lw / 2 > end + 3) : -1;   // ★ an unlabelled channel is a faint line only
      if (text && r < 0) {
        text = shortLabel(k.label); lw = text.length * 6.2 + 8;
        r = rows.findIndex((end) => x - lw / 2 > end + 3);
        if (r < 0) r = rows.indexOf(Math.min(...rows));
      }
      const shown = !!text && r >= 0;
      if (shown) rows[r] = Math.max(rows[r], x + lw / 2); else r = 0;
      const band = k.loHz != null && k.hiHz != null && k.hiHz > k.loHz
        ? `<span class="mwBand" style="left:${pct(k.loHz).toFixed(2)}%;width:${(pct(k.hiHz) - pct(k.loHz)).toFixed(2)}%"></span>` : '';
      html.push(`${band}<span class="mwLine${k.active ? ' on' : ''}" style="left:${pct(k.hz).toFixed(2)}%"></span>`
        + (shown ? `<span class="mwLbl${k.active ? ' on' : ''}" style="left:${pct(k.hz).toFixed(2)}%;top:${2 + r * 13}px" title="${esc(k.label)}">${esc(text)}</span>` : ''));
      if (shown) labels.push(text);
    }
    this.marksEl.style.setProperty('--mw-spec', `${specHOf(this.h) / this.dpr}px`);   // ★ the ticks span the trace only, never the history
    this.marksEl.innerHTML = html.join('');
    /* ★ Stuart 16:11: an edge label is shifted INWARD to stay whole (the page's signal labels do the same); its line stays
     *  at the true frequency. Measured once per rebuild of the markers — they change only with the span or the channels. */
    let inside = true;
    for (const el of Array.from(this.marksEl.querySelectorAll('.mwLbl')) as HTMLElement[]) {
      const lw = el.offsetWidth, x = (parseFloat(el.style.left) / 100) * cw;
      const left = Math.max(2, Math.min(cw - lw - 2, x - lw / 2));
      el.style.left = `${left.toFixed(1)}px`; el.style.transform = 'none';
      if (left < 0 || left + lw > cw) inside = false;
    }
    this.stats.inside = inside;
    this.stats.marks = inView.length; this.stats.labels = labels;
  }
}

/** A label's short form when crowded: "Ch12 136.975" → "12", "DTY 116.40" → "DTY". */
function shortLabel(l: string): string { const t = l.split(' ')[0] || l; return /^Ch\d+$/.test(t) ? t.slice(2) : t; }

function esc(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string));
}
