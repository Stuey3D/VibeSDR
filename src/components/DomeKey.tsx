/**
 * DomeKey — the ONE key type on every chassis: a flat cap over a snap dome (faceplates brief §5).
 *
 * > "I want to feel like I can reach out and my screen will physically click in when pressing the
 * >  buttons." — Stuart
 *
 * `useDomeKey()` is the animation + haptic timing, built once so no key can be missed (§5: "Build
 * it once … so no key can be missed"). `DomeKey` draws the cap for the current chassis:
 *   • default — TODAY'S key at rest (outline + tint). It still snaps 2 pt and dims, and clicks.
 *   • silver / black — a brushed cap in a recessed slot (Deck.mockup `mkKey`, `keyStyle: 'flat'`).
 *
 * ★★★ Depress, haptic and legend flare fire on onPressIn; the ACTION fires on release (§5).
 *   (TunerKeys is the exception the brief keeps: its step lands on the way DOWN, as it always has —
 *   it uses useDomeKey's pressIn/pressOut directly for the feel and keeps its own sweep law.)
 * ★★ THE SNAP RUNS ON THE UI THREAD (Reanimated withTiming): press 45 ms `cubic-bezier(0.9,0,1,0.6)`
 *   — resists, then collapses; release 35 ms `cubic-bezier(0.2,0.9,0.3,1.4)` — springs back past
 *   rest. A busy JS thread cannot make it slide.
 * ★ The press click is 45 ms late ON PURPOSE — see domeClick.ts.
 * ★ Haptics: iOS `.rigid` press / `.light` release (UIImpactFeedbackGenerator — expo-haptics 57 has
 *   no Core Haptics transient and no intensity, so "light ~0.5" is `.light`); Android
 *   KEYBOARD_TAP / KEYBOARD_RELEASE through performHapticFeedback — never raw vibrate(). Gated on the
 *   ONE controls-haptics switch the drums and tuner keys use (getControlHaptics).
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Image, Platform, Pressable, StyleSheet, Text, View,
  type Insets, type StyleProp, type TextStyle, type ViewStyle } from 'react-native';
import Animated, {
  Easing, interpolateColor, useAnimatedStyle, useDerivedValue, useSharedValue, withTiming,
  type SharedValue,
} from 'react-native-reanimated';
import {
  BlurMask, Canvas, FilterMode, Group, ImageShader, Line, LinearGradient, MipmapMode, Path,
  RoundedRect, Skia, vec, type SkImage, type SkPath,
} from '@shopify/react-native-skia';
import * as Haptics from 'expo-haptics';
import { useFaceplate } from '../contexts/FaceplateContext';
import { getControlHaptics } from './controlHaptics';
import {
  createDomeClick, DOME_PRESS_MS, DOME_RELEASE_MS, DOME_PRESS_BEZIER, DOME_RELEASE_BEZIER,
} from './domeClick';
import { LEGEND_GLOW_REST, LEGEND_GLOW_DOWN, type ChassisTokens } from '../constants/faceplate';

// ── Haptics ───────────────────────────────────────────────────────────────────

function clickPress() {
  if (!getControlHaptics()) return;
  if (Platform.OS === 'android') {
    Haptics.performAndroidHapticsAsync(Haptics.AndroidHaptics.Keyboard_Tap).catch(() => {});
  } else {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Rigid).catch(() => {});
  }
}
function clickRelease() {
  if (!getControlHaptics()) return;
  if (Platform.OS === 'android') {
    Haptics.performAndroidHapticsAsync(Haptics.AndroidHaptics.Keyboard_Release).catch(() => {});
  } else {
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Light).catch(() => {});
  }
}

const PRESS_EASE   = Easing.bezier(...DOME_PRESS_BEZIER);
const RELEASE_EASE = Easing.bezier(...DOME_RELEASE_BEZIER);

/** §5: 2 pt of travel (rest top +1.5 → clicked +3.5). */
export const DOME_TRAVEL = 2;

// ── The hook ──────────────────────────────────────────────────────────────────

export interface DomeKeyState {
  /** 0 = at rest, 1 = clicked. Overshoots below 0 on the release spring, as the mockup's curve does. */
  progress: SharedValue<number>;
  pressIn:  () => void;
  pressOut: () => void;
}

