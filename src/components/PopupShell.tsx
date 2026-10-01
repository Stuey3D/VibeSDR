/**
 * PopupShell — popups, menus and chat take the chassis (faceplates brief §10.3).
 *
 * > "Popup boxes can they be styled to the controls please same with menus too … be weird having a
 * >  light control scheme with black popups … and chat menu too." — Stuart
 *
 * ONE set of parts for every popup — FreqModal (Tune / Bookmarks), AudioSheet, MenuSheet,
 * ChatDrawer, RecordingsOverlay, KeyboardShortcuts, PasswordModal, IdentModal, CityPickerModal,
 * AboutOverlay, StepPicker — the way DecoderShell is one frame for every decoder box (§10.1):
 *
 *   usePopupTheme() / usePopupStyles()  the palette (src/constants/popupTokens.ts), per setting
 *   PopupScrim      the full-screen tap-to-close view; DIMS only while Transparency is ON
 *   PopupPlate      silver / black: the deck's brushed plate (texture + lighting), OPAQUE, drawn once
 *   PopupHandle     the grab handle — today's pill, or an ENGRAVED GROOVE on metal
 *   PopupKey        silver / black: a DOME KEY (useDomeKey — snap + click), engraved legend, and the
 *                   input-selector LED PIP on every key in an exclusive / toggle group
 *   PopupWindow     silver / black: the recessed dark window every piece of DATA sits in
 *   PopupFader      silver / black: the slide fader that replaces a slider
 *
 * ★★★ THE DEFAULT CHASSIS IS TODAY, PIXEL FOR PIXEL (§10.3). Every popup keeps its own default JSX
 *   and asks `pt.metal` before drawing a part from here; the tokens it reads on default are today's
 *   literals (test_popup proves it). Nothing here draws on the default chassis except the scrim —
 *   which is today's dim with Transparency ON.
 * ★★★ TRANSPARENCY OFF (§10.2, Stuart: "opaque panels over a live, undimmed waterfall"): the scrim
 *   keeps its tap-to-close view and loses its colour (`useSurface().scrimOpacity` 0); a glass
 *   popup's fill goes through `useSurface().fill()` (alpha 1.0, today's colour over black); drop
 *   shadows go (NO_DROP_SHADOW); and there is no BlurView. Silver / black are opaque plates in both
 *   settings — no BlurView ever (§3.4) — so only the scrim and the shadow change there.
 * ★★ THE WATERFALL NEVER STOPS DRAWING under a popup. Nothing here pauses, skips or throttles a
 *   frame; a popup is a view on top, nothing more.
 * ★★ SILVER MEANS DARK TEXT ON A LIGHT PLATE (§10.3 TRAP). Light text belongs in a PopupWindow;
 *   anything left on the plate is an engraved label (`engraveText(pt)`).
 */

import React, { useCallback, useRef, useState } from 'react';
import {
  Animated, PanResponder, Pressable, StyleSheet, Text, View,
  type GestureResponderEvent, type Insets, type StyleProp, type TextStyle, type ViewStyle,
} from 'react-native';
import Reanimated, { useAnimatedStyle } from 'react-native-reanimated';
import { useFaceplate, useSurface } from '../contexts/FaceplateContext';
import { popupTokensFor, type PopupTokens } from '../constants/popupTokens';
import { NO_DROP_SHADOW, type PlateTokens } from '../constants/faceplate';
import ChassisPlate from './ChassisPlate';
import { useDomeKey, DOME_TRAVEL } from './DomeKey';
import { NAV_FOCUS } from './PanelNav';
import { CAP_SHEEN } from '../constants/capSheen';

export type { PopupTokens } from '../constants/popupTokens';
/** ★ The scroll indicator's lane (popupTokens.ts) — every popup scroller takes it from here. */
export { SCROLL_LANE, scrollLane, scrollLaneOutset } from '../constants/popupTokens';

export const POPUP_FONT = 'Atkinson Hyperlegible';

// ── Tokens ────────────────────────────────────────────────────────────────────

/** The popups' palette — live: follows the chassis, the controls colour and the resolved text
 *  colour (neon under Nixie). Components never hold their own gold. */
