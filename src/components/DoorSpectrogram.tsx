/**
 * DoorSpectrogram — the receiver's last 24 hours behind the radio list.
 *
 * ★★★ THE WEB'S LANDING PAGE HAS DRAWN THIS SINCE 2026-08-27 and the app's door was a plain black
 *     list. It is not decoration: a visitor choosing a receiver can see at a glance whether the
 *     band has been alive today, where the carriers sit and when the aerial went quiet — before
 *     they pick. Same endpoint, same "VSPG" bytes, same auto-contrast and the same Sonar Green
 *     ramp as web/client/src/main.ts drawSplashSpectrogram(), so the two pages agree about what a
 *     signal of a given strength looks like.
 * ★★ ONE FETCH, ONE IMAGE. Asked at the size this view can draw (capped as the web caps it), turned
 *    into an RGBA buffer once and handed to Skia as a single image — no per-row work, nothing on a
 *    timer. Newest row at the bottom, as the live waterfall.
 * ★ Silent on any failure: a server too old for the endpoint, a door with no history yet, a slow
 *   link. The list is the page; this is what is behind it.
 */
import React, { useEffect, useState } from 'react';
import { PixelRatio, StyleSheet, Text, View } from 'react-native';
import { AlphaType, Canvas, ColorType, Image as SkImage, Skia, type SkImage as SkImageT } from '@shopify/react-native-skia';

// ★ The real stops of the app's own 'Sonar Green' palette — see src/assets/colormaps.ts.
const SONAR_GREEN: [number, number, number][] = [
  [0x00, 0x00, 0x00], [0x00, 0x08, 0x00], [0x00, 0x1a, 0x00], [0x00, 0x33, 0x00],
  [0x00, 0x50, 0x00], [0x00, 0x78, 0x00], [0x00, 0xaa, 0x00], [0x00, 0xcc, 0x00],
  [0x00, 0xff, 0x00], [0x80, 0xff, 0x80], [0xcc, 0xff, 0xcc], [0xef, 0xff, 0xff],
];
const LUT: Uint8Array = (() => {
  const out = new Uint8Array(256 * 3);
  for (let v = 0; v < 256; v++) {
    const x = (v / 255) * (SONAR_GREEN.length - 1);
    const i = Math.min(SONAR_GREEN.length - 2, Math.floor(x));
    const f = x - i;
    const a = SONAR_GREEN[i], b = SONAR_GREEN[i + 1];
    out[v * 3]     = Math.round(a[0] + (b[0] - a[0]) * f);
    out[v * 3 + 1] = Math.round(a[1] + (b[1] - a[1]) * f);
    out[v * 3 + 2] = Math.round(a[2] + (b[2] - a[2]) * f);
  }
  return out;
})();

async function fetchSpectrogram(base: string, bins: number, rows: number): Promise<{ img: SkImageT; hours: number } | null> {
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), 8000);
  try {
    const r = await fetch(`${base}/vibeserver/spectrogram?bins=${bins}&rows=${rows}`, { signal: ctrl.signal });
    if (!r.ok) return null;
    const buf = await r.arrayBuffer();
    if (buf.byteLength < 25) return null;
    const dv = new DataView(buf);
    if (String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3)) !== 'VSPG') return null;
    const nb = dv.getUint16(5, true), nr = dv.getUint16(7, true);
    if (!nb || !nr || buf.byteLength < 25 + nr * (8 + nb)) return null;
    const bytes = new Uint8Array(buf);
    // ★★★ AUTO-CONTRAST FROM THE DATA — percentiles, so one lightning crash cannot set the top of
    //     the scale and darken the whole day. Same 0.30 / 0.998 the web settled on by looking.
    const hist = new Uint32Array(256);
    for (let rr = 0; rr < nr; rr++) {
      const off = 25 + rr * (8 + nb) + 8;
      for (let i = 0; i < nb; i++) hist[bytes[off + i]]++;
    }
    const total = nr * nb;
    const pct = (p: number) => { let seen = 0; for (let v = 0; v < 256; v++) { seen += hist[v]; if (seen >= total * p) return v; } return 255; };
    const loV = pct(0.30), hiV = Math.max(loV + 6, pct(0.998));
    const rgba = new Uint8Array(nb * nr * 4);
    for (let rr = 0; rr < nr; rr++) {
      const off = 25 + rr * (8 + nb) + 8;
      const rowOut = (nr - 1 - rr) * nb * 4;           // newest at the bottom
      for (let i = 0; i < nb; i++) {
        const v = bytes[off + i];
        const t01 = Math.pow(Math.max(0, Math.min(1, (v - loV) / (hiV - loV))), 0.80);
        const li = Math.round(t01 * 255) * 3;
        const p = rowOut + i * 4;
        rgba[p] = LUT[li]; rgba[p + 1] = LUT[li + 1]; rgba[p + 2] = LUT[li + 2]; rgba[p + 3] = 255;
      }
    }
    const data = Skia.Data.fromBytes(rgba);
    const img = Skia.Image.MakeImage(
      { width: nb, height: nr, colorType: ColorType.RGBA_8888, alphaType: AlphaType.Opaque }, data, nb * 4);
    if (!img) return null;
    // The row header carries the row's time; first and last give the span the picture covers.
    const t0 = dv.getFloat64(25, true), t1 = dv.getFloat64(25 + (nr - 1) * (8 + nb), true);
    const hours = Number.isFinite(t0) && Number.isFinite(t1) && t1 > t0 ? (t1 - t0) / 3600 : 0;
    return { img, hours };
  } catch { return null; }
  finally { clearTimeout(t); }
}

export default function DoorSpectrogram({ base, width, height }: { base: string; width: number; height: number }) {
  const [pic, setPic] = useState<{ img: SkImageT; hours: number } | null>(null);
  useEffect(() => {
    let dead = false;
    if (!(width > 0 && height > 0)) return;
    const dpr = PixelRatio.get();
    const bins = Math.min(2048, Math.max(512, Math.floor(width * dpr)));
    const rows = Math.min(1440, Math.max(180, Math.floor(height * dpr)));
    fetchSpectrogram(base.replace(/\/+$/, ''), bins, rows).then(p => { if (!dead) setPic(p); });
    return () => { dead = true; };
  }, [base, width, height]);
  if (!pic) return null;
  return (
    <View style={StyleSheet.absoluteFill} pointerEvents="none">
      <Canvas style={StyleSheet.absoluteFill}>
        <SkImage image={pic.img} x={0} y={0} width={width} height={height} fit="fill" opacity={0.55} />
      </Canvas>
      {/* ★ A dark veil so the amber list reads over the green — the web dims radially. */}
      <View style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(0,0,0,0.45)' }]} />
      <Text style={s.cap}>BAND ACTIVITY{pic.hours >= 1 ? ` · ${Math.round(pic.hours)} H` : ''}</Text>
    </View>
  );
}

const s = StyleSheet.create({
  cap: { position: 'absolute', right: 14, bottom: 10, color: 'rgba(255,184,51,0.45)', fontSize: 9, letterSpacing: 2 },
});
