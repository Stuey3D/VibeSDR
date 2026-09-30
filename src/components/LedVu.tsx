/**
 * LedVu — the ten rectangular 1980s LEDs (faceplates brief §4.3; Deck.mockup `segs`, ref
 * all-states/*, 07-squelch.jpg).
 *
 * 5 green / 3 orange / 2 red, `S1 S3 S5 S7 S9 +10 +20 +30 +40 +60` (S9 = the top of the green).
 * ★★★ LED colours never follow either colour setting — an LED is the colour of its die.
 *
 * ★★★ PERFORMANCE (acceptance §13.7: 60 fps on an Xcover 4S with DAB playing):
 *   • The six LED looks (lit / unlit × green, orange, red) — plus the squelch-muted "dim" lit look —
 *     are PRE-RENDERED ONCE at device pixel ratio (glowSprite.ts). A frame blits images; there is
 *     never a live BlurMask per update (§4.3).
 *   • NO REACT RENDER PER METER UPDATE. The bus is read by a subscription that writes shared values;
 *     the per-frame work runs in a Reanimated frame callback on the UI thread and Skia draws straight
 *     from the shared values. React renders this component when its SIZE changes, and that is all.
 *
 * ★★ The squelch RING sits on the segment `ringSegment()` picks from the SAME table the segments
 *   light from (§4.3 TRAP) — green while audio passes, red while the squelch mutes, full strength
 *   either way; no ring when squelch is off (−1).
 */

import React, { useEffect, useMemo } from 'react';
import { Text, View } from 'react-native';
import { useDerivedValue, useFrameCallback, useSharedValue, type SharedValue } from 'react-native-reanimated';
import { scheduleOnRN } from 'react-native-worklets';
import {
  Canvas, ClipOp, Image as SkImageNode, PaintStyle, RoundedRect, Skia, TileMode, type SkCanvas, type SkImage,
} from '@shopify/react-native-skia';
import {
  LED_SPEC, RING_CLOSED, RING_OPEN, VU_LABELS, VU_SEGMENTS, VU_THRESHOLDS, eyeStep, ledColourOf, makeWindow,
  peakStep, pushSample, ringSegment, segmentTarget, sqlClosedOf, vuPos, type LedColourName,
} from '../constants/meters';
import { FONT_HYPER } from '../constants/faceplate';
import { glowPaint, makeSprite } from './glowSprite';
import { useBoxSize } from './VfdParts';
import { useUiScale } from '../hooks/useUiScale';
import { useReduceMotion } from '../hooks/useReduceMotion';
import { useFaceplate } from '../contexts/FaceplateContext';
import type { MeterBus, MeterValues } from './ControlsBar';

/** Glow reach round each LED in its sprite (the 14 pt glow). */
const PAD = 14;
const RADIUS = 1.5;

// ── The sprites ───────────────────────────────────────────────────────────────

type Look = 'lit' | 'dim' | 'unlit';

/** A CSS `radial-gradient(ellipse RX RY at CX CY, …)` over the rect, clipped to the LED. */
function radial(c: SkCanvas, x: number, y: number, w: number, h: number, rx: number, ry: number,
                cx: number, cy: number, colors: string[], pos: number[]) {
  const p = Skia.Paint();
  p.setAntiAlias(true);
  p.setShader(Skia.Shader.MakeRadialGradient({ x: 0, y: 0 }, rx, colors.map(k => Skia.Color(k)), pos, TileMode.Clamp));
  c.save();
  c.clipRRect(Skia.RRectXY(Skia.XYWHRect(x, y, w, h), RADIUS, RADIUS), ClipOp.Intersect, true);
  c.translate(x + cx * w, y + cy * h);
  c.scale(1, ry / rx);
  c.drawRect(Skia.XYWHRect(-rx * 2, -rx * 2, rx * 4, rx * 4), p);
  c.restore();
}

function linearV(c: SkCanvas, x: number, y: number, w: number, h: number, colors: string[], pos: number[]) {
  const p = Skia.Paint();
  p.setAntiAlias(true);
  p.setShader(Skia.Shader.MakeLinearGradient({ x: 0, y }, { x: 0, y: y + h }, colors.map(k => Skia.Color(k)), pos, TileMode.Clamp));
  c.drawRRect(Skia.RRectXY(Skia.XYWHRect(x, y, w, h), RADIUS, RADIUS), p);
}

