/**
 * DecoderShell — the ONE frame every box over the waterfall renders inside (brief §10.1, §10.2).
 *
 * ★★ WHY IT EXISTS: each panel carried its own copy of the frame, and the copies drifted.
 *   DecoderPanel's title was 10 pt at .65 while DAB's and RDS's were 11 pt at .86; its muted text
 *   was .38 where theirs was .60; DAB padded its header 10/7 where the others used 12/8; and
 *   DecoderPanel took `theme.font`, which would have leaked Nixie One into RTTY text. The majority
 *   values won (title 11 pt / .86, padding 12 / 8) and live here, once.
 *
 * What it owns: the floating wrap, the frame (border, radius, shadow, tint / plate, blur), the
 * header row, the title, the header keys, the body's recessed window on metal, and the colour
 * TOKENS the bodies use (src/constants/decoderTokens.ts, resolved from the live faceplate).
 * What it does NOT own: the bodies. A DAB station list and a spots table share nothing but the
 * frame and the palette, and forcing them into one layout would be the drift in reverse.
 *
 * ★★★ TRANSPARENCY EFFECTS (§10.2, widened 2026-09-30 from the boxes' own Transparent / Solid row to
 *   ONE app-wide switch) — and where the per-panel tints finally unify. They used to be DecoderPanel
 *   0.95, DAB 0.94 + blur 24, RDS 0.72 + blur 35 (BIG 0.62): every one had a measured reason, and
 *   every reason was a readability / see-through trade made ONCE, for everybody. The setting hands
 *   that trade to the user (and, until they choose, to low-end detection), so:
 *     • ON  = "today's glass" — the RDS panel's 0.72 (BIG 0.62), iOS blur 35 under SMALL, on the
 *       default chassis; the same glass without blur on silver / black.
 *     • OFF = that glass made OPAQUE on default (today's colour composited over black, alpha 1.0
 *       exactly, no BlurView, no tint layer); brushed metal + recessed window on silver / black.
 *   ★ Why not keep each panel's own tint under ON: then DAB (0.94) and the decoders (0.95) would
 *     look nearly identical in both settings — a control whose every use is a no-op on two of the
 *     three boxes (AGENTS.md). DAB's measured complaint (0.72 unreadable over a hot 11A on the
 *     Xcover, 2026-09-08) is exactly the case OFF now answers, and its subtitle says so.
 *   ★★★ OFF = OPAQUE PANELS OVER A LIVE, UNDIMMED WATERFALL (Stuart): alpha 1.0 exactly (not the old
 *     0.95), the colour on the box itself (one layer, no tint child), no BlurView, and NO DROP
 *     SHADOW — the 14 pt blurred shadow is a blend over the live spectrum. (On the glass it was
 *     worse than it looked: iOS RN precomputes a shadowPath only for a view whose OWN background is
 *     > 0.999 alpha — RCTViewComponentView "Stage 1. Shadow Path" — so the glass box's shadow was
 *     drawn per pixel, offscreen, every frame the waterfall moved.) The border stays.
 *
 * ★ MEANING COLOURS ARE NOT TOKENS OF THE LOOK. good / warn / bad (and MER pink, SNR green) never
 *   follow a colour setting: a red-controls user must still see good from bad (§10.2 TRAP).
 * ★ PERFORMANCE: tokens and every style sheet built from them are memoised per setting (below), so
 *   a spectrum frame re-renders nothing here; the metal plate is ChassisPlate's cached canvas,
 *   redrawn only when the box changes size; there is no BlurView at all on silver / black, and none
 *   anywhere with Transparency OFF.
 */

import React, { createContext, useContext } from 'react';
import {
  Animated, Platform, Pressable, StyleSheet, Text, TouchableOpacity, View,
  type PressableProps, type StyleProp, type TextStyle, type ViewStyle,
} from 'react-native';
import Reanimated, { useAnimatedStyle } from 'react-native-reanimated';
import { BlurView } from 'expo-blur';
import { Fonts } from '../constants/theme';
import { useFaceplate } from '../contexts/FaceplateContext';
import { decoderTokensFor, type DecoderTokens } from '../constants/decoderTokens';
import { NO_DROP_SHADOW, type Transparency } from '../constants/faceplate';
import ChassisPlate from './ChassisPlate';
import { useDomeKey, DOME_TRAVEL } from './DomeKey';
import { CAP_SHEEN } from '../constants/capSheen';

