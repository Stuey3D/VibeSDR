/**
 * RotateFade — whatever it wraps leaves on a ROTATION (from the turn's own render) and FADES back in as the control
 * bar comes back up (services/rotateReveal). Stuart, 2026-10-11 (the Moto): "just have the decoder box fade in after
 * the rotation". A full-screen, touch-through layer: its children keep their own absolute positions.
 * ★ A plain View does the hiding (instant), the Animated one inside only fades — a native-driven opacity cannot be set
 *   back to a plain value.
 */
import React, { useEffect, useRef } from 'react';
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
  useEffect(() => {
    Animated.spring(y, { toValue: 0, damping: 22, stiffness: 200, useNativeDriver: true }).start();
  }, [y]);
  return (
    <Animated.View pointerEvents="box-none" style={[StyleSheet.absoluteFill, { transform: [{ translateY: y }] }]}>
      {children}
    </Animated.View>
  );
}
