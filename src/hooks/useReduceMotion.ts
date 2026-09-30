/**
 * useReduceMotion — the OS's "reduce motion" switch, live: iOS Settings › Accessibility › Motion ›
 * Reduce Motion; Android Settings › Accessibility › Remove animations (React Native reads the
 * animator / transition scale being 0).
 *
 * ★ Read through AccessibilityInfo (isReduceMotionEnabled + the `reduceMotionChanged` listener), so
 *   turning it on while the app is open takes effect at once — the faceplate meters (brief §4.4 / §4.5)
 *   switch to steady LEDs and an overshoot-free needle without a restart.
 */
import { useEffect, useState } from 'react';
import { AccessibilityInfo } from 'react-native';

export function useReduceMotion(): boolean {
  const [on, setOn] = useState(false);
  useEffect(() => {
    let live = true;
    AccessibilityInfo.isReduceMotionEnabled().then(v => { if (live) setOn(!!v); }).catch(() => {});
    const sub = AccessibilityInfo.addEventListener('reduceMotionChanged', v => setOn(!!v));
    return () => { live = false; sub.remove(); };
  }, []);
  return on;
}