export function usePopupTheme(): PopupTokens {
  const fp = useFaceplate();
  return popupTokensFor(fp.settings.chassis, fp.controls.rgb, fp.text.rgb, fp.settings.display);
}

const styleCache = new WeakMap<(pt: PopupTokens) => unknown, WeakMap<PopupTokens, unknown>>();

/**
 * ★ A popup's style sheet, built from the LIVE tokens — once per setting, then shared (the same
 * pattern as DecoderShell's useDecoderStyles). `make` must be a module-level function: it is the
 * cache key. Token objects are memoised per setting, so this is a lookup on every render and a build
 * only when a setting changes.
 */
export function usePopupStyles<S>(make: (pt: PopupTokens) => S): S {
  const pt = usePopupTheme();
  let per = styleCache.get(make) as WeakMap<PopupTokens, S> | undefined;
  if (!per) { per = new WeakMap(); styleCache.set(make, per as WeakMap<PopupTokens, unknown>); }
  let s = per.get(pt);
  if (!s) { s = make(pt); per.set(pt, s); }
  return s;
}

/** `base` on default; `base` + `metal` on silver / black — for a makeStyles() entry. */
export function onMetal<T extends object>(pt: PopupTokens, base: T, metal: object): T {
  return pt.metal ? { ...base, ...metal } as T : base;
}

/** Engraved text on the plate (§10.3): the label colour, Atkinson, the lip as a hard 1 pt shadow. */
export function engraveText(pt: PopupTokens, color: string = pt.label): TextStyle {
  return {
    color, fontFamily: POPUP_FONT,
    ...(pt.engrave ? { textShadowColor: pt.engrave.color, textShadowOffset: { width: 0, height: pt.engrave.dy },
                       textShadowRadius: 0.01 } : null),
  };
}

/** A recessed window's own style (for a TextInput or a list that IS the window). */
export function windowStyle(pt: PopupTokens): ViewStyle {
  return { backgroundColor: pt.window.bg, borderWidth: 1, borderColor: pt.window.border, borderRadius: pt.window.radius };
}

/** Drop the colour keys from a caller's style (a default key's tint must not reach a metal cap). */
function layoutOnly(style: StyleProp<ViewStyle>): ViewStyle | undefined {
  if (!style) return undefined;
  const f = { ...(StyleSheet.flatten(style) as ViewStyle) };
  delete f.backgroundColor; delete f.borderColor; delete f.borderWidth; delete f.borderRadius;
  delete (f as any).borderTopColor; delete (f as any).borderBottomColor;
  delete f.paddingVertical; delete f.paddingHorizontal; delete f.padding;
  return f;
}

// ── Scrim ─────────────────────────────────────────────────────────────────────

/**
 * ★★★ The full-screen tap-to-close view. Transparency ON: today's dim (`color`, faded by `opacity`
 * when the popup animates it). OFF: the SAME view with no colour — the dim is a full-screen blend
 * over the live waterfall, and an invisible view costs nothing to composite — so a tap outside still
 * closes the popup. `style` is the caller's own layout (flex: 1 in a column, absoluteFill, …).
 */
export function PopupScrim({ onPress, color, opacity, style, onTouchStart, children }: {
  onPress?: () => void; color: string; opacity?: Animated.Value | Animated.AnimatedInterpolation<number>;
  style?: StyleProp<ViewStyle>; onTouchStart?: () => void; children?: React.ReactNode;
}) {
  const surface = useSurface();
  const dim = surface.scrimOpacity > 0;
  const bg: ViewStyle = { backgroundColor: dim ? color : 'transparent' };
  if (opacity != null) {
    return (
      <Pressable style={[StyleSheet.absoluteFill, style]} onPress={onPress} onTouchStart={onTouchStart}
                 accessibilityRole="button" accessibilityLabel="Close">
        <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, bg, { opacity }]} />
        {children}
      </Pressable>
    );
  }
  return (
    <Pressable style={[style, bg]} onPress={onPress} onTouchStart={onTouchStart}
               accessibilityRole="button" accessibilityLabel="Close">
      {children}
    </Pressable>
  );
}

/** The surface contract for a glass popup that draws its own background: its colour at alpha 1.0
 *  with Transparency OFF, and its drop shadow off. `fill(c)` is today's `c` when ON. */
