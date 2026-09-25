/* ═══ SERVER HEALTH PILL (React Native) ════════════════════════════════════════════════════════
 *
 * The RN port of the web client's `renderHealthPill()` (web/client/src/main.ts, and the #srvHealth
 * rules in web/client/index.html). Same glyph paths, same colours, same slot rules — see
 * briefs/BRIEF-server-health-pill.md. Two readers of one design: if a rule changes here it changes
 * there too, or the app and the browser describe the same server differently.
 *
 * ★★★ LEVELS, NOT FIGURES. Listeners asked for the server's CPU; the raw numbers stay on the admin
 *     page where the owner is. Four levels answer "is this receiver struggling?" without inviting a
 *     stranger to misread 83 %/800 % as an overload.
 * ★★ COMPACT IS THE REQUIREMENT, NOT AN ASPIRATION. Stuart, 2026-09-25: "the risk is that we can
 *    make this pill huge it needs to be a compact almost widget pill like the battery one is now".
 *    Variant B — caption above, icons beneath — because that keeps it NARROW, and it sits top-right
 *    over the frequency scale where width costs spectrum ("so it isnt too wide"). Budget ~150x44.
 *    ✗ No padding above 8, no radius above 12, no shadow that reads as a card, no per-slot text.
 * ★ NEVER A DEAD SLOT. A machine with no temperature source and no observed cap has no TEMP slot,
 *   and a Pi has no battery slot; the pill narrows instead. Same rule as AGENTS.md's "a control that
 *   only works on one radio should not be there" — an inert icon reads as a broken FEATURE.         */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import {
  AccessibilityInfo, Animated, AppState, Easing, Platform, StyleSheet, Text, View,
  type StyleProp, type ViewStyle,
} from 'react-native';
import Svg, { Circle, G, Path, Rect, Text as SvgText } from 'react-native-svg';

export type HealthLevel = 0 | 1 | 2 | 3;

export interface Health {
  cpu: HealthLevel;
  ram: HealthLevel;
  /** `kind: 'none'` means the slot is omitted entirely — not drawn grey. */
  temp: { kind: 'sensor' | 'thermal' | 'power' | 'throttle' | 'none'; level: HealthLevel };
  bat: { present: boolean; pct?: number; charging?: boolean; level?: HealthLevel };
}

/** 0 OK · 1 Elevated · 2 High · 3 Critical. Verbatim from the web client's HEALTH_COLOURS. */
const HEALTH_COLOURS = ['#5BE36B', '#E8C547', '#FF8A3D', '#FF4B4B'];
const HEALTH_WORDS = ['OK', 'elevated', 'high', 'critical'];

/** ★ The readout amber this client wears everywhere else — the battery figure, and nothing else. */
const READOUT_AMBER = '#ffb833';
const CAPTION_GREY = '#B9C0B4';

const clampLevel = (n: number): number => Math.max(0, Math.min(3, Math.round(n) || 0));

/* ─── Glyphs ─────────────────────────────────────────────────────────────────────────────────────
 * 16x16, stroke 1.4, round caps, single-colour so each slot tints by its own level. The `d` strings
 * are copied VERBATIM from HEALTH_ICONS in web/client/src/main.ts (which took them from the brief):
 * they were drawn and checked for legibility at 16 px on the design reference's icon boards, and a
 * redraw here would quietly fork the two clients' glyphs.                                          */
type GlyphName = 'cpu' | 'ram' | 'temp' | 'snailFire' | 'snailBolt' | 'snail';

const SNAIL_BODY = 'M1.5 13.4h9.8a2.3 2.3 0 0 0 2.3-2.3V9.4';
const SNAIL_HORNS = 'M13.6 9.4l-.8-2M13.6 9.4l1-1.8';

function Glyph({ name, color }: { name: GlyphName; color: string }) {
  return (
    <Svg
      width={16}
      height={16}
      viewBox="0 0 16 16"
      fill="none"
      stroke={color}
      strokeWidth={1.4}
      strokeLinecap="round"
      strokeLinejoin="round"
    >
      {glyphBody(name, color)}
    </Svg>
  );
}

