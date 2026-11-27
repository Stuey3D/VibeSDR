/**
 * MapMiniWaterfall — ★★★ beta97 THE MAP DRAWER'S FLY-OVER WATERFALL in the app (the web's web/client/src/mapWf.ts;
 * docs/v12/MAP-CONTROL-CARDS.md "The live waterfall in the drawer"). What it shows is src/utils/mapWfModel.ts — the SAME
 * model as the web: a slice of the dial a few times the filter, every decoder channel the server lists ("Ch1 136.700"),
 * the VOR window fresh per dwell, the NDB band; DECODED idents stamped into the history as they are read ("DTY" at
 * 116.400), never a listed one.
 *
 *  ★ PASSIVE: no touch handlers at all — nothing here can tune (the ask lock never comes into it).
 *  ★ LIGHT: about one column a point (DPR 1), at most 10 rows a second (mapChrome MWF_MAX_FPS), one Skia image redrawn at
 *    that rate; the markers and stamps are plain views over it.
 *  ★ THE USER'S LOOK: the palette is the waterfall's own colormap (getColorLUT); the slice is re-levelled the way auto
 *    levels do — its noise floor to index 70 — so a quiet slice of a wide FFT is not black.
 *  ★ HISTORY: a ring of palette indices the screen's longer side deep (≤ 1600 rows), KEPT in the caller's store across
 *    list ↔ waterfall and maximise / rotation (a grey gap line marks the time it was hidden), freed when the map closes;
 *    a retune (the model's key) clears it.
 *  ★ FRAMES: the caller points its spectrum sink at `feed` only while this is on screen; the spectrum itself is on only
 *    then on a live map (mapChrome mapSpecWanted).
 */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import { AlphaType, Canvas, ColorType, Image as SkImage, Skia, type SkImage as SkImageT } from '@shopify/react-native-skia';
import { getColorLUT } from '../assets/colormapUtils';
import { mwfCrop, mwfDims, mwfDue, mwfX } from '../utils/mapChrome';
import type { MapWfModel } from '../utils/mapWfModel';
import { DECODER_FONT } from './DecoderShell';

/** The history, owned by the map (one per map session) so it outlives this view while the list is showing. */
export interface MwfStore {
  cols: number; rows: number;
  /** palette index per cell; 255 = the grey gap line, 0 = nothing */
  ring: Uint8Array;
  /** rows written so far (the newest is (seq - 1) % rows) */
  seq: number;
  key: string;
  floor: number;
  stamps: Array<{ seq: number; x: number; text: string }>;
  hiddenAt: number;
}
export function mwfStoreNew(): MwfStore { return { cols: 0, rows: 0, ring: new Uint8Array(0), seq: 0, key: '', floor: NaN, stamps: [], hiddenAt: 0 }; }
export type MwfFrame = { bins: Float32Array; centreHz: number; spanHz: number };

const GAP = 255;
const LEVEL_IDX = 70, LEVEL_SPAN_DB = 40;

