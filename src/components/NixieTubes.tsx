/**
 * NixieTubes — the frequency on REAL TUBES (faceplates brief §7; refs docs/faceplates/nixie/N1…N5,
 * nixie-tune.gif; numbers from Deck.mockup `nixie` / `tb`).
 *
 * "Nixie is warm analog ancient orange like the real thing. It's the only thing I will not budge on."
 * ★★★ There is NO tubeless Nixie. Under the `nixie` display the frequency is always this.
 *
 * Domed side-view IN-14 tubes standing in sockets on the floor of a RECESS, the decimal point its own
 * INS-1 bulb tube, a FIXED row per radio (constants/nixie.ts has every rule, tested).
 *
 * ★★ TWO LAYERS, and the split is the performance design (§7 TRAP, Xcover 4S):
 *   1. NixieStatic — the recess (back wall, grain, neon spill, lip shadows), every tube's cast
 *      shadow and halo, pip, glass, anode mesh, collar, and the bulbs' glass, electrode and leads.
 *      None of it ever changes, so it is one memoised canvas that redraws only when the WINDOW
 *      SIZE or the radio's tube count changes — never on a tune step.
 *   2. NixieCathodes — only what a tune step changes: the cathode wires around each lit digit, the
 *      lit digit, the afterglow, and the lit bead. The glows are pre-rendered SPRITES
 *      (glowSprite.ts) — one image per digit, rasterised once — so a fast spin blits images and
 *      never runs a live BlurMask.
 * ★ Neon afterglow: a cathode that goes out is drawn at ≈38 % for one frame, then gone, while the
 *   new one lights. A fade, never a flash; nothing animates at rest, so there is no flicker.
 * ★ Fast tune: every frequency this component is given is drawn, at the rate it is given — no
 *   easing, no skipping, no per-frame interpolation. A real tube cannot skip digits.
 */

import React, { useEffect, useMemo, useRef, useState } from 'react';
import { View, type LayoutChangeEvent, type StyleProp, type ViewStyle } from 'react-native';
import {
  Canvas, Group, Image as SkImageNode, LinearGradient, Path, RadialGradient, Rect, Skia, Text as SkText,
  useTypeface, vec, BlurMask, TileMode, type SkFont, type SkImage, type SkPath,
} from '@shopify/react-native-skia';
import {
  cathodeDepth, cathodeNeighbours, nixieGeometry, nixieReadout, nixieSpec, mhzDigitsFor,
  COLLAR_H, PIP_H, type NixieGeometry, type NixieLayout, type NixieSpec, type NixieUnit, type TubeDesign,
} from '../constants/nixie';
import { glowPaint, makeSprite, spriteBuild, useSharedSprite, type GlowStack } from './glowSprite';

const NIXIE_TTF = require('../../assets/fonts/NixieOne-Regular.ttf');

/** §7 digit: `#ffc48a`, glow `0 0 1.5 #ff8a2e, 0 0 5 #ff6410, 0 0 11 rgba(255,80,10,.75), 0 0 20 rgba(255,70,0,.35)`. */
const DIGIT_CORE = '#ffc48a';
const DIGIT_GLOW: GlowStack = [[20, 'rgba(255,70,0,0.35)'], [11, 'rgba(255,80,10,0.75)'], [5, '#ff6410'], [1.5, '#ff8a2e']];
/** §7 bead: 4 × 5, `#fff1dc → #ffbd70 → #ff6a14`, glow `0 0 2 #ff8a2e, 0 0 6 #ff6410, 0 0 12 …, 0 0 20 …`. */
const BEAD_GLOW: GlowStack = [[20, 'rgba(255,70,0,0.3)'], [12, 'rgba(255,80,10,0.7)'], [6, '#ff6410'], [2, '#ff8a2e']];
const WIRE_BACK  = 'rgba(150,130,110,0.10)';
const WIRE_FRONT = 'rgba(70,48,34,0.55)';
const AFTERGLOW  = 0.38;

// ── Shapes ────────────────────────────────────────────────────────────────────