export function usePopupSurface() {
  const surface = useSurface();
  return {
    fill: surface.fill,
    blur: surface.blur,
    /** Spread onto the view that casts the shadow: nothing when ON, NO_DROP_SHADOW when OFF. */
    shadow: surface.dropShadow ? null : NO_DROP_SHADOW,
    opaque: surface.opaque,
  };
}

// ── Plate + handle ────────────────────────────────────────────────────────────

const plateNoScrews = new WeakMap<PlateTokens, PlateTokens>();
/** The deck's plate WITHOUT its corner screws — the mockup's popups have none. */
function popupPlate(p: PlateTokens): PlateTokens {
  let q = plateNoScrews.get(p);
  if (!q) { q = { ...p, screws: false }; plateNoScrews.set(p, q); }
  return q;
}

/**
 * Silver / black: the brushed plate behind a popup — the deck's own texture and lighting layer
 * (§3.2 / §3.3), opaque, drawn ONCE into a cached Skia canvas (ChassisPlate redraws only when the
 * popup changes size). Put it first inside the popup's container, which must `overflow: 'hidden'`.
 * Returns null on the default chassis. Includes the chassis-edge border and the top highlight.
 */
export function PopupPlate({ radius = 16 }: { radius?: number }) {
  const plate = useFaceplate().chassis.plate;
  if (!plate) return null;
  return <ChassisPlate plate={popupPlate(plate)} radius={radius} />;
}

/** The container style a metal popup needs: its plate colour (seen for a frame before the texture
 *  decodes), the 1 pt chassis-edge border, and no drop shadow with Transparency OFF. */
export function usePopupFrame(radius = 16, sheet = true): ViewStyle | null {
  const pt = usePopupTheme();
  const plate = useFaceplate().chassis.plate;
  const surface = useSurface();
  if (!pt.metal || !plate) return null;
  return {
    backgroundColor: plate.base, borderColor: pt.shell.border, borderWidth: 1,
    ...(sheet ? { borderBottomWidth: 0, borderTopLeftRadius: radius, borderTopRightRadius: radius }
              : { borderRadius: radius }),
    overflow: 'hidden',
    ...(surface.dropShadow ? { shadowColor: '#000', shadowOpacity: 0.8, shadowRadius: 9, shadowOffset: { width: 0, height: -4 } }
                           : NO_DROP_SHADOW),
  };
}

/** The grab handle: on metal an engraved groove (silver rgba(0,0,0,.38) over a white 1 pt lip);
 *  on default whatever `children` (today's pill) the popup draws. */
export function PopupHandle({ children, style }: { children?: React.ReactNode; style?: StyleProp<ViewStyle> }) {
  const pt = usePopupTheme();
  if (!pt.metal) return <>{children}</>;
  return (
    <View style={[{ alignItems: 'center', paddingTop: 8, paddingBottom: 4 }, style]} pointerEvents="none">
      <View style={{ width: 38, height: 4, borderRadius: 2, backgroundColor: pt.shell.handle }} />
      <View style={{ width: 34, height: 1, marginTop: 0, backgroundColor: pt.shell.handleLip }} />
    </View>
  );
}

// ── Window ────────────────────────────────────────────────────────────────────

/**
 * The recessed dark window (§10.3): #070605, an inner shadow at the top, a 1 pt light lip below
 * (silver α .8, black α .14). Everything that is DATA sits in one — the tune station and entry, the
 * SIGNAL readout, the chat thread, every text input — so it keeps light-on-dark colours on a silver
 * plate. On default it is a plain View with `style` (the popup's own container), so a caller can
 * wrap unconditionally.
 */
export function PopupWindow({ style, metalStyle, children, onLayout }: {
  style?: StyleProp<ViewStyle>; metalStyle?: StyleProp<ViewStyle>; children?: React.ReactNode;
  onLayout?: (e: any) => void;
}) {
  const pt = usePopupTheme();
  if (!pt.metal) return <View style={style} onLayout={onLayout}>{children}</View>;
  const w = pt.window;
  return (
    <View style={[style, windowStyle(pt), metalStyle]} onLayout={onLayout}>
      <View pointerEvents="none" style={[ps.winShade, { backgroundColor: w.shade, borderTopLeftRadius: w.radius, borderTopRightRadius: w.radius }]} />
      {children}
      <View pointerEvents="none" style={[ps.winLip, { backgroundColor: w.lip, left: w.radius, right: w.radius }]} />
    </View>
  );
}

