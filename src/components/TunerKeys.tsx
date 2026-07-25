/**
 * TunerKeys — the HiFi separates tuner keys. The alternative control mode to
 * DrumWheel (BRIEF-inputs-shack-mode-mac.md §2), and deliberately NOT the app's
 * standard buttons.
 *
 *   ┌───────────────────────────────────┐  ← same machined panel + green LED
 *   │  ╭─────╮    ·glyph·    ╭─────╮   │    border as DrumWheel, so a drum and
 *   │  │  ‹  │              │  ›  │   │    a key pair sit side by side as one
 *   │  ╰─────╯              ╰─────╯   │    front panel
 *   └───────────────────────────────────┘
 *
 * One instance renders ONE control pair — `<` `>` for VFO, `−` `+` for zoom —
 * with that pair's static glyph between them (radio = tune, magnifier = zoom).
 * Two of them side by side give the four-key row from the design.
 *
 * The look, per Stuart's brief:
 *  - black brushed-metal key bodies, physical-key proportions
 *  - a green LED halo SURROUNDING each key, so they look set INTO the panel
 *  - the icon is LASER-CUT: the green backlight glows THROUGH the cut, rather
 *    than the icon being painted on. Drawn as light escaping a recess, not as a
 *    coloured symbol.
 *  - the centre glyph is static and non-interactive — it LABELS the pair.
 *
 * The behaviour, which matters as much as the look: see useHoldSweep below.
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { View, StyleSheet, Pressable, ViewStyle } from 'react-native';
import {
  Canvas, Rect, RoundedRect, Path, Skia, vec,
  BlurMask, LinearGradient, RadialGradient, Group,
} from '@shopify/react-native-skia';
import * as Haptics from 'expo-haptics';
import { getControlHaptics, buildGlyphPath } from './DrumWheel';

// ── Look (matched to DrumWheel's TUNABLES so the panels are siblings) ─────────

const GLOW_HUE = 120;
const GLOW_INT = 1.0;
function hsl(h: number, s: number, l: number, a: number) { return `hsla(${h},${s}%,${l}%,${a})`; }
const G = (a: number) => hsl(GLOW_HUE, 100, 45, Math.min(1, a * GLOW_INT));

// ── Behaviour (BRIEF §2, "they must ACT like a HiFi tuner") ──────────────────
//
// ★ TAP = one step, fired IMMEDIATELY on press. Ten fast taps are ten steps —
//   no debounce, no accumulation, no cleverness.
// ★ HOLD = the only special case: after HOLD_MS of UNBROKEN contact the key
//   auto-repeats and ACCELERATES smoothly to a ceiling. Release stops it dead.
//
// ★★ Fast clicks must NEVER be read as a hold, and the guarantee is STRUCTURAL
// rather than a heuristic: the timer is armed on press and cancelled on EVERY
// release, so it can only fire after one unbroken 350 ms. Nothing watches click
// frequency, and nothing looks across clicks — so a burst of taps physically
// cannot reach the threshold. Do NOT add cross-click debouncing or merging;
// that is exactly what would break "rapid taps = rapid steps".
const HOLD_MS   = 350;    // unbroken contact before a sweep starts
const SWEEP_LO  = 3;      // steps/sec at the moment the sweep begins
const SWEEP_HI  = 25;     // hard ceiling on steps/sec (feel + send rate)
const SWEEP_RAMP_MS = 2500; // time from LO to the target — a smooth ramp, NOT gears

// ★★ THE CEILING IS NOT A FIXED STEP RATE — it is a constant SCREEN-CROSSING TIME.
//
// A fixed 22 steps/sec means the frequency rate is whatever the step size makes it,
// and that ranges over three orders of magnitude: at 100 Hz you crawl at 2.2 kHz/s,
// at 9 kHz you cover 198 kHz/s and cross the entire MW broadcast band in five
// seconds (Stuart: "it moves too fast"). Same control, completely different
// meaning. What should be constant is how fast SIGNALS MOVE ACROSS THE SCREEN.
//
// So the target rate is derived from the visible span: cross it in SWEEP_SPAN_SECS.
//   stepsPerSec = span / (SWEEP_SPAN_SECS * stepHz), clamped to [SWEEP_LO, SWEEP_HI]
//
// ★ It also fixes the VFO wobble, and provably rather than by luck. The wobble is
// the readout (which moves every step) running ahead of the view (which is
// coalesced to ~11 sends/sec), so the error is stepHz * stepsPerSecond * interval.
// Substitute the law above and stepHz CANCELS: the error is span/SWEEP_SPAN_SECS *
// interval — a constant ~2% of screen width at any step size and any zoom. A flat
// "cap the kHz/sec" rule would not do that; it would still wobble at coarse steps.
// And at coarse steps the rate drops BELOW the send rate, so every step gets its
// own send and the coalescer never engages at all — zero wobble exactly where it
// used to be worst.
//
// One case is beyond help: a 9 kHz step inside a 20 kHz span crosses the screen in
// under a second even at SWEEP_LO, because the step is simply coarse relative to
// the window. The floor stops it going slower than one step per third of a second,
// which is as far as this can sensibly go.
const SWEEP_SPAN_SECS = 4;

/** Target steps/sec so the sweep crosses `spanHz` in SWEEP_SPAN_SECS. */
export function sweepTargetRate(stepHz: number, spanHz: number): number {
  if (!(stepHz > 0) || !(spanHz > 0)) return SWEEP_HI;
  const want = spanHz / (SWEEP_SPAN_SECS * stepHz);
  return Math.max(SWEEP_LO, Math.min(SWEEP_HI, want));
}