type Corner = [number, number];
/** A box with elliptical per-corner radii — CSS `border-radius: a b c d / e f g h`. */
function cornerPath(x: number, y: number, w: number, h: number, tl: Corner, tr: Corner, br: Corner, bl: Corner): SkPath {
  const p = Skia.Path.Make();
  const R = (cx: number, cy: number, rx: number, ry: number) => Skia.XYWHRect(cx, cy, 2 * rx, 2 * ry);
  p.moveTo(x, y + tl[1]);
  p.arcToOval(R(x, y, tl[0], tl[1]), 180, 90, false);
  p.lineTo(x + w - tr[0], y);
  p.arcToOval(R(x + w - 2 * tr[0], y, tr[0], tr[1]), 270, 90, false);
  p.lineTo(x + w, y + h - br[1]);
  p.arcToOval(R(x + w - 2 * br[0], y + h - 2 * br[1], br[0], br[1]), 0, 90, false);
  p.lineTo(x + bl[0], y + h);
  p.arcToOval(R(x, y + h - 2 * bl[1], bl[0], bl[1]), 90, 90, false);
  p.close();
  return p;
}
/** The digit tube's glass: `48% 48% 3 3 / 26% 26% 3 3`. */
const tubeGlass = (x: number, y: number, w: number, h: number) =>
  cornerPath(x, y, w, h, [0.48 * w, 0.26 * h], [0.48 * w, 0.26 * h], [3, 3], [3, 3]);
/** The bulb's glass: `48% 48% 2 2 / 22% 22% 2 2`. */
const bulbGlass = (x: number, y: number, w: number, h: number) =>
  cornerPath(x, y, w, h, [0.48 * w, 0.22 * h], [0.48 * w, 0.22 * h], [2, 2], [2, 2]);

/** A CSS `radial-gradient(ellipse RX RY at CX CY, a, b stop)` over a box, as a squashed circle. */
function Ellipse({ cx, cy, rx, ry, colors, positions }: {
  cx: number; cy: number; rx: number; ry: number; colors: string[]; positions: number[];
}) {
  const k = ry / Math.max(0.001, rx);
  return (
    <Group transform={[{ translateX: cx }, { translateY: cy }, { scaleY: k }, { translateX: -cx }, { translateY: -cy }]}>
      <Rect x={cx - rx} y={cy - rx} width={2 * rx} height={2 * rx}>
        <RadialGradient c={vec(cx, cy)} r={rx} colors={colors} positions={positions} />
      </Rect>
    </Group>
  );
}

// ── Layer 1: everything that never changes ────────────────────────────────────

interface StaticProps { w: number; h: number; radius: number; geo: NixieGeometry }

/** Tube stack edges for a box: pip top, glass top, collar top. */
function stackY(geo: NixieGeometry, glassH: number) {
  const collarY = geo.collarY;
  const glassY = collarY - glassH;
  return { collarY, glassY, pipY: glassY - PIP_H };
}