function glyphBody(name: GlyphName, color: string) {
  switch (name) {
    case 'cpu':        // chip with pins
      return (
        <>
          <Rect x={4.5} y={4.5} width={7} height={7} rx={1} />
          <Path d="M6.5 1.8v2.7M9.5 1.8v2.7M6.5 11.5v2.7M9.5 11.5v2.7M1.8 6.5h2.7M1.8 9.5h2.7M11.5 6.5h2.7M11.5 9.5h2.7" />
        </>
      );
    case 'ram':        // DIMM: body, legs, cells
      return (
        <>
          <Rect x={1.8} y={4.5} width={12.4} height={7} rx={1} />
          <Path d="M4.6 11.5v2.2M8 11.5v2.2M11.4 11.5v2.2M5 7v2M8 7v2M11 7v2" />
        </>
      );
    case 'temp':       // thermometer — a REAL reading, never an inferred one
      return (
        <>
          <Path d="M8 2.6a1.7 1.7 0 0 1 1.7 1.7v4.4a3 3 0 1 1-3.4 0V4.3A1.7 1.7 0 0 1 8 2.6z" />
          <Circle cx={8} cy={11.4} r={1.2} fill={color} stroke="none" />
        </>
      );
    /* ★★★ TWO SNAILS, AND THE DIFFERENCE IS THE CAUSE (Stuart, 2026-09-25): "snail on fire =
     *  thermal throttle, snail with a lightning bolt = power limit throttled." A plain snail says
     *  only "slow" and leaves the owner guessing — and the two causes want opposite fixes: cool it
     *  down, or find a better supply/cable. The plain snail survives only for a cap whose cause
     *  nothing reports. */
    case 'snailFire':  // shell shrunk to make room for three flame tongues at stroke 1.2
      return (
        <>
          <Circle cx={6.3} cy={9.6} r={3.3} />
          <Path d="M6.3 9.6a1.2 1.2 0 1 1 1.2-1.2" />
          <Path d={SNAIL_BODY} />
          <Path d={SNAIL_HORNS} />
          <G strokeWidth={1.2}>
            <Path d="M4.1 5.6c-.8-.8-.4-1.8.2-2.5.1.7.6 1 .5 1.9" />
            <Path d="M6.3 5.2c-1-1.1-.4-2.5.4-3.6.2 1.1.9 1.6.6 3" />
            <Path d="M8.5 5.7c-.7-.7-.3-1.6.3-2.2.1.7.6.9.4 1.8" />
          </G>
        </>
      );
    case 'snailBolt':  // ★ its own bolt, drawn where the flames were — ✗ not the battery's, rescaled
      return (
        <>
          <Circle cx={6.3} cy={9.6} r={3.3} />
          <Path d="M6.3 9.6a1.2 1.2 0 1 1 1.2-1.2" />
          <Path d={SNAIL_BODY} />
          <Path d={SNAIL_HORNS} />
          <Path d="M6.6 5.4L5.2 2.2h2.6L6.6 4.4h1.8L5.6 7.2l1-1.8z" strokeWidth={1.2} />
        </>
      );
    case 'snail':      // a cap is observed but nothing says why — bigger shell, no cause marking
      return (
        <>
          <Circle cx={6.5} cy={8.2} r={4} />
          <Path d="M6.5 8.2a1.4 1.4 0 1 1 1.4-1.4" />
          <Path d={SNAIL_BODY} />
          <Path d={SNAIL_HORNS} />
        </>
      );
  }
}

/* ─── The TEMP slot ──────────────────────────────────────────────────────────────────────────────
 * Three-state, not two: a trusted sensor, a throttle with a cause, or nothing at all.
 * ★★ `none` is NOT "unknown, show grey" — on a Pi 500 reporting under-voltage while its clock sits
 *    at the full 2400 MHz the server deliberately sends `none`, because the snail must follow an
 *    OBSERVED cap and not a status bit ([[pi500_undervoltage_reboot]]: "undervoltage REAL, box
 *    STABLE, do not raise it"). Drawing anything here would put permanent furniture on his machine.
 * ★ A throttle is at least High whatever level arrives: the server has measured a cap, so "OK" would
 *   contradict the glyph the same message asked for. Matches the web client's Math.max(2, level).  */
function tempSlot(temp: Health['temp']): { name: GlyphName; level: number; word: string } | null {
  switch (temp.kind) {
    case 'sensor':   return { name: 'temp', level: clampLevel(temp.level), word: `temperature ${HEALTH_WORDS[clampLevel(temp.level)]}` };
    case 'thermal':  return { name: 'snailFire', level: Math.max(2, clampLevel(temp.level)), word: 'throttling' };
    case 'power':    return { name: 'snailBolt', level: Math.max(2, clampLevel(temp.level)), word: 'throttling' };
    case 'throttle': return { name: 'snail', level: Math.max(2, clampLevel(temp.level)), word: 'throttling' };
    default:         return null;
  }
}

