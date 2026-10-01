/**
 * TunerKeys — the HiFi separates tuner keys. The alternative control mode to
 * DrumWheel (briefs/BRIEF-inputs-shack-mode-mac.md §2), and deliberately NOT the app's
 * standard buttons.
 *
 *   ┌───────────────────────────────────┐  ← THE DRUM WELL'S OWN face (faceplates
 *   │ ┌──────┐               ┌──────┐ │    §6.2) — and, since 2026-10-01, NO ring:
 *   │ │  ‹   │    ·glyph·    │  ›   │ │    each key is lit on its own instead, by
 *   │ └──────┘               └──────┘ │    the light coming up out of the panel
 *   └───────────────────────────────────┘    gap round it (constants/keyLight.ts)
 *
 * One instance renders ONE control pair — `<` `>` for VFO, `−` `+` for zoom —
 * with that pair's static glyph between them (radio = tune, magnifier = zoom).
 * Two of them side by side give the four-key row from the design.
 *
 * ★★ FACEPLATES §6.2 (ref docs/faceplates/tuner-keys/K1, K2; Deck.mockup `tk`). The keys are DOME
 *   KEYS — larger versions of the four main keys (§5): same cap, same 45 ms snap, same press +
 *   release clicks (DomeKey / useDomeKey). Each sits in its own dark slot INSIDE the well's
 *   recessed face, a step below the plate, 31 % of the well wide (34 % in landscape), full height.
 *   The legends and the glyph between are in the CONTROLS colour, on every chassis.
 * ★★★ Stuart, 2026-10-01 — the RING IS GONE: "try removing the outline ring that surrounds both
 *   buttons and replace it with a glow coming up in the panel gap around each button, also apply that
 *   same lighting to all the buttons please." (It was the drum well's border and glow, 2026-09-30's
 *   "ring around them to indicate their importance".) Each key is now marked by its own light, exactly
 *   as every other front-panel key is (DomeKey `lightReach`); the default chassis's keys sit in their
 *   own dark slots, so there the SLOT is the cut-out the light comes up round (DomeKey `lightSlot`).
 * ★★ THE GLYPH BETWEEN THE KEYS IS LASER-ETCHED (Stuart, 2026-10-01): the radio / magnifier is cut into
 *   the case and the same lamp shines THROUGH the cut — the strokes are the lit part, in the controls
 *   colour, backlit exactly as before; with Transparency effects on, a gentle glow bleeds out of the
 *   etching onto the case round it (constants/keyLight.ts ETCH_LIGHT), rasterised ONCE (useEtchGlow).
 *   It is a printed legend, not a key: no panel-gap light of its own.
 * ★ The previous look (tilted matte-black keys with laser-cut backlit legends, a plain machined
 *   edge) is retired by the brief: §5 makes every key on the deck the one dome key.
 *
 * The behaviour, which matters as much as the look: see useHoldSweep below — UNCHANGED.
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { View, StyleSheet, ViewStyle } from 'react-native';
import { Canvas, Path, Skia, BlurMask, Image as SkImageNode, PaintStyle, StrokeCap, StrokeJoin,
  type SkImage, type SkPath } from '@shopify/react-native-skia';
import * as Haptics from 'expo-haptics';
import { getControlHaptics } from './controlHaptics';
import { buildGlyphPath } from './DrumWheel';
import { DomeKey, DomeIcon, type IconStroke } from './DomeKey';
import { WellFace } from './DrumWell';
import { glowPaint, makeSprite } from './glowSprite';
import { useFaceplate } from '../contexts/FaceplateContext';
import { useUiScale } from '../hooks/useUiScale';
import { ledA, type LedColour } from '../constants/faceplate';
import { tunerKeysLayout, TK_SLOT_R } from '../constants/drumWell';
import { DECK_MIN_GAP, ETCH_LIGHT, keyLightReach } from '../constants/keyLight';

// ── Look ──────────────────────────────────────────────────────────────────────
// ★ The colours are the faceplate's (constants/faceplate.ts): the well's face and edge are the drum
//   well's tokens, the caps are DomeKey's, and the controls colour lights the legends and glyph.

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
 * The press/hold control law, with no React in it, so every surface that lands a
 * "tune" or "zoom" mapping shares ONE implementation: the on-screen keys, the
 * hardware arrow keys, mouse side buttons. The law was tuned at length on air
 * (see the constants above) and is far too easy to get subtly wrong twice.
 */