const NixieStatic = React.memo(function NixieStatic({ w, h, radius, geo }: StaticProps) {
  const clip = useMemo(() => cornerPath(0, 0, w, h, [radius, radius], [0, 0], [0, 0], [radius, radius]), [w, h, radius]);
  // ★ The recess's inset lip shadows are specified for a ~60 pt window; the bar pill is half that,
  //   so their depth scales with the window (a 16 pt top shadow would black out a 30 pt recess).
  const lk = Math.min(1, h / 48);
  const grain = useMemo(() => {
    const p = Skia.Path.Make();
    for (let x = 0; x < w; x += 3) p.addRect(Skia.XYWHRect(x, 0, 1, h));
    return p;
  }, [w, h]);
  // One path for every tube's anode mesh: a 3 × 2.6 dot grid, the second set offset by half.
  const { mesh, glassClip } = useMemo(() => {
    const m = Skia.Path.Make();
    const gc = Skia.Path.Make();
    const { glassY } = stackY(geo, geo.glassH);
    for (const t of geo.tubes) {
      gc.addPath(tubeGlass(t.x, glassY, t.w, geo.glassH));
      const x0 = t.x + 3, x1 = t.x + t.w - 3, y0 = glassY + 3, y1 = glassY + geo.glassH - 5;
      for (let y = y0; y <= y1; y += 2.6) {
        for (let x = x0; x <= x1; x += 3) m.addCircle(x, y, 0.7);
        for (let x = x0 + 1.5; x <= x1; x += 3) if (y + 1.3 <= y1) m.addCircle(x, y + 1.3, 0.7);
      }
    }
    return { mesh: m, glassClip: gc };
  }, [geo]);

  const T = stackY(geo, geo.glassH);
  const B = stackY(geo, geo.bulbH);
  return (
    <Canvas style={{ position: 'absolute', left: 0, top: 0, width: w, height: h }} pointerEvents="none">
      <Group clip={clip}>
        {/* ── The recess (ref nixie/N2): a cavity, not a flat backing ── */}
        <Rect x={0} y={0} width={w} height={h}>
          <LinearGradient start={vec(0, 0)} end={vec(0, h)}
            colors={['#060403', '#15100c', '#1e1610', '#2a1c12']} positions={[0, 0.28, 0.70, 1]} />
        </Rect>
        <Rect x={0} y={0} width={w} height={h} color="rgba(0,0,0,0.03)" />
        <Path path={grain} color="rgba(255,255,255,0.012)" />
        {/* ★★ The spill is the TUBES' light on the wall behind them, so it moves down with the
            anchored row (geo.drop) — left at the brief's 58 % it hung above the glass and the row
            read as standing in front of a glow, not set into the panel (Stuart, 2026-10-01). The
            floor spill below is the floor's, at the window's foot, where the tubes now stand. */}
        <Ellipse cx={w * 0.5} cy={h * 0.58 + geo.drop} rx={w * 0.58} ry={h * 0.60}
          colors={['rgba(255,100,30,0.16)', 'rgba(255,100,30,0)']} positions={[0, 0.72]} />
        <Ellipse cx={w * 0.5} cy={h} rx={w * 0.70} ry={h * 0.40}
          colors={['rgba(255,110,40,0.14)', 'rgba(255,110,40,0)']} positions={[0, 0.70]} />
        {/* Lip shadows: top, both sides, bottom, and the front floor edge catching the neon. */}
        <Rect x={0} y={0} width={w} height={16 * lk}>
          <LinearGradient start={vec(0, 0)} end={vec(0, 16 * lk)}
            colors={['rgba(0,0,0,0.97)', 'rgba(0,0,0,0.55)', 'rgba(0,0,0,0)']} positions={[0, 0.45, 1]} />
        </Rect>
        <Rect x={0} y={0} width={12 * lk} height={h}>
          <LinearGradient start={vec(0, 0)} end={vec(12 * lk, 0)} colors={['rgba(0,0,0,0.92)', 'rgba(0,0,0,0)']} />
        </Rect>
        <Rect x={w - 12 * lk} y={0} width={12 * lk} height={h}>
          <LinearGradient start={vec(w, 0)} end={vec(w - 12 * lk, 0)} colors={['rgba(0,0,0,0.92)', 'rgba(0,0,0,0)']} />
        </Rect>
        <Rect x={0} y={h - 7 * lk} width={w} height={7 * lk}>
          <LinearGradient start={vec(0, h)} end={vec(0, h - 7 * lk)} colors={['rgba(0,0,0,0.8)', 'rgba(0,0,0,0)']} />
        </Rect>
        <Rect x={0} y={h - 1} width={w} height={1} color="rgba(255,200,160,0.08)" />

        {/* ── Each tube's cast shadow and neon halo on the back wall (drawn once) ── */}
        {geo.tubes.map((t, i) => {
          const H = geo.collarY + COLLAR_H - T.pipY;
          const shape = cornerPath(t.x, T.pipY, t.w, H, [0.4 * t.w, 0.2 * H], [0.4 * t.w, 0.2 * H], [3, 3], [3, 3]);
          return (
            <Group key={`c${i}`}>
              <Group transform={[{ translateX: 4 }, { translateY: 3 }]}>
                <Path path={shape} color="rgba(0,0,0,0.8)"><BlurMask blur={1.5} style="normal" /></Path>
              </Group>
              <Group transform={[{ translateX: 7 }, { translateY: 6 }]}>
                <Path path={shape} color="rgba(0,0,0,0.6)"><BlurMask blur={5} style="normal" /></Path>
              </Group>
              <Path path={shape} color="rgba(255,90,20,0.10)" style="stroke" strokeWidth={6}>
                <BlurMask blur={8} style="normal" />
              </Path>
            </Group>
          );
        })}
        {geo.bulbs.map((b, i) => {
          const H = geo.collarY + COLLAR_H - B.pipY;
          const shape = cornerPath(b.x - 1, B.pipY, b.w + 2, H, [0.4 * b.w, 0.2 * H], [0.4 * b.w, 0.2 * H], [2, 2], [2, 2]);
          return (
            <Group key={`bc${i}`}>
              <Group transform={[{ translateX: 4 }, { translateY: 3 }]}>
                <Path path={shape} color="rgba(0,0,0,0.8)"><BlurMask blur={1.5} style="normal" /></Path>
              </Group>
              <Group transform={[{ translateX: 7 }, { translateY: 6 }]}>
                <Path path={shape} color="rgba(0,0,0,0.6)"><BlurMask blur={5} style="normal" /></Path>
              </Group>
            </Group>
          );
        })}

        {/* ── The digit tubes: pip, glass, mesh, collar ── */}
        {geo.tubes.map((t, i) => {
          const glass = tubeGlass(t.x, T.glassY, t.w, geo.glassH);
          const cx = t.x + t.w / 2;
          return (
            <Group key={`t${i}`}>
              <Path path={cornerPath(cx - 2, T.pipY, 4, PIP_H, [2, 2], [2, 2], [0, 0], [0, 0])} color="rgba(255,235,210,0.22)" />
              <Group clip={glass}>
                <Rect x={t.x} y={T.glassY} width={t.w} height={geo.glassH} color="#0a0504" />
                <Ellipse cx={cx} cy={T.glassY + geo.glassH * 0.45} rx={t.w * 0.70} ry={geo.glassH * 0.55}
                  colors={['rgba(255,110,30,0.16)', 'rgba(255,90,20,0)']} positions={[0, 0.70]} />
                <Rect x={t.x} y={T.glassY} width={t.w} height={geo.glassH}>
                  <LinearGradient start={vec(t.x, 0)} end={vec(t.x + t.w, 0)}
                    colors={['rgba(255,255,255,0.13)', 'rgba(255,255,255,0.03)', 'rgba(255,255,255,0)', 'rgba(255,255,255,0.04)', 'rgba(255,255,255,0.14)']}
                    positions={[0, 0.22, 0.5, 0.8, 1]} />
                </Rect>
                {/* Floor shade `inset 0 -3 4 rgba(0,0,0,.8)`, inner edge `inset 0 0 0 1 rgba(255,235,210,.10)`. */}
                <Rect x={t.x} y={T.glassY + geo.glassH - 6} width={t.w} height={6}>
                  <LinearGradient start={vec(0, T.glassY + geo.glassH)} end={vec(0, T.glassY + geo.glassH - 6)}
                    colors={['rgba(0,0,0,0.8)', 'rgba(0,0,0,0)']} />
                </Rect>
                <Path path={glass} color="rgba(255,235,210,0.10)" style="stroke" strokeWidth={2} />
              </Group>
              <Rect x={t.x} y={T.collarY - 1} width={t.w} height={1} color="rgba(0,0,0,0.8)" />
              <Path path={cornerPath(t.x, T.collarY, t.w, COLLAR_H, [1, 1], [1, 1], [2, 2], [2, 2])}>
                <LinearGradient start={vec(0, T.collarY)} end={vec(0, T.collarY + COLLAR_H)}
                  colors={['#3a322c', '#1a1511', '#070504']} positions={[0, 0.45, 1]} />
              </Path>
              <Rect x={t.x + 1} y={T.collarY} width={t.w - 2} height={1} color="rgba(255,255,255,0.12)" />
            </Group>
          );
        })}
        <Group clip={glassClip}>
          <Path path={mesh} color="rgba(150,140,125,0.20)" />
        </Group>

        {/* ── The INS-1 bulbs (the decimal point): glass, bare electrode, two leads ── */}
        {geo.bulbs.map((b, i) => {
          const glass = bulbGlass(b.x, B.glassY, b.w, geo.bulbH);
          const cx = b.x + b.w / 2;
          const eY = B.glassY + geo.bulbH * 0.58;
          return (
            <Group key={`b${i}`}>
              <Path path={cornerPath(cx - 1.5, B.pipY, 3, PIP_H, [1.5, 1.5], [1.5, 1.5], [0, 0], [0, 0])} color="rgba(255,235,210,0.22)" />
              <Group clip={glass}>
                <Rect x={b.x} y={B.glassY} width={b.w} height={geo.bulbH} color="#0a0504" />
                <Ellipse cx={cx} cy={eY} rx={b.w * 0.80} ry={geo.bulbH * 0.45}
                  colors={['rgba(255,110,30,0.22)', 'rgba(255,90,20,0)']} positions={[0, 0.75]} />
                <Rect x={b.x} y={B.glassY} width={b.w} height={geo.bulbH}>
                  <LinearGradient start={vec(b.x, 0)} end={vec(b.x + b.w, 0)}
                    colors={['rgba(255,255,255,0.15)', 'rgba(255,255,255,0.03)', 'rgba(255,255,255,0)', 'rgba(255,255,255,0.13)']}
                    positions={[0, 0.28, 0.55, 1]} />
                </Rect>
                <Rect x={cx - 1.4} y={B.glassY + geo.bulbH * 0.62} width={0.6} height={geo.bulbH * 0.38} color="rgba(160,140,120,0.45)" />
                <Rect x={cx + 0.8} y={B.glassY + geo.bulbH * 0.62} width={0.6} height={geo.bulbH * 0.38} color="rgba(160,140,120,0.45)" />
                <Path path={cornerPath(cx - 1.5, eY - 2, 3, 4, [1.35, 1.8], [1.35, 1.8], [1.35, 1.8], [1.35, 1.8])} color="rgba(150,130,110,0.28)" />
                <Rect x={b.x} y={B.glassY + geo.bulbH - 4} width={b.w} height={4}>
                  <LinearGradient start={vec(0, B.glassY + geo.bulbH)} end={vec(0, B.glassY + geo.bulbH - 4)}
                    colors={['rgba(0,0,0,0.8)', 'rgba(0,0,0,0)']} />
                </Rect>
                <Path path={glass} color="rgba(255,235,210,0.12)" style="stroke" strokeWidth={2} />
              </Group>
              <Rect x={b.x - 1} y={B.collarY - 1} width={b.w + 2} height={1} color="rgba(0,0,0,0.8)" />
              <Path path={cornerPath(b.x - 1, B.collarY, b.w + 2, COLLAR_H, [1, 1], [1, 1], [2, 2], [2, 2])}>
                <LinearGradient start={vec(0, B.collarY)} end={vec(0, B.collarY + COLLAR_H)}
                  colors={['#3a322c', '#1a1511', '#070504']} positions={[0, 0.45, 1]} />
              </Path>
              <Rect x={b.x} y={B.collarY} width={b.w} height={1} color="rgba(255,255,255,0.12)" />
            </Group>
          );
        })}
      </Group>
    </Canvas>
  );
});