export type { DecoderTokens } from '../constants/decoderTokens';

/** ★ Pinned Atkinson Hyperlegible — see Fonts.decoder. Never the display style (§10.2: no Nixie
 *  One, tubes, dot matrix or segments, not even in RTTY / CW text). */
export const DECODER_FONT = Fonts.decoder;

/** The box's corner radius (the plate's too). */
const RADIUS = 14;

// ── Tokens ────────────────────────────────────────────────────────────────────

/**
 * ★ A surface override for a box that is NOT a DecoderShell but borrows its chrome — the local
 * hardware sheet, an opaque settings sheet whose title and close key are the shell's. It is always
 * dark, so it must resolve the GLASS text tokens (transparency="on"): the OFF metal header's
 * engraved dark title would vanish on it. It draws its own background, so this changes only text.
 * Row 10 (PopupShell) is where that sheet takes the chassis properly.
 */
const TransparencyOverride = createContext<Transparency | null>(null);
export function DecoderSurface({ transparency, children }: { transparency: Transparency; children: React.ReactNode }) {
  return <TransparencyOverride.Provider value={transparency}>{children}</TransparencyOverride.Provider>;
}

/** The tokens every decoder body reads — live: they follow the chassis, the controls colour and
 *  TRANSPARENCY EFFECTS (the effective value: the user's choice, or the device's default until they
 *  make one). Components never hold their own palette. */
export function useDecoderTokens(): DecoderTokens {
  const fp = useFaceplate();
  const override = useContext(TransparencyOverride);
  return decoderTokensFor(fp.settings.chassis, fp.controls.rgb, override ?? fp.settings.transparency);
}

const styleCache = new WeakMap<(tk: DecoderTokens) => unknown, WeakMap<DecoderTokens, unknown>>();

/**
 * ★★ A panel's style sheet, built from the LIVE tokens — once per setting, then shared. Panels used
 * to build theirs at module load from a palette read once (`decoderTokensFor()` at import), so a
 * chassis change reached the frame and not the text inside it. `make` must be a module-level
 * function (it is the cache key); the result is cached per token object, and token objects are
 * themselves memoised per setting, so this is a lookup on every render but a build only when a
 * setting changes.
 */
export function useDecoderStyles<S>(make: (tk: DecoderTokens) => S): S {
  const tk = useDecoderTokens();
  let per = styleCache.get(make) as WeakMap<DecoderTokens, S> | undefined;
  if (!per) { per = new WeakMap(); styleCache.set(make, per as WeakMap<DecoderTokens, unknown>); }
  let s = per.get(tk);
  if (!s) { s = make(tk); per.set(tk, s); }
  return s;
}

/** Engraved header text on metal (§10.3): the lip as a hard 1 pt text shadow. Null elsewhere. */
export function engraveStyle(tk: DecoderTokens): TextStyle | null {
  return tk.engrave
    ? { textShadowColor: tk.engrave.color, textShadowOffset: { width: 0, height: tk.engrave.dy }, textShadowRadius: 0 }
    : null;
}

// ── Frame ─────────────────────────────────────────────────────────────────────

export interface DecoderShellProps {
  /** Distance from the bottom of the screen (the deck's top + a gap). */
  bottom:      number;
  /** ★ The content's width, not a guess — each panel states why its number is what it is. */
  maxWidth:    number;
  maxHeight?:  number;
  /** BIG mode: the glass is thinner (0.62) and never blurred — see decoderTokens. (OFF: the same
   *  opaque colour as SMALL.) */
  tall?:       boolean;
  /** Replaces the border colour — the keyboard-focus ring. */
  borderColor?: string;
  /** Extra style on the outer wrap — DecoderPanel animates its opacity and slide here. */
  wrapStyle?:  any;
  onTouchStart?: () => void;
  children:    React.ReactNode;
}