export function createHoldSweep(
  fire: (dir: -1 | 1) => void,
  targetRate?: () => number,
  onSweepChange?: (dir: -1 | 1 | 0) => void,
  /** What each auto-repeat tick does, when it must differ from a tap.
   *  ★ Zoom needs this: the server snaps binBandwidth to a LADDER, so a tap has to
   *  move a WHOLE RUNG or the request is snapped straight back and nothing happens
   *  (symptom: a single tap makes the waterfall lurch and return, and only a double
   *  tap zooms). A held sweep wants the opposite — small compounding factors, which
   *  do cross rungs cumulatively and give the smooth ramp. Hence two magnitudes. */
  fireSweep?: (dir: -1 | 1) => void,
  /** ★ The on-screen keys are DOME KEYS (faceplates §5) and click press + release through
   *  useDomeKey, so they pass `true` here. The hardware arrow keys (SDRScreen) keep the Light /
   *  Medium impacts this law has always given them. */
  quiet = false,
) {
  const sweepFire = fireSweep ?? fire;
  let holdT: ReturnType<typeof setTimeout> | null = null;
  let tickT: ReturnType<typeof setTimeout> | null = null;
  let heldDir: -1 | 1 | 0 = 0;

  const release = () => {
    if (holdT) { clearTimeout(holdT); holdT = null; }
    if (tickT) { clearTimeout(tickT); tickT = null; }
    if (heldDir !== 0) { heldDir = 0; onSweepChange?.(0); }
  };

  const press = (dir: -1 | 1) => {
    release();                    // cancel anything armed by a previous press
    fire(dir);                    // ★ the step happens NOW, not on release
    if (!quiet && getControlHaptics()) void Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Light);
    holdT = setTimeout(() => {
      holdT = null;
      heldDir = dir;
      onSweepChange?.(dir);
      // ★ A heavier thump at the step→sweep transition, so the change of mode is FELT.
      if (!quiet && getControlHaptics()) void Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Medium);
      const started = Date.now();
      const tick = () => {
        sweepFire(dir);
        // Rate AND ceiling recomputed every tick: the ramp is continuous (no gears)
        // and a step-rate or zoom change mid-sweep is picked up at once.
        const t = Math.min(1, (Date.now() - started) / SWEEP_RAMP_MS);
        const hi = Math.max(SWEEP_LO, targetRate ? targetRate() : SWEEP_HI);
        tickT = setTimeout(tick, 1000 / (SWEEP_LO + (hi - SWEEP_LO) * t));
      };
      tickT = setTimeout(tick, 1000 / SWEEP_LO);
    }, HOLD_MS);
  };

  return { press, release };
}

/**
 * React wrapper around createHoldSweep for the on-screen keys.
 */
export function useHoldSweep(
  fire: (dir: -1 | 1) => void,
  disabled = false,
  /** Steps/sec the ramp climbs to. Re-read at every tick. */
  targetRate?: () => number,
  /** Per-tick action when it must differ from a tap — see createHoldSweep. */
  fireSweep?: (dir: -1 | 1) => void,
) {
  const [sweeping, setSweeping] = useState<-1 | 1 | 0>(0);
  const fireRef  = useRef(fire);       fireRef.current  = fire;
  const rateRef  = useRef(targetRate); rateRef.current  = targetRate;
  const sweepRef = useRef(fireSweep);  sweepRef.current = fireSweep;

  // Built ONCE and driven through refs, so a re-render never rebuilds a sweeper
  // mid-press and orphans its timers.
  const sweep = useMemo(() => createHoldSweep(
    (d) => fireRef.current(d),
    () => (rateRef.current ? rateRef.current() : SWEEP_HI),
    setSweeping,
    (d) => (sweepRef.current ?? fireRef.current)(d),
    true,
  ), []);

  // A component unmounting mid-press must not leave a timer walking the VFO up the
  // band forever.
  useEffect(() => sweep.release, [sweep]);

  const press = useCallback((dir: -1 | 1) => {
    if (disabled) return;
    sweep.press(dir);
  }, [disabled, sweep]);

  return { press, release: sweep.release, sweeping };
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
  /** Per-tick action while sweeping, when it must differ from a tap (zoom). */
  onSweepStep?: (dir: -1 | 1) => void;
  width?: number;
  style?: ViewStyle;
  disabled?: boolean;
  /** The landscape bar: keys 34 % of the well and a 6 pt padding (Deck.mockup `tk`). */
  landscape?: boolean;
}