// ── Layer 2: the cathodes (per tune step) ─────────────────────────────────────

interface Sprite { img: SkImage; w: number; h: number }

/** One glowing digit per 0-9, rasterised once per typeface × size. */
function useDigitSprites(font: SkFont | null, nf: number): Record<string, Sprite> | null {
  // ★ One set per digit size (the typeface is fixed), shared and freed when unused — see useSharedSprite.
  return useSharedSprite(font ? `nixie|${nf}` : null, () => {
    if (!font) return null;
    const out: Record<string, Sprite> = {};
    const gk = Math.max(0.6, Math.min(1, nf / 30));        // the glow shrinks with the tube
    const m = Math.ceil(24 * gk);
    for (const d of '0123456789') {
      const b = font.measureText(d);
      const w = b.width + 2 * m, h = b.height + 2 * m;
      const img = makeSprite(w, h, (c) => {
        const x = m - b.x, y = m - b.y;
        for (const [r, col] of DIGIT_GLOW) c.drawText(d, x, y, glowPaint(col, r * gk), font);
        c.drawText(d, x, y, glowPaint(DIGIT_CORE), font);
      });
      if (img) out[d] = { img, w, h };
    }
    return spriteBuild(out, Object.values(out).map(s => s.img));
  });
}

/** The lit bead with its glow, once. */
function useBeadSprite(): Sprite | null {
  return useSharedSprite('nixie-bead', () => {
    const m = 24, w = 4 + 2 * m, h = 5 + 2 * m;
    const img = makeSprite(w, h, (c) => {
      const oval = Skia.XYWHRect(m, m, 4, 5);
      for (const [r, col] of BEAD_GLOW) c.drawOval(oval, glowPaint(col, r));
      const p = Skia.Paint();
      p.setAntiAlias(true);
      p.setShader(Skia.Shader.MakeRadialGradient(vec(m + 2, m + 5 * 0.42), 3,
        [Skia.Color('#fff1dc'), Skia.Color('#ffbd70'), Skia.Color('#ff6a14')], [0, 0.4, 0.85], TileMode.Clamp));
      c.drawOval(oval, p);
    });
    return spriteBuild(img ? { img, w, h } : null, [img]);
  });
}