// ── Keys ──────────────────────────────────────────────────────────────────────

const AnimatedPressable = Reanimated.createAnimatedComponent(Pressable);

export interface PopupKeyProps {
  label?: string;
  /** A custom legend (an icon) — drawn in `legendColor(lit)`. */
  children?: (legendColor: string) => React.ReactNode;
  /** Selected / on. Lights the pip AND the legend (§10.3). */
  active?: boolean;
  /** A member of an exclusive / toggle group: carries the pip (lit or unlit). Plain action keys
   *  (RECORDINGS, MIN / MAX, CLOSE) do not. */
  pip?: boolean;
  /** The primary action (TUNE ▶, chat send): legend lit in the controls colour, no pip. */
  primary?: boolean;
  /** A destructive action: red legend (a meaning colour). */
  danger?: boolean;
  /** A colour key: a lit LED dot in this colour instead of a legend (CONTROL CUSTOMISATION). */
  dot?: string;
  /** An icon drawn before the label, in the legend's colour. */
  icon?: (legendColor: string) => React.ReactNode;
  onPress?: () => void;
  onPressIn?: (e: GestureResponderEvent) => void;
  onPressOut?: (e: GestureResponderEvent) => void;
  disabled?: boolean;
  /** Layout only (flex, width, margins, opacity) — colours are the cap's. */
  style?: StyleProp<ViewStyle>;
  height?: number;
  fontSize?: number;
  legendStyle?: StyleProp<TextStyle>;
  numberOfLines?: number;
  /** Keyboard / D-pad focus (PanelNav) — a ring outranks everything. */
  focused?: boolean;
  hitSlop?: number | Insets;
  accessibilityLabel?: string;
  accessibilityHint?: string;
}

/**
 * ★★ A DOME KEY for a popup on silver / black (§10.3 "every button is a dome key", §5): the
 * mockup's cap (silver #c4c1ba under a sheen; black #1c1d20 → #0c0c0e), a cast shadow that goes on
 * the click and 2 pt of travel from useDomeKey() — SILENT: only the front panel clicks (Stuart). The
 * action fires on release, as every dome key.
 * ★ Legends are ENGRAVED (silver #2a2824 / black #cfd2d7); lit ones take the controls colour with
 *   its glow (on silver with the deck keys' dark shade under it, which a single RN text shadow
 *   cannot do beside the glow).
 * ★ Default chassis: never drawn — callers keep today's key (see the file header).
 */
