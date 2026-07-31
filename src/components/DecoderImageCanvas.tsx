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
 * Rendering: pixel buffer → SkImage, rebuilt at most every REBUILD_LINES lines
 * (or 150ms) so a 1809-wide WEFAX at 120 LPM doesn't thrash the GPU upload.
 * Displayed scaled to panel width, aspect preserved, scrolls as it grows.
 */

import React, {
  forwardRef, useCallback, useEffect, useImperativeHandle, useRef, useState,
} from 'react';
import { ScrollView, Share, StyleSheet, View, useWindowDimensions } from 'react-native';
// ★★ A REAL FILE, NOT A data: URL — see save() below for why.
import { File, Paths } from 'expo-file-system';
import {
  Canvas, Image as SkiaImage, Skia,
  AlphaType, ColorType, ImageFormat, type SkImage,
} from '@shopify/react-native-skia';

const REBUILD_LINES = 8;
const REBUILD_MS    = 150;
const WEFAX_INIT_H  = 500;  // skin: _initCanvas(w, 500)
const GROW_ROWS     = 100;  // skin: _canvas.height = ln + 100

interface PixBuf {
  w: number;
  h: number;
  data: Uint8Array;   // RGBA
  complete: boolean;
  maxLine: number;    // highest line written (display crop)
}

// Persistent per-decoder image store. The live/prev buffers live OUTSIDE the
// component so they survive a remount — rotating the device, minimising the
// decoder tab, or any re-layout used to drop the buffer and restart the image
// from the current scanline. Keyed by decoder name; in-place pixel writes
// persist automatically (the store holds the SAME PixBuf reference). This also
// gives every image decoder the 1-image PREV buffer for free.
interface ImgStore { live: PixBuf | null; prev: PixBuf | null }
const imgStores: Record<string, ImgStore> = {};
function getImgStore(name: string): ImgStore {
  return (imgStores[name] ??= { live: null, prev: null });
}

export interface DecoderImageHandle {
  imageStart: (w: number, h: number) => void;
  wefaxLine:  (ln: number, w: number, px: Uint8Array) => void;
  sstvLine:   (ln: number, w: number, px: Uint8Array) => void;
  imageDone:  () => void;
  reset:      () => void;
  showPrev:   () => void;
  showLive:   () => void;
  save:       () => Promise<void>;
}

export interface DecoderImageCanvasProps {
  maxHeight: number;
  /** Header info string updates: "1809x500", "prev — 1809x842", … */
  onInfo:    (info: string) => void;
  onStatus:  (status: string) => void;
  /** PREV button availability + current view, for the panel header. */
  onPrevState: (hasPrev: boolean, viewingPrev: boolean) => void;
  decoderName: string;   // for the save filename: wefax_2026-06-10T18-31-02.png
}

function mkBuf(w: number, h: number): PixBuf {
  const data = new Uint8Array(w * h * 4); // zero-filled = black, alpha set on write
  for (let i = 3; i < data.length; i += 4) data[i] = 255;
  return { w, h, data, complete: false, maxLine: 0 };
}

// WEFAX post-process (greyscale): auto-level contrast stretch + 3×3 median
// despeckle. Runs once on the completed image — weak HF fax comes in faint and
// speckled, this pulls the black/white levels to the 2nd/98th percentile and
// removes salt-and-pepper noise. Operates in place on the RGBA buffer (R=G=B).
function enhanceWefax(buf: PixBuf) {
  const { w, data } = buf;
  const h = Math.max(1, buf.maxLine + 1);
  const N = w * h;
  if (N < w * 4) return; // too little to bother

  const g = new Uint8Array(N);
  for (let i = 0; i < N; i++) g[i] = data[i * 4];

  // Auto-level: stretch the 2nd…98th percentile to full range.
  const hist = new Uint32Array(256);
  for (let i = 0; i < N; i++) hist[g[i]]++;
  const loCount = N * 0.02, hiCount = N * 0.98;
  let acc = 0, lo = 0, hi = 255;
  for (let v = 0; v < 256; v++) { acc += hist[v]; if (acc >= loCount) { lo = v; break; } }
  acc = 0;
  for (let v = 255; v >= 0; v--) { acc += hist[v]; if (acc >= N - hiCount) { hi = v; break; } }
  if (hi <= lo) hi = lo + 1;
  const scale = 255 / (hi - lo);
  const lut = new Uint8Array(256);
  for (let v = 0; v < 256; v++) { const x = (v - lo) * scale; lut[v] = x < 0 ? 0 : x > 255 ? 255 : x; }

  // 3×3 median despeckle (on the raw luma), then apply the contrast LUT.
  const win = new Uint8Array(9);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      let m: number;
      if (x === 0 || y === 0 || x === w - 1 || y === h - 1) {
        m = g[y * w + x];
      } else {
        let k = 0;
        for (let dy = -1; dy <= 1; dy++)
          for (let dx = -1; dx <= 1; dx++) win[k++] = g[(y + dy) * w + (x + dx)];
        for (let a = 1; a < 9; a++) { const t = win[a]; let b = a - 1; while (b >= 0 && win[b] > t) { win[b + 1] = win[b]; b--; } win[b + 1] = t; }
        m = win[4];
      }
      const v = lut[m];
      const o = (y * w + x) * 4;
      data[o] = v; data[o + 1] = v; data[o + 2] = v;
    }
  }
}

