/**
 * EdgeMeter — the analogue EDGEWISE meter (faceplates brief §4.5; Deck.mockup `edge`; refs
 * all-states/E1…E4, annotated/A3-analogue-meter.jpg).
 *
 * The slim moving-coil meter from cassette decks: the scale printed on a curved drum, lit from
 * behind by two old bulbs, the needle moving sideways. It takes the LED strip's space (34 pt housing,
 * 28 pt window). No colour setting touches it, on any chassis.
 *
 * ★★ THREE LAYERS, and the split is the performance design:
 *   1. the CARD — lamp lighting and the printed scale — react-native-svg, drawn once per size;
 *   2. the HANDS — the only thing that moves — one Skia canvas driven by shared values on the UI
 *      thread (no React render per meter update); their soft shadows are pre-rendered sprites, so a
 *      moving needle never runs a live blur;
 *   3. the GLASS — drum curvature, side falloff, the streak, the squelch dim — in that same canvas,
 *      over the hands, as the mockup stacks it.
 * ★★ THE INCANDESCENT LIGHTING IS UNEVEN ON PURPOSE (§4.5 TRAP): left bulb brighter, right weaker,
 *   a dip between, dark corners. Do not even it out — the unevenness is the style.
 *
 * Hands (§4.5):
 *   • signal needle — rises from the bottom with an 11 × 7 arrowhead; springs to the RAW level on the
 *     UI thread (withSpring, 99 % in 300 ms, ~1 % overshoot; no overshoot under Reduce Motion — but
 *     it still MOVES: the needles are the reading). ★ TRAP: raw, not the smoothed level — smoothing
 *     and then springing doubles the lag.
 *   • peak-decay needle — translucent, no arrowhead, PUSHED by the signal needle's ON-SCREEN position
 *     in the same UI-thread frame, holds ~1 s, then drifts down ~6 dB/s easing in.
 *   • red squelch hand — hangs from the top at the threshold, on the SAME table as the needles.
 * Squelch closed: the lamp dims 50 %, the needles fall (the peak at its slow rate), the red hand
 * stays, and the mode box says SQL (§4.6).
 */

import React, { useEffect, useMemo } from 'react';
import { View } from 'react-native';
import Svg, { Defs, G, Line, LinearGradient as SvgLinear, RadialGradient as SvgRadial, Rect as SvgRect,
  Stop, Text as SvgText } from 'react-native-svg';
import {
  ReduceMotion, useDerivedValue, useFrameCallback, useSharedValue, withSpring, withTiming,
} from 'react-native-reanimated';
import { scheduleOnRN } from 'react-native-worklets';
import {
  Canvas, Group, Image as SkImageNode, LinearGradient, Path, Rect, Skia, vec, type SkImage,
} from '@shopify/react-native-skia';
import {
  VU_LABELS, VU_SEGMENTS, needleSpring, needleX, peakNeedleStep, scalePointX, sqlClosedOf, vuPos,
} from '../constants/meters';
import { FONT_HYPER } from '../constants/faceplate';
import { glowPaint, makeSprite } from './glowSprite';
import { useBoxSize } from './VfdParts';
import { useReduceMotion } from '../hooks/useReduceMotion';
import type { MeterBus, MeterValues } from './ControlsBar';

const CARD  = '#e9e2cf';
const PRINT = '#16120d';
const RED_PRINT = '#b8160c';
const RED_BAND  = '#c21a0e';
const SQL_HAND  = '#d0140a';
/** Where the red zone begins: +30 (§4.5 "small red zone +30 to +60"). */
const RED_FROM = VU_LABELS.indexOf('+30');

// ── 1. The card ───────────────────────────────────────────────────────────────

