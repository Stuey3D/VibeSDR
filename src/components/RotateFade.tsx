/**
 * RotateFade — whatever it wraps leaves on a ROTATION (from the turn's own render) and FADES back in as the control
 * bar comes back up (services/rotateReveal). Stuart, 2026-10-11 (the Moto): "just have the decoder box fade in after
 * the rotation". A full-screen, touch-through layer: its children keep their own absolute positions.
 * ★ A plain View does the hiding (instant), the Animated one inside only fades — a native-driven opacity cannot be set
 *   back to a plain value.
 */
import React, { useEffect, useRef } from 'react';
import { Animated, StyleSheet, View, useWindowDimensions } from 'react-native';
import { useRotateAway } from '../services/rotateReveal';

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