export function useDomeKey(): DomeKeyState {
  const progress = useSharedValue(0);
  const click = useMemo(() => createDomeClick({
    press: clickPress, release: clickRelease,
    setTimer: (ms, f) => setTimeout(f, ms),
    clearTimer: (t) => clearTimeout(t as ReturnType<typeof setTimeout>),
    now: Date.now,
    // ★ §5 TRAP: "check the timer isn't late under load on the Xcover 4S". A JS timer is what can
    //   be late (the snap itself is on the UI thread); say so in development when it is.
    onLate: __DEV__ ? (late) => { if (late > 25) console.warn(`[DomeKey] press click ${Math.round(late)} ms late`); } : undefined,
  }), []);
  useEffect(() => click.dispose, [click]);

  const pressIn = useCallback(() => {
    progress.value = withTiming(1, { duration: DOME_PRESS_MS, easing: PRESS_EASE });
    click.down();
  }, [progress, click]);
  const pressOut = useCallback(() => {
    progress.value = withTiming(0, { duration: DOME_RELEASE_MS, easing: RELEASE_EASE });
    click.up();
  }, [progress, click]);
  return { progress, pressIn, pressOut };
}

// ── Textures (shared with ChassisPlate) ──────────────────────────────────────

const TEXTURES = {
  silver: require('../../assets/faceplates/silver-brushed.jpg'),
  black:  require('../../assets/faceplates/black-brushed.jpg'),
};
/** Brushed grain moirés when minified (§3.4 TRAP): linear filtering between mip levels. */
export const TEXTURE_SAMPLING = { filter: FilterMode.Linear, mipmap: MipmapMode.Linear };
const texCache: Partial<Record<'silver' | 'black', SkImage | Promise<SkImage | null>>> = {};

function loadTexture(which: 'silver' | 'black'): SkImage | Promise<SkImage | null> {
  const hit = texCache[which];
  if (hit) return hit;
  const uri = Image.resolveAssetSource(TEXTURES[which]).uri;
  const p = Skia.Data.fromURI(uri)
    .then(d => {
      const img = Skia.Image.MakeImageFromEncoded(d);
      if (img) texCache[which] = img;
      return img;
    })
    .catch(() => { delete texCache[which]; return null; });
  texCache[which] = p;
  return p;
}

/** The brushed texture, decoded ONCE per app run and shared by the plate and every cap. */
export function useTexture(which: 'silver' | 'black' | null): SkImage | null {
  const [img, setImg] = useState<SkImage | null>(() => {
    if (!which) return null;
    const c = texCache[which];
    return c && !(c instanceof Promise) ? c : null;
  });
  useEffect(() => {
    if (!which) { setImg(null); return; }
    let live = true;
    const r = loadTexture(which);
    if (r instanceof Promise) r.then(i => { if (live) setImg(i); });
    else setImg(r);
    return () => { live = false; };
  }, [which]);
  return img;
}

// ── The cap face (silver / black) — static, so it is drawn once ──────────────

const CapFace = React.memo(function CapFace({ w, h, r, dome, texture }: {
  w: number; h: number; r: number; dome: ChassisTokens['dome']; texture: 'silver' | 'black';
}) {
  const img = useTexture(texture);
  // `.bz-*` sets the grain at 360 px wide over a 1200 px image.
  const k = 360 / 1200;
  return (
    <Canvas style={{ width: w, height: h }} pointerEvents="none">
      <RoundedRect x={0} y={0} width={w} height={h} r={r} color={dome.capBase} />
      {img && (
        <RoundedRect x={0} y={0} width={w} height={h} r={r}>
          <ImageShader image={img} tx="mirror" ty="mirror" fit="none" sampling={TEXTURE_SAMPLING}
            transform={[{ translateX: w / 2 - 600 * k }, { translateY: h / 2 - 450 * k }, { scale: k }]} />
        </RoundedRect>
      )}
      <RoundedRect x={0} y={0} width={w} height={h} r={r}>
        <LinearGradient start={vec(0, 0)} end={vec(0, h)} colors={dome.capSheen} positions={[0, 0.42, 1]} />
      </RoundedRect>
      {/* chamfer (top), the far edge (bottom), the rim, and the border */}
      <Line p1={vec(r * 0.6, 1.5)} p2={vec(w - r * 0.6, 1.5)} color={dome.chamfer} strokeWidth={1} />
      <Line p1={vec(r * 0.6, h - 1.5)} p2={vec(w - r * 0.6, h - 1.5)} color={dome.bottomEdge} strokeWidth={1} />
      <RoundedRect x={1.5} y={1.5} width={w - 3} height={h - 3} r={Math.max(0, r - 1)}
        color={dome.rim} style="stroke" strokeWidth={1} />
      <RoundedRect x={0.5} y={0.5} width={w - 1} height={h - 1} r={r}
        color={dome.capBorder} style="stroke" strokeWidth={1} />
    </Canvas>
  );
});

// ── Legends ───────────────────────────────────────────────────────────────────