export const PopupKey = React.forwardRef<View, PopupKeyProps>(function PopupKey({
  label, children, active = false, pip = false, primary = false, danger = false, dot, icon, onPress, onPressIn, onPressOut,
  disabled, style, height = 34, fontSize = 11, legendStyle, numberOfLines = 1, focused, hitSlop = 4,
  accessibilityLabel, accessibilityHint,
}, ref) {
  const pt = usePopupTheme();
  const { progress, pressIn, pressOut } = useDomeKey({ silent: true });   // ★ not front panel: snaps, no click
  const silver = pt.silver;
  const lit = active || primary;
  const legendColor = danger ? pt.danger : lit ? pt.legendLit : pt.legend;

  const motion = useAnimatedStyle(() => ({
    transform: [{ translateY: progress.value * DOME_TRAVEL }],
    // The cast shadow is gone when the cap is down (§5).
    shadowOpacity: (silver ? 0.6 : 0.85) * (1 - Math.max(0, Math.min(1, progress.value))),
  }), [silver]);
  const dim = useAnimatedStyle(() => ({ opacity: Math.max(0, Math.min(1, progress.value)) }));

  const onIn  = disabled ? undefined : (e: GestureResponderEvent) => { pressIn(); onPressIn?.(e); };
  const onOut = disabled ? undefined : (e: GestureResponderEvent) => { pressOut(); onPressOut?.(e); };

  const legendBase: TextStyle = {
    fontFamily: POPUP_FONT, fontSize, fontWeight: '700', letterSpacing: 1, color: legendColor, textAlign: 'center',
    ...(lit && !danger
      ? { textShadowColor: pt.legendLitGlow, textShadowRadius: 4, textShadowOffset: { width: 0, height: 0 } }
      : pt.legendShadow ? { textShadowColor: pt.legendShadow.color, textShadowRadius: 0.01,
                            textShadowOffset: { width: 0, height: pt.legendShadow.dy } } : null),
  };

  return (
    <AnimatedPressable ref={ref as any} accessibilityRole="button" disabled={disabled}
      accessibilityState={{ selected: active, disabled }} accessibilityLabel={accessibilityLabel ?? label}
      accessibilityHint={accessibilityHint}
      onPress={disabled ? undefined : onPress} onPressIn={onIn} onPressOut={onOut} hitSlop={hitSlop}
      style={[ps.cap, { minHeight: height, backgroundColor: silver ? '#c4c1ba' : '#111214',
                        borderColor: silver ? '#8d8a83' : '#050505' },
              layoutOnly(style), focused && { borderColor: NAV_FOCUS, borderWidth: 2 },
              disabled && { opacity: 0.45 }, motion]}>
      {/* The cap's sheen: light over the top 45 %, a shade at the foot, a 1 pt highlight on the edge.
          ★★★ The 45 / 35 % are FLEX SHARES of a layer pinned to the cap (CAP_SHEEN), never `height:
          '45%'` — see CAP_SHEEN for the chat drawer's ghost slabs that percentage heights drew. */}
      <View pointerEvents="none" style={ps.capSheen}>
        <View style={[ps.capHi, { backgroundColor: silver ? 'rgba(255,255,255,0.40)' : 'rgba(255,255,255,0.05)' }]} />
        <View style={ps.capMid} />
        <View style={[ps.capLo, { backgroundColor: silver ? 'rgba(0,0,0,0.12)' : 'rgba(0,0,0,0.30)' }]} />
      </View>
      <View pointerEvents="none" style={[ps.capTop, { backgroundColor: silver ? 'rgba(255,255,255,0.80)' : 'rgba(255,255,255,0.22)' }]} />
      {/* Clicked: brightness .84 / .82 and an inset shadow from the top edge. */}
      <Reanimated.View pointerEvents="none" style={[ps.capDown, dim]}>
        <View style={[StyleSheet.absoluteFill, { backgroundColor: `rgba(0,0,0,${silver ? 0.16 : 0.18})` }]} />
        <View style={ps.capInset} />
      </Reanimated.View>
      {pip && (
        <View pointerEvents="none"
          style={[ps.pip, active
            ? { backgroundColor: pt.pipOn, shadowColor: pt.pipGlow, shadowOpacity: 1, shadowRadius: 3,
                shadowOffset: { width: 0, height: 0 } }
            : { backgroundColor: pt.pipOff, borderTopWidth: 1, borderTopColor: pt.pipOffInset }]} />
      )}
      {dot ? (
        <View style={[ps.dot, { backgroundColor: dot, shadowColor: dot }]} />
      ) : children ? children(legendColor) : (
        <View style={icon ? ps.iconRow : undefined}>
          {icon?.(legendColor)}
          {lit && pt.legendLitShade && !danger && !icon && (
            <Text numberOfLines={numberOfLines} style={[legendBase, legendStyle, ps.shade,
                  { color: pt.legendLitShade, textShadowRadius: 0 }]}>{label}</Text>
          )}
          <Text numberOfLines={numberOfLines} style={[legendBase, legendStyle]}>{label}</Text>
        </View>
      )}
    </AnimatedPressable>
  );
});

// ── Fader ─────────────────────────────────────────────────────────────────────