/** An inset shadow `inset 0 dy blur colour`, approximated as a blurred stroke clipped to the LED. */
function insetShade(c: SkCanvas, x: number, y: number, w: number, h: number, blur: number, dy: number, colour: string) {
  const p = glowPaint(colour, blur);
  p.setStyle(PaintStyle.Stroke);
  p.setStrokeWidth(Math.max(1, blur));
  c.save();
  c.clipRRect(Skia.RRectXY(Skia.XYWHRect(x, y, w, h), RADIUS, RADIUS), ClipOp.Intersect, true);
  c.drawRRect(Skia.RRectXY(Skia.XYWHRect(x, y + dy, w, h), RADIUS, RADIUS), p);
  c.restore();
}

const MATTE_LIT   = ['rgba(255,255,255,0.20)', 'rgba(255,255,255,0.07)', 'rgba(255,255,255,0)', 'rgba(0,0,0,0.10)'];
/** Unlit: "the matte layer at a third" (Deck.mockup: .20 → .09, .07 → .03). */
const MATTE_UNLIT = ['rgba(255,255,255,0.09)', 'rgba(255,255,255,0.03)', 'rgba(255,255,255,0)', 'rgba(0,0,0,0.10)'];
const MATTE_POS   = [0, 0.30, 0.55, 1];

/** One LED look, drawn into a (w + 2·PAD) × (h + 2·PAD) sprite with the LED at (PAD, PAD). */
function drawLed(c: SkCanvas, w: number, h: number, colour: LedColourName, look: Look) {
  const L = LED_SPEC[colour];
  const x = PAD, y = PAD;
  const rr = (dx: number) => Skia.RRectXY(Skia.XYWHRect(x - dx, y - dx, w + 2 * dx, h + 2 * dx), RADIUS + dx, RADIUS + dx);
  if (look === 'lit') {
    // glow 7 + 14 pt (outer first): `0 0 7px glow, 0 0 14px glow@.30`
    c.drawRRect(rr(0), glowPaint(`rgba(${L.glowRgb},0.30)`, 14));
    c.drawRRect(rr(0), glowPaint(L.glow, 7));
  } else if (look === 'dim') {
    // Squelch muting: the glow cut to 4 pt at α .22 (§4.3).
    c.drawRRect(rr(0), glowPaint(`rgba(${L.glowRgb},0.22)`, 4));
  }
  // seat ring `0 0 0 1px rgba(0,0,0,.9)`
  c.drawRRect(rr(1), glowPaint('rgba(0,0,0,0.9)'));
  if (look === 'unlit') {
    // dead tinted plastic: `radial ellipse 60% 65% at 50% 50%: offCentre → offEdge`, matte at a third
    radial(c, x, y, w, h, 0.60 * w, 0.65 * h, 0.5, 0.5, [L.offCentre, L.offEdge], [0, 1]);
    linearV(c, x, y, w, h, MATTE_UNLIT, MATTE_POS);
    insetShade(c, x, y, w, h, 2, 0, 'rgba(0,0,0,0.6)');
    insetShade(c, x, y, w, h, 1, 1, 'rgba(0,0,0,0.5)');
    return;
  }
  // lit: `radial ellipse 78% 60% at 50% 54%: hot 0 → hi 24% → base 60% → dark 100%`, the matte top
  radial(c, x, y, w, h, 0.78 * w, 0.60 * h, 0.5, 0.54, [L.hot, L.hi, L.base, L.dark], [0, 0.24, 0.60, 1]);
  linearV(c, x, y, w, h, MATTE_LIT, MATTE_POS);
  // inner vignette `inset 0 0 2.5px rgba(0,0,0,.45)` and the lower lip `inset 0 -1px 1px rgba(0,0,0,.35)`
  insetShade(c, x, y, w, h, 2.5, 0, 'rgba(0,0,0,0.45)');
  if (look === 'lit') insetShade(c, x, y, w, h, 1, -1, 'rgba(0,0,0,0.35)');
  // muted: a 55 % black overlay over the lit face
  if (look === 'dim') c.drawRRect(rr(0), glowPaint('rgba(0,0,0,0.55)'));
}

type SpriteSet = Record<LedColourName, Record<Look, SkImage | null>>;
const spriteCache = new Map<string, SpriteSet>();

/** The nine looks at this LED size — rasterised ONCE per size per app run. */
function ledSprites(w: number, h: number): SpriteSet {
  const key = `${w.toFixed(2)}x${h.toFixed(2)}`;
  const hit = spriteCache.get(key);
  if (hit) return hit;
  const mk = (col: LedColourName, look: Look) =>
    makeSprite(w + 2 * PAD, h + 2 * PAD, c => drawLed(c, w, h, col, look));
  const set = {} as SpriteSet;
  for (const col of ['green', 'orange', 'red'] as const) {
    set[col] = { lit: mk(col, 'lit'), dim: mk(col, 'dim'), unlit: mk(col, 'unlit') };
  }
  spriteCache.set(key, set);
  return set;
}

// ── The meter ────────────────────────────────────────────────────────────────