export default function MapMiniWaterfall({ width, height, screenW, screenH, model, colormap, store, sinkRef }: {
  width: number; height: number; screenW: number; screenH: number;
  model: MapWfModel | null;
  colormap: string;
  store: React.MutableRefObject<MwfStore>;
  /** Where the caller's spectrum frames go while this is shown (null when hidden). */
  sinkRef: React.MutableRefObject<((f: MwfFrame) => void) | null>;
}) {
  const lut = useMemo(() => getColorLUT(colormap || 'gqrx'), [colormap]);
  const [img, setImg] = useState<SkImageT | null>(null);
  const [tick, setTick] = useState(0);
  const modelRef = useRef(model); modelRef.current = model;
  const lastDraw = useRef(0);
  const dims = mwfDims(width, screenW, screenH);

  // ── (re)size the ring: a rotation / maximise keeps the rows it can (columns resampled by restarting the ring) ──
  const st = store.current;
  if (st.cols !== dims.cols || st.rows !== dims.rows) {
    const old = st.ring, oc = st.cols, orows = st.rows, oseq = st.seq;
    st.ring = new Uint8Array(dims.cols * dims.rows);
    if (oc > 0 && orows > 0) {
      // ★ kept across a resize: each old row resampled to the new width
      const keep = Math.min(orows, dims.rows, oseq);
      for (let k = keep; k >= 1; k--) {
        const src = ((oseq - k) % orows) * oc, dst = ((keep - k) % dims.rows) * dims.cols;
        for (let c = 0; c < dims.cols; c++) st.ring[dst + c] = old[src + Math.min(oc - 1, Math.floor((c * oc) / dims.cols))];
      }
      st.seq = keep;
      st.stamps = st.stamps.filter((s) => oseq - s.seq <= keep).map((s) => ({ ...s, seq: s.seq - (oseq - keep) }));
    } else st.seq = 0;
    st.cols = dims.cols; st.rows = dims.rows;
  }

  // ── shown: a grey line for the time it was hidden; the sink fed ──
  useEffect(() => {
    const s = store.current;
    if (s.hiddenAt && s.seq > 0) pushRow(s, null);
    s.hiddenAt = 0;
    sinkRef.current = (f) => {
      const m = modelRef.current; const z = store.current;
      if (!m) return;
      if (z.key !== m.key) { z.seq = 0; z.stamps = []; z.key = m.key; z.floor = NaN; }   // ★ a retune: fresh
      const now = Date.now();
      if (!mwfDue(lastDraw.current, now)) return;
      lastDraw.current = now;
      const row = mwfCrop(f.bins, f.centreHz, f.spanHz, m.lo, m.hi, z.cols);
      pushRow(z, row);
      for (const sp of m.stamps) { const x = mwfX(sp.hz, m.lo, m.hi); if (x != null) z.stamps.push({ seq: z.seq - 1, x, text: sp.text }); }
      if (z.stamps.length > 200) z.stamps = z.stamps.slice(-200);
      setTick((t) => (t + 1) & 0xffff);
    };
    return () => { sinkRef.current = null; store.current.hiddenAt = Date.now(); };
  }, []);   // eslint-disable-line react-hooks/exhaustive-deps

  // ── draw: the visible rows, newest at the top, into one image ──
  useEffect(() => {
    const s = store.current;
    const h = Math.max(1, Math.min(s.rows, Math.round(height)));
    if (!s.cols || !width) return;
    const rgba = new Uint8Array(s.cols * h * 4);
    const n = Math.min(h, s.seq);
    for (let y = 0; y < n; y++) {
      const src = ((s.seq - 1 - y) % s.rows) * s.cols, dst = y * s.cols * 4;
      for (let c = 0; c < s.cols; c++) {
        const v = s.ring[src + c], p = dst + c * 4;
        if (v === GAP) { rgba[p] = 90; rgba[p + 1] = 90; rgba[p + 2] = 90; }
        else { const li = v * 3; rgba[p] = lut[li]; rgba[p + 1] = lut[li + 1]; rgba[p + 2] = lut[li + 2]; }
        rgba[p + 3] = 255;
      }
    }
    for (let i = n * s.cols * 4; i < rgba.length; i += 4) { rgba[i] = lut[0]; rgba[i + 1] = lut[1]; rgba[i + 2] = lut[2]; rgba[i + 3] = 255; }
    const data = Skia.Data.fromBytes(rgba);
    const next = Skia.Image.MakeImage({ width: s.cols, height: h, colorType: ColorType.RGBA_8888, alphaType: AlphaType.Opaque }, data, s.cols * 4);
    setImg((prev) => { if (prev) setTimeout(() => prev.dispose(), 300); return next; });
    data.dispose();
  }, [tick, width, height, lut]);   // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => () => { setImg((prev) => { if (prev) setTimeout(() => prev.dispose(), 300); return null; }); }, []);

  const m = model;
  const s = store.current;
  return (
    <View style={[mw.wrap, { width, height }]} pointerEvents="none" accessibilityLabel={m?.info ?? 'Live waterfall'}>
      {img && <Canvas style={StyleSheet.absoluteFill}><SkImage image={img} x={0} y={0} width={width} height={height} fit="fill" /></Canvas>}
      {m?.marks.map((k, i) => {
        const x = mwfX(k.hz, m.lo, m.hi); if (x == null) return null;
        const lo = k.loHz != null ? mwfX(Math.max(m.lo, k.loHz), m.lo, m.hi) : null, hi = k.hiHz != null ? mwfX(Math.min(m.hi, k.hiHz), m.lo, m.hi) : null;
        return (
          <React.Fragment key={`${k.hz}:${i}`}>
            {lo != null && hi != null && <View style={[mw.band, { left: lo * width, width: Math.max(1, (hi - lo) * width) }]} />}
            <View style={[mw.mark, { left: x * width, opacity: k.active ? 0.9 : 0.4 }]} />
            {!!k.label && <Text style={[mw.label, { left: Math.min(width - 90, Math.max(2, x * width + 3)), top: 2 + (i % 3) * 12 }]} numberOfLines={1}>{k.label}</Text>}
          </React.Fragment>
        );
      })}
      {s.stamps.map((sp, i) => {
        const y = s.seq - 1 - sp.seq; if (y < 0 || y > height - 12) return null;
        return <Text key={`st${i}`} style={[mw.stamp, { left: Math.min(width - 50, Math.max(2, sp.x * width + 3)), top: y }]} numberOfLines={1}>{sp.text}</Text>;
      })}
    </View>
  );
}