/**
 * ★ The SLIDE FADER (§10.3) that replaces a slider on silver / black: a recessed slot, the fill in
 * the controls colour, an optional live LEVEL behind it (squelch: the signal at white α .22), and a
 * brushed 20 × 18 cap with a lit index line. Same value contract as @react-native-community/slider
 * (min / max / step / value / onValueChange), so a caller swaps one for the other.
 * ★ The drag uses pageX against the slot's MEASURED window position — never locationX, which
 *   changes meaning the moment the cap slides under the finger (AudioSheet's SquelchBar learnt that).
 * ★ Capture phase, and it never hands the touch back mid-drag: the popups are ScrollViews, which
 *   otherwise steal a drag with any vertical component.
 */
export function PopupFader({ value, minimumValue = 0, maximumValue = 1, step, onValueChange,
  onSlidingComplete, level, active = true, lineColor, style, focused, innerRef, disabled, fillFrom = 'left' }: {
  value: number; minimumValue?: number; maximumValue?: number; step?: number;
  onValueChange?: (v: number) => void; onSlidingComplete?: (v: number) => void;
  /** 0..1 live level drawn behind the fill (squelch), or undefined for none. */
  level?: number;
  /** false = the fill is unlit (the setting is OFF) — the cap stays, parked. */
  active?: boolean;
  /** Override the index line (squelch: red while the gate is muting). */
  lineColor?: string;
  style?: StyleProp<ViewStyle>; focused?: boolean; innerRef?: React.Ref<View>; disabled?: boolean;
  /** Which end the fill runs from to the cap: 'right' for a mirrored control whose value is on the
   *  cap's right (the demodulator sheet's LOWER passband edge — its passband is toward the carrier). */
  fillFrom?: 'left' | 'right';
}) {
  const pt = usePopupTheme();
  const f = pt.fader;
  const geo = useRef({ x: 0, w: 1 });
  const box = useRef<View | null>(null);
  const [w, setW] = useState(0);
  const range = Math.max(1e-9, maximumValue - minimumValue);
  const frac = Math.max(0, Math.min(1, (value - minimumValue) / range));

  const measure = useCallback(() => {
    box.current?.measureInWindow((x, _y, width) => { geo.current = { x, w: Math.max(1, width) }; });
  }, []);
  const cb = useRef({ onValueChange, onSlidingComplete, minimumValue, range, step, value });
  cb.current = { onValueChange, onSlidingComplete, minimumValue, range, step, value };
  const at = (pageX: number) => {
    const { x, w: ww } = geo.current;
    const { minimumValue: mn, range: rg, step: st } = cb.current;
    // The cap's centre travels 10 pt in from each end of the slot.
    const t = Math.max(0, Math.min(1, (pageX - x - 10) / Math.max(1, ww - 20)));
    let v = mn + t * rg;
    if (st && st > 0) v = mn + Math.round((v - mn) / st) * st;
    return Math.max(mn, Math.min(mn + rg, v));
  };
  const last = useRef(value);
  const pan = React.useMemo(() => PanResponder.create({
    onStartShouldSetPanResponderCapture: () => !disabled,
    onMoveShouldSetPanResponderCapture: () => !disabled,
    onPanResponderTerminationRequest: () => false,
    onShouldBlockNativeResponder: () => true,
    onPanResponderGrant: (e) => {
      measure();
      const v = at(e.nativeEvent.pageX); last.current = v;
      if (v !== cb.current.value) cb.current.onValueChange?.(v);
    },
    onPanResponderMove: (e) => {
      const v = at(e.nativeEvent.pageX);
      if (v !== last.current) { last.current = v; cb.current.onValueChange?.(v); }
    },
    onPanResponderRelease: () => cb.current.onSlidingComplete?.(last.current),
    onPanResponderTerminate: () => cb.current.onSlidingComplete?.(last.current),
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }), [disabled, measure]);

  const capX = 10 + frac * Math.max(0, w - 20);
  const lvl = level == null ? null : Math.max(0, Math.min(1, level));
  return (
    <View ref={(r: any) => { box.current = r; if (typeof innerRef === 'function') innerRef(r); else if (innerRef) (innerRef as any).current = r; }}
      style={[ps.fader, style, focused && ps.faderFocus, disabled && { opacity: 0.45 }]}
      onLayout={(e) => { setW(e.nativeEvent.layout.width); measure(); }}
      hitSlop={{ top: 10, bottom: 10, left: 6, right: 6 }}
      accessibilityRole="adjustable" accessibilityValue={{ min: minimumValue, max: maximumValue, now: value }}
      {...pan.panHandlers}>
      <View pointerEvents="none" style={[ps.slot, { backgroundColor: f.slot }]} />
      <View pointerEvents="none" style={[ps.slotLip, { backgroundColor: f.slotLip }]} />
      {lvl != null && (
        <View pointerEvents="none" style={[ps.fill, { width: Math.max(0, lvl * (w - 2)), backgroundColor: f.level }]} />
      )}
      {active && (
        <View pointerEvents="none" style={[ps.fill, fillFrom === 'right'
          ? { left: capX, width: Math.max(0, w - capX - 1) }
          : { width: Math.max(0, capX - 1) }, { backgroundColor: f.fill,
               shadowColor: f.fillGlow, shadowOpacity: 1, shadowRadius: 3, shadowOffset: { width: 0, height: 0 } }]} />
      )}
      <View pointerEvents="none" style={[ps.faderCap, { left: capX - 10, borderColor: f.capBorder, backgroundColor: f.capColors[1] }]}>
        <View style={[ps.faderCapTop, { backgroundColor: f.capColors[0] }]} />
        <View style={[ps.faderCapBot, { backgroundColor: f.capColors[2] }]} />
        <View style={[ps.faderCapHi, { backgroundColor: f.capHi }]} />
        <View style={[ps.faderLine, { backgroundColor: lineColor ?? (active ? f.lineColor : pt.legend),
                      shadowColor: lineColor ?? f.lineGlow, shadowOpacity: active || lineColor ? 1 : 0,
                      shadowRadius: 2, shadowOffset: { width: 0, height: 0 } }]} />
      </View>
    </View>
  );
}