export function DecoderShell({ bottom, maxWidth, maxHeight, tall = false, borderColor,
  wrapStyle, onTouchStart, children }: DecoderShellProps) {
  const tk = useDecoderTokens();
  const plate = useFaceplate().chassis.plate;
  const metal = tk.surface === 'metal' && plate != null;
  // ★★★ Transparency OFF on default: one opaque colour ON this view — no BlurView, no tint layer,
  //   no drop shadow (any chassis). See the header.
  const opaque = tk.transparency === 'off';
  const solid = !metal ? tk.solidBg : null;
  const blur = !metal && !solid && !tall ? tk.blur : 0;
  return (
    /* ★ box-none: the wrap spans the screen's width so it can centre the box; without this its
       empty sides ate taps meant for the waterfall on anything wider than the box. */
    <Animated.View style={[sh.wrap, { bottom }, wrapStyle]} pointerEvents="box-none">
      <View style={[sh.inner, { maxWidth, borderColor: borderColor ?? tk.border },
                    // Silver / black, Transparency ON: the controls-colour glow round the glass
                    // (mockup `0 0 8px L(.18)`) as well as the drop shadow.
                    tk.glow != null && { boxShadow: `0 0 8px ${tk.glow}, 0 4px 14px rgba(0,0,0,0.8)` },
                    metal && { backgroundColor: plate.base },
                    solid != null && { backgroundColor: solid },
                    opaque && NO_DROP_SHADOW,
                    maxHeight != null && { maxHeight }]}
            onTouchStart={onTouchStart}>
        {metal ? (
          // ★ Opaque brushed metal, drawn once (ChassisPlate caches it until the box resizes).
          <ChassisPlate plate={plate} radius={RADIUS} />
        ) : solid != null ? null : (<>
          {Platform.OS === 'ios' && blur > 0 && (
            <BlurView intensity={blur} tint="dark" style={StyleSheet.absoluteFill} />
          )}
          <View style={[StyleSheet.absoluteFill,
                        { backgroundColor: `rgba(${tk.tintRgb},${tall ? tk.tintTall : tk.tint})` }]}
                pointerEvents="none" />
        </>)}
        {children}
      </View>
    </Animated.View>
  );
}

/**
 * Everything below the header. On glass it is nothing at all (the children render exactly as they
 * did); on metal it is the recessed dark window the data sits in (mockup: #070605, margin 0 8 8,
 * radius 6, an inner shadow at the top and a light lip below), so the read-outs keep their
 * light-on-dark colours on a silver plate.
 * ★ The window's 8 pt bottom margin is height a panel must budget for — `decoderBodyInset(tk)`.
 */
export function DecoderBody({ style, children }: { style?: StyleProp<ViewStyle>; children: React.ReactNode }) {
  const tk = useDecoderTokens();
  const w = tk.window;
  if (!w) return <>{children}</>;
  return (
    <View style={[sh.window, { marginHorizontal: w.inset, marginBottom: w.inset, borderRadius: w.radius,
                               backgroundColor: w.bg }, style]}>
      {children}
      <View pointerEvents="none" style={[sh.windowShade, { borderTopLeftRadius: w.radius, borderTopRightRadius: w.radius }]} />
      <View pointerEvents="none" style={[sh.windowLip, { backgroundColor: w.lip, left: w.radius, right: w.radius }]} />
    </View>
  );
}

/** Height the body's window adds below it (0 on glass) — for panels that size their body from
 *  the screen and must not let BIG grow past the notch. */
export function decoderBodyInset(tk: DecoderTokens): number {
  return tk.window ? tk.window.inset : 0;
}

// ── Header ────────────────────────────────────────────────────────────────────

/** The header row. Pressable when `onPress` is given (DecoderPanel's tap-to-minimise). */
export function DecoderHeader({ onPress, style, children }: {
  onPress?: () => void; style?: StyleProp<ViewStyle>; children: React.ReactNode;
}) {
  const tk = useDecoderTokens();
  // Metal: no hairline, 6 pt under the header before the window (mockup `padding-bottom: 6px`).
  const st = [sh.header, tk.hdrBdr ? { borderBottomColor: tk.hdrBdr } : sh.headerMetal, style];
  if (onPress) {
    return <TouchableOpacity style={st} onPress={onPress} activeOpacity={0.85}>{children}</TouchableOpacity>;
  }
  return <View style={st}>{children}</View>;
}

export function DecoderTitle({ minimised = false, style, children }: {
  minimised?: boolean; style?: StyleProp<TextStyle>; children: React.ReactNode;
}) {
  const tk = useDecoderTokens();
  return (
    <Text style={[sh.title, { color: minimised ? tk.titleMin : tk.title }, engraveStyle(tk), style]}
          numberOfLines={1}>
      {children}
    </Text>
  );
}