/** The lamp and the printed scale — static; react-native-svg draws it once per size. */
const Card = React.memo(function Card({ w, h, printH = h, printTop = 0 }: {
  w: number; h: number; printH?: number; printTop?: number;
}) {
  const U = (w - 16) / VU_SEGMENTS;
  // The print is designed on a 28 pt window. ★ Landscape (§9, Deck.mockup `svgTop: -2px`) keeps the
  // 28 pt print and shows it 2 pt up in a 24 pt window: printH / printTop say so, scaled with it.
  const k = printH / 28;
  const y = (v: number) => printTop + v * k;
  const redX = scalePointX(RED_FROM, w) - U / 2;
  return (
    <Svg width={w} height={h} style={{ position: 'absolute', left: 0, top: 0 }}>
      <Defs>
        {/* Two old bulbs behind the card: LEFT brighter (.55 at 27 % / 62 %), RIGHT weaker (.32 at
            77 % / 58 %). ★ Uneven on purpose. */}
        <SvgRadial id="bulbL" cx={0.27 * w} cy={0.62 * h} rx={0.34 * w} ry={1.5 * h} gradientUnits="userSpaceOnUse">
          <Stop offset="0" stopColor="rgb(255,214,150)" stopOpacity={0.55} />
          <Stop offset="0.45" stopColor="rgb(255,214,150)" stopOpacity={0.18} />
          <Stop offset="0.72" stopColor="rgb(255,214,150)" stopOpacity={0} />
        </SvgRadial>
        <SvgRadial id="bulbR" cx={0.77 * w} cy={0.58 * h} rx={0.28 * w} ry={1.4 * h} gradientUnits="userSpaceOnUse">
          <Stop offset="0" stopColor="rgb(255,206,140)" stopOpacity={0.32} />
          <Stop offset="0.70" stopColor="rgb(255,206,140)" stopOpacity={0} />
        </SvgRadial>
        {/* Shadowed corners and the dip between the bulbs. */}
        <SvgLinear id="corners" x1="0" y1="0" x2="1" y2="0">
          <Stop offset="0"    stopColor="rgb(70,52,28)" stopOpacity={0.40} />
          <Stop offset="0.16" stopColor="rgb(70,52,28)" stopOpacity={0} />
          <Stop offset="0.44" stopColor="rgb(70,52,28)" stopOpacity={0} />
          <Stop offset="0.53" stopColor="rgb(70,52,28)" stopOpacity={0.12} />
          <Stop offset="0.64" stopColor="rgb(70,52,28)" stopOpacity={0} />
          <Stop offset="0.86" stopColor="rgb(70,52,28)" stopOpacity={0} />
          <Stop offset="1"    stopColor="rgb(70,52,28)" stopOpacity={0.46} />
        </SvgLinear>
      </Defs>
      <SvgRect x={0} y={0} width={w} height={h} fill={CARD} />
      <SvgRect x={0} y={0} width={w} height={h} fill="url(#corners)" />
      <SvgRect x={0} y={0} width={w} height={h} fill="url(#bulbR)" />
      <SvgRect x={0} y={0} width={w} height={h} fill="url(#bulbL)" />
      <G>
        {/* the scale line, then the red band from half a division before +30 */}
        <Line x1={8} y1={y(14.3)} x2={redX} y2={y(14.3)} stroke={PRINT} strokeWidth={1} />
        <SvgRect x={redX} y={y(13.4)} width={w - 8 - redX} height={2.8 * k} fill={RED_BAND} />
        {VU_LABELS.map((label, i) => {
          const x = scalePointX(i, w);
          const c = i >= RED_FROM ? RED_PRINT : PRINT;
          return (
            <G key={label}>
              <Line x1={x} y1={y(5)} x2={x} y2={y(14)} stroke={c} strokeWidth={1.2} />
              <SvgText x={x} y={y(23.5)} textAnchor="middle" fontSize={7 * k} fontWeight="700"
                fill={c} fontFamily={FONT_HYPER}>{label}</SvgText>
              {/* two minor ticks between each pair of scale points */}
              {i < VU_SEGMENTS - 1 && [1, 2].map(m => (
                <Line key={m} x1={x + (m * U) / 3} y1={y(9)} x2={x + (m * U) / 3} y2={y(14)}
                  stroke={c} strokeWidth={0.8} />
              ))}
            </G>
          );
        })}
        <SvgText x={w - 10} y={y(8)} textAnchor="end" fontSize={5.5 * k} fontWeight="700" fill={PRINT}
          fontFamily={FONT_HYPER} letterSpacing={0.6}>SIGNAL</SvgText>
      </G>
    </Svg>
  );
});