/** One segment: the unlit plastic always, the lit (or muted-dim) sprite over it at the segment's
 *  brightness — a cross-fade, so the plastic shows under a dim LED (§4.4). The sprites are
 *  rasterised at device pixel ratio and drawn back at their point size (sw × sh). */
function Segment({ i, x, y, sw, sh, sprites, bright, muting }: {
  i: number; x: number; y: number; sw: number; sh: number; sprites: Record<Look, SkImage | null>;
  bright: SharedValue<number[]>; muting: SharedValue<number>;
}) {
  const litOp = useDerivedValue(() => (muting.value ? 0 : bright.value[i] ?? 0));
  const dimOp = useDerivedValue(() => (muting.value ? bright.value[i] ?? 0 : 0));
  if (!sprites.unlit) return null;
  const box = { x: x - PAD, y: y - PAD, width: sw, height: sh };
  return (<>
    <SkImageNode image={sprites.unlit} {...box} />
    {sprites.lit && <SkImageNode image={sprites.lit} {...box} opacity={litOp} />}
    {sprites.dim && <SkImageNode image={sprites.dim} {...box} opacity={dimOp} />}
  </>);
}

export interface LedVuProps {
  bus?: MeterBus;
  /** The housing's height (the deck decided it, §4.1). */
  height: number;
  /** Shared banner showing: the housing's top padding is 3, not 6. */
  shared: boolean;
  /** ★ Landscape (§9, constants/meters.ts landscapeDeck): the strip's own geometry — LEDs 9, labels
   *  6.5, padding 3 6 2 — and `labelH: 0` for the strip without labels on a small screen. */
  geom?: { padTop: number; padX: number; ledH: number; labelH: number; labelGap: number };
  /** ★★★ The frame callback threw (on the JS thread, once): the housing puts the BAR back
   *  (services/meterGuard.ts) instead of the throw aborting the app. */
  onFault?: (message: string) => void;
}

