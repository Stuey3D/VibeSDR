/**
 * DrumWheel — physical drum wheel inset into a machined tuning panel.
 *
 * Redesign from the 2026-06-10 design session (preview-widget iterated):
 *
 *   ┌─────────────────────────────────┐  ← outer panel, green LED border glow
 *   │ −   ╲   [icon window]   ╱    + │  ← panel face; trapezoid cut-out, NO top
 *   │      ╲                 ╱       │    edge (outer border serves as the top);
 *   │       ╲_______________╱        │    +/− live in the dead corner triangles
 *   ├────────────▼───────────────────┤  ← drum rim; LED carrier at the V base
 *   │▓▓▓▓▓▓▓▓▓▓▓▓│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│  ← knurled rubber drum, grey notches,
 *   │▓▓▓▓▓▓▓▓▓▓▓▓│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│    red LED beam shines DOWNWARD into the
 *   └─────────────────────────────────┘    drum only — never into the trapezoid
 *
 * NOTE: the final slider-locked parameters from the preview widget were not
 * recoverable from the session transcript — the TUNABLES block below carries
 * the documented design values; adjust there only.
 *
 * Physics unchanged: FRICTION=0.974, MAX_VEL=580, PX_STEP=22, UPDATE_RATE=40.
 * Fixes the previous dp/physical-pixel mismatch — all coordinates are dp.
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { View, Text, StyleSheet, ViewStyle } from 'react-native';
import {
  Canvas,
  Rect,
  RoundedRect,
  Path,
  Line,
  Skia,
  vec,
  BlurMask,
  LinearGradient,
  RadialGradient,
  Group,
} from '@shopify/react-native-skia';
import { Gesture, GestureDetector } from 'react-native-gesture-handler';
import * as Haptics from 'expo-haptics';

// ── Drum haptics (menu ✦ HAPTICS toggle) ──────────────────────────────────────
// Module-level so SDRScreen can flip it without threading a prop through
// ControlsBar's layouts into every drum instance.
let _hapticsOn = true;
export function setDrumHaptics(on: boolean) { _hapticsOn = on; }
/** Same switch, read by the other controls that share the setting (TunerKeys). */
export function getControlHaptics() { return _hapticsOn; }

// ── TUNABLES (preview-widget parameters, 2026-06-10 session) ──────────────────

const DRUM_FRAC   = 0.60;  // drum body fraction of total height
const TRAP_TOP_W  = 0.78;  // trapezoid top width fraction of panel width
const TRAP_BOT_W  = 0.38;  // trapezoid bottom width fraction
const GLOW_HUE    = 120;   // 120 = LED green; ~100 warmer, ~145 colder
const GLOW_INT    = 1.0;   // overall LED burn intensity
const RIDGES      = 4;     // horizontal knurl ridge pairs on the drum
const NEEDLE_HUE  = 4;     // 0–8 = warm orange-red LED
const RIM_H       = 2;     // drum rim highlight height

// ── Physics (locked — v1.5 feel) ───────────────────────────────────────────────

const FRICTION    = 0.974;
const MAX_VEL     = 580;
const MIN_VEL     = 0.8;
const LSV_PX_STEP = 22;
const UPDATE_RATE = 40; // Hz

// ── Colour helpers ─────────────────────────────────────────────────────────────

function hsl(h: number, s: number, l: number, a: number): string {
  return `hsla(${h},${s}%,${l}%,${a})`;
}
const G  = (a: number) => hsl(GLOW_HUE, 100, 45, Math.min(1, a * GLOW_INT));
const RD = (a: number, l = 50) => hsl(NEEDLE_HUE, 95, l, a);

// ── Types ──────────────────────────────────────────────────────────────────────

export type DrumType = 'vfo' | 'zoom';

interface Props {
  type:    DrumType;
  width?:  number;   // 0/omit → onLayout measurement
  height:  number;
  onDelta: (pxDelta: number) => void;
  style?:  ViewStyle;
  fontFamily?: string;
  /** Disable fling inertia — lift = stop. FM-DX shared tuner (coasting past your
   *  target retunes for everyone). Default false keeps the SDR coast. */
  noInertia?: boolean;
}

// ── Component ──────────────────────────────────────────────────────────────────