/** A text legend with the §5 flare: colour → hot, glow 4 → 7 pt, on the UI thread. */
export function DomeText({ progress, style, children, numberOfLines, adjustsFontSizeToFit,
  minimumFontScale }: {
  progress: SharedValue<number>; style?: StyleProp<TextStyle>; children: React.ReactNode;
  numberOfLines?: number; adjustsFontSizeToFit?: boolean; minimumFontScale?: number;
}) {
  const kl = useFaceplate().keyLegend;
  const flares = kl.glow != null || kl.hot !== kl.color;
  const anim = useAnimatedStyle(() => {
    const p = Math.max(0, Math.min(1, progress.value));
    return {
      color: interpolateColor(p, [0, 1], [kl.color, kl.hot]),
      ...(kl.glow ? { textShadowRadius: LEGEND_GLOW_REST + (LEGEND_GLOW_DOWN - LEGEND_GLOW_REST) * p } : null),
    };
  }, [kl.color, kl.hot, kl.glow]);
  const base: TextStyle = {
    color: kl.color, fontFamily: kl.font,
    ...(kl.glow ? { textShadowColor: kl.glow, textShadowRadius: LEGEND_GLOW_REST,
                    textShadowOffset: { width: 0, height: 0 } } : null),
  };
  const common = { numberOfLines, adjustsFontSizeToFit, minimumFontScale };
  return (
    <View>
      {/* ★ The engraving's dark shadow on silver — a still copy under the lit legend (RN takes one
          text shadow, and the glow is that one). */}
      {kl.shade && (
        <Text {...common} style={[style, base, { position: 'absolute', top: -0.5, left: 0, right: 0,
          color: kl.shade, textShadowRadius: 0 }]}>{children}</Text>
      )}
      {flares
        ? <Animated.Text {...common} style={[style, base, anim]}>{children}</Animated.Text>
        : <Text {...common} style={[style, base]}>{children}</Text>}
    </View>
  );
}

export interface IconStroke { path: SkPath; fill?: boolean; width?: number; color?: string }

/** A Skia icon legend with the same flare. `k` scales the paths' authoring space to `size`. */
export function DomeIcon({ size, k, strokes, progress }: {
  size: number; k: number; strokes: IconStroke[]; progress?: SharedValue<number>;
}) {
  const kl = useFaceplate().keyLegend;
  const zero = useSharedValue(0);
  const p = progress ?? zero;
  const color = useDerivedValue(() => {
    const t = Math.max(0, Math.min(1, p.value));
    return interpolateColor(t, [0, 1], [kl.color, kl.hot]);
  }, [kl.color, kl.hot]);
  // CSS drop-shadow radius r ≈ a blur sigma of r / 2.
  const blur = useDerivedValue(() =>
    (LEGEND_GLOW_REST + (LEGEND_GLOW_DOWN - LEGEND_GLOW_REST) * Math.max(0, Math.min(1, p.value))) / 2);
  const draw = (c: any, key: string, extra?: React.ReactNode) => strokes.map((s, i) => (
    <Path key={`${key}${i}`} path={s.path} color={s.color ?? c}
      {...(s.fill ? { style: 'fill' as const } : {
        style: 'stroke' as const, strokeWidth: (s.width ?? 1.6) / k, strokeCap: 'round' as const,
        strokeJoin: 'round' as const })}>
      {extra}
    </Path>
  ));
  return (
    <Canvas pointerEvents="none" style={{ width: size, height: size }}>
      <Group transform={[{ scale: k }]}>
        {kl.shade && <Group transform={[{ translateY: -0.5 / k }]}>{draw(kl.shade, 's')}</Group>}
        {kl.glow && draw(kl.glow, 'g', <BlurMask blur={blur} style="normal" respectCTM={false} />)}
        {draw(color, 'm')}
      </Group>
    </Canvas>
  );
}

// ── The key ──────────────────────────────────────────────────────────────────

export interface DomeKeyProps {
  onPress?:  () => void;
  disabled?: boolean;
  /** The SLOT's height (silver/black: the cap is the slot less 4 pt, §4.1 "58 / 54"). */
  height:    number;
  /** Slot corner radius — 10 portrait, 6 landscape (Deck.mockup `keyRadius`). */
  radius?:   number;
  style?:    StyleProp<ViewStyle>;
  hitSlop?:  number | Insets;
  /** Default chassis: the outline colour (the recording / unread-chat states in landscape). */
  outline?:  string;
  /** Default chassis only: `height` is a MINIMUM (portrait's por.btn used minHeight), so the
   *  default key stays exactly today's size. Landscape keys are set, not negotiated (§11). */
  minHeight?: boolean;
  /** Drawn over the slot, not moved by the press (the recording / chat pulse rings). */
  overlay?:  React.ReactNode;
  accessibilityLabel?: string;
  /** The legend, given the press progress for its flare. */
  children:  (progress: SharedValue<number>) => React.ReactNode;
}