/** The key legends, drawn in the mockup's 24-unit SVG space: ‹ › for tune, − + for zoom. */
function legendStrokes(type: TunerKeyType, dir: -1 | 1): IconStroke[] {
  const p = Skia.Path.Make();
  if (type === 'vfo') {
    if (dir === 1) { p.moveTo(9, 5); p.lineTo(16, 12); p.lineTo(9, 19); }    // M9 5l7 7-7 7
    else           { p.moveTo(15, 5); p.lineTo(8, 12); p.lineTo(15, 19); }   // M15 5l-7 7 7 7
  } else {
    p.moveTo(5, 12); p.lineTo(19, 12);                                       // M5 12h14
    if (dir === 1) { p.moveTo(12, 5); p.lineTo(12, 19); }                    // M12 5v14
  }
  return [{ path: p, width: 1.9 }];
}

export default function TunerKeys({
  type, height, onStep, sweepRate, onSweepStep, width: widthProp = 0, style, disabled = false,
  landscape = false,
}: Props) {
  const [measuredW, setMeasuredW] = useState(widthProp);
  const W = widthProp > 0 ? widthProp : measuredW;
  const H = height;
  const fp = useFaceplate();
  const ct = fp.chassis;
  const s  = useUiScale();
  const G  = (a: number) => ledA(fp.controls, a);

  const { press, release } = useHoldSweep(onStep, disabled, sweepRate, onSweepStep);
  // A key that becomes disabled while held (a shared dial taken away mid-sweep) must stop dead: the
  // disabled DomeKey no longer reports its release.
  useEffect(() => { if (disabled) release(); }, [disabled, release]);

  // ★ The keys are DOME KEYS (§5): the dome's own press / release clicks and snap come from DomeKey
  //   (useDomeKey), one per key, so a thumb rolling from one key to the other clicks each. The STEP
  //   still lands on the way DOWN and the hold timer dies the instant contact breaks — the sweep
  //   law above is untouched; DomeKey only hands us its press-in and press-out.
  const onDown = useCallback((dir: -1 | 1) => { if (!disabled) press(dir); }, [press, disabled]);
  const onUp   = useCallback(() => release(), [release]);

  const L = useMemo(() => tunerKeysLayout(W, H, landscape, s.r), [W, H, landscape, s.r]);
  const glyphPath = useMemo(
    () => buildGlyphPath(type === 'vfo', L.glyphCx, L.glyphCy, L.glyphSz),
    [type, L.glyphCx, L.glyphCy, L.glyphSz]);
  const legend = useMemo(() => ({
    color: fp.controls.core, hot: fp.controls.hot, glow: fp.controls.glow, shade: null,
  }), [fp.controls]);
  const strokes = useMemo(() => ({
    lo: legendStrokes(type, -1), hi: legendStrokes(type, 1),
  }), [type]);

  if (W <= 0) {
    return (
      <View style={[{ height }, style]}
            onLayout={e => setMeasuredW(e.nativeEvent.layout.width)} />
    );
  }

  const dim = disabled ? 0.35 : 1;
  // ★ The keys are LIT (constants/keyLight.ts) while Transparency effects are on. With the ring gone, a
  //   key's nearest neighbour outside the well is the padding away plus at least the deck's tightest gap
  //   (DECK_MIN_GAP — test_faceplate_wells checks every layout), so the light takes its share of that.
  const lit = fp.settings.transparency === 'on';   // (the etched glyph's glow; DomeKey reads it for the keys)
  const lightReach = keyLightReach(L.pad + DECK_MIN_GAP);
  // Default: today's outline key sits in its own dark slot (Deck.mockup `t.slot`, cap inset
  // 2 / 1.5 / 3). Metal: DomeKey's cap already sits in its slot, so it IS the slot.
  const slot = ct.keysSlot;
  const capInset = slot ? { top: 1.5, bottom: 3, x: 2 } : { top: 0, bottom: 0, x: 0 };
  const capH = L.keyH - capInset.top - capInset.bottom;
  const iconSz = Math.max(10, Math.min(s.r(22), (slot ? capH : capH - 4) * 0.55));

  return (
    <View style={[{ height }, style]}
          onLayout={widthProp <= 0 ? e => setMeasuredW(e.nativeEvent.layout.width) : undefined}>
      {/* ── The well's face (static) and the glyph that labels the pair ── */}
      <Canvas style={StyleSheet.absoluteFill} pointerEvents="none">
        <WellFace W={W} H={H} ct={ct} keys />
        {/* Centre glyph — static, non-interactive, LASER-ETCHED and backlit in the controls colour:
            today's glow BEHIND a crisp stroke, as the drum's icon is drawn. Lit, the glow and the
            light bleeding out of the etching come from one sprite (useEtchGlow); unlit, today's
            drawing exactly. */}
        {lit
          ? <EtchGlow path={glyphPath} W={W} H={H} led={fp.controls} dim={dim} />
          : <Path path={glyphPath} color={G(0.55 * dim)} strokeWidth={2.6} style="stroke"
                  strokeCap="round" strokeJoin="round">
              <BlurMask blur={3} style="normal" respectCTM />
            </Path>}
        <Path path={glyphPath} color={G(0.95 * dim)} strokeWidth={1.4} style="stroke"
              strokeCap="round" strokeJoin="round" />
      </Canvas>

      {/* ── The two keys, each in its own dark slot, a step below the plate ── */}
      {([-1, 1] as const).map(dir => (
        <View key={`k${dir}`}
              style={{ position: 'absolute', left: dir === 1 ? L.rightX : L.leftX, top: L.pad,
                       width: L.keyW, height: L.keyH, opacity: dim,
                       ...(slot ? { backgroundColor: slot, borderRadius: TK_SLOT_R,
                                    paddingTop: capInset.top, paddingHorizontal: capInset.x } : null) }}>
          {/* ★ The light is DomeKey's, as on every front-panel key. Default: the dark SLOT is the
              cut-out the light comes up round and in (lightSlot); metal: the key IS its slot. */}
          <DomeKey
            lightReach={lightReach}
            lightSlot={slot ? { x: capInset.x, top: capInset.top, bottom: capInset.bottom, r: TK_SLOT_R } : undefined}
            height={capH} radius={slot ? TK_SLOT_R - 1 : TK_SLOT_R}
            style={slot ? { borderRadius: TK_SLOT_R - 1 } : { width: L.keyW }}
            disabled={disabled}
            onPressIn={() => onDown(dir)}
            onPressOut={onUp}
            // Into the padding and the gap — the keys are the well's only targets.
            hitSlop={{ top: L.pad, bottom: L.pad, left: dir === 1 ? 4 : L.pad, right: dir === 1 ? L.pad : 4 }}
            accessibilityLabel={
              type === 'vfo'
                ? (dir === 1 ? 'Tune up' : 'Tune down')
                : (dir === 1 ? 'Zoom in' : 'Zoom out')
            }
            accessibilityHint="Press for one step, hold to sweep">
            {p => <DomeIcon size={iconSz} k={iconSz / 24} strokes={dir === 1 ? strokes.hi : strokes.lo}
                            progress={p} legend={legend} />}
          </DomeKey>
        </View>
      ))}
    </View>
  );
}