export default function DrumWheel({
  type, width: widthProp = 0, height, onDelta, style,
  fontFamily = 'Atkinson Hyperlegible', noInertia = false,
}: Props) {
  const [measuredW, setMeasuredW] = useState(widthProp);
  const W = widthProp > 0 ? widthProp : measuredW;
  const H = height;

  const [scroll, setScroll] = useState(0);

  const scrollRef = useRef(0);
  const vel       = useRef(0);
  const lastX     = useRef(0);
  const lastT     = useRef(0);
  const rafId     = useRef<ReturnType<typeof requestAnimationFrame> | null>(null);
  const rafTS     = useRef(0);
  const pending   = useRef(0);
  const lastSend  = useRef(0);
  const touching  = useRef(false);

  // ── Throttled send (UPDATE_RATE=40 → 25ms — UberSDR's confirmed max) ────────
  // Haptic detents: ACCUMULATED distance, one tick per LSV_PX_STEP crossing.
  // The old per-send gate (|dPx| ≥ half a step) meant slow deliberate tuning
  // never ticked while fast drags buzzed at the throttle cap. Now every
  // detent crossing registers regardless of speed; intensity adapts —
  // deliberate speeds get a Rigid mechanical click, flick speeds get the
  // lighter selection tick (a fast spin feels like a freewheeling ratchet,
  // not a buzz), capped at ~35 ticks/s.
  const lastHaptic = useRef(0);
  const hapticAcc  = useRef(0);
  const detentTick = useCallback((dPx: number) => {
    if (!_hapticsOn) { hapticAcc.current = 0; return; }
    hapticAcc.current += dPx;
    if (Math.abs(hapticAcc.current) < LSV_PX_STEP) return;
    // Consume ALL whole crossings (a single fast frame can cross several
    // detents — they collapse into one tick, which is the ratchet feel)
    hapticAcc.current %= LSV_PX_STEP;
    const now  = performance.now();
    const gap  = now - lastHaptic.current;
    if (gap < 28) return;  // ratchet cap
    lastHaptic.current = now;
    if (gap > 90) Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Rigid).catch(() => {});
    else          Haptics.selectionAsync().catch(() => {});
  }, []);

  // Soft landing thunk when a flick finishes coasting
  const settleTick = useCallback(() => {
    if (!_hapticsOn) return;
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Soft).catch(() => {});
  }, []);

  const sendDelta = useCallback((dPx: number) => {
    const now = performance.now();
    if (now - lastSend.current < 1000 / UPDATE_RATE) return;
    lastSend.current = now;
    onDelta(dPx);
    detentTick(dPx);
  }, [onDelta, detentTick]);

  // ── Inertia (unchanged) ──────────────────────────────────────────────────────
  const inertia = useCallback((ts: number) => {
    const dt = Math.min(0.05, (ts - rafTS.current) / 1000);
    rafTS.current = ts;
    const fric = type === 'vfo' ? FRICTION : 0.90;
    vel.current *= Math.pow(fric, dt * 60);
    // Skin-parity backlog brake (vInertia): if ≥1.5 steps of movement are
    // queued unsent, kill the flick — prevents tune-queue saturation; light
    // extra friction above 0.1 steps so the drum settles onto a detent.
    const backlog = Math.abs(pending.current) / LSV_PX_STEP;
    if (backlog >= 1.5) {
      vel.current = 0; pending.current = 0; rafId.current = null;
      setScroll(scrollRef.current);
      return;
    }
    if (backlog > 0.1) vel.current *= Math.pow(Math.max(0.2, 1 - backlog), dt * 60);
    if (Math.abs(vel.current) < Math.max(MIN_VEL, 1)) {
      vel.current = 0;
      // Flush only a meaningful remainder — a dying sub-step flush rounded up
      // to a whole step and knocked the tune off its landing.
      if (Math.abs(pending.current) >= LSV_PX_STEP * 0.6) sendDelta(pending.current);
      pending.current = 0;
      rafId.current = null;
      settleTick();  // soft thunk — the flick has landed
      return;
    }
    const dx = vel.current * dt;
    scrollRef.current -= dx;
    pending.current   += dx;
    if (Math.abs(pending.current) >= LSV_PX_STEP || performance.now() - lastSend.current > 25) {
      sendDelta(pending.current);
      pending.current = 0;
    }
    setScroll(scrollRef.current);
    rafId.current = requestAnimationFrame(inertia);
  }, [type, sendDelta, settleTick]);

  const startInertia = useCallback(() => {
    if (noInertia) { vel.current = 0; return; }   // shared tuner: lift = stop
    // Flick gate: real flicks release at hundreds of px/s; anything under
    // ~50 px/s is deliberate positioning and must NOT coast (MIN_VEL=0.8 let
    // gentle releases tick the tune off the signal).
    if (Math.abs(vel.current) < 50) return;
    vel.current = Math.max(-MAX_VEL, Math.min(MAX_VEL, vel.current));
    if (rafId.current) cancelAnimationFrame(rafId.current);
    rafTS.current = performance.now();
    rafId.current = requestAnimationFrame(inertia);
  }, [inertia, noInertia]);

  // ── Gesture (unchanged) ──────────────────────────────────────────────────────
  const gesture = Gesture.Pan()
    .runOnJS(true)
    .onBegin(e => {
      if (rafId.current) { cancelAnimationFrame(rafId.current); rafId.current = null; }
      touching.current = true;
      vel.current = 0;
      pending.current = 0;
      lastX.current = e.absoluteX;
      lastT.current = performance.now();
    })
    .onUpdate(e => {
      if (!touching.current) return;
      const now = performance.now();
      const dt  = Math.max(8, now - lastT.current);
      const dx  = e.absoluteX - lastX.current;
      scrollRef.current -= dx;
      vel.current = Math.max(-MAX_VEL, Math.min(MAX_VEL, dx / (dt / 1000)));
      pending.current += dx;
      if (Math.abs(pending.current) >= LSV_PX_STEP) {
        sendDelta(pending.current);
        pending.current = 0;
      }
      lastX.current = e.absoluteX;
      lastT.current = now;
      setScroll(scrollRef.current);
    })
    .onEnd(() => {
      touching.current = false;
      // Stale-flick guard: velocity only updates on MOVE events, so "land on
      // a signal, hold still, lift" replayed the pre-stop velocity as inertia
      // and ticked the tune one more step. Held still ⇒ no flick.
      if (performance.now() - lastT.current > 80) vel.current = 0;
      if (pending.current) { sendDelta(pending.current); pending.current = 0; }
      startInertia();
    })
    .onFinalize(() => { touching.current = false; });

  useEffect(() => () => { if (rafId.current) cancelAnimationFrame(rafId.current); }, []);

  // ── Geometry (all dp) ────────────────────────────────────────────────────────
  const cx      = W / 2;
  const drumTop = Math.round(H * (1 - DRUM_FRAC));   // panel face above, drum below
  const drumH   = H - drumTop;
  const trapWT  = W * TRAP_TOP_W;
  const trapWB  = W * TRAP_BOT_W;
  const tx0 = cx - trapWT / 2, tx1 = cx + trapWT / 2;
  const bx0 = cx - trapWB / 2, bx1 = cx + trapWB / 2;

  // Trapezoid window — top edge IS the panel border (not drawn)
  const trapPath = useMemo(() => {
    const p = Skia.Path.Make();
    p.moveTo(tx0, 0); p.lineTo(tx1, 0);
    p.lineTo(bx1, drumTop); p.lineTo(bx0, drumTop);
    p.close();
    return p;
  }, [tx0, tx1, bx0, bx1, drumTop]);

  // Scrolling ticks projected onto a CYLINDER: world arc distance d maps to
  // screen x = cx + R·sin(d/R) with brightness/width ∝ cos(d/R) — ticks
  // compress and roll away at the edges, so motion reads as rotation instead
  // of a sliding strip. Centre spacing equals world spacing (sin′(0)=1), so
  // tuning landings look identical to before.
  const ticks = useMemo(() => {
    if (W <= 0) return [] as Array<{ x: number; major: boolean; med: boolean; fade: number }>;
    const R   = W / 2 - 2;
    const pxs = W > 120 ? 13 : W > 80 ? 11 : W > 55 ? 9 : 7;
    const span = (R * Math.PI) / 2;
    const i0 = Math.floor((scroll - span) / pxs) - 1;
    const i1 = Math.ceil((scroll + span) / pxs) + 1;
    const out: Array<{ x: number; major: boolean; med: boolean; fade: number }> = [];
    for (let i = i0; i <= i1; i++) {
      const d = i * pxs - scroll;
      const a = d / R;
      if (a <= -Math.PI / 2 + 0.05 || a >= Math.PI / 2 - 0.05) continue;
      const x = W / 2 + R * Math.sin(a);
      out.push({ x, major: i % 8 === 0, med: i % 4 === 0, fade: Math.cos(a) });
    }
    return out;
  }, [W, scroll]);

  // Knurl ridge Y positions (pairs: highlight + shadow)
  const ridges = useMemo(() => {
    const out: number[] = [];
    for (let r = 1; r <= RIDGES; r++) out.push(drumTop + (drumH * r) / (RIDGES + 1));
    return out;
  }, [drumTop, drumH]);

  const iconSz   = Math.max(7, Math.round(drumTop * 0.52));
  const iconPath = useMemo(
    () => buildIconPath(type === 'vfo', cx, drumTop * 0.48, iconSz),
    [type, cx, drumTop, iconSz]);

  // LED carrier at the V base

  const pmFontSz = Math.max(10, Math.round(drumTop * 0.51));

  if (W <= 0) {
    return (
      <View style={[{ height }, style]}
            onLayout={e => setMeasuredW(e.nativeEvent.layout.width)} />
    );
  }

  return (
    <GestureDetector gesture={gesture}>
      <View style={[{ height }, style]}
            onLayout={widthProp <= 0 ? e => setMeasuredW(e.nativeEvent.layout.width) : undefined}>
        <Canvas style={StyleSheet.absoluteFill}>

          {/* ── Panel face — machined dark metal, subtle vertical sheen ── */}
          <RoundedRect x={0} y={0} width={W} height={H} r={6}>
            <LinearGradient start={vec(0, 0)} end={vec(0, H)}
              colors={['#101410', '#0a0c0a', '#060706']} positions={[0, 0.4, 1]} />
          </RoundedRect>

          {/* ── Drum body — convex plastic wheel poking out of the panel:
              crown catches the light mid-face, falls away to the seams ── */}
          <Rect x={1} y={drumTop} width={W - 2} height={drumH - 1}>
            <LinearGradient start={vec(0, drumTop)} end={vec(0, H)}
              colors={['#070807', '#191a18', '#232422', '#181917', '#050505']}
              positions={[0, 0.28, 0.50, 0.74, 1]} />
          </Rect>

          {/* Slot shadows — the panel edge occludes the wheel at both seams */}
          <Rect x={1} y={drumTop} width={W - 2} height={Math.max(4, drumH * 0.14)}>
            <LinearGradient start={vec(0, drumTop)} end={vec(0, drumTop + Math.max(4, drumH * 0.14))}
              colors={['rgba(0,0,0,0.62)', 'rgba(0,0,0,0)']} />
          </Rect>
          <Rect x={1} y={H - 1 - Math.max(4, drumH * 0.16)} width={W - 2} height={Math.max(4, drumH * 0.16)}>
            <LinearGradient start={vec(0, H - 1 - Math.max(4, drumH * 0.16))} end={vec(0, H - 1)}
              colors={['rgba(0,0,0,0)', 'rgba(0,0,0,0.58)']} />
          </Rect>

          {/* Green backlight seeping through the panel/wheel gaps */}
          <Line p1={vec(3, drumTop + 0.5)} p2={vec(W - 3, drumTop + 0.5)}
                color={G(0.30)} strokeWidth={1.4}>
            <BlurMask blur={4} style="normal" respectCTM />
          </Line>
          <Line p1={vec(3, H - 1.5)} p2={vec(W - 3, H - 1.5)}
                color={G(0.20)} strokeWidth={1.2}>
            <BlurMask blur={4} style="normal" respectCTM />
          </Line>

          {/* Drum rim — caught light along the cylinder's top edge */}
          <Rect x={1} y={drumTop} width={W - 2} height={RIM_H}
                color="rgba(180,185,175,0.14)" />

          {/* Knurl ridges — highlight/shadow pairs suggest the grip texture */}
          {ridges.map((y, i) => (
            <Group key={`rg${i}`}>
              <Line p1={vec(2, y)} p2={vec(W - 2, y)}
                    color="rgba(0,0,0,0.45)" strokeWidth={1.2} />
              <Line p1={vec(2, y + 1.2)} p2={vec(W - 2, y + 1.2)}
                    color="rgba(160,160,150,0.10)" strokeWidth={0.8} />
            </Group>
          ))}

          {/* Engraved notches — cosine-faded with the curvature; each line
              carries a shadow pair so the cuts read as depth, not paint */}
          <Group clip={Skia.XYWHRect(1, drumTop + RIM_H, W - 2, drumH - RIM_H - 1)}>
            {ticks.map((t, i) => {
              const base = t.major ? 0.55 : t.med ? 0.36 : 0.22;
              const a    = base * (0.15 + 0.85 * t.fade);
              const sw   = (t.major ? 1.5 : 0.8) * (0.5 + 0.5 * t.fade);
              return (
                <Group key={i}>
                  {(t.major || t.med) && (
                    <Line
                      p1={vec(t.x + 0.9, drumTop + RIM_H + 2)} p2={vec(t.x + 0.9, H - 3)}
                      color={`rgba(0,0,0,${(0.5 * t.fade).toFixed(3)})`}
                      strokeWidth={sw} />
                  )}
                  <Line
                    p1={vec(t.x, drumTop + RIM_H + 2)} p2={vec(t.x, H - 3)}
                    color={`rgba(168,166,158,${a.toFixed(3)})`}
                    strokeWidth={sw} />
                </Group>
              );
            })}
          </Group>

          {/* Specular sheen — studio light caught across the curvature */}
          <Rect x={1} y={drumTop + drumH * 0.16} width={W - 2} height={drumH * 0.26}>
            <LinearGradient
              start={vec(0, drumTop + drumH * 0.16)}
              end={vec(0, drumTop + drumH * 0.42)}
              colors={['rgba(255,255,255,0)', 'rgba(255,255,255,0.07)', 'rgba(255,255,255,0)']}
              positions={[0, 0.45, 1]} />
          </Rect>

          {/* Drum side shading — cylindrical falloff at the edges */}
          <Rect x={1} y={drumTop} width={W * 0.12} height={drumH - 1}>
            <LinearGradient start={vec(0, 0)} end={vec(W * 0.12, 0)}
              colors={['rgba(0,0,0,0.55)', 'rgba(0,0,0,0)']} />
          </Rect>
          <Rect x={W - 1 - W * 0.12} y={drumTop} width={W * 0.12} height={drumH - 1}>
            <LinearGradient start={vec(W - 1, 0)} end={vec(W - 1 - W * 0.12, 0)}
              colors={['rgba(0,0,0,0.55)', 'rgba(0,0,0,0)']} />
          </Rect>

          {/* ── Trapezoid window — darker inset, green-lit from within ── */}
          <Path path={trapPath} color="rgba(3,4,3,0.96)" />
          <Path path={trapPath}>
            <RadialGradient c={vec(cx, drumTop * 0.55)} r={trapWT * 0.55}
              colors={[G(0.16), G(0.05), 'rgba(0,0,0,0)']}
              positions={[0, 0.55, 1]} />
          </Path>

          {/* Trapezoid edges — left/right/bottom only (NO top edge) */}
          {[
            [tx0, 0, bx0, drumTop], [tx1, 0, bx1, drumTop], [bx0, drumTop, bx1, drumTop],
          ].map(([x0, y0, x1, y1], i) => (
            <Group key={`te${i}`}>
              <Line p1={vec(x0, y0)} p2={vec(x1, y1)} color={G(0.30)} strokeWidth={3}>
                <BlurMask blur={3} style="normal" respectCTM />
              </Line>
              <Line p1={vec(x0, y0)} p2={vec(x1, y1)} color={G(0.60)} strokeWidth={0.9} />
            </Group>
          ))}

          {/* Icon — green LED: glow BEHIND a crisp stroke (BlurMask on the
              stroke itself smudged the icons — acrylic rule applies) */}
          <Path path={iconPath} color={G(0.45)} strokeWidth={2.6} style="stroke"
                strokeCap="round" strokeJoin="round">
            <BlurMask blur={3} style="normal" respectCTM />
          </Path>
          <Path path={iconPath} color={G(0.95)} strokeWidth={1.1} style="stroke"
                strokeCap="round" strokeJoin="round" />

          {/* (LED carrier housing removed per design review — the beam
              emerges straight from the V base.) */}
          {/* Deep-red LED beam (no dot at the carrier — per design brief):
              a soft pool of red light on the wheel surface + glow layers +
              razor filament */}
          <Group clip={Skia.XYWHRect(1, drumTop, W - 2, drumH - 1)}>
            <Rect x={cx - W * 0.16} y={drumTop} width={W * 0.32} height={drumH - 1}>
              <RadialGradient c={vec(cx, drumTop + drumH * 0.30)} r={W * 0.16}
                colors={[RD(0.16, 42), RD(0.05, 40), 'rgba(0,0,0,0)']}
                positions={[0, 0.55, 1]} />
            </Rect>
            <Line p1={vec(cx, drumTop)} p2={vec(cx, H - 1)}
                  color={RD(0.14, 40)} strokeWidth={9}>
              <BlurMask blur={6} style="normal" respectCTM />
            </Line>
            <Line p1={vec(cx, drumTop)} p2={vec(cx, H - 1)}
                  color={RD(0.50, 44)} strokeWidth={2.6}>
              <BlurMask blur={3} style="normal" respectCTM />
            </Line>
            {/* Crisp deep-red filament — glow BEHIND a razor line */}
            <Line p1={vec(cx, drumTop)} p2={vec(cx, H - 1)}
                  color={RD(1, 52)} strokeWidth={0.9} />
          </Group>

          {/* ── Outer panel border — green LED glow + solid ── */}
          <RoundedRect x={1} y={1} width={W - 2} height={H - 2} r={6}
                       color={G(0.10)} strokeWidth={5} style="stroke">
            <BlurMask blur={6} style="normal" respectCTM />
          </RoundedRect>
          <RoundedRect x={0.5} y={0.5} width={W - 1} height={H - 1} r={6}
                       color={G(0.70)} strokeWidth={0.9} style="stroke" />
        </Canvas>

        {/* ── +/− in the dead corner triangles flanking the V ── */}
        <View pointerEvents="none"
              style={[StyleSheet.absoluteFill, {
                flexDirection: 'row', justifyContent: 'space-between',
                paddingHorizontal: Math.max(3, W * 0.05),
              }]}>
          <Text style={{
            color: G(0.70), fontSize: pmFontSz, fontFamily,
            lineHeight: drumTop, includeFontPadding: false,
          }}>−</Text>
          <Text style={{
            color: G(0.70), fontSize: pmFontSz, fontFamily,
            lineHeight: drumTop, includeFontPadding: false,
          }}>+</Text>
        </View>
      </View>
    </GestureDetector>
  );
}

