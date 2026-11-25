/**
 * ★★★ REACH — THE DECK'S GRAB BAR ON THE FOLDED FRONT SCREEN (iPhone Duo, Stuart 2026-11-25).
 *
 *  The Duo's front screen is 466 pt wide — wider than any iPhone — and the deck runs the full width of it, so the
 *  far-left drum and key are a stretch for a right thumb. A small grab bar sits at the deck's RIGHT end:
 *    • pull it to the RIGHT → the whole deck shrinks a little and lines up on the right (one-handed reach);
 *    • push it to the LEFT  → back to its full size.
 *  Remembered per device (AsyncStorage — this key is not one of the iCloud-synced settings: a reach preference
 *  belongs to the hand holding THIS phone). Eased both ways, never a snap; with Reduce Motion it moves at once.
 *
 *  ★ Only on the folded front screen (isFoldedFrontWindow). Open, rotated or side by side the deck follows the iPad
 *    rules and there is no bar: the caller does not mount this, and `reach` reads 0 there whatever is stored.
 *  ★ The finger has little room to travel right of a bar that already sits near the edge, so the drag is geared
 *    (GEAR × the finger) and a flick decides on its own; the deck follows the finger while it is held.
 *  ★ Accessibility: an adjustable element — swipe up / down (increment / decrement) with VoiceOver.
 */
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, PanResponder, StyleSheet, View } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { useReduceMotion } from '../hooks/useReduceMotion';

const KEY = 'lsv_deck_reach_v1';
/** How much narrower the deck gets, as a share of the window's width (466 pt → 65 pt). "Shrinks slightly". */
export const REACH_SHARE = 0.14;
/** The gutter on the deck's right that holds the bar (the deck's own right margin grows by this on the front screen). */
export const GRAB_GUTTER = 14;
const GEAR = 2.2;
const MOVE_MS = 320;

export interface DeckReach {
  /** 0 = full width, 1 = reach; animated (JS driver — it moves a layout edge). */
  progress: Animated.Value;
  /** Whether the deck is moving under a finger or an animation (the caller holds re-measuring until it lands). */
  moving: boolean;
}

export function useDeckReach(enabled: boolean, onMoving?: (moving: boolean) => void) {
  const progress = useRef(new Animated.Value(0)).current;
  const [stored, setStored] = useState(false);
  const reduceMotion = useReduceMotion();
  const live = useRef(0);
  useEffect(() => {
    const id = progress.addListener(({ value }) => { live.current = value; });
    return () => progress.removeListener(id);
  }, [progress]);
  useEffect(() => {
    let on = true;
    AsyncStorage.getItem(KEY).then((v) => { if (on) setStored(v === '1'); }).catch(() => {});
    return () => { on = false; };
  }, []);
  const target = enabled && stored ? 1 : 0;
  const settle = useCallback((to: number, animate = true) => {
    progress.stopAnimation();
    if (!animate || reduceMotion) { progress.setValue(to); onMoving?.(false); return; }
    onMoving?.(true);
    Animated.timing(progress, { toValue: to, duration: MOVE_MS, easing: Easing.out(Easing.cubic), useNativeDriver: false })
      .start(() => onMoving?.(false));
  }, [progress, reduceMotion, onMoving]);
  /* Folding, unfolding or the stored value arriving: land on the target. Arriving from nothing (the first frame)
   * is placed, not animated — the deck was never anywhere else. */
  const first = useRef(true);
  useEffect(() => {
    settle(target, !first.current);
    first.current = false;
  }, [target, settle]);
  const choose = useCallback((reach: boolean) => {
    setStored(reach);
    AsyncStorage.setItem(KEY, reach ? '1' : '0').catch(() => {});
    if ((reach ? 1 : 0) === target) settle(reach ? 1 : 0);   // same target: the effect will not run — land here
  }, [settle, target]);
  return { progress, reach: target === 1, choose, settle };
}

/** The grab bar itself: drawn in the gutter at the deck's right end, full deck height for the touch. */
export function DeckGrabBar({ progress, reach, choose, settle, shift, colour, onMoving }: {
  progress: Animated.Value; reach: boolean; choose: (reach: boolean) => void;
  settle: (to: number, animate?: boolean) => void;
  /** How far the deck's left edge travels between 0 and 1, in pt. */
  shift: number;
  colour: string;
  onMoving?: (moving: boolean) => void;
}) {
  const start = useRef(0);
  const cur = useRef(0);
  const reachRef = useRef(reach); reachRef.current = reach;
  const shiftRef = useRef(shift); shiftRef.current = shift;
  const pan = useMemo(() => PanResponder.create({
    onStartShouldSetPanResponder: () => true,
    onMoveShouldSetPanResponder: (_e, g) => Math.abs(g.dx) > 3 && Math.abs(g.dx) > Math.abs(g.dy),
    onPanResponderTerminationRequest: () => false,
    onPanResponderGrant: () => {
      progress.stopAnimation((v) => { start.current = v; cur.current = v; });
      onMoving?.(true);
    },
    onPanResponderMove: (_e, g) => {
      const s = Math.max(1, shiftRef.current);
      const v = Math.max(0, Math.min(1, start.current + (g.dx * GEAR) / s));
      cur.current = v;
      progress.setValue(v);
    },
    onPanResponderRelease: (_e, g) => {
      const v = cur.current;
      const to = g.vx > 0.25 ? true : g.vx < -0.25 ? false : v > 0.5;
      if (to === reachRef.current) settle(to ? 1 : 0); else choose(to);
    },
    onPanResponderTerminate: () => settle(reachRef.current ? 1 : 0),
  }), [progress, choose, settle, onMoving]);
  return (
    <View {...pan.panHandlers} style={st.touch}
      accessible accessibilityRole="adjustable" accessibilityLabel="Deck reach"
      accessibilityHint="Pull right to shrink the controls towards your right hand; push left for full width"
      accessibilityValue={{ text: reach ? 'Right-hand reach' : 'Full width' }}
      accessibilityActions={[{ name: 'increment' }, { name: 'decrement' }]}
      onAccessibilityAction={(e) => {
        if (e.nativeEvent.actionName === 'increment') choose(true);
        if (e.nativeEvent.actionName === 'decrement') choose(false);
      }}>
      <View style={[st.bar, { backgroundColor: colour }]} />
    </View>
  );
}

const st = StyleSheet.create({
  // The touch runs from the screen edge (the deck's 8 pt margin + the gutter) 36 pt in — over the deck's own right
  // padding, never its controls — and the deck's full height, so the bar is easy to find. The bar sits mid-gutter.
  touch: { position: 'absolute', top: 0, bottom: 0, right: -(GRAB_GUTTER + 8), width: 36, justifyContent: 'center' },
  bar:   { position: 'absolute', right: 8 + GRAB_GUTTER / 2 - 2.5, width: 5, height: 44, borderRadius: 3 },
});