// ── 2. The hands (drawn at x = 0, moved by a transform) ──────────────────────

/** A pointed bar: CSS `clip-path: polygon(50% 0, 100% t, 100% 100%, 0 100%, 0 t)`. */
function pointed(x: number, y: number, w: number, h: number, tip: number) {
  const p = Skia.Path.Make();
  p.moveTo(x + w / 2, y);
  p.lineTo(x + w, y + tip * h);
  p.lineTo(x + w, y + h);
  p.lineTo(x, y + h);
  p.lineTo(x, y + tip * h);
  p.close();
  return p;
}
function tri(ax: number, ay: number, bx: number, by: number, cx: number, cy: number) {
  const p = Skia.Path.Make();
  p.moveTo(ax, ay); p.lineTo(bx, by); p.lineTo(cx, cy); p.close();
  return p;
}
/** A soft shadow bar, rasterised once (a moving needle must not blur live every frame). */
function shadowSprite(w: number, h: number, blur: number, colour: string): SkImage | null {
  const pad = blur * 2;
  return makeSprite(w + 2 * pad, h + 2 * pad, c => c.drawRect(Skia.XYWHRect(pad, pad, w, h), glowPaint(colour, blur)));
}

export default function EdgeMeter({ bus, height, printH, printTop, onFault }: {
  bus?: MeterBus; height: number;
  /** ★★★ The frame callback threw (JS thread, once): the housing puts the BAR back (meterGuard). */
  onFault?: (message: string) => void;
  /** Landscape: the print's design height and offset in a shorter window (default: the window). */
  printH?: number; printTop?: number;
}) {
  const [{ w }, onLayout] = useBoxSize();
  const H = height;
  const reduceMotion = useReduceMotion();

  // Shapes at x = 0 (the needle's centre line).
  const shapes = useMemo(() => ({
    signal:   pointed(-1.3, 1, 2.6, H - 6, 0.05),
    head:     tri(-5.5, H, 5.5, H, 0, H - 7),
    peak:     pointed(-0.8, 3, 1.6, H - 3, 0.06),
    sqlHand:  Skia.Path.MakeFromSVGString(`M -0.7 0 h 1.4 v ${H - 3} h -1.4 Z`)!,
    sqlHead:  tri(-4.5, 0, 4.5, 0, 0, 6),
  }), [H]);
  const shadows = useMemo(() => ({
    needle: shadowSprite(3, H - 4, 1.4, 'rgba(0,0,0,0.30)'),
    sql:    shadowSprite(2, H - 3, 1.2, 'rgba(0,0,0,0.22)'),
  }), [H]);

  // ── The bus → shared values ──
  const needle = useSharedValue(0);      // segment position, springing
  const peak   = useSharedValue(0);
  const held   = useSharedValue(0);
  const sqlPos = useSharedValue(-1);
  const dim    = useSharedValue(0);
  const faulted = useSharedValue(0);
  useEffect(() => {
    if (!bus) return;
    const cfg = { ...needleSpring(reduceMotion), reduceMotion: ReduceMotion.Never };
    let lastTarget = NaN, lastClosed: boolean | null = null;
    const take = (m: MeterValues) => {
      const closed = sqlClosedOf(m.sql ?? -1, m.gate, m.level);
      // ★ The RAW level (§4.5 TRAP) — and while the squelch mutes, the needles fall.
      const target = closed ? 0 : vuPos(m.raw ?? m.level);
      if (target !== lastTarget) { lastTarget = target; needle.value = withSpring(target, cfg); }
      sqlPos.value = m.sql != null && m.sql >= 0 ? vuPos(m.sql) : -1;
      if (closed !== lastClosed) { lastClosed = closed; dim.value = withTiming(closed ? 0.5 : 0, { duration: 180 }); }
    };
    take(bus.value);
    bus.subs.add(take);
    return () => { bus.subs.delete(take); };
  }, [bus, reduceMotion, needle, sqlPos, dim]);

  // ★ The peak needle, on the SAME UI thread as the spring, pushed by where the needle IS on screen.
  // ★★★ A throw in a UI-thread callback is a native abort: caught, handed to the JS thread once (as LedVu).
  useFrameCallback((f) => {
    'worklet';
    if (faulted.value) return;
    try {
      const st = { pos: peak.value, heldMs: held.value };
      const p = peakNeedleStep(st, needle.value, f.timeSincePreviousFrame ?? 16);
      if (p !== peak.value) peak.value = p;
      held.value = st.heldMs;
    } catch (e) {
      faulted.value = 1;
      if (onFault) scheduleOnRN(onFault, String((e as Error)?.message ?? e));
    }
  });

  const needleT = useDerivedValue(() => [{ translateX: needleX(needle.value, w) }]);
  const peakT   = useDerivedValue(() => [{ translateX: needleX(peak.value, w) }]);
  const sqlT    = useDerivedValue(() => [{ translateX: needleX(Math.max(0, sqlPos.value), w) }]);
  const sqlOp   = useDerivedValue(() => (sqlPos.value >= 0 ? 1 : 0));

  return (
    <View style={{ height: H, borderRadius: 3, overflow: 'hidden' }} onLayout={onLayout}>
      {w > 0 && <Card w={w} h={H} printH={printH} printTop={printTop} />}
      {w > 0 && (
        <Canvas style={{ position: 'absolute', left: 0, top: 0, width: w, height: H }} pointerEvents="none">
          {/* the red squelch hand: a set-point pointer parked at the threshold, hanging from the top */}
          <Group transform={sqlT} opacity={sqlOp}>
            {shadows.sql && <SkImageNode image={shadows.sql} x={1.3 - 2.4} y={-2.4} width={2 + 4.8} height={H - 3 + 4.8} />}
            <Path path={shapes.sqlHand} color={SQL_HAND} />
            <Path path={shapes.sqlHead} color={SQL_HAND} />
          </Group>
          {/* the peak-decay needle: translucent, no arrowhead */}
          <Group transform={peakT}>
            <Path path={shapes.peak} color="rgba(38,26,14,0.62)" />
          </Group>
          {/* the signal needle, its shadow on the card to its right, and its arrowhead at the bottom */}
          <Group transform={needleT}>
            {shadows.needle && <SkImageNode image={shadows.needle} x={1.6 - 2.8} y={2 - 2.8} width={3 + 5.6} height={H - 4 + 5.6} />}
            <Path path={shapes.signal}>
              <LinearGradient start={vec(0, 1)} end={vec(0, H - 5)} colors={['#2a1d12', '#0d0906']} />
            </Path>
            <Path path={shapes.head} color="#120c07" />
          </Group>
          {/* ── the glass ── drum curvature: the scale rolls away top (.62) and bottom (.70) */}
          <Rect x={0} y={0} width={w} height={H}>
            <LinearGradient start={vec(0, 0)} end={vec(0, H)}
              colors={['rgba(0,0,0,0.62)', 'rgba(0,0,0,0.10)', 'rgba(0,0,0,0)', 'rgba(0,0,0,0)', 'rgba(0,0,0,0.18)', 'rgba(0,0,0,0.70)']}
              positions={[0, 0.22, 0.45, 0.62, 0.80, 1]} />
          </Rect>
          {/* the 1 pt glass streak */}
          <Rect x={0} y={3} width={w} height={1} color="rgba(255,255,255,0.28)" />
          {/* 16 pt side falloff */}
          <Rect x={0} y={0} width={16} height={H}>
            <LinearGradient start={vec(0, 0)} end={vec(16, 0)} colors={['rgba(0,0,0,0.45)', 'rgba(0,0,0,0)']} />
          </Rect>
          <Rect x={w - 16} y={0} width={16} height={H}>
            <LinearGradient start={vec(w, 0)} end={vec(w - 16, 0)} colors={['rgba(0,0,0,0.45)', 'rgba(0,0,0,0)']} />
          </Rect>
          {/* squelch closed: the lamp dims 50 % */}
          <Rect x={0} y={0} width={w} height={H} color="black" opacity={dim} />
        </Canvas>
      )}
    </View>
  );
}