export default function LedVu({ bus, height, shared, geom, onFault }: LedVuProps) {
  const s = useUiScale();
  const [{ w }, onLayout] = useBoxSize();
  const padX = geom?.padX ?? s.r(7), padTop = geom?.padTop ?? s.r(shared ? 3 : 6), gap = s.r(4);
  const ledH = geom?.ledH ?? s.r(13), labelGap = geom?.labelGap ?? s.r(2), labelH = geom?.labelH ?? s.r(8);
  const ledW = w > 0 ? (w - 2 * padX - (VU_SEGMENTS - 1) * gap) / VU_SEGMENTS : 0;

  const sprites = useMemo(() => (ledW > 1 ? ledSprites(ledW, ledH) : null), [ledW, ledH]);

  // ── The bus → shared values (no React render per update) ──
  /* ★★★ §4.4 STEADY LEDS: the setting, OR the OS's Reduce Motion / Remove animations — on
   *  automatically, because someone who has asked the phone to stop moving things has asked us too. */
  const reduceMotion = useReduceMotion();
  const steady  = useFaceplate().settings.steadyLeds || reduceMotion;
  const steadySv = useSharedValue(steady ? 1 : 0);
  useEffect(() => { steadySv.value = steady ? 1 : 0; }, [steady, steadySv]);
  const muPos   = useSharedValue(0);
  const sigma   = useSharedValue(0);
  const muting  = useSharedValue(0);
  const ring    = useSharedValue(-1);
  const bright  = useSharedValue<number[]>(new Array(VU_SEGMENTS).fill(0));
  const litState = useSharedValue<number[]>(new Array(VU_SEGMENTS).fill(0));
  const peakIdx = useSharedValue(-1);
  const peakAt  = useSharedValue(0);
  const faulted = useSharedValue(0);
  useEffect(() => {
    if (!bus) return;
    // σ: the running std-dev of the RAW level over ~0.5 s (a fading HF signal gets a soft, wide edge;
    // a steady carrier a crisp one). Kept here, on the JS side, at the bus's own 5–25 Hz.
    const win = makeWindow();
    const take = (m: MeterValues) => {
      muPos.value  = vuPos(m.level);
      sigma.value  = pushSample(win, Date.now(), vuPos(m.raw ?? m.level));
      ring.value   = ringSegment(m.sql ?? -1);
      muting.value = sqlClosedOf(m.sql ?? -1, m.gate, m.level) ? 1 : 0;
    };
    take(bus.value);
    bus.subs.add(take);
    return () => { bus.subs.delete(take); };
  }, [bus, muPos, sigma, ring, muting]);

  // ── Per frame, on the UI thread ──
  /* ★★★ NO FLICKER, EVER (§4.4). Every frame draws a STEADY brightness — the fraction of time the
   *  level spends above each threshold, Φ((μ − T)/σ) — never a segment toggled to fake a duty cycle.
   *  The eye filter (τ ≈ 100 ms, ≤ 0.35 per frame) is the ONLY easing: at 5 fps on fading HF the edge
   *  LED glides between updates instead of stepping. Steady LEDs: solid on / off with ~1 dB of
   *  hysteresis, no easing. While the squelch mutes: the plain threshold, dimmed — no σ shimmer. */
  /* ★★★ A THROW HERE IS A NATIVE ABORT — an uncaught error in a UI-thread callback kills the app (the
   *  11 B7 crash: meters.ts WORKLET DEFAULTS). So the frame is caught, stops drawing, and the fault is
   *  handed to the JS thread ONCE, where the housing falls back to the bar. Not a fix for anything —
   *  scripts/test_worklet_defaults.mjs runs this callback as the UI thread does; this is the net. */
  const thresholds = VU_THRESHOLDS as number[];
  useFrameCallback((f) => {
    'worklet';
    if (faulted.value) return;
    try {
      const dt = f.timeSincePreviousFrame ?? 16;
      const mu = muPos.value, sg = sigma.value;
      const st = steadySv.value === 1, mute = muting.value === 1;
      const was = litState.value, prev = bright.value;
      const tgt = new Array(VU_SEGMENTS);
      const lit = new Array(VU_SEGMENTS);
      let top = -1, litChanged = false;
      for (let i = 0; i < VU_SEGMENTS; i++) {
        const t = segmentTarget(i, mu, sg, st, mute, was[i] === 1, thresholds);
        tgt[i] = t;
        lit[i] = t >= 0.5 ? 1 : 0;
        if (lit[i] !== was[i]) litChanged = true;
        if (t >= 0.5) top = i;
      }
      if (litChanged) litState.value = lit;
      // Peak hold: one segment above the level, full brightness, ~1 s (§4.3).
      const ph = { idx: peakIdx.value, at: peakAt.value };
      const pk = peakStep(ph, top, f.timestamp);
      peakIdx.value = ph.idx; peakAt.value = ph.at;
      if (pk >= 0) tgt[pk] = 1;
      let changed = false;
      const next = new Array(VU_SEGMENTS);
      for (let i = 0; i < VU_SEGMENTS; i++) {
        let b = st ? tgt[i] : eyeStep(prev[i], tgt[i], dt);
        if (Math.abs(b - tgt[i]) < 0.002) b = tgt[i];
        next[i] = b;
        if (Math.abs(b - prev[i]) > 0.0005) changed = true;
      }
      if (changed) bright.value = next;
    } catch (e) {
      faulted.value = 1;
      if (onFault) scheduleOnRN(onFault, String((e as Error)?.message ?? e));
    }
  });

  // ── The ring ──
  const ringX = useDerivedValue(() => padX + Math.max(0, ring.value) * (ledW + gap) - 2.75);
  const ringColour = useDerivedValue(() => (muting.value ? RING_CLOSED : RING_OPEN));
  const ringOp = useDerivedValue(() => (ring.value >= 0 ? 1 : 0));

  return (
    <View style={{ height }} onLayout={onLayout}>
      {sprites && (
        <Canvas style={{ position: 'absolute', left: 0, top: 0, width: w, height: padTop + ledH + PAD / 2 }} pointerEvents="none">
          {Array.from({ length: VU_SEGMENTS }, (_, i) => (
            <Segment key={i} i={i} x={padX + i * (ledW + gap)} y={padTop} sprites={sprites[ledColourOf(i)]}
              sw={ledW + 2 * PAD} sh={ledH + 2 * PAD} bright={bright} muting={muting} />
          ))}
          {/* §4.3: a 1.5 pt outline, 2 pt out from the segment. */}
          <RoundedRect x={ringX} y={padTop - 2.75} width={ledW + 5.5} height={ledH + 5.5} r={RADIUS + 2.75}
            color={ringColour} opacity={ringOp} style="stroke" strokeWidth={1.5} />
        </Canvas>
      )}
      {labelH > 0 && <View style={{ position: 'absolute', left: padX, right: padX, top: padTop + ledH + labelGap, height: labelH,
                     flexDirection: 'row', gap }} pointerEvents="none">
        {VU_LABELS.map(l => (
          <Text key={l} style={{ flex: 1, textAlign: 'center', fontFamily: FONT_HYPER, fontSize: labelH,
                                 lineHeight: labelH, fontWeight: '600', letterSpacing: 0.3,
                                 color: 'rgba(255,255,255,0.45)', includeFontPadding: false }}
                numberOfLines={1} adjustsFontSizeToFit>{l}</Text>
        ))}
      </View>}
    </View>
  );
}