export default function HealthPill({
  health, style,
}: { health: Health; style?: StyleProp<ViewStyle> }) {
  const temp = useMemo(() => tempSlot(health.temp), [health.temp]);
  const cpu = clampLevel(health.cpu);
  const ram = clampLevel(health.ram);
  const batLevel = clampLevel(health.bat.level ?? 0);
  const batPct = Math.max(0, Math.min(100, Math.round(health.bat.pct ?? 0)));

  /* ★★ The border takes the WORST VISIBLE slot — visible being the point. A battery level from a
   *    machine that reports `present: false`, or a temp level under `kind: 'none'`, must not colour
   *    a pill that is not showing that slot: the reader would see red with nothing red in it. */
  const worst = Math.max(cpu, ram, temp ? temp.level : 0, health.bat.present ? batLevel : 0);

  /* ── Reduced motion ────────────────────────────────────────────────────────────────────────────
   * ★ Read the CURRENT setting, don't wait for a change event: `reduceMotionChanged` only fires on a
   *   transition, so a listener who has had it on for months would otherwise get the animation until
   *   they toggled it. (The same shape as WaterfallView's AppState.currentState read.) */
  const [reduceMotion, setReduceMotion] = useState(false);
  useEffect(() => {
    let alive = true;
    AccessibilityInfo.isReduceMotionEnabled().then((on) => { if (alive) setReduceMotion(on); }).catch(() => {});
    const sub = AccessibilityInfo.addEventListener('reduceMotionChanged', setReduceMotion);
    return () => { alive = false; sub.remove(); };
  }, []);

  /* ── Visibility ────────────────────────────────────────────────────────────────────────────────
   * ★★ INITIALISED FROM AppState.currentState, not assumed foreground — a `change` event only fires
   *    on a TRANSITION, so a cold launch straight into the background (what the Watch does when it
   *    wakes the phone) would leave a loop running with nobody looking at the screen. Same trap, and
   *    same cure, as WaterfallView's Skia gate. */
  const [foreground, setForeground] = useState(AppState.currentState === 'active');
  useEffect(() => {
    const sub = AppState.addEventListener('change', (s) => setForeground(s === 'active'));
    return () => sub.remove();
  }, []);

  /* ── The breath ────────────────────────────────────────────────────────────────────────────────
   * ★★★ ONLY CRITICAL BREATHES, and only opacity, on the native driver. Everything else is static:
   *     an icon that moves is a claim on the listener's attention, and making three of them move at
   *     once spends that claim on nothing. ✗ Never animate layout here — the pill sits over the
   *     frequency scale, and a width that breathes would repaint the scale every frame. */
  const breathe = useRef(new Animated.Value(1)).current;
  const critical = worst >= 3;
  const animate = critical && !reduceMotion && foreground;
  useEffect(() => {
    if (!animate) { breathe.setValue(1); return; }
    const loop = Animated.loop(Animated.sequence([
      Animated.timing(breathe, { toValue: 0.3, duration: 700, easing: Easing.inOut(Easing.ease), useNativeDriver: true }),
      Animated.timing(breathe, { toValue: 1, duration: 700, easing: Easing.inOut(Easing.ease), useNativeDriver: true }),
    ]));
    loop.start();
    // ★ Stop on unmount AND on backgrounding: a native-driver loop keeps ticking off-screen.
    return () => { loop.stop(); breathe.setValue(1); };
  }, [animate, breathe]);

  /* ★ One sentence, because the icons carry no text of their own (§2a: per-slot labels are what turn
   *   this into a dashboard). Reads in the order the slots are drawn. */
  const label = 'Server health: processor ' + HEALTH_WORDS[cpu] + ', memory ' + HEALTH_WORDS[ram]
    + (temp ? `, ${temp.word}` : '')
    + (health.bat.present ? `, battery ${batPct}%${health.bat.charging ? ' on power' : ''}` : '');

  const borderColor = HEALTH_COLOURS[worst] + (worst === 0 ? '73' : 'bf');   // 0.45 / 0.75 alpha

  return (
    <Animated.View
      accessible
      accessibilityRole="text"
      accessibilityLabel={label}
      style={[styles.pill, { borderColor }, critical && { opacity: breathe }, style]}
    >
      <Text style={styles.caption} numberOfLines={1}>SERVER HEALTH</Text>
      <View style={styles.row}>
        <Slot name="cpu" level={cpu} breathe={breathe} />
        <Slot name="ram" level={ram} breathe={breathe} />
        {temp ? <Slot name={temp.name} level={temp.level} breathe={breathe} /> : null}
        {health.bat.present ? (
          <>
            <View style={styles.divider} />
            <Battery pct={batPct} level={batLevel} charging={!!health.bat.charging} breathe={breathe} />
          </>
        ) : null}
      </View>
    </Animated.View>
  );
}

/* ★★ A NON-COLOUR CUE FROM "HIGH" UPWARDS, FOR EVERYONE — not just under reduced motion. Elevated
 *    and High differ only by hue, which is exactly the pair a red/green colour-blind listener cannot
 *    separate, so from level 2 the slot also wears a 4 px dot at its top right. */
