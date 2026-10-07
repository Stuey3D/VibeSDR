/**
 * DecoderImageCanvas — Skia port of the skin's decoder image canvas
 * (initLsvDecoder _initCanvas/_wefaxLine/_sstvLine/_imageDone/_newImageStarting,
 * Scalable_Mobile_UI v6.3.1, behaviour-exact):
 *
 *   - WEFAX lazy-inits at width×500 and GROWS height by +100 rows whenever a
 *     line lands past the bottom; pixels are greyscale (v,v,v,255).
 *   - SSTV pre-sizes from imageStart(w,h); lines are RGB triplets → RGBA.
 *   - line 0 arriving after a completed image (or a new imageStart) rolls the
 *     finished image into the PREV buffer — toggle LIVE/PREV like the skin.
 *   - done() marks complete: "done — tap SAVE".
 *   - save() encodes the visible image to PNG and opens the share sheet
 *     (skin used navigator.share with the same mode_timestamp.png naming).
 *
 * Rendering: pixel buffer → SkImage, rebuilt at most once per REBUILD_MS (with a
 * trailing rebuild so the last lines always land) so a 1809-wide WEFAX at 120 LPM
 * doesn't thrash the GPU upload; each replaced image is disposed after a grace.
 * Displayed scaled to panel width, aspect preserved, scrolls as it grows.
 */