export type DecoderKeyTone = 'normal' | 'accent' | 'close';

/** Style of a default-chassis (outline) header key's box. `active` = a latched state. */
export function decoderKeyStyle(tk: DecoderTokens, active = false): ViewStyle {
  return active
    ? { ...sh.key, borderColor: tk.keyBorderAct, backgroundColor: tk.keyBgAct }
    : { ...sh.key, borderColor: tk.keyBorder };
}

/** Style of a header key's legend. `accent` lights the legend without latching the key (SAVE). */
export function decoderKeyTextStyle(tk: DecoderTokens, active = false,
                                    tone: DecoderKeyTone = 'normal'): TextStyle {
  const color = tone === 'close' ? tk.close
              : (active || tone === 'accent') ? tk.keyTextAct : tk.keyText;
  return {
    ...sh.keyTxt, color,
    // The cap's lit legend glows (mockup `text-shadow: 0 0 4px L(.55)`).
    ...(tk.keyGlow ? { textShadowColor: tk.keyGlow, textShadowRadius: 4, textShadowOffset: { width: 0, height: 0 } } : null),
  };
}

/**
 * A header key's legend. ★ Use this, not a bare Text, for a key whose legend is not a plain
 * `label`: on silver it lays the deck keys' dark shade under the lit colour (§5), which a single
 * RN text shadow cannot do alongside the glow.
 */
export function DecoderKeyLabel({ active = false, tone = 'normal', style, children }: {
  active?: boolean; tone?: DecoderKeyTone; style?: StyleProp<TextStyle>; children: React.ReactNode;
}) {
  const tk = useDecoderTokens();
  const base = decoderKeyTextStyle(tk, active, tone);
  if (!tk.keyShade) return <Text style={[base, style]}>{children}</Text>;
  return (
    <View>
      <Text style={[base, style, sh.keyShade, { color: tk.keyShade }]}>{children}</Text>
      <Text style={[base, style]}>{children}</Text>
    </View>
  );
}

const AnimatedPressable = Reanimated.createAnimatedComponent(Pressable);

export type DecoderKeyProps = Omit<PressableProps, 'style' | 'children'> & {
  label?: string; active?: boolean; tone?: DecoderKeyTone;
  style?: StyleProp<ViewStyle>; textStyle?: StyleProp<TextStyle>;
  children?: React.ReactNode;
};

/**
 * A header key — a DOME KEY on every chassis (§5, §10: "the decoder header keys, on every chassis,
 * including default, where they keep today's look but still use useDomeKey()").
 *   • default — today's outline key at rest; the press snaps the legend down 2 pt and dims the face.
 *   • silver / black — the mockup's cap (silver #c4c1ba / black #1c1d20 → #0c0c0e, a 1 pt top
 *     highlight, a cast shadow that goes on the click), legend lit in the controls colour.
 * ★ Snap, click and dim on press-IN (useDomeKey: 45 ms curve on the UI thread, haptic at its end,
 *   gated on the controls-haptics switch); the action on release, as every dome key.
 * ★ Every header key in every panel must come through here rather than a local Touchable, or it
 *   will be the one key that does not click.
 */