const NixieCathodes = React.memo(function NixieCathodes({ w, h, geo, tubes, bulbs, fading }: {
  w: number; h: number; geo: NixieGeometry; tubes: (string | null)[]; bulbs: boolean[];
  /** The digits that went out on the last step (index → digit), drawn at the afterglow for one frame. */
  fading: (string | null)[] | null;
}) {
  const typeface = useTypeface(NIXIE_TTF);
  const nf = Math.round(geo.nf * 2) / 2;
  const font = useMemo(() => (typeface ? Skia.Font(typeface, nf) : null), [typeface, nf]);
  const sprites = useDigitSprites(font, nf);
  const bead = useBeadSprite();
  const centre = useMemo(() => {
    if (!font) return null;
    const c: Record<string, { dx: number; dy: number }> = {};
    for (const d of '0123456789') {
      const b = font.measureText(d);
      c[d] = { dx: -(b.x + b.width / 2), dy: -(b.y + b.height / 2) };
    }
    return c;
  }, [font]);
  if (!font || !centre) return null;
  const T = stackY(geo, geo.glassH);
  const B = stackY(geo, geo.bulbH);

  const wire = (d: string, cx: number, cy: number, color: string, key: string) => {
    const z = cathodeDepth(d);
    return (
      <Group key={key} origin={vec(cx, cy)} transform={[{ translateY: z.dy }, { scale: z.scale }]} opacity={z.opacity}>
        <SkText x={cx + centre[d].dx} y={cy + centre[d].dy} text={d} font={font} color={color} />
      </Group>
    );
  };
  const lit = (d: string, cx: number, cy: number, alpha: number, key: string) => {
    const sp = sprites?.[d];
    const z = cathodeDepth(d);
    if (!sp) return <Group key={key}>{null}</Group>;
    return (
      <Group key={key} origin={vec(cx, cy)} transform={[{ translateY: z.dy }, { scale: z.scale }]} opacity={z.opacity * alpha}>
        <SkImageNode image={sp.img} x={cx - sp.w / 2} y={cy - sp.h / 2} width={sp.w} height={sp.h} />
      </Group>
    );
  };

  return (
    <Canvas style={{ position: 'absolute', left: 0, top: 0, width: w, height: h }} pointerEvents="none">
      {geo.tubes.map((t, i) => {
        const d = tubes[i] ?? null;
        const cx = t.x + t.w / 2, cy = T.glassY + geo.glassH / 2;
        const { back, front } = cathodeNeighbours(d);
        const gone = fading?.[i];
        return (
          <Group key={i} clip={tubeGlass(t.x, T.glassY, t.w, geo.glassH)}>
            {back.map((c, j) => wire(c, cx, cy, WIRE_BACK, `b${j}`))}
            {gone && gone !== d ? lit(gone, cx, cy, AFTERGLOW, 'ag') : null}
            {d !== null ? lit(d, cx, cy, 1, 'lit') : null}
            {front.map((c, j) => wire(c, cx, cy, WIRE_FRONT, `f${j}`))}
          </Group>
        );
      })}
      {bead && geo.bulbs.map((b, i) => bulbs[i] ? (
        <Group key={`bead${i}`} clip={bulbGlass(b.x, B.glassY, b.w, geo.bulbH)}>
          <SkImageNode image={bead.img} x={b.x + b.w / 2 - bead.w / 2} y={B.glassY + geo.bulbH * 0.58 - bead.h / 2}
            width={bead.w} height={bead.h} />
        </Group>
      ) : null)}
    </Canvas>
  );
});

