/**
 * RotateFade — whatever it wraps leaves on a ROTATION (from the turn's own render) and FADES back in as the control
 * bar comes back up (services/rotateReveal). Stuart, 2026-10-11 (the Moto): "just have the decoder box fade in after
 * the rotation". A full-screen, touch-through layer: its children keep their own absolute positions.
 * ★ A plain View does the hiding (instant), the Animated one inside only fades — a native-driven opacity cannot be set
 *   back to a plain value.
 */
import React, { useContext, useEffect, useRef, useState } from 'react';
import { Animated, StyleSheet, View, useWindowDimensions } from 'react-native';
import { rotSlide, useRotateAway } from '../services/rotateReveal';

const FADE_MS = 240;

export default function RotateFade({ children }: { children: React.ReactNode }) {
  const { width, height } = useWindowDimensions();
  const away = useRotateAway(width, height);
  const fade = useRef(new Animated.Value(1)).current;
  const wasAway = useRef(false);
  useEffect(() => {
    if (away) { wasAway.current = true; fade.setValue(0); return; }
    if (wasAway.current) {
      wasAway.current = false;
      Animated.timing(fade, { toValue: 1, duration: FADE_MS, useNativeDriver: true }).start();
    }
  }, [away, fade]);
  return (
    <View pointerEvents="box-none" style={[StyleSheet.absoluteFill, { opacity: away ? 0 : 1 }]}>
      <Animated.View pointerEvents="box-none" style={[StyleSheet.absoluteFill, { opacity: fade }]}>
        {children}
      </Animated.View>
    </View>
  );
}

/**
 * RotateSheet — for a popup sheet (inside its Modal): on a ROTATION it leaves at once and comes back UP with the control
 * bar, on the bar's own slide (Stuart, 2026-10-11: "if menu open and screen rotated the menu pops up again with the
 * controls"). Wrap the Modal's whole content in it.
 */
export function RotateSheet({ children }: { children: React.ReactNode }) {
  const { width, height } = useWindowDimensions();
  const away = useRotateAway(width, height);
  return (
    <View pointerEvents="box-none" style={[StyleSheet.absoluteFill, { opacity: away ? 0 : 1 }]}>
      <Animated.View pointerEvents="box-none" style={[StyleSheet.absoluteFill, { transform: [{ translateY: rotSlide }] }]}>
        {children}
      </Animated.View>
    </View>
  );
}

/**
 * SheetRise — a bottom sheet rises on the MAIN MENU's own spring (MenuSheet: damping 22, stiffness 200, native driver)
 * instead of the Modal's built-in "slide", which on Android is a different, stiffer curve (Stuart, 2026-10-11: "the main
 * menu and chat open at a much smoother rate than the step and audio menu, can the opening speed be matched"). Use with
 * the Modal's animationType="none".
 */
export function SheetRise({ children }: { children: React.ReactNode }) {
  const { height } = useWindowDimensions();
  const y = useRef(new Animated.Value(height)).current;
  const [rising, setRising] = useState(true);
  useEffect(() => {
    let live = true;
    Animated.spring(y, { toValue: 0, damping: 22, stiffness: 200, useNativeDriver: true })
      .start(() => { if (live) setRising(false); });
    return () => { live = false; };
  }, [y]);
  return (
    <SheetRisingCtx.Provider value={rising}>
      <Animated.View pointerEvents="box-none" style={[StyleSheet.absoluteFill, { transform: [{ translateY: y }] }]}>
        {children}
      </Animated.View>
    </SheetRisingCtx.Provider>
  );
}

/**
 * ★★★ A PRESS WHILE THE SHEET IS STILL RISING WAS LOST (Stuart on 483, Mac: "its as the sheet slides up that the
 *     first press doesnt register" — one ⏺ REC file, no error, so the press never reached the handler).
 *  WHY: a Pressable measures its own rectangle when the touch begins (Pressability._measureResponderRegion) and,
 *  on every MOVE, cancels the press if the touch is outside that rectangle (+ its press-retention offset). Under the
 *  new architecture `measure` reads the SHADOW TREE — and a native-driver animation never writes its frames there
 *  (only its final value, once it ends). So mid-rise the key is measured where the sheet STARTED, a screen-height
 *  below where it is drawn, and the first pixel of movement in the click — a trackpad nudge, a finger's roll — reads
 *  as "dragged off the key": the press goes quietly to RESPONDER_ACTIVE_PRESS_OUT and onPress never fires. A still
 *  click survives (no move = no check), which is why it is intermittent; once the spring ends the shadow tree is
 *  synced and every press works — "pressed it a few seconds later it worked".
 *  THE FIX: while a sheet is rising, its keys keep the press however far the touch "moves" from the measured
 *  rectangle (a retention offset bigger than the window). Only for the ~½ s of the rise: afterwards the normal
 *  offset — slide off a key to cancel — is back. A real SCROLL still wins: the ScrollView takes the responder and
 *  the press is terminated, which is a different path from this one.
 *  ★ The main menu (MenuSheet) rises on its own spring and provides the same context.
 */
export const SheetRisingCtx = React.createContext(false);

/** For a key inside a sheet: the pressRetentionOffset to pass (undefined = the platform default). */
export function useRisingPressRetention(): { top: number; bottom: number; left: number; right: number } | undefined {
  const rising = useContext(SheetRisingCtx);
  const { width, height } = useWindowDimensions();
  if (!rising) return undefined;
  const r = Math.max(width, height) + 100;
  return { top: r, bottom: r, left: r, right: r };
}