export const DecoderKey = React.forwardRef<View, DecoderKeyProps>(function DecoderKey(
  { label, active = false, tone = 'normal', style, textStyle, children, disabled,
    onPressIn, onPressOut, ...rest }, ref) {
  const tk = useDecoderTokens();
  const { progress, pressIn, pressOut } = useDomeKey({ silent: true });   // ★ not front panel: snaps, no click
  const cap = tk.cap;

  const travel = useAnimatedStyle(() => ({ transform: [{ translateY: progress.value * DOME_TRAVEL }] }));
  const dim = useAnimatedStyle(() => ({ opacity: Math.max(0, Math.min(1, progress.value)) }));
  const castOpacity = cap?.castOpacity ?? 0;
  const capMotion = useAnimatedStyle(() => ({
    transform: [{ translateY: progress.value * DOME_TRAVEL }],
    // The cast shadow is gone when the cap is down (§5).
    shadowOpacity: castOpacity * (1 - Math.max(0, Math.min(1, progress.value))),
  }), [castOpacity]);

  const onIn  = disabled ? undefined : (e: any) => { pressIn(); onPressIn?.(e); };
  const onOut = disabled ? undefined : (e: any) => { pressOut(); onPressOut?.(e); };
  const legend = label != null
    ? <DecoderKeyLabel active={active} tone={tone} style={textStyle}>{label}</DecoderKeyLabel>
    : children;

  if (!cap) {
    return (
      <Pressable ref={ref} accessibilityRole="button" disabled={disabled} onPressIn={onIn} onPressOut={onOut}
        style={[decoderKeyStyle(tk, active), sh.keyClip, style]} {...rest}>
        <Reanimated.View pointerEvents="none"
          style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(0,0,0,0.16)' }, dim]} />
        <Reanimated.View style={travel}>{legend}</Reanimated.View>
      </Pressable>
    );
  }

  return (
    <AnimatedPressable ref={ref as any} accessibilityRole="button" disabled={disabled}
      onPressIn={onIn} onPressOut={onOut}
      style={[sh.key, sh.cap, { backgroundColor: cap.base, borderColor: cap.border, shadowColor: cap.cast,
                                shadowOpacity: cap.castOpacity }, style, capMotion]}
      {...rest}>
      {/* The cap's sheen: light over the top 45 %, a shade at the foot, a 1 pt highlight on the edge. */}
      {/* ★★★ Flex shares of a layer pinned to the cap, never percentage heights — a decoder key in a
          multi-line wrap (the mode grid) would paint the ghost slabs the chat drawer did (capSheen.ts). */}
      <View pointerEvents="none" style={sh.capSheen}>
        <View style={[sh.capHi, { backgroundColor: cap.hi }]} />
        <View style={sh.capMid} />
        <View style={[sh.capLo, { backgroundColor: cap.lo }]} />
      </View>
      <View pointerEvents="none" style={[sh.capTop, { backgroundColor: cap.topLine }]} />
      {/* Clicked: brightness .84 / .82 and an inset shadow from the top edge. */}
      <Reanimated.View pointerEvents="none" style={[sh.capDown, dim]}>
        <View style={[StyleSheet.absoluteFill, { backgroundColor: `rgba(0,0,0,${1 - cap.pressDim})` }]} />
        <View style={sh.capInset} />
      </Reanimated.View>
      {legend}
    </AnimatedPressable>
  );
});

const sh = StyleSheet.create({
  wrap:  { position: 'absolute', left: 8, right: 8, zIndex: 200, alignItems: 'center' },
  inner: {
    width: '100%',
    borderWidth: 1, borderRadius: RADIUS,
    shadowColor: '#000', shadowOffset: { width: 0, height: 4 },
    shadowOpacity: 0.80, shadowRadius: 14, elevation: 16,
    overflow: 'hidden',
  },
  window: {
    flexShrink: 1, borderWidth: 1, borderColor: 'rgba(0,0,0,0.8)',
  },
  windowShade: { position: 'absolute', left: 0, right: 0, top: 0, height: 4, backgroundColor: 'rgba(0,0,0,0.55)' },
  windowLip:   { position: 'absolute', bottom: -2, height: 1 },
  header: {
    flexDirection: 'row', alignItems: 'center', gap: 6,
    paddingHorizontal: 12, paddingVertical: 8,
    borderBottomWidth: StyleSheet.hairlineWidth,
  },
  headerMetal: { borderBottomWidth: 0, paddingBottom: 6 },
  title:  { fontSize: 11, letterSpacing: 2, fontFamily: DECODER_FONT, flexShrink: 0 },
  key:    { borderWidth: 1, borderRadius: 4, paddingHorizontal: 8, paddingVertical: 3 },
  keyClip: { overflow: 'hidden' },
  keyTxt: { fontFamily: DECODER_FONT, fontSize: 11 },
  keyShade: { position: 'absolute', left: 0, right: 0, top: -0.5, textShadowRadius: 0 },
  cap:     { borderRadius: 5, shadowOffset: { width: 0, height: 1.5 }, shadowRadius: 1, elevation: 1 },
  ...CAP_SHEEN(4),
  capTop:  { position: 'absolute', left: 3, right: 3, top: 0, height: 1 },
  capDown: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, borderRadius: 4, overflow: 'hidden' },
  capInset: { position: 'absolute', left: 0, right: 0, top: 0, height: 2, backgroundColor: 'rgba(0,0,0,0.45)' },
});