/**
 * Press/hold-to-sweep, shared by the HiFi keys and anywhere else a "tune" or
 * "zoom" mapping lands (the ◀▶ step buttons, mouse side buttons, arrow keys).
 * Returns handlers to spread onto a Pressable.
 */
export function useHoldSweep(
  fire: (dir: -1 | 1) => void,
  disabled = false,
  /** Steps/sec the ramp climbs to. Re-read at every tick, so changing the step
   *  rate or zooming mid-sweep takes effect immediately. */
  targetRate?: () => number,
) {
  const holdT  = useRef<ReturnType<typeof setTimeout> | null>(null);
  const tickT  = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [sweeping, setSweeping] = useState<-1 | 1 | 0>(0);

  const stop = useCallback(() => {
    if (holdT.current) { clearTimeout(holdT.current); holdT.current = null; }
    if (tickT.current) { clearTimeout(tickT.current); tickT.current = null; }
    setSweeping(0);
  }, []);

  // Belt and braces: a component unmounting mid-press must not leave a timer
  // walking the VFO up the band forever.
  useEffect(() => stop, [stop]);

  const press = useCallback((dir: -1 | 1) => {
    if (disabled) return;
    stop();                       // cancel anything still armed from a previous press
    fire(dir);                    // ★ the step happens NOW, not on release
    if (getControlHaptics()) void Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Light);

    holdT.current = setTimeout(() => {
      holdT.current = null;
      setSweeping(dir);
      // ★ A heavier thump at the step→sweep transition, so the change of mode is
      // FELT rather than only seen.
      if (getControlHaptics()) void Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
      const started = Date.now();
      const tick = () => {
        fire(dir);
        // Rate is recomputed every tick, so the ramp is continuous — no gears —
        // and the ceiling is re-read too, so changing step rate or zoom mid-sweep
        // is picked up straight away.
        const t = Math.min(1, (Date.now() - started) / SWEEP_RAMP_MS);
        const hi = Math.max(SWEEP_LO, targetRate ? targetRate() : SWEEP_HI);
        const rate = SWEEP_LO + (hi - SWEEP_LO) * t;
        tickT.current = setTimeout(tick, 1000 / rate);
      };
      tickT.current = setTimeout(tick, 1000 / SWEEP_LO);
    }, HOLD_MS);
  }, [disabled, fire, stop, targetRate]);

  return { press, release: stop, sweeping };
}

// ── Component ────────────────────────────────────────────────────────────────

export type TunerKeyType = 'vfo' | 'zoom';