export const DomeKey = React.forwardRef<View, DomeKeyProps>(function DomeKey({
  onPress, disabled, height, radius = 10, style, hitSlop = 10, outline, minHeight, overlay,
  accessibilityLabel, children,
}, ref) {
  const fp = useFaceplate();
  const ct = fp.chassis;
  const dome = ct.dome;
  const { progress, pressIn, pressOut } = useDomeKey();
  const [capW, setCapW] = useState(0);
  const lastW = useRef(0);

  const travel = useAnimatedStyle(() => ({
    transform: [{ translateY: progress.value * DOME_TRAVEL }],
  }));
  const dim = useAnimatedStyle(() => ({ opacity: Math.max(0, Math.min(1, progress.value)) }));
  const cast = useAnimatedStyle(() => ({ opacity: 1 - Math.max(0, Math.min(1, progress.value)) }));

  const onIn  = disabled ? undefined : pressIn;
  const onOut = disabled ? undefined : pressOut;

  if (dome.look === 'outline') {
    // ★ TODAY'S KEY at rest, pixel for pixel: por.btn / lnd.lsBtn's outline and tint. The snap moves
    //   the legend (the only part a flat outline key has to move) and dims the face.
    return (
      <Pressable ref={ref} onPress={disabled ? undefined : onPress} onPressIn={onIn} onPressOut={onOut}
        disabled={disabled} hitSlop={hitSlop} accessibilityRole="button" accessibilityLabel={accessibilityLabel}
        style={[{ ...(minHeight ? { minHeight: height } : { height }), borderWidth: 1, borderRadius: 4, borderColor: outline ?? ct.keyBorder,
                  backgroundColor: ct.keyBg, alignItems: 'center', justifyContent: 'center',
                  overflow: 'hidden' }, style]}>
        {overlay}
        <Animated.View pointerEvents="none"
          style={[StyleSheet.absoluteFill, { backgroundColor: `rgba(0,0,0,${1 - dome.pressDim})` }, dim]} />
        <Animated.View pointerEvents="none" style={[styles.legend, travel]}>{children(progress)}</Animated.View>
      </Pressable>
    );
  }

  // Silver / black: the slot, the cap's cast shadow, the cap (texture drawn once), the click shading.
  const capH = Math.max(8, height - 4);
  const capR = Math.max(2, radius - 2);
  return (
    <Pressable ref={ref} onPress={disabled ? undefined : onPress} onPressIn={onIn} onPressOut={onOut}
      disabled={disabled} hitSlop={hitSlop} accessibilityRole="button" accessibilityLabel={accessibilityLabel}
      style={[{ height, borderRadius: radius, backgroundColor: dome.slotBg }, style]}
      onLayout={e => {
        const w = Math.round(e.nativeEvent.layout.width - 4);
        if (w !== lastW.current) { lastW.current = w; setCapW(w); }
      }}>
      {/* The machined lip below the slot, and the lip's shadow into it at the top. */}
      <View pointerEvents="none" style={[styles.slotLip, { backgroundColor: dome.slotLip, left: radius * 0.5, right: radius * 0.5 }]} />
      <View pointerEvents="none" style={[styles.slotShade, { backgroundColor: dome.slotShade,
        borderTopLeftRadius: radius, borderTopRightRadius: radius }]} />
      <Animated.View pointerEvents="none"
        style={[styles.cast, { top: 1.5 + 1.5, height: capH, borderRadius: capR, backgroundColor: dome.cast }, cast]} />
      <Animated.View pointerEvents="none"
        style={[styles.cap, { top: 1.5, height: capH, borderRadius: capR }, travel]}>
        {capW > 0 && <CapFace w={capW} h={capH} r={capR} dome={dome} texture={ct.plate?.texture ?? 'silver'} />}
        {/* Clicked: the cast shadow is gone (above), an inset shadow falls from the top edge and
            the whole cap darkens to brightness .84 / .82. */}
        <Animated.View style={[StyleSheet.absoluteFill, { borderRadius: capR, overflow: 'hidden' }, dim]}>
          <View style={[StyleSheet.absoluteFill, { backgroundColor: `rgba(0,0,0,${1 - dome.pressDim})` }]} />
          <View style={styles.insetTop} />
        </Animated.View>
        <View style={[StyleSheet.absoluteFill, styles.legend]}>{children(progress)}</View>
      </Animated.View>
      {overlay}
    </Pressable>
  );
});

const styles = StyleSheet.create({
  legend:    { alignItems: 'center', justifyContent: 'center', alignSelf: 'stretch', flex: 1 },
  cap:       { position: 'absolute', left: 2, right: 2, overflow: 'hidden' },
  cast:      { position: 'absolute', left: 2, right: 2 },
  slotLip:   { position: 'absolute', bottom: -1, height: 1 },
  slotShade: { position: 'absolute', left: 0, right: 0, top: 0, height: 3, opacity: 0.5 },
  insetTop:  { position: 'absolute', left: 0, right: 0, top: 0, height: 2, backgroundColor: 'rgba(0,0,0,0.45)' },
});