const ps = StyleSheet.create({
  winShade: { position: 'absolute', left: 0, right: 0, top: 0, height: 4 },
  winLip:   { position: 'absolute', bottom: -2, height: 1 },

  cap:     { borderWidth: 1, borderRadius: 6, paddingHorizontal: 8, alignItems: 'center', justifyContent: 'center',
             shadowColor: '#000', shadowOffset: { width: 0, height: 1.5 }, shadowRadius: 1, elevation: 1 },
  ...CAP_SHEEN(5),
  capTop:  { position: 'absolute', left: 3, right: 3, top: 0, height: 1 },
  capDown: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, borderRadius: 5, overflow: 'hidden' },
  capInset: { position: 'absolute', left: 0, right: 0, top: 0, height: 2, backgroundColor: 'rgba(0,0,0,0.45)' },
  pip:     { position: 'absolute', top: 4, left: 5, width: 4, height: 4, borderRadius: 2 },
  dot:     { width: 9, height: 9, borderRadius: 4.5, shadowOpacity: 0.9, shadowRadius: 3.5, shadowOffset: { width: 0, height: 0 } },
  shade:   { position: 'absolute', left: 0, right: 0, top: -0.5 },
  iconRow: { flexDirection: 'row', alignItems: 'center', gap: 7 },

  fader:      { height: 24, justifyContent: 'center' },
  faderFocus: { borderWidth: 2, borderColor: NAV_FOCUS, borderRadius: 6, margin: -2 },
  slot:       { position: 'absolute', left: 0, right: 0, top: 9, height: 6, borderRadius: 3 },
  slotLip:    { position: 'absolute', left: 3, right: 3, top: 15, height: 1 },
  fill:       { position: 'absolute', left: 1, top: 10, height: 4, borderRadius: 2 },
  faderCap:   { position: 'absolute', top: 3, width: 20, height: 18, borderRadius: 4, borderWidth: 1, overflow: 'hidden',
                shadowColor: '#000', shadowOpacity: 0.6, shadowRadius: 1, shadowOffset: { width: 0, height: 2 }, elevation: 2 },
  faderCapTop: { position: 'absolute', left: 0, right: 0, top: 0, height: '45%' },
  faderCapBot: { position: 'absolute', left: 0, right: 0, bottom: 0, height: '30%' },
  faderCapHi:  { position: 'absolute', left: 1, right: 1, top: 0, height: 1 },
  faderLine:   { position: 'absolute', left: 8, top: 3, bottom: 3, width: 2 },
});