// ── The window ────────────────────────────────────────────────────────────────

export interface NixieTubesProps {
  hz:      number;
  unit:    NixieUnit;
  layout:  NixieLayout;
  design:  TubeDesign;
  /** The bar-meter window (16 pt tubes, 1 pt gaps). */
  bar:     boolean;
  /** UI scale (s.r(1)). */
  scale?:  number;
  /** Left-hand corner radius of the recess (it meets the mode box on the right). */
  radius?: number;
  /** Points kept free at the right of the recess for the unit label (which the caller passes as
   *  children). ★ The tubes are laid out in the window LEFT of it, so kHz / MHz / Hz can never move
   *  a tube (§7 TRAP: anchor the tube group, never centre tubes and label together). */
  reserveRight?: number;
  style?:  StyleProp<ViewStyle>;
  children?: React.ReactNode;
}

/** The natural width a window should ASK for; the tubes narrow into whatever it is given. */
export function nixieNaturalWidth(layout: NixieLayout, design: TubeDesign, bar: boolean, scale = 1): number {
  return nixieGeometry(1e6, 100, nixieSpec(layout), design, { bar, scale }).naturalW;
}

export default function NixieTubes({ hz, unit, layout, design, bar, scale = 1, radius = 5, reserveRight = 0,
  style, children }: NixieTubesProps) {
  const [sz, setSz] = useState({ w: 0, h: 0 });
  const onLayout = (e: LayoutChangeEvent) => {
    const w = Math.round(e.nativeEvent.layout.width), h = Math.round(e.nativeEvent.layout.height);
    setSz(p => (p.w === w && p.h === h ? p : { w, h }));
  };
  /* ★ The MHz-tube count belongs to the RADIO, not the frequency — but a server that tunes above
   *  its declared range (an OpenWebRX on 2 m, say) must never have its top digits cut off. So the
   *  count LATCHES UPWARD, once, for the life of this window; it never shrinks back while tuning. */
  const extra = useRef(0);
  const base = nixieSpec(layout);
  if (layout !== 'fm') extra.current = Math.max(extra.current, mhzDigitsFor(hz) - base.mhzTubes);
  const spec: NixieSpec = useMemo(() => nixieSpec(layout, layout === 'fm' ? undefined : base.mhzTubes + extra.current),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [layout, extra.current]);
  const tubeW = sz.w - reserveRight;
  const geo = useMemo(() => (tubeW > 0 && sz.h > 0 ? nixieGeometry(tubeW, sz.h, spec, design, { bar, scale }) : null),
    [tubeW, sz.h, spec, design, bar, scale]);
  // ★ Memoised: the cathode layer is React.memo, and a fresh array every parent render would defeat it.
  const ro = useMemo(() => nixieReadout(hz, spec, unit), [hz, spec, unit]);
  const key = ro.tubes.map(d => d ?? ' ').join('');

  /* Afterglow: remember what was lit, show what went out at 38 % for ONE frame, then drop it. */
  const [fading, setFading] = useState<(string | null)[] | null>(null);
  const shown = useRef<(string | null)[] | null>(null);
  useEffect(() => {
    const prev = shown.current;
    shown.current = ro.tubes;
    if (!prev || prev.length !== ro.tubes.length) return;
    const gone = prev.map((d, i) => (d !== ro.tubes[i] ? d : null));
    if (!gone.some(Boolean)) return;
    setFading(gone);
    let r2 = 0;
    const r1 = requestAnimationFrame(() => { r2 = requestAnimationFrame(() => setFading(null)); });
    return () => { cancelAnimationFrame(r1); cancelAnimationFrame(r2); };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key]);

  return (
    <View style={style} onLayout={onLayout} pointerEvents="none">
      {geo && <NixieStatic w={sz.w} h={sz.h} radius={radius} geo={geo} />}
      {geo && <NixieCathodes w={sz.w} h={sz.h} geo={geo} tubes={ro.tubes} bulbs={ro.bulbs} fading={fading} />}
      {children}
    </View>
  );
}