/**
 * The etched glyph's light, rasterised ONCE per well size × glyph × colour: today's glow behind the
 * strokes (α .55, blur 3) and, wider and fainter, the light bleeding out of the etching onto the case
 * (ETCH_LIGHT). The crisp lit strokes are drawn over it, live (they are cheap; the blur is not).
 * ★ `dim` (a disabled pair) is applied as the image's opacity — never a rebuild.
 */
function useEtchGlow(path: SkPath, W: number, H: number, led: LedColour): SkImage | null {
  const key = ledA(led, 1);
  const pathKey = path.toSVGString();
  return useMemo(() => {
    if (!(W > 2) || !(H > 2)) return null;
    // The well's own size: the glyph sits in its middle with more than ETCH_LIGHT.reach clear all round
    // (test_faceplate_wells), and the keys either side are drawn over any bleed that reaches them.
    return makeSprite(W, H, (c) => {
      for (const l of [ETCH_LIGHT.bleed, ETCH_LIGHT.glow]) {
        const p = glowPaint(ledA(led, l.a), l.blur);
        p.setStyle(PaintStyle.Stroke); p.setStrokeWidth(l.width);
        p.setStrokeCap(StrokeCap.Round); p.setStrokeJoin(StrokeJoin.Round);
        c.drawPath(path, p);
      }
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [pathKey, W, H, key]);
}

function EtchGlow({ path, W, H, led, dim }: { path: SkPath; W: number; H: number; led: LedColour; dim: number }) {
  const img = useEtchGlow(path, W, H, led);
  if (!img) return null;
  return <SkImageNode image={img} x={0} y={0} width={W} height={H} opacity={dim} />;
}
