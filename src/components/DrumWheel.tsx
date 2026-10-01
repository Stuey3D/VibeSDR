/**
 * DrumWheel — physical drum wheel inset into a machined tuning panel.
 *
 * Redesign from the 2026-06-10 design session (preview-widget iterated):
 *
 *   ┌─────────────────────────────────┐  ← outer panel, green LED border glow
 *   │ −   ╲   [icon window]   ╱    + │  ← panel face; trapezoid cut-out, NO top
 *   │      ╲                 ╱       │    edge (outer border serves as the top);
 *   │       ╲_______________╱        │    +/− live in the dead corner triangles
 *   ├─────────────────────────────────┤  ← drum rim
 *   │▓▓▓▓▓▓▓▓▓▓▓▓░░░▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│  ← knurled drum, notches; the controls-colour
 *   │▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│    LED glows THROUGH it from behind (§6.1 —
 *   └─────────────────────────────────┘    no index needle any more)
 *
 * ★★ FACEPLATES §6.1 — PORTED, NOT REDRAWN. The layering below is today's; the chassis tokens
 *   (constants/faceplate.ts) parameterise the face, drum, ridges and notches, and components/
 *   DrumWell.tsx draws the face, the edge and the LED pool that DrumWheel shares with TunerKeys.
 *   The default chassis is today's drum exactly, EXCEPT the red index needle — removed on every
 *   chassis by the brief.
 * ★★ THREE CANVASES, and the split is the performance work (§3.4, Xcover 4S): the face + drum body
 *   below, the rolling notches in the middle, the sheen / trapezoid / icon / edge above. Only the
 *   middle one repaints while the drum turns; the blurs (seams, trapezoid edges, icon, border glow)
 *   used to be re-run on every frame of every drag because they shared its canvas. Stacked canvases
 *   composite exactly as the layers did in one, so the picture is unchanged.
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
  Mask,
  type SkPath,
} from '@shopify/react-native-skia';
import { useSharedValue, useAnimatedReaction } from 'react-native-reanimated';
import { Gesture, GestureDetector } from 'react-native-gesture-handler';
import * as Haptics from 'expo-haptics';
import { useFaceplate } from '../contexts/FaceplateContext';
import { ledA } from '../constants/faceplate';
import { notchOrder, wellOutset } from '../constants/drumWell';
import { WellFace, WellEdge, DrumPool } from './DrumWell';
import { getControlHaptics } from './controlHaptics';

// ── Drum haptics (menu ✦ HAPTICS toggle) ──────────────────────────────────────
// ★ The switch lives in controlHaptics.ts now (DomeKey needs it, and DrumWheel draws with DomeKey's
//   texture — an import cycle otherwise). Re-exported so every existing caller is unchanged.
export { setDrumHaptics, getControlHaptics } from './controlHaptics';

// ── TUNABLES (preview-widget parameters, 2026-06-10 session) ──────────────────

const DRUM_FRAC   = 0.60;  // drum body fraction of total height
const TRAP_TOP_W  = 0.78;  // trapezoid top width fraction of panel width
const TRAP_BOT_W  = 0.38;  // trapezoid bottom width fraction
// ★ The LED hue and the needle hue are gone from here: colour is the faceplate's (§6.1). The
//   controls colour lights the well; the chassis tokens carry the face, drum and notches.
const RIDGES      = 4;     // horizontal knurl ridge pairs on the drum
const RIM_H       = 2;     // drum rim highlight height

// ── Physics (locked — v1.5 feel) ───────────────────────────────────────────────

const FRICTION    = 0.974;
const MAX_VEL     = 580;
const MIN_VEL     = 0.8;
const LSV_PX_STEP = 22;
const UPDATE_RATE = 40; // Hz

// ── Colour ─────────────────────────────────────────────────────────────────────
// ★★ §6.1 TRAP: this was `G = hsl(GLOW_HUE, 100, 45)`, which cannot make white or neon at all and
//   made blue and amber at the wrong brightness. The well now takes the controls colour's RGB
//   triplet from the faceplate (ledA), and the default chassis's green IS that same hsl, so today's
//   drum is unchanged.

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
  const fp = useFaceplate();
  const ct = fp.chassis;
  const G  = (a: number) => ledA(fp.controls, a);

  /** ★★★ THE DRUM'S POSITION IS A SHARED VALUE NOW, NOT REACT STATE.
   *  It was `useState`, written on every rAF tick of a coast and on every gesture event of a drag
   *  — so each one rebuilt the tick array and reconciled a <Group> with two <Line>s PER TICK,
   *  about 100 Skia elements, on the JS thread. The notches are now four PATHS built in worklets
   *  from this value, so a drag or a flick does no React work whatsoever.
   *  ★ scrollRef stays: the JS side still needs the position for sendDelta, the detents and the
   *  backlog brake. The two are written together, never separately. */
  const scrollSv  = useSharedValue(0);
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
    if (!getControlHaptics()) { hapticAcc.current = 0; return; }
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
    if (!getControlHaptics()) return;
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Soft).catch(() => {});
  }, []);

  const sendDelta = useCallback((dPx: number) => {
    const now = performance.now();
    if (now - lastSend.current < 1000 / UPDATE_RATE) return;
    lastSend.current = now;
    onDelta(dPx);
    detentTick(dPx);
  }, [onDelta, detentTick]);

  // ── Inertia ──────────────────────────────────────────────────────────────────
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
      scrollSv.value = scrollRef.current;
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
      // ★ PAINT THE LANDING. This branch used to rely on the PREVIOUS frame's write being current;
      //   with the coast repaint coalesced to ~30 fps that could leave the drum resting up to a
      //   frame behind where it actually stopped. The settle must always show its true position.
      scrollSv.value = scrollRef.current;
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
    // ★ NO THROTTLE ANY MORE. Publishing the position is a shared-value write — the notches
    //   redraw on the UI thread from it — so there is nothing left worth coalescing. The 30 fps
    //   cap existed only because this used to be a React render.
    scrollSv.value = scrollRef.current;
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
      scrollSv.value = scrollRef.current;
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
  // ── The notches, built on the UI thread ─────────────────────────────────────
  // ★★★ FOUR PATHS, NOT ~50 ELEMENTS. Every notch is the same vertical line differing only in x,
  // alpha and width, and the ONLY reason they were separate elements is that alpha and width
  // varied per tick. Both varied with `fade`, and fade is cos(a) where x = W/2 + R·sin(a) — so it
  // is a pure function of the tick's POSITION, not of the tick. That means it can come from a
  // positional MASK over uniform paths instead of from per-element colour, which is what collapses
  // fifty React children into four Skia paths that never re-render.
  // ★ What is lost: the width taper (0.5..1x with fade). What is kept: the fade itself, and
  // CONTINUOUSLY rather than in the bands a bucketed conversion would have given. At the edges,
  // where the taper mattered, the mask has the notches down to 15% anyway.
  // ★★ Double-buffered, exactly as the spectrum trace is and for the same reason: an SkPath holds
  // native memory Hermes cannot see, so building a fresh one per frame in a worklet would leak.
  // Two per class, reset() and rebuilt in place, handed over alternately so the identity still
  // changes and Skia repaints.
  const tickPx = W > 120 ? 13 : W > 80 ? 11 : W > 55 ? 9 : 7;
  const tickR  = W / 2 - 2;
  const bufs = useMemo(() => Array.from({ length: 8 }, () => Skia.Path.Make()), []);
  useEffect(() => () => { const b = bufs; setTimeout(() => { for (const p of b) { try { p.dispose(); } catch {} } }, 300); }, [bufs]);

  /** kind: 0 = minor, 1 = med, 2 = major, 3 = the shadow pair (major+med only). */
  const buildTicks = (kind: number, path: SkPath, scroll: number, w: number, R: number, pxs: number,
                      y0: number, y1: number) => {
    'worklet';
    path.reset();
    if (w <= 0 || R <= 0) return path;
    const span = (R * Math.PI) / 2;
    const i0 = Math.floor((scroll - span) / pxs) - 1;
    const i1 = Math.ceil((scroll + span) / pxs) + 1;
    const lim = Math.PI / 2 - 0.05;
    for (let i = i0; i <= i1; i++) {
      const a = (i * pxs - scroll) / R;
      if (a <= -lim || a >= lim) continue;
      const major = i % 8 === 0;
      const med   = i % 4 === 0;
      // Each class draws ONLY its own notches, so the three can carry different alpha and width.
      if (kind === 0 && (major || med)) continue;
      if (kind === 1 && (major || !med)) continue;
      if (kind === 2 && !major) continue;
      if (kind === 3 && !(major || med)) continue;
      const x = w / 2 + R * Math.sin(a) + (kind === 3 ? 0.9 : 0);
      path.moveTo(x, y0);
      path.lineTo(x, y1);
    }
    return path;
  };

  const y0 = drumTop + RIM_H + 2;
  const y1 = H - 3;
  // ★★★ THE TICKS ARE REBUILT ONLY WHEN THE DRUM MOVES (B8 power audit, emulator: each drum redrew ~80
  //     times a second FOREVER — in the background and with the screen off, the whole of the app's
  //     background CPU). The old builders were four useDerivedValues that each did `f.value ^= 1` on a
  //     shared value they also READ: a derived value subscribes to what it reads, so its own write
  //     marked it dirty again and it re-ran — and asked Skia to redraw — every frame, unmoved.
  //  ★ A reaction subscribes ONLY to its first function (the scroll); its body may read and write
  //     the flip freely. One reaction rebuilds all four paths in a fixed order, so the old worry —
  //     derived values running in an unspecified order around a shared flip — cannot arise either.
  //  ★ Double-buffered as before: each path alternates between two Skia paths, so the one on screen
  //     is never the one being rewritten, and the new object is what tells Skia to redraw.
  // ★ Hooks written out in a fixed order (no helper calls a hook — one "rendered more hooks" crash
  //   already came from that).
  const flip = useSharedValue(0);
  const pathMinor  = useSharedValue<SkPath>(bufs[0]);
  const pathMed    = useSharedValue<SkPath>(bufs[2]);
  const pathMajor  = useSharedValue<SkPath>(bufs[4]);
  const pathShadow = useSharedValue<SkPath>(bufs[6]);
  useAnimatedReaction(() => scrollSv.value, (scroll) => {
    'worklet';
    const f = flip.value ^ 1;
    flip.value = f;
    pathMinor.value  = buildTicks(0, bufs[0 + f], scroll, W, tickR, tickPx, y0, y1);
    pathMed.value    = buildTicks(1, bufs[2 + f], scroll, W, tickR, tickPx, y0, y1);
    pathMajor.value  = buildTicks(2, bufs[4 + f], scroll, W, tickR, tickPx, y0, y1);
    pathShadow.value = buildTicks(3, bufs[6 + f], scroll, W, tickR, tickPx, y0, y1);
  }, [W, tickR, tickPx, y0, y1]);

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

  const pmFontSz = Math.max(10, Math.round(drumTop * 0.51));

  if (W <= 0) {
    return (
      <View style={[{ height }, style]}
            onLayout={e => setMeasuredW(e.nativeEvent.layout.width)} />
    );
  }

  // The edge canvas is M larger than the well on each side: the metal ring and glow sit OUTSIDE it.
  const M = wellOutset(ct);
  const notchPaths = {
    pair:  <Path key="pair"  path={pathShadow} style="stroke" strokeWidth={1.1} color={ct.notchPair} />,
    minor: <Path key="minor" path={pathMinor}  style="stroke" strokeWidth={0.8} color={ct.notchMinor} />,
    med:   <Path key="med"   path={pathMed}    style="stroke" strokeWidth={0.8} color={ct.notchMed} />,
    major: <Path key="major" path={pathMajor}  style="stroke" strokeWidth={1.5} color={ct.notchMajor} />,
  };

  return (
    <GestureDetector gesture={gesture}>
      <View style={[{ height }, style]}
            onLayout={widthProp <= 0 ? e => setMeasuredW(e.nativeEvent.layout.width) : undefined}>
        {/* ════ 1. BELOW THE NOTCHES — static ════ */}
        <Canvas style={StyleSheet.absoluteFill} pointerEvents="none">

          {/* ── Panel face — today's machined dark metal, or the chassis's brushed grain ── */}
          <WellFace W={W} H={H} ct={ct} />

          {/* ── Drum body — convex wheel poking out of the panel:
              crown catches the light mid-face, falls away to the seams ── */}
          <Rect x={1} y={drumTop} width={W - 2} height={drumH - 1}>
            <LinearGradient start={vec(0, drumTop)} end={vec(0, H)}
              colors={ct.drumBody}
              positions={ct.drumPos} />
          </Rect>

          {/* Slot shadows — the panel edge occludes the wheel at both seams */}
          <Rect x={1} y={drumTop} width={W - 2} height={Math.max(4, drumH * 0.14)}>
            <LinearGradient start={vec(0, drumTop)} end={vec(0, drumTop + Math.max(4, drumH * 0.14))}
              colors={ct.drumShadeTop} />
          </Rect>
          <Rect x={1} y={H - 1 - Math.max(4, drumH * 0.16)} width={W - 2} height={Math.max(4, drumH * 0.16)}>
            <LinearGradient start={vec(0, H - 1 - Math.max(4, drumH * 0.16))} end={vec(0, H - 1)}
              colors={ct.drumShadeBot} />
          </Rect>

          {/* Backlight seeping through the panel/wheel gaps, in the controls colour */}
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
                color={ct.rimLine} />

          {/* Knurl ridges — highlight/shadow pairs suggest the grip texture */}
          {ridges.map((y, i) => (
            <Group key={`rg${i}`}>
              <Line p1={vec(2, y)} p2={vec(W - 2, y)}
                    color={ct.ridgeShadow} strokeWidth={1.2} />
              <Line p1={vec(2, y + 1.2)} p2={vec(W - 2, y + 1.2)}
                    color={ct.ridgeHighlight} strokeWidth={0.8} />
            </Group>
          ))}
        </Canvas>

        {/* ════ 2. THE ROLLING NOTCHES — the only canvas that repaints while the drum turns ════ */}
        <Canvas style={StyleSheet.absoluteFill} pointerEvents="none">
          {/* Engraved notches — cosine-faded with the curvature; each line
              carries a pair so the cuts read as depth, not paint */}
          <Group clip={Skia.XYWHRect(1, drumTop + RIM_H, W - 2, drumH - RIM_H - 1)}>
            {/* ★★ THE CURVATURE FADE IS A MASK, not per-notch alpha — see buildTicks. The stops
                sample 0.15 + 0.85·cos(asin(u)) across the drum, which is exactly the falloff the
                per-tick version computed, so the shading is unchanged and now continuous rather
                than quantised by however many elements happened to be on screen.
                ★ luminance: white = full strength, grey = faded, and the ends never reach black
                because the original never faded below 0.15 either. */}
            <Mask mode="luminance" mask={
              <Rect x={1} y={drumTop + RIM_H} width={W - 2} height={drumH - RIM_H - 1}>
                <LinearGradient
                  start={vec(1, 0)} end={vec(W - 1, 0)}
                  positions={[0, 0.08, 0.2, 0.35, 0.5, 0.65, 0.8, 0.92, 1]}
                  colors={ct.glint} />
              </Rect>
            }>
              {/* ★ §6.1 TRAP: the ORDER is a chassis token. On rubber the pair is the shadow and
                  must sit UNDER the light notch; on aluminium the notch is the dark cut and the
                  pair its white highlight — drawn over it, the highlight would paint the cut out. */}
              {notchOrder(ct).map(k => notchPaths[k])}
            </Mask>
          </Group>
        </Canvas>

        {/* ════ 3. ABOVE THE NOTCHES — static; M larger than the well for the metal glow ════ */}
        <Canvas pointerEvents="none"
                style={{ position: 'absolute', left: -M, top: -M, width: W + 2 * M, height: H + 2 * M }}>
          <Group transform={[{ translateX: M }, { translateY: M }]}>
            {/* Specular sheen — studio light caught across the curvature */}
            <Rect x={1} y={drumTop + drumH * 0.16} width={W - 2} height={drumH * 0.26}>
              <LinearGradient
                start={vec(0, drumTop + drumH * 0.16)}
                end={vec(0, drumTop + drumH * 0.42)}
                colors={ct.sheen}
                positions={[0, 0.45, 1]} />
            </Rect>

            {/* Drum side shading — cylindrical falloff at the edges */}
            <Rect x={1} y={drumTop} width={W * 0.12} height={drumH - 1}>
              <LinearGradient start={vec(0, 0)} end={vec(W * 0.12, 0)}
                colors={ct.sideShade} />
            </Rect>
            <Rect x={W - 1 - W * 0.12} y={drumTop} width={W * 0.12} height={drumH - 1}>
              <LinearGradient start={vec(W - 1, 0)} end={vec(W - 1 - W * 0.12, 0)}
                colors={ct.sideShade} />
            </Rect>

            {/* ★★ §6.1: NO INDEX NEEDLE, on any chassis. The LED behind the drum glows through it
                instead — a soft pool in the controls colour, high on the wheel. */}
            <DrumPool x={1} y={drumTop} w={W - 2} h={drumH - 1} led={fp.controls} />

            {/* ── Trapezoid window — darker inset, lit from within ── */}
            <Path path={trapPath} color={ct.trapFill} />
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

            {/* Icon — the controls-colour LED: glow BEHIND a crisp stroke (BlurMask on the
                stroke itself smudged the icons — acrylic rule applies) */}
            <Path path={iconPath} color={G(0.45)} strokeWidth={2.6} style="stroke"
                  strokeCap="round" strokeJoin="round">
              <BlurMask blur={3} style="normal" respectCTM />
            </Path>
            <Path path={iconPath} color={G(0.95)} strokeWidth={1.1} style="stroke"
                  strokeCap="round" strokeJoin="round" />
          </Group>

          {/* ── The well's edge — today's lit border, or the metal gap + ring + glow ── */}
          <WellEdge W={W} H={H} M={M} ct={ct} led={fp.controls} />
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