// ── Icon path builders (unchanged) ─────────────────────────────────────────────

/**
 * The two panel legends: the radio glyph (tune) and the magnifier (zoom).
 * Exported so TunerKeys labels its pairs with the SAME marks the drums use —
 * two hand-drawn copies would drift apart the first time either is adjusted.
 */
export function buildGlyphPath(isTune: boolean, cx: number, cy: number, sz: number) {
  return buildIconPath(isTune, cx, cy, sz);
}

function buildIconPath(isTune: boolean, cx: number, cy: number, sz: number) {
  const p = Skia.Path.Make();
  const s = sz / 14;
  const ox = cx - 7 * s;
  const oy = cy - 7 * s;
  if (isTune) {
    p.moveTo(ox + 9 * s, oy + 1 * s);
    p.lineTo(ox + 11.5 * s, oy + 4.5 * s);
    p.addRRect({
      rect: { x: ox + 1.5 * s, y: oy + 4.5 * s, width: 11 * s, height: 8 * s },
      rx: 1.2 * s, ry: 1.2 * s,
    });
    p.addCircle(ox + 4.5 * s, oy + 9 * s, 2 * s);
  } else {
    p.addCircle(ox + 6 * s, oy + 6 * s, 4 * s);
    p.moveTo(ox + 9.2 * s, oy + 9.2 * s);
    p.lineTo(ox + 13 * s, oy + 13 * s);
    p.moveTo(ox + 3.5 * s, oy + 6 * s);
    p.lineTo(ox + 8.5 * s, oy + 6 * s);
  }
  return p;
}