/** One row into the ring: palette indices re-levelled (floor → LEVEL_IDX), or the grey gap line (null). */
function pushRow(s: MwfStore, row: Float32Array | null) {
  if (!s.cols || !s.rows) return;
  const off = (s.seq % s.rows) * s.cols;
  if (!row) { s.ring.fill(GAP, off, off + s.cols); s.seq++; return; }
  // the slice's noise floor: its 25th percentile, eased so a burst does not pump the picture
  const vals: number[] = [];
  for (let c = 0; c < row.length; c += Math.max(1, Math.floor(row.length / 128))) if (Number.isFinite(row[c])) vals.push(row[c]);
  if (vals.length) {
    vals.sort((a, b) => a - b);
    const f = vals[Math.floor(vals.length * 0.25)];
    s.floor = Number.isFinite(s.floor) ? s.floor + (f - s.floor) * 0.15 : f;
  }
  const scale = (255 - LEVEL_IDX) / LEVEL_SPAN_DB;
  for (let c = 0; c < s.cols; c++) {
    const v = row[c];
    s.ring[off + c] = Number.isFinite(v) && Number.isFinite(s.floor) ? Math.max(1, Math.min(254, Math.round(LEVEL_IDX + (v - s.floor) * scale))) : 0;
  }
  s.seq++;
}

const mw = StyleSheet.create({
  wrap:  { overflow: 'hidden', backgroundColor: '#000' },
  band:  { position: 'absolute', top: 0, bottom: 0, backgroundColor: 'rgba(255,255,255,0.10)' },
  mark:  { position: 'absolute', top: 0, bottom: 0, width: 1, backgroundColor: '#ffd34d' },
  label: { position: 'absolute', fontFamily: DECODER_FONT, fontSize: 9.5, color: '#ffe9a8', backgroundColor: 'rgba(0,0,0,0.55)', paddingHorizontal: 2, maxWidth: 88 },
  stamp: { position: 'absolute', fontFamily: DECODER_FONT, fontSize: 10, fontWeight: '700', color: '#fff', backgroundColor: 'rgba(0,0,0,0.6)', paddingHorizontal: 2 },
});
