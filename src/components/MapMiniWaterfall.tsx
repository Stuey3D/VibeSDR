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
import { mwfCrop, mwfMinSpan, mwfDims, mwfDue, mwfX } from '../utils/mapChrome';
import type { MapWfModel } from '../utils/mapWfModel';
import { GAP_ROW as GAP, mwfPaint, pushRow, mwfClampToCapture, type MwfStore, type MwfFrame } from '../utils/mapWfPaint';   // ★ the pure half (tested)
export { mwfStoreNew, type MwfStore, type MwfFrame } from '../utils/mapWfPaint';
import { DECODER_FONT } from './DecoderShell';

/** The history, owned by the map (one per map session) so it outlives this view while the list is showing. */


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
  /* ★ each model's stamps (a NEW decoded reading — mapWfModel only gives one once) queued as the model arrives, drained into
   *  the history at the next row: never lost to a re-render between two rows, never stamped twice */
  const pending = useRef<Array<{ key: string; hz: number; text: string }>>([]);
  useEffect(() => { if (model?.stamps.length) { pending.current.push(...model.stamps.map((x) => ({ key: model.key, hz: x.hz, text: x.text }))); if (pending.current.length > 64) pending.current = pending.current.slice(-64); } }, [model]);
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
      // ★★★ widened to 16 bins, then kept INSIDE the frame (never a span starting below its lowest bin: the left strip)
      const v = mwfClampToCapture(mwfMinSpan(m.lo, m.hi, f.bins.length ? f.spanHz / f.bins.length : 0), f.centreHz, f.spanHz);
      if (z.vlo !== v.lo || z.vhi !== v.hi) { z.vlo = v.lo; z.vhi = v.hi; }
      const row = mwfCrop(f.bins, f.centreHz, f.spanHz, v.lo, v.hi, z.cols);
      pushRow(z, row);
      for (const sp of pending.current) { if (sp.key !== m.key) continue; const x = mwfX(sp.hz, v.lo, v.hi); if (x != null) z.stamps.push({ seq: z.seq - 1, x, text: sp.text }); }
      pending.current = [];
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
    const rgba = mwfPaint(s, h, lut);
    const data = Skia.Data.fromBytes(rgba);
    const next = Skia.Image.MakeImage({ width: s.cols, height: h, colorType: ColorType.RGBA_8888, alphaType: AlphaType.Opaque }, data, s.cols * 4);
    setImg((prev) => { if (prev) setTimeout(() => prev.dispose(), 300); return next; });
    data.dispose();
  }, [tick, width, height, lut]);   // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => () => { setImg((prev) => { if (prev) setTimeout(() => prev.dispose(), 300); return null; }); }, []);

  const s = store.current;
  // ★★★ the markers on the span actually drawn (a widened narrow view keeps them in line with the picture)
  const m = model && s.key === model.key && s.vlo != null && s.vhi != null ? { ...model, lo: s.vlo, hi: s.vhi } : model;
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


const mw = StyleSheet.create({
  wrap:  { overflow: 'hidden', backgroundColor: '#000' },
  band:  { position: 'absolute', top: 0, bottom: 0, backgroundColor: 'rgba(255,255,255,0.10)' },
  mark:  { position: 'absolute', top: 0, bottom: 0, width: 1, backgroundColor: '#ffd34d' },
  label: { position: 'absolute', fontFamily: DECODER_FONT, fontSize: 9.5, color: '#ffe9a8', backgroundColor: 'rgba(0,0,0,0.55)', paddingHorizontal: 2, maxWidth: 88 },
  stamp: { position: 'absolute', fontFamily: DECODER_FONT, fontSize: 10, fontWeight: '700', color: '#fff', backgroundColor: 'rgba(0,0,0,0.6)', paddingHorizontal: 2 },
});