function Slot({ name, level, breathe }: { name: GlyphName; level: number; breathe: Animated.Value }) {
  const color = HEALTH_COLOURS[clampLevel(level)];
  return (
    <Animated.View style={[styles.slot, level >= 3 && { opacity: breathe }]}>
      <Glyph name={name} color={color} />
      {level >= 2 ? <View style={[styles.dot, { backgroundColor: color }]} /> : null}
    </Animated.View>
  );
}

/* ─── Battery ────────────────────────────────────────────────────────────────────────────────────
 * ★★★ THE NUMBER LIVES INSIDE THE OUTLINE. Stuart, 2026-09-25: "I wonder if the number inside the
 *     battery with the lightning bolt would be the better choice to save even more space." It is —
 *     text and bolt alongside cost about 40 px, most of the slot, on a pill whose whole requirement
 *     is to stay narrow.
 * ★★ NO PER CENT SIGN: the outline already says what the number is, and dropping it is what lets
 *    "100" fit at a legible size. ★★ AND THE NUMBER IS AMBER, ALWAYS — the OUTLINE carries the
 *    level, so tinting the digits too said the same thing twice and left them hard to read against
 *    a red outline at 9 px.
 * ★ THE NUMBER READS FIRST, THEN THE BOLT — "55 ⚡", the order it is spoken (Stuart). With the bolt
 *   on the left it read as a charging symbol that happened to have a figure after it, rather than a
 *   battery level that happens to be charging.
 * ★ The charge bar sits BEHIND the text at 0.22 alpha, so the glyph still reads as a battery filling
 *   up rather than a box with a number in it.                                                      */
function Battery({
  pct, level, charging, breathe,
}: { pct: number; level: number; charging: boolean; breathe: Animated.Value }) {
  const color = HEALTH_COLOURS[clampLevel(level)];
  const charged = 29.6 * pct / 100;          // the 29.6 px of usable width inside the 33 px body
  return (
    <Animated.View style={[styles.slot, level >= 3 && { opacity: breathe }]}>
      <Svg width={40} height={16} viewBox="0 0 40 16" fill="none" stroke={color} strokeWidth={1.4}>
        <Rect x={0.7} y={1.7} width={33} height={12.6} rx={2} />
        <Rect x={35} y={5} width={2} height={6} rx={1} fill={color} stroke="none" />
        <Rect x={2.4} y={3.4} width={charged} height={9.2} rx={1} fill={color} stroke="none" opacity={0.22} />
        <SvgText
          x={charging ? 14 : 17}
          y={11.6}
          fontFamily={MONO}
          fontSize={9}
          fontWeight="700"
          fill={READOUT_AMBER}
          stroke="none"
          textAnchor="middle"
        >
          {String(pct)}
        </SvgText>
        {charging ? (
          <Path d="M27.4 4.6L24.6 8.6h2.1l-.7 2.9 2.8-4h-2.1z" fill={READOUT_AMBER} stroke="none" />
        ) : null}
      </Svg>
      {level >= 2 ? <View style={[styles.dot, { backgroundColor: color }]} /> : null}
    </Animated.View>
  );
}

/** ★ `ui-monospace` is a web keyword; RN needs a real family per platform or it silently falls back
 *   to the proportional system face, and the digits then jitter in width as the charge changes. */
const MONO = Platform.select({ ios: 'Menlo', android: 'monospace', default: 'monospace' });

const styles = StyleSheet.create({
  // ★ Content-width and right-anchored (alignSelf), so the pill grows leftwards when a slot appears
  //   and shrinks when one goes away. The caller positions it; we only refuse to stretch.
  pill: {
    alignSelf: 'flex-end',
    backgroundColor: 'rgba(8,12,8,0.86)',
    borderWidth: 1.5,
    borderRadius: 12,
    paddingTop: 4,
    paddingBottom: 5,
    paddingHorizontal: 8,
  },
  caption: {
    // ★ 9 px is marginal on low-density hosts (§2 TRAP). If it proves unreadable, DROP the caption
    //   — the icons and the accessibility label carry the meaning. ✗ Do not grow the pill for it.
    fontSize: 9,
    lineHeight: 10,
    fontWeight: '700',
    letterSpacing: 1.4,          // ≈ 0.16em at 9 px; RN letterSpacing is absolute, not em
    color: CAPTION_GREY,
    textAlign: 'right',
    marginBottom: 3,
  },
  row: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  slot: { position: 'relative' },
  divider: { width: 1, alignSelf: 'stretch', backgroundColor: 'rgba(255,255,255,0.16)' },
  dot: { position: 'absolute', top: -1, right: -2, width: 4, height: 4, borderRadius: 2 },
});