interface Props {
  type: TunerKeyType;
  height: number;
  /** One step in `dir`. Fired on press, and repeatedly while sweeping. */
  onStep: (dir: -1 | 1) => void;
  /** Steps/sec the sweep ramps to — see sweepTargetRate. Omit for the fixed cap. */
  sweepRate?: () => number;
  width?: number;
  style?: ViewStyle;
  disabled?: boolean;
}

export default function TunerKeys({
  type, height, onStep, sweepRate, width: widthProp = 0, style, disabled = false,
}: Props) {
  const [measuredW, setMeasuredW] = useState(widthProp);
  const W = widthProp > 0 ? widthProp : measuredW;
  const H = height;

  const [down, setDown] = useState<-1 | 1 | 0>(0);
  const { press, release, sweeping } = useHoldSweep(onStep, disabled, sweepRate);

  const onDown = useCallback((dir: -1 | 1) => { setDown(dir); press(dir); }, [press]);
  const onUp   = useCallback(() => { setDown(0); release(); }, [release]);

  // ── Geometry: [key] [glyph] [key], the keys generous and the glyph a label ──
  const padX = Math.max(4, W * 0.045);
  const padY = Math.max(3, H * 0.13);
  const keyW = Math.max(18, (W - padX * 2) * 0.335);
  const keyH = Math.max(14, H - padY * 2);
  const cx   = W / 2;
  const keys = useMemo(() => ([
    { dir: -1 as const, x: padX },
    { dir:  1 as const, x: W - padX - keyW },
  ]), [padX, W, keyW]);

  const glyphSz   = Math.max(9, Math.round(H * 0.42));
  const glyphPath = useMemo(
    () => buildGlyphPath(type === 'vfo', cx, H / 2, glyphSz),
    [type, cx, H, glyphSz]);

  // The laser-cut symbol on each key: ‹ › for tune, − + for zoom.
  const symPath = useCallback((dir: -1 | 1, kx: number) => {
    const p = Skia.Path.Make();
    const c = vec(kx + keyW / 2, padY + keyH / 2);
    const r = Math.max(4, Math.min(keyW, keyH) * 0.22);
    if (type === 'vfo') {
      // Chevron — pointing the way it tunes.
      const s = dir === 1 ? 1 : -1;
      p.moveTo(c.x - s * r * 0.45, c.y - r);
      p.lineTo(c.x + s * r * 0.55, c.y);
      p.lineTo(c.x - s * r * 0.45, c.y + r);
    } else {
      p.moveTo(c.x - r, c.y); p.lineTo(c.x + r, c.y);
      if (dir === 1) { p.moveTo(c.x, c.y - r); p.lineTo(c.x, c.y + r); }
    }
    return p;
  }, [type, keyW, keyH, padY]);

  if (W <= 0) {
    return (
      <View style={[{ height }, style]}
            onLayout={e => setMeasuredW(e.nativeEvent.layout.width)} />
    );
  }

  const dim = disabled ? 0.35 : 1;

  return (
    <View style={[{ height }, style]}
          onLayout={widthProp <= 0 ? e => setMeasuredW(e.nativeEvent.layout.width) : undefined}>
      <Canvas style={StyleSheet.absoluteFill}>

        {/* ── Panel face — identical to DrumWheel's, so a drum and a key pair
            read as one continuous front panel when mixed ── */}
        <RoundedRect x={0} y={0} width={W} height={H} r={6}>
          <LinearGradient start={vec(0, 0)} end={vec(0, H)}
            colors={['#101410', '#0a0c0a', '#060706']} positions={[0, 0.4, 1]} />
        </RoundedRect>

        {keys.map(({ dir, x }) => {
          const active = down === dir || sweeping === dir;
          const lit = (active ? 1 : 0.62) * dim;
          return (
            <Group key={`k${dir}`}>
              {/* ── Backlight LEAKING out around the key ──────────────────────
                  ★ The physical model, and it matters for getting this right: the
                  key is an opaque piece of metal sitting in front of a green lamp.
                  It is NOT a key with an LED ring drawn around it. So the light we
                  see is (a) the bright shaft through the laser cut, below, and
                  (b) a thin spill escaping the gap around the key's edge.
                  That spill is per-KEY because each key has its own lamp behind
                  it — drawing one glow around both made the pair read as a single
                  illuminated box, which no real tuner looks like.
                  Kept deliberately restrained: on the reference tuners the keys
                  are MATTE BLACK and barely lit at their edges. Overdo this and it
                  stops looking like machined metal and starts looking like a toy. */}
              <RoundedRect x={x - 2.5} y={padY - 2.5} width={keyW + 5} height={keyH + 5} r={8}
                           color={G(active ? 0.30 : 0.10)} strokeWidth={6} style="stroke">
                <BlurMask blur={7} style="normal" respectCTM />
              </RoundedRect>
              <RoundedRect x={x - 1.2} y={padY - 1.2} width={keyW + 2.4} height={keyH + 2.4} r={6.2}
                           color={G(active ? 0.62 : 0.26)} strokeWidth={1.1} style="stroke">
                <BlurMask blur={1.4} style="normal" respectCTM />
              </RoundedRect>

              {/* The dark recess the key sits down inside, with a machined metal
                  LIP catching light along its top edge — the bright frame the keys
                  are let into on a real tuner (Stuart's reference photos). It is
                  what gives the key somewhere to be recessed INTO. */}
              <RoundedRect x={x - 1} y={padY - 1} width={keyW + 2} height={keyH + 2} r={6}
                           color="rgba(0,0,0,0.9)" />
              <Rect x={x - 1} y={padY - 1} width={keyW + 2} height={1.2}>
                <LinearGradient start={vec(x - 1, 0)} end={vec(x + keyW + 1, 0)}
                  colors={['rgba(255,255,255,0.05)', 'rgba(255,255,255,0.22)', 'rgba(255,255,255,0.05)']} />
              </Rect>

              {/* ── Key body, LEANING BACK into the panel ─────────────────────
                  The face is tilted away at the top, so the light source (above
                  and in front) rakes across the BOTTOM of the key and leaves the
                  top in shadow. Hence dark-at-top → light-at-bottom, the reverse
                  of a flat cap. Pressed = it lies back further still and the
                  whole face falls into shadow. */}
              <RoundedRect x={x} y={padY} width={keyW} height={keyH} r={5}>
                <LinearGradient
                  start={vec(0, padY)} end={vec(0, padY + keyH)}
                  colors={active
                    ? ['#050605', '#090a09', '#0d0f0d']
                    : ['#0a0b0a', '#141614', '#1b1e1b']}
                  positions={[0, 0.55, 1]} />
              </RoundedRect>

              {/* The shadow the panel lip casts down over the top of the key —
                  what actually reads as "leaning back". */}
              <Rect x={x} y={padY} width={keyW} height={keyH * 0.42}>
                <LinearGradient start={vec(0, padY)} end={vec(0, padY + keyH * 0.42)}
                  colors={[active ? 'rgba(0,0,0,0.75)' : 'rgba(0,0,0,0.6)', 'rgba(0,0,0,0)']} />
              </Rect>

              {/* Light caught on the lower face that tilts up towards you. Faint:
                  these are MATTE keys, so it is a hint of sheen, not a gloss
                  highlight — the reference tuners have almost none. */}
              {!active && (
                <Rect x={x + 1} y={padY + keyH * 0.70} width={keyW - 2} height={keyH * 0.28}>
                  <LinearGradient start={vec(0, padY + keyH * 0.70)} end={vec(0, padY + keyH * 0.98)}
                    colors={['rgba(255,255,255,0)', 'rgba(255,255,255,0.05)']} />
                </Rect>
              )}

              {/* ── Brushed grain ────────────────────────────────────────────
                  The reference tuners are MATTE with a fine directional grain, not
                  gloss. A few very low-contrast vertical strands read as machined
                  aluminium at this size; a smooth gradient alone looks like
                  plastic. Skipped while pressed — the face is in shadow then and
                  the grain would only muddy it. */}
              {!active && [0.22, 0.38, 0.55, 0.72].map((f, gi) => (
                <Rect key={`gr${gi}`} x={x + keyW * f} y={padY + keyH * 0.18}
                      width={0.7} height={keyH * 0.64}
                      color={gi % 2 ? 'rgba(255,255,255,0.030)' : 'rgba(255,255,255,0.045)'} />
              ))}

              {/* Bottom lip highlight — the near edge, closest to the light */}
              <RoundedRect x={x + 0.5} y={padY + 0.5} width={keyW - 1} height={keyH - 1} r={4.5}
                           color={active ? 'rgba(255,255,255,0.03)' : 'rgba(255,255,255,0.07)'}
                           strokeWidth={0.9} style="stroke" />

              {/* ── The laser cut ──────────────────────────────────────────────
                  Light escaping a cut in the metal, not a painted symbol: a soft
                  pool bleeding onto the surrounding face, then the bright core of
                  the cut itself. Glow BEHIND a crisp stroke — blurring the stroke
                  itself just smudges it (the same rule DrumWheel's icons follow). */}
              <Path path={symPath(dir, x)} color={G(0.34 * lit)} strokeWidth={7}
                    style="stroke" strokeCap="round" strokeJoin="round">
                <BlurMask blur={6} style="normal" respectCTM />
              </Path>
              <Path path={symPath(dir, x)} color={G(0.55 * lit)} strokeWidth={3.4}
                    style="stroke" strokeCap="round" strokeJoin="round">
                <BlurMask blur={2.5} style="normal" respectCTM />
              </Path>
              <Path path={symPath(dir, x)} color={hsl(GLOW_HUE, 100, 78, 0.95 * lit)}
                    strokeWidth={1.7} style="stroke" strokeCap="round" strokeJoin="round" />
            </Group>
          );
        })}

        {/* ── Centre glyph — static, non-interactive, labels the pair.
            Backlit from behind like a panel legend. ── */}
        <Rect x={cx - glyphSz} y={H / 2 - glyphSz} width={glyphSz * 2} height={glyphSz * 2}>
          <RadialGradient c={vec(cx, H / 2)} r={glyphSz}
            colors={[G(0.10 * dim), 'rgba(0,0,0,0)']} positions={[0, 1]} />
        </Rect>
        <Path path={glyphPath} color={G(0.40 * dim)} strokeWidth={2.6} style="stroke"
              strokeCap="round" strokeJoin="round">
          <BlurMask blur={3} style="normal" respectCTM />
        </Path>
        <Path path={glyphPath} color={G(0.88 * dim)} strokeWidth={1.1} style="stroke"
              strokeCap="round" strokeJoin="round" />

        {/* ── Panel edge — deliberately NOT the drum's green LED border ──────
            DrumWheel rings its whole panel in green because the drum IS the lit
            component. Here the KEYS are, and a green ring around the pair as
            well made the two keys read as one illuminated box instead of two
            separate keys set into metal. So this is a plain machined edge and
            the only green in the panel comes from the keys and the legend. */}
        <RoundedRect x={0.5} y={0.5} width={W - 1} height={H - 1} r={6}
                     color="rgba(255,255,255,0.10)" strokeWidth={0.9} style="stroke" />
      </Canvas>

      {/* ── Touch targets. Views over the Canvas, as DrumWheel does for its +/−.
          onPressIn/onPressOut rather than onPress: the step must land on the way
          DOWN, and the hold timer must die the instant contact breaks. ── */}
      {keys.map(({ dir, x }) => (
        <Pressable
          key={`p${dir}`}
          disabled={disabled}
          onPressIn={() => onDown(dir)}
          onPressOut={onUp}
          // A drag off the key still ends the press, so a sweep cannot be
          // orphaned by sliding a finger away instead of lifting it.
          onTouchCancel={onUp}
          style={{ position: 'absolute', left: x - 2, top: padY - 2,
                   width: keyW + 4, height: keyH + 4 }}
          hitSlop={{ top: 6, bottom: 6, left: 4, right: 4 }}
          accessibilityRole="button"
          accessibilityLabel={
            type === 'vfo'
              ? (dir === 1 ? 'Tune up' : 'Tune down')
              : (dir === 1 ? 'Zoom in' : 'Zoom out')
          }
          accessibilityHint="Press for one step, hold to sweep"
        />
      ))}
    </View>
  );
}