import React, {
  forwardRef, useCallback, useEffect, useImperativeHandle, useMemo, useRef, useState,
} from 'react';
import { PanResponder, ScrollView, Share, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
// ★★ A REAL FILE, NOT A data: URL — see save() below for why.
import { File, Paths } from 'expo-file-system';
import { WEFAX_ALIGN_ZERO, chartAlignStep, drawnAlign, wefaxOffset, type ChartAlignState, type WefaxAlign, type WefaxFormat } from '../utils/wefaxAlign';
import { addToHist, crispLevels, crispLine, fillLostLines, newHist } from '../utils/wefaxCrisp';
import {
  Canvas, Image as SkiaImage, Skia,
  AlphaType, ColorType, ImageFormat, type SkData, type SkImage,
} from '@shopify/react-native-skia';

/* ★★ AT MOST ONE REBUILD PER REBUILD_MS, AND ONLY WHEN A LINE HAS LANDED (2026-10-03). The gate was
 *   `lines < 8 && ms < 150` → skip, i.e. rebuild on 8 lines OR 150 ms — so at WEFAX's 2 lines/s
 *   (500 ms apart) EVERY line rebuilt, each a full copy of the written picture (up to 1809 x 1200 x 4
 *   = 8.7 MB) into a new SkData + SkImage, and nothing disposed the old pair. A fast SSTV mode (or
 *   a burst of queued lines after a stall) rebuilt per line too. Now: time-gated, with a TRAILING
 *   rebuild so the final lines of a burst always appear even if no further line arrives.
 *   ★ Trade-off: the picture can lag the newest line by up to REBUILD_MS. At WEFAX's 2 lines/s
 *   that is unchanged (one line per rebuild either way — the saving there is the disposal);
 *   on faster image modes and bursts it caps copies at ~2.5/s. */
const REBUILD_MS    = 400;
/** Grace before a replaced image is freed — WaterfallView swapWfImage's rule: the UI thread may
 *  still be drawing it. */
const RETIRE_MS     = 300;
const WEFAX_INIT_H  = 500;  // skin: _initCanvas(w, 500)
const GROW_ROWS     = 100;  // skin: _canvas.height = ln + 100

interface PixBuf {
  w: number;
  h: number;
  data: Uint8Array;   // RGBA
  complete: boolean;
  maxLine: number;    // highest line written (display crop)
  /** ★ WEFAX: each line AS RECEIVED (greyscale, w×h), so a SHIFT/SLANT change can redraw the whole chart. */
  raw?: Uint8Array;
  /** ★ WEFAX: THIS chart's own alignment (utils/wefaxAlign chartAlignStep — margin, else blank border). */
  auto?: ChartAlignState;
  /** ★ WEFAX: each line AFTER shift/slant (greyscale) — what the crisp rendering smooths (utils/wefaxCrisp). */
  al?: Uint8Array;
  /** ★ WEFAX: histogram of every raw pixel received — the chart's own paper and ink levels. */
  hist?: Uint32Array;
  /** ★ WEFAX: 1 per line that ARRIVED — a lost line is drawn as the one above (wefaxCrisp fillLostLines), never
   *  counted as received (2026-10-05). */
  got?: Uint8Array;
}

// Persistent per-decoder image store. The live/prev buffers live OUTSIDE the
// component so they survive a remount — rotating the device, minimising the
// decoder tab, or any re-layout used to drop the buffer and restart the image
// from the current scanline. Keyed by decoder name; in-place pixel writes
// persist automatically (the store holds the SAME PixBuf reference). This also
// gives every image decoder the 1-image PREV buffer for free.
/** ★ `lineBase` / `rebase` (2026-10-07, CLR): the decoder numbers WEFAX lines from its last start tone, and CLR starts a
 *  chart without one — so the canvas counts lines from the first one after CLR (see clear()). */
interface ImgStore { live: PixBuf | null; prev: PixBuf | null; lineBase: number; rebase: boolean }
const imgStores: Record<string, ImgStore> = {};
function getImgStore(name: string): ImgStore {
  return (imgStores[name] ??= { live: null, prev: null, lineBase: 0, rebase: false });
}

export interface DecoderImageHandle {
  imageStart: (w: number, h: number) => void;
  wefaxLine:  (ln: number, w: number, px: Uint8Array) => void;
  sstvLine:   (ln: number, w: number, px: Uint8Array) => void;
  imageDone:  () => void;
  /** ★ CLR (2026-10-07): the picture so far goes to PREV, and the next line starts a fresh one — its own auto-align. */
  clear:      (why?: string) => void;
  reset:      () => void;
  showPrev:   () => void;
  showLive:   () => void;
  save:       () => Promise<void>;
}

export interface DecoderImageCanvasProps {
  /** ★ The listener's zoom over FIT (1 = the whole width in the box). The header's − / + (DecoderPanel). */
  zoom?: number;
  maxHeight: number;
  /** Header info string updates: "1809x500", "prev — 1809x842", … */
  onInfo:    (info: string) => void;
  onStatus:  (status: string) => void;
  /** PREV button availability + current view, for the panel header. */
  onPrevState: (hasPrev: boolean, viewingPrev: boolean) => void;
  decoderName: string;   // for the save filename: wefax_2026-06-10T18-31-02.png
  /** ★ WEFAX SHIFT / SLANT for this frequency (utils/wefaxAlign) — applied as each line is drawn. */
  align?: WefaxAlign;
  /** ★ Find each chart's margin (or DDK-style blank border) and move it to the left edge (utils/wefaxAlign
   *  chartAlignStep) — off while the listener is nudging the shift by hand. */
  autoMargin?: boolean;
  /** ★ Use the slant MEASURED on each chart (off when the listener has set the slant by hand on THIS chart). */
  autoSlant?: boolean;
  /** ★★ The STATION's slant (utils/wefaxAlign wefaxPreset) — the centre of each chart's slant search. Never the
   *  saved or drawn slant (2026-10-07: a stale one centred the search where the chart's real slant was out of reach). */
  stationSlant?: number;
  /** ★★ The chart formats auto-align may act on for this station (utils/wefaxAlign wefaxFormat, 2026-10-07): a white
   *  border only on DDK/SVJ4, Northwood's margin only on Northwood; a black strip and DDK's header bar anywhere. */
  stationFormat?: WefaxFormat;
  /** Reports the per-chart automatic alignment (null = nothing found) and the slant measured on the chart
   *  (undefined = nothing on it could measure one) for the ADJ strip. */
  onAutoAlign?: (a: WefaxAlign | null, measuredSlant?: number) => void;
  /** ★ ALIGN open (DecoderPanel): the SHIFT the listener is choosing. Drawn at once by sliding the picture already
   *  on screen (wrapping) — the chart itself is re-aligned only when the panel commits it. undefined = closed. */
  alignPreview?: number;
  /** ★ ALIGN open: a sideways drag on the picture has ended — the SHIFT to commit (previewed here while dragging). */
  onAlignDrag?: (shift: number, done: boolean) => void;
  /** ★ RAW: draw every line exactly as received — no shift, no slant (utils/wefaxAlign drawnAlign). The automatic
   *  alignment still runs underneath, so turning RAW off restores it. */
  raw?: boolean;
  /** ★ A new chart has started on this canvas (the line count went back) — RAW and a manual shift are per chart. */
  onNewChart?: () => void;
}

/** Row `y` of a WEFAX buffer from its kept raw line, moved per `a` (left by shift + slant·y, wrapping) — into `al`. */
function alignRow(buf: PixBuf, y: number, a: WefaxAlign) {
  if (!buf.raw) return;
  if (!buf.al) buf.al = new Uint8Array(buf.w * buf.h);
  const w = buf.w, off = wefaxOffset(a, y, w), base = y * w;
  for (let x = 0; x < w; x++) buf.al[base + x] = buf.raw[base + ((x + off) % w)];
}

/** ★ Draw the lost lines in y0..y1 as the line above (utils/wefaxCrisp fillLostLines) — `al` only, never `raw`. */
function fillLost(buf: PixBuf, y0: number, y1: number) {
  const got = buf.got, al = buf.al;
  if (!got || !al) return;
  const w = buf.w;
  fillLostLines((j) => got[j] === 1, (y) => al.copyWithin(y * w, (y - 1) * w, y * w), y0, Math.min(y1, buf.h - 1));
}

/** Paint rows y0..y1 of a WEFAX buffer from `al`, crisp (utils/wefaxCrisp) once the chart's levels are known; rows
 *  past maxLine have not arrived and count as missing to the smoothing. */
const crispTmp = { out: new Uint8Array(0) };
function paintRows(buf: PixBuf, y0: number, y1: number) {
  if (!buf.al) return;
  const w = buf.w, last = buf.maxLine, al = buf.al;
  const lv = buf.hist ? crispLevels(buf.hist) : null;
  if (crispTmp.out.length < w) crispTmp.out = new Uint8Array(w);
  const out = crispTmp.out;
  const row = (j: number) => (j >= 0 && j <= last && j < buf.h ? al.subarray(j * w, (j + 1) * w) : undefined);
  for (let y = Math.max(0, y0); y <= y1 && y < buf.h; y++) {
    if (lv) crispLine(row, y, w, lv, out);
    else out.set(al.subarray(y * w, (y + 1) * w));
    const o0 = y * w * 4;
    for (let x = 0; x < w; x++) {
      const v = out[x], o = o0 + x * 4;
      buf.data[o] = v; buf.data[o + 1] = v; buf.data[o + 2] = v; buf.data[o + 3] = 255;
    }
  }
}

/** Re-align and repaint the whole chart (a SHIFT/SLANT change, a margin found, the chart finished — final levels). */
function redrawAll(buf: PixBuf, a: WefaxAlign) {
  if (!buf.raw) return;
  for (let y = 0; y <= buf.maxLine && y < buf.h; y++) alignRow(buf, y, a);
  fillLost(buf, 0, buf.maxLine);
  paintRows(buf, 0, buf.maxLine);
}

function mkBuf(w: number, h: number): PixBuf {
  const data = new Uint8Array(w * h * 4); // zero-filled = black, alpha set on write
  for (let i = 3; i < data.length; i += 4) data[i] = 255;
  return { w, h, data, complete: false, maxLine: 0 };
}

const DecoderImageCanvas = forwardRef<DecoderImageHandle, DecoderImageCanvasProps>(
  function DecoderImageCanvas({ maxHeight, onInfo, onStatus, onPrevState, decoderName, zoom = 1, align,
                               autoMargin = false, autoSlant = true, stationSlant = 0, stationFormat, onAutoAlign,
                               alignPreview, onAlignDrag, raw = false, onNewChart }, ref) {
    const rawRef = useRef(raw);
    rawRef.current = raw;
    const alignRef = useRef<WefaxAlign>(align ?? WEFAX_ALIGN_ZERO);
    alignRef.current = align ?? WEFAX_ALIGN_ZERO;
    const autoRef = useRef(autoMargin);
    autoRef.current = autoMargin;
    const autoSlantRef = useRef(autoSlant);
    autoSlantRef.current = autoSlant;
    const stationSlantRef = useRef(stationSlant);
    stationSlantRef.current = stationSlant;
    const stationFormatRef = useRef(stationFormat);
    stationFormatRef.current = stationFormat;
    /** The SHIFT / SLANT a buffer is drawn with: this chart's own shift when found (else the listener's), and the
     *  slant MEASURED on this chart when there is one (else the listener's saved one, else the station's).
     *  ★★ Measured beats saved (2026-10-07): a saved slant is one receiver's clock on one day — Stuart's DDK on the
     *  Pi 500's HF+ leaned +0.11 px/line where the RX888 leans +0.01. Setting the slant by hand on THIS chart turns
     *  autoSlant off, and then the listener's wins. */
    const effAlign = (buf: PixBuf): WefaxAlign => {
      const st = buf.auto, a = st?.al;
      const slant = autoSlantRef.current && st?.slant !== undefined ? st.slant : alignRef.current.slant;
      const shift = autoRef.current && a ? a.shift : alignRef.current.shift;
      return drawnAlign({ shift, slant }, rawRef.current);
    };
    const { width: winW } = useWindowDimensions();
    /* ★★★ THE WIDTH THIS CANVAS ACTUALLY HAS — MEASURED, not the window's. It was `winW - 16 - 24`, true
     *  on a phone (the box is full-bleed there) and false everywhere else since the box was capped at
     *  PANEL_MAX_W (760, DecoderPanel, 2026-07-31): on a full-screen Mac the WEFAX scale came out ~1.08
     *  for a ~645 pt box, so the chart was drawn ~1950 wide and CLIPPED — the left third, magnified, with
     *  no way to pan (Stuart, 2026-10-04: "seems too zoomed in and I also cannot pan it"). The window
     *  figure is only the first-frame guess until onLayout reports the real one. */
    const [boxW, setBoxW] = useState(0);
    const panelW = boxW > 0 ? boxW : winW - 16 - 24;

    const store = getImgStore(decoderName || 'img');
    const live = useRef<PixBuf | null>(store.live);
    const prev = useRef<PixBuf | null>(store.prev);
    const [viewingPrev, setViewingPrev] = useState(false);
    const [img, setImg] = useState<SkImage | null>(null);
    const [dispDims, setDispDims] = useState({ w: 1, h: 1 });
    const [imgShift, setImgShift] = useState<number | null>(null);

    const linesSince = useRef(0);
    const lastBuild  = useRef(0);
    const trailTimer = useRef<ReturnType<typeof setTimeout> | null>(null);

    // ── Deterministic disposal (WaterfallView swapWfImage pattern) ───────────
    // Hermes sees only the tiny JS wrapper, never the multi-MB native pixels behind each image, so
    // replaced images waited for a GC that native memory never triggers. Retire the old pair after
    // a TIME grace (not a count): the render thread may still be drawing it.
    const imgLive    = useRef<{ img: SkImage; data: SkData } | null>(null);
    const imgPending = useRef<Set<{ img: SkImage; data: SkData }>>(new Set());
    const swapImg = useCallback((next: { img: SkImage; data: SkData } | null) => {
      const old = imgLive.current;
      imgLive.current = next;
      setImg(next ? next.img : null);
      if (old) {
        imgPending.current.add(old);
        setTimeout(() => {
          if (imgPending.current.delete(old)) { try { old.img.dispose(); old.data.dispose(); } catch {} }
        }, RETIRE_MS);
      }
    }, []);
    useEffect(() => () => {   // unmount: the store keeps the PIXELS; the Skia copies can all go
      if (trailTimer.current) { clearTimeout(trailTimer.current); trailTimer.current = null; }
      imgPending.current.forEach(r => { try { r.img.dispose(); r.data.dispose(); } catch {} });
      imgPending.current.clear();
      const cur = imgLive.current;
      imgLive.current = null;
      if (cur) setTimeout(() => { try { cur.img.dispose(); cur.data.dispose(); } catch {} }, RETIRE_MS);
    }, []);

    // ── SkImage rebuild ──────────────────────────────────────────────────────
    const rebuild = useCallback((buf: PixBuf | null, force = false) => {
      // A forced rebuild (new image, PREV/LIVE switch, done, reset) supersedes any trailing one —
      // which would otherwise repaint the LIVE buffer over a PREV the user just asked for.
      if (force && trailTimer.current) { clearTimeout(trailTimer.current); trailTimer.current = null; }
      if (!buf) { swapImg(null); return; }
      const now = Date.now();
      if (!force) {
        if (linesSince.current === 0) return;
        const wait = REBUILD_MS - (now - lastBuild.current);
        if (wait > 0) {
          if (!trailTimer.current) {
            // ★ live.current, not `buf`: a WEFAX grow (+100 rows) replaces the buffer object meanwhile.
            //   Only the live view ever takes the ungated path — PREV/LIVE switches are forced and clear this.
            trailTimer.current = setTimeout(() => { trailTimer.current = null; rebuild(live.current); }, wait);
          }
          return;
        }
      }
      if (trailTimer.current) { clearTimeout(trailTimer.current); trailTimer.current = null; }
      linesSince.current = 0;
      lastBuild.current = now;
      // Crop display to written lines (+2 margin) so a fresh 500-row WEFAX
      // buffer doesn't show as a giant black void
      const visH = Math.max(1, Math.min(buf.h, buf.maxLine + 2));
      const slice = buf.data.subarray(0, buf.w * visH * 4);
      const data = Skia.Data.fromBytes(slice);
      const sk = Skia.Image.MakeImage(
        { width: buf.w, height: visH, colorType: ColorType.RGBA_8888, alphaType: AlphaType.Opaque },
        data,
        buf.w * 4,
      );
      if (sk) {
        swapImg({ img: sk, data }); setDispDims({ w: buf.w, h: visH });
        // ★ The SHIFT this picture was drawn with — ALIGN's preview slides it by the difference (see alignPreview).
        setImgShift(buf.raw && buf === live.current ? effAlign(buf).shift : null);
      }
      else data.dispose();
    // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [swapImg]);

    /* ★★ ANY PICTURE WITH LINES IN IT GOES TO PREV when the next one starts — finished or not (Stuart,
     *  2026-10-04: "once an image has been received it stays on screen until the next one is received, and then
     *  when the new image is started the old one goes into a previous button"). It waited for `complete`, so a
     *  partial SSTV frame (signal faded, late join) was simply thrown away by the next image's start. */
    /* ★ A SHIFT / SLANT change redraws the WHOLE live chart from its kept lines — not only the lines to come —
     *  so the listener sees the correction land on the picture they are looking at. */
    const alignKey = `${align?.shift ?? 0}|${align?.slant ?? 0}|${autoMargin ? 1 : 0}|${autoSlant ? 1 : 0}|${raw ? 1 : 0}`;
    const firstAlign = useRef(true);
    useEffect(() => {
      if (firstAlign.current) { firstAlign.current = false; return; }
      const buf = live.current;
      if (!buf?.raw) return;
      redrawAll(buf, effAlign(buf));
      if (!viewingPrev) rebuild(buf, true);
    // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [alignKey]);

    const rollToPrev = useCallback(() => {
      if (live.current && live.current.maxLine > 0) live.current.complete = true;
      if (live.current?.complete) {
        prev.current = live.current;  store.prev = prev.current;
        onPrevState(true, false);
        setViewingPrev(false);
      }
    // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [onPrevState]);

    const growTo = useCallback((buf: PixBuf, newH: number): PixBuf => {
      const next = mkBuf(buf.w, newH);
      next.data.set(buf.data);
      if (buf.raw) { next.raw = new Uint8Array(buf.w * newH); next.raw.set(buf.raw); }
      if (buf.al) { next.al = new Uint8Array(buf.w * newH); next.al.set(buf.al); }
      if (buf.got) { next.got = new Uint8Array(newH); next.got.set(buf.got); }
      next.hist = buf.hist;
      next.auto = buf.auto;
      next.maxLine = buf.maxLine;
      next.complete = buf.complete;
      return next;
    }, []);

    // Restore the persisted image on (re)mount — rotation/minimise rebuild this
    // component, but the buffers live in `store`, so repaint from them instead of
    // starting blank. In-place pixel writes keep updating the same store buffer.
    useEffect(() => {
      live.current = store.live;
      prev.current = store.prev;
      if (store.live) { rebuild(store.live, true); onInfo(`${store.live.w}x${store.live.maxLine + 1}`); }
      onPrevState(!!store.prev, false);
    // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [store]);

    // ── Imperative API (skin _decCtx parity) ────────────────────────────────
    useImperativeHandle(ref, () => ({
      imageStart(w: number, h: number) {
        rollToPrev();
        live.current = mkBuf(w, h); store.live = live.current;
        onInfo(`${w}x${h}`);
        onStatus(`receiving ${w}x${h}`);
        rebuild(live.current, true);
      },

      wefaxLine(ln: number, w: number, px: Uint8Array) {
        // ★★ CLR STARTS A CHART THE DECODER DOES NOT KNOW ABOUT (2026-10-07, Stuart: "we need a clear button like we
        //    used to have and like we do for RTTY"). A retune keeps the decoder's line count running — JMH on 7795, JMH
        //    on 3620.6 and Korea's HLL2 stacked into one 1201-line picture, and the auto-align's 300/600-line looks were
        //    spent on the first station. So after CLR the canvas numbers lines from the first one it gets; a count going
        //    below that base is the decoder's own new chart (a start tone), numbered from 0 again.
        if (store.rebase) { store.rebase = false; store.lineBase = ln; }
        if (ln < store.lineBase) store.lineBase = 0;
        ln -= store.lineBase;
        // ★★ The FIRST chart on a fresh canvas is a new chart too (2026-10-07): only a line count going back said so,
        //    so the panel's per-chart state (a manual shift, RAW, the last chart's measured slant) carried over into it.
        if (!live.current) { live.current = mkBuf(w, WEFAX_INIT_H); store.live = live.current; onNewChart?.(); }  // lazy init
        /* ★★★ THE LINE COUNT GOING BACK IS A NEW CHART — complete or not. This waited for `complete`, which only
         *  the STOP tone sets; the app does not ask for auto-stop (so it can draw from mid-chart), so a chart
         *  joined part-way never completed and the NEXT chart, numbered from 0 again, was painted OVER it
         *  (Stuart, 2026-10-04: "the new image is writing over the old one … the previous image is meant to go
         *  into a buffer so the user can press previous image to view and save it"). The old one is finished
         *  as imageDone() would (enhance), rolled to PREV, and the new chart starts on a clean canvas.
         *  ★ A small step back (≤ 2) is tolerated — never a reason to throw a picture away. */
        if (live.current.maxLine > 0 && (ln === 0 || ln < live.current.maxLine - 2)) {     // new image
          if (!live.current.complete) {
            live.current.complete = true;
            try { redrawAll(live.current, effAlign(live.current)); } catch {}   // final levels, last rows settled
          }
          rollToPrev();
          live.current = mkBuf(w, WEFAX_INIT_H); store.live = live.current;
          onStatus('new chart — the last one is under PREV');
          onNewChart?.();
        }
        let buf = live.current;
        if (ln >= buf.h) {                                                  // grow +100
          buf = live.current = growTo(buf, ln + GROW_ROWS); store.live = buf;
          onInfo(`${w}x${ln + 1}`);
        }
        const n = Math.min(w, buf.w);
        // ★ Keep the line as received, then draw it moved by this frequency's SHIFT / SLANT (utils/wefaxAlign).
        if (!buf.raw) buf.raw = new Uint8Array(buf.w * buf.h);
        buf.raw.set(px.subarray(0, n), ln * buf.w);
        (buf.got ??= new Uint8Array(buf.h))[ln] = 1;
        const prevMax = buf.maxLine;
        addToHist(buf.hist ??= newHist(), px.subarray(0, n));
        // ★ Once the chart is long enough, find its margin / border and redraw the whole chart around it — and once
        //   more later if a longer look disagrees (chartAlignStep).
        let moved = false;
        const st = buf.auto ??= {};
        const before = st.al, slantBefore = st.slant;
        const raw = buf.raw;
        if (autoRef.current && chartAlignStep(st, () => Array.from({ length: ln + 1 },
              (_, y) => raw.subarray(y * buf.w, (y + 1) * buf.w)), buf.w, stationSlantRef.current,
              raw.subarray(ln * buf.w, (ln + 1) * buf.w), stationFormatRef.current)) {
          for (let y = 0; y < ln; y++) alignRow(buf, y, effAlign(buf));
          moved = true;
        }
        if ((st.al !== before || st.slant !== slantBefore) && st.al !== undefined) onAutoAlign?.(st.al, st.slant);
        // a slant measured where nothing moved (a phased chart) still redraws the chart at that slant
        if (!moved && st.slant !== slantBefore && autoSlantRef.current) {
          for (let y = 0; y < ln; y++) alignRow(buf, y, effAlign(buf));
          moved = true;
        }
        alignRow(buf, ln, effAlign(buf));
        if (ln > buf.maxLine) buf.maxLine = ln;
        // ★ Lost lines (2026-10-05): a jump in the line number leaves rows that never came — draw them as the line
        //   above. A late line (≤ 2 back) re-seeds the lost rows under it. After a re-align, all of them.
        const g0 = moved ? 0 : Math.min(prevMax + 1, ln + 1);
        if (moved || ln !== prevMax + 1) fillLost(buf, g0, buf.maxLine);
        // ★ The gold-standard rendering (utils/wefaxCrisp): this line and the two above it, which now have it below.
        // ★ Line 40: the paper/ink levels have settled — repaint the top, drawn while they were still being learned.
        if (moved || ln === 40) paintRows(buf, 0, ln);
        else paintRows(buf, Math.min(ln, g0) - 2, buf.maxLine);
        linesSince.current++;
        if (!viewingPrev) rebuild(buf);
      },

      sstvLine(ln: number, w: number, px: Uint8Array) {
        const buf = live.current;
        if (!buf || ln >= buf.h) return;                                    // skin: requires imageStart
        const off = ln * buf.w * 4;
        const n = Math.min(w, buf.w);
        for (let x = 0; x < n; x++) {
          const s = x * 3, o = off + x * 4;
          buf.data[o] = px[s]; buf.data[o + 1] = px[s + 1]; buf.data[o + 2] = px[s + 2]; buf.data[o + 3] = 255;
        }
        if (ln > buf.maxLine) buf.maxLine = ln;
        linesSince.current++;
        if (!viewingPrev) rebuild(buf);
      },

      imageDone() {
        if (live.current) {
          live.current.complete = true;
          // WEFAX: repaint the finished chart with its final levels (utils/wefaxCrisp).
          if (live.current.raw) { try { redrawAll(live.current, effAlign(live.current)); } catch {} }
          rebuild(live.current, true);
        }
        onStatus('done — tap SAVE');
      },

      clear(why?: string) {
        // ★ Never throws a picture away — a mis-press costs nothing: what was on screen is under PREV.
        const had = !!live.current && live.current.maxLine > 0;
        if (live.current && live.current.raw && !live.current.complete && had) {
          try { redrawAll(live.current, effAlign(live.current)); } catch {}   // final levels, as a finished chart
        }
        rollToPrev();
        live.current = null;  store.live = null;
        store.rebase = true;                    // the next WEFAX line is line 0 of a new chart (lazy init → onNewChart)
        setViewingPrev(false);
        rebuild(null, true);
        onPrevState(!!prev.current, false);
        onInfo('');
        // ★ `why` (a retune): said only when a picture actually went to PREV — an empty canvas has nothing to report.
        if (why) { if (had) onStatus(why); } else onStatus(had ? 'cleared — the last picture is under PREV' : 'cleared');
      },

      reset() {
        live.current = null;  store.live = null;
        store.lineBase = 0;  store.rebase = false;
        prev.current = null;  store.prev = null;
        setViewingPrev(false);
        rebuild(null, true);   // clears any trailing rebuild and retires the shown image
        onPrevState(false, false);
      },

      showPrev() {
        if (!prev.current) return;
        setViewingPrev(true);
        onPrevState(true, true);
        onInfo(`prev — ${prev.current.w}x${prev.current.maxLine + 1}`);
        rebuild(prev.current, true);
      },

      showLive() {
        setViewingPrev(false);
        onPrevState(!!prev.current, false);
        if (live.current) onInfo(`${live.current.w}x${live.current.maxLine + 1}`);
        rebuild(live.current, true);
      },

      async save() {
        const buf = viewingPrev ? prev.current : live.current;
        if (!buf || !img) { onStatus('nothing to save'); return; }
        const ts = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
        const name = `${decoderName}_${ts}.png`;
        try {
          const b64 = img.encodeToBase64(ImageFormat.PNG, 100);
          // ★★★ WRITE A REAL FILE AND SHARE A file:// URL. This used to hand the share sheet a
          // `data:` URL, which carries NEITHER A FILENAME NOR A TYPE — so the OS cannot classify it
          // as an image. Three symptoms, one cause (Stuart, 2026-07-30):
          //   • macOS  — a completely BLANK share sheet (AppKit will not take a data: URL).
          //   • iPhone — the sheet appears and Messages renders the picture, but Save to Photos and
          //              Save to Files are ABSENT, because those actions require a known image type.
          //   • Android — documented as message-only in the comment this replaces.
          // ★★ A file:// URL ending .png is recognised as an IMAGE, so the user gets the image share
          // sheet — Photos, AirDrop with a thumbnail, Messages with a preview — which is what someone
          // who has just decoded a picture expects. ONE fix, all three platforms.
          const f = new File(Paths.cache, name);
          try { f.create({ overwrite: true }); } catch {}   // exists ⇒ fine, write overwrites
          f.write(b64, { encoding: 'base64' });
          await Share.share({ url: f.uri } as any, { subject: name } as any);
          onStatus(`shared: ${name}`);
        } catch (e: any) {
          // ★ The old code reported "shared" unconditionally, so a blank sheet still claimed success.
          // Say what actually happened, or say nothing — "shared" over a blank sheet is worse than
          // silence because it sends the user looking for the file.
          if (e?.message === 'User did not share') onStatus('share cancelled');
          else onStatus('save failed');
        }
      },
    }), [rebuild, rollToPrev, growTo, viewingPrev, img, onInfo, onStatus, onPrevState, decoderName]);

    // ── Render ────────────────────────────────────────────────────────────────
    // ★★★ SHRINK TO FIT WHEN THE BOX CANNOT GROW ANY FURTHER (Stuart, 2026-07-30).
    // Scaling by WIDTH alone is what made a big screen show LESS of the picture: the wider the
    // panel, the taller the drawn image, against a fixed cap. Scaling by min(width, height) means a
    // whole SSTV frame is visible at once — and it fixes a phone in landscape too, where the box is
    // short and wide.
    //
    // ★★ WEFAX IS THE EXCEPTION AND MUST KEEP SCROLLING. It is an endless fax roll that grows
    // continuously, so "fit the whole thing" would shrink it to a thread. Shrink-to-fit is right for
    // a FIXED-SIZE frame and wrong for a stream, so it is chosen per decoder rather than globally.
    /* ★★★ …SUPERSEDED (Stuart, 2026-10-04): "We want the image to be seen in its full in the decoder window, with
     *  zoom and pan as needed, but by default the whole image is shown correctly". Every picture — the WEFAX roll
     *  included — fits WHOLE by default; as a chart grows it scales down to stay whole, and the header's − / +
     *  magnifies from there with panning both ways. */
    const fitsWhole = true;
    const wScale = dispDims.w > 0 ? panelW / dispDims.w : 1;
    const hScale = dispDims.h > 0 ? maxHeight / dispDims.h : 1;
    // ★★★ NEVER MAGNIFY PAST THE SOURCE BY MORE THAN THIS. An SSTV frame is 320x240 — on a Mac
    // window the fit-to-box maths wanted a 4-5x upscale, which is not "big", it is BLOCKY AND SOFT:
    // there are no extra pixels to show, so all the extra area buys is a magnified JPEG-ish mush
    // (Stuart, 2026-07-31: "the SSTV image will be huge and it's only a 320x240 image").
    // ★ 2x is the honest ceiling: enough that a phone still fills its width, not so much that a
    // desktop turns a postage stamp into a poster.
    const MAX_UPSCALE = 2;
    /* ★★ FIT FIRST, THEN THE LISTENER'S ZOOM (Stuart, 2026-10-04: "the picture needs to be scaled to the box but
     *  we could add a zoom in and out to the header"). zoom 1 is exactly the fit above; more magnifies from there,
     *  still never past MAX_UPSCALE source pixels — beyond that there is nothing more to see. */
    const fit = Math.min(fitsWhole ? Math.min(wScale, hScale) : wScale, MAX_UPSCALE);
    const scale = zoom > 1 ? Math.min(fit * zoom, Math.max(fit, MAX_UPSCALE)) : fit;
    const drawW  = Math.max(1, Math.round(dispDims.w * scale));
    const drawH  = Math.max(1, Math.round(dispDims.h * scale));
    // ★★★ AND THE BOX TAKES WHAT THE IMAGE NEEDS, NOT WHAT IT IS ALLOWED. maxHeight is a CEILING;
    // using it as the height left a fixed-size frame floating in a vast black box on a big screen.
    // WEFAX keeps the full ceiling because it grows without limit and genuinely wants the room.
    const boxH = fitsWhole ? Math.min(maxHeight, drawH) : maxHeight;

    /* ★★★ ALIGN BY DRAGGING (Stuart, 2026-10-06: the ◀ ▶ keys were "really hard to press and are finicky and cannot
     *  be held … Simpler way of doing this is on the ALIGN button have an overlay pop up over the chart <----------->
     *  drag for rough alignment then use buttons to fine tune"). While ALIGN is open the picture follows the finger
     *  sideways, wrapping as the chart itself does. The preview SLIDES the picture already on screen (two copies,
     *  side by side — a GPU move, no re-align per touch); the chart is re-aligned once, when the shift is committed,
     *  and the slide is measured from the shift the shown picture was drawn with, so the committed picture lands
     *  where the preview was without a jump back. */
    const aligning = alignPreview !== undefined;
    // ★ The drag lives HERE, not in the panel: a move re-renders this picture only, never the whole decoder box.
    const [drag, setDrag] = useState<number | null>(null);
    const shown = drag ?? alignPreview ?? 0;
    const W0 = dispDims.w;
    const slidePx = aligning && imgShift !== null && W0 > 1 ? (((shown - imgShift) % W0) + W0) % W0 : 0;
    const dragStart = useRef(0);
    const previewRef = useRef(alignPreview ?? 0);
    previewRef.current = alignPreview ?? 0;
    const scaleRef = useRef(scale);
    scaleRef.current = scale;
    const dragCb = useRef(onAlignDrag);
    dragCb.current = onAlignDrag;
    const pan = useMemo(() => {
      // finger right → picture right → the line starts further LEFT in the received line → shift DOWN
      const at = (dx: number) => Math.round(dragStart.current - dx / Math.max(scaleRef.current, 1e-3));
      const end = (dx: number) => { const s = at(dx); setDrag(null); dragCb.current?.(s, true); };
      return PanResponder.create({
        onStartShouldSetPanResponder: () => true,
        onStartShouldSetPanResponderCapture: () => true,
        onMoveShouldSetPanResponderCapture: () => true,
        onPanResponderTerminationRequest: () => false,
        onPanResponderGrant: () => { dragStart.current = previewRef.current; },
        onPanResponderMove: (_e, g) => setDrag(at(g.dx)),
        onPanResponderRelease: (_e, g) => end(g.dx),
        onPanResponderTerminate: (_e, g) => end(g.dx),
      });
    }, []);
    // The shift as the listener reads it: px, signed, the short way round (−904…+904 on a 1809 px chart).
    const hintShift = W0 > 1 ? ((((shown % W0) + W0 + W0 / 2) % W0) - W0 / 2) : shown;
    const alignHint = `SHIFT ${hintShift > 0 ? '+' : ''}${Math.round(hintShift)}`;
    // ★ Room for the hint on a chart only a few lines tall (it is fitted whole, so early on it is a sliver).
    const ALIGN_MIN_H = 72;

    return (
      <View>
      <ScrollView style={{ height: aligning ? Math.max(boxH, Math.min(maxHeight, ALIGN_MIN_H)) : boxH, maxHeight }}
                  showsVerticalScrollIndicator
                  onLayout={(e) => { const w = Math.floor(e.nativeEvent.layout.width); setBoxW((p) => (p === w ? p : w)); }}
                  /* ★ scroll lane: a picture, centred, no controls in it */>
        {/* Centred: once the image is narrower than the panel (shrunk to fit a short box) it would
            otherwise sit against the left edge with dead space beside it. */}
        {/* ★ Zoomed wider than the box: the picture pans SIDEWAYS too (the outer view scrolls down). */}
        <ScrollView horizontal scrollEnabled={drawW > panelW + 1} showsHorizontalScrollIndicator={drawW > panelW + 1}
                    contentContainerStyle={{ flexGrow: 1, justifyContent: 'center' }}>
          <View style={[styles.canvasWrap, { width: drawW, height: drawH, alignSelf: 'center' }]}>
            {img && (
              <Canvas style={{ width: drawW, height: drawH }}>
                <SkiaImage image={img} x={-slidePx * scale} y={0} width={drawW} height={drawH} fit="fill" />
                {slidePx > 0 && (
                  <SkiaImage image={img} x={(W0 - slidePx) * scale} y={0} width={drawW} height={drawH} fit="fill" />
                )}
              </Canvas>
            )}
          </View>
        </ScrollView>
      </ScrollView>
      {aligning && (
        <View style={styles.alignCover} {...pan.panHandlers}
              accessibilityLabel="Drag sideways to align the chart" accessibilityRole="adjustable">
          <View pointerEvents="none" style={styles.alignHint}>
            <Text style={styles.alignHintTxt} numberOfLines={1}>{`◀ ─── drag ───▶   ${alignHint}`}</Text>
          </View>
        </View>
      )}
      </View>
    );
  },
);

const styles = StyleSheet.create({
  canvasWrap: { backgroundColor: '#000', borderRadius: 4, overflow: 'hidden' },
  /* ★ ALIGN's cover: the whole picture is the drag target; the hint sits on the chart in a dark pill so it reads on
   *  white paper and black alike, on every chassis (the picture is the same black-and-white whatever the skin). */
  alignCover: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'flex-start', paddingTop: 8 },
  alignHint: { backgroundColor: 'rgba(0,0,0,0.62)', borderRadius: 14, paddingHorizontal: 12, paddingVertical: 5,
               borderWidth: StyleSheet.hairlineWidth, borderColor: 'rgba(255,255,255,0.35)', maxWidth: '94%' },
  alignHintTxt: { color: '#fff', fontSize: 13, fontWeight: '600', letterSpacing: 0.5, fontVariant: ['tabular-nums'] },
});

export default DecoderImageCanvas;