const DecoderImageCanvas = forwardRef<DecoderImageHandle, DecoderImageCanvasProps>(
  function DecoderImageCanvas({ maxHeight, onInfo, onStatus, onPrevState, decoderName }, ref) {
    const { width: winW } = useWindowDimensions();
    const panelW = winW - 16 - 24; // wrap margins + body padding

    const store = getImgStore(decoderName || 'img');
    const live = useRef<PixBuf | null>(store.live);
    const prev = useRef<PixBuf | null>(store.prev);
    const [viewingPrev, setViewingPrev] = useState(false);
    const [img, setImg] = useState<SkImage | null>(null);
    const [dispDims, setDispDims] = useState({ w: 1, h: 1 });

    const linesSince = useRef(0);
    const lastBuild  = useRef(0);

    // ── SkImage rebuild ──────────────────────────────────────────────────────
    const rebuild = useCallback((buf: PixBuf | null, force = false) => {
      if (!buf) { setImg(null); return; }
      const now = Date.now();
      if (!force && linesSince.current < REBUILD_LINES && now - lastBuild.current < REBUILD_MS) return;
      linesSince.current = 0;
      lastBuild.current = now;
      // Crop display to written lines (+2 margin) so a fresh 500-row WEFAX
      // buffer doesn't show as a giant black void
      const visH = Math.max(1, Math.min(buf.h, buf.maxLine + 2));
      const slice = buf.data.subarray(0, buf.w * visH * 4);
      const sk = Skia.Image.MakeImage(
        { width: buf.w, height: visH, colorType: ColorType.RGBA_8888, alphaType: AlphaType.Opaque },
        Skia.Data.fromBytes(slice),
        buf.w * 4,
      );
      if (sk) { setImg(sk); setDispDims({ w: buf.w, h: visH }); }
    }, []);

    const rollToPrev = useCallback(() => {
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
        if (!live.current) { live.current = mkBuf(w, WEFAX_INIT_H); store.live = live.current; }  // lazy init
        if (ln === 0 && live.current.complete) {                            // new image
          rollToPrev();
          live.current = mkBuf(w, WEFAX_INIT_H); store.live = live.current;
        }
        let buf = live.current;
        if (ln >= buf.h) {                                                  // grow +100
          buf = live.current = growTo(buf, ln + GROW_ROWS); store.live = buf;
          onInfo(`${w}x${ln + 1}`);
        }
        const off = ln * buf.w * 4;
        const n = Math.min(w, buf.w);
        for (let x = 0; x < n; x++) {
          const v = px[x] ?? 0;
          const o = off + x * 4;
          buf.data[o] = v; buf.data[o + 1] = v; buf.data[o + 2] = v; buf.data[o + 3] = 255;
        }
        if (ln > buf.maxLine) buf.maxLine = ln;
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
          // WEFAX: enhance the finished greyscale fax (contrast + despeckle).
          if ((decoderName || '').toLowerCase() === 'wefax') {
            try { enhanceWefax(live.current); } catch {}
          }
          rebuild(live.current, true);
        }
        onStatus('done — tap SAVE');
      },

      reset() {
        live.current = null;  store.live = null;
        prev.current = null;  store.prev = null;
        setViewingPrev(false);
        setImg(null);
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
    const fitsWhole = decoderName !== 'wefax';
    const wScale = dispDims.w > 0 ? panelW / dispDims.w : 1;
    const hScale = dispDims.h > 0 ? maxHeight / dispDims.h : 1;
    // ★★★ NEVER MAGNIFY PAST THE SOURCE BY MORE THAN THIS. An SSTV frame is 320x240 — on a Mac
    // window the fit-to-box maths wanted a 4-5x upscale, which is not "big", it is BLOCKY AND SOFT:
    // there are no extra pixels to show, so all the extra area buys is a magnified JPEG-ish mush
    // (Stuart, 2026-07-31: "the SSTV image will be huge and it's only a 320x240 image").
    // ★ 2x is the honest ceiling: enough that a phone still fills its width, not so much that a
    // desktop turns a postage stamp into a poster.
    const MAX_UPSCALE = 2;
    const scale = Math.min(fitsWhole ? Math.min(wScale, hScale) : wScale, MAX_UPSCALE);
    const drawW  = Math.max(1, Math.round(dispDims.w * scale));
    const drawH  = Math.max(1, Math.round(dispDims.h * scale));
    // ★★★ AND THE BOX TAKES WHAT THE IMAGE NEEDS, NOT WHAT IT IS ALLOWED. maxHeight is a CEILING;
    // using it as the height left a fixed-size frame floating in a vast black box on a big screen.
    // WEFAX keeps the full ceiling because it grows without limit and genuinely wants the room.
    const boxH = fitsWhole ? Math.min(maxHeight, drawH) : maxHeight;

    return (
      <ScrollView style={{ height: boxH, maxHeight }} showsVerticalScrollIndicator>
        {/* Centred: once the image is narrower than the panel (shrunk to fit a short box) it would
            otherwise sit against the left edge with dead space beside it. */}
        <View style={[styles.canvasWrap, { width: drawW, height: drawH, alignSelf: 'center' }]}>
          {img && (
            <Canvas style={{ width: drawW, height: drawH }}>
              <SkiaImage image={img} x={0} y={0} width={drawW} height={drawH} fit="fill" />
            </Canvas>
          )}
        </View>
      </ScrollView>
    );
  },
);

const styles = StyleSheet.create({
  canvasWrap: { backgroundColor: '#000', borderRadius: 4, overflow: 'hidden' },
});

export default DecoderImageCanvas;
