/**
 * ★★★ REACH — SHRINK / EXPAND THE DECK ON THE FOLDED FRONT SCREEN (iPhone Duo, Stuart 2026-11-25).
 *
 *  The Duo's front screen is 466 pt wide — wider than any iPhone — and the deck runs the full width of it, so the
 *  far-left drum and key are a stretch for a right thumb. A small key BETWEEN THE DRUMS (ControlsBar, in each
 *  faceplate's own key style — it replaced a grab bar beside the deck, Stuart: the control belongs IN the deck):
 *    • tap → the whole deck shrinks a little and lines up on the right (one-handed reach), the legend turns EXPAND;
 *    • tap again → full size, centred.
 *  Remembered per device (AsyncStorage — this key is not one of the iCloud-synced settings: a reach preference
 *  belongs to the hand holding THIS phone). Eased both ways, never a snap; with Reduce Motion it moves at once.
 *
 *  ★ Only on the folded front screen (isFoldedFrontWindow). Open, rotated or side by side the deck follows the iPad
 *    rules and there is no bar: the caller does not mount this, and `reach` reads 0 there whatever is stored.
 *  ★ VoiceOver: a button, "Shrink controls" / "Expand controls".
 */
import { useCallback, useEffect, useRef, useState } from 'react';
import { Animated, Easing } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { useReduceMotion } from '../hooks/useReduceMotion';

const KEY = 'lsv_deck_reach_v1';
/** How much narrower the deck gets, as a share of the window's width (466 pt → 65 pt). "Shrinks slightly". */
export const REACH_SHARE = 0.14;
const MOVE_MS = 320;

export interface DeckReach {
  /** 0 = full width, 1 = reach; animated (JS driver — it moves a layout edge). */
  progress: Animated.Value;
  /** Whether the deck is moving (the caller holds re-measuring until it lands). */
  moving: boolean;
}

export function useDeckReach(enabled: boolean, onMoving?: (moving: boolean) => void) {
  const progress = useRef(new Animated.Value(0)).current;
  const [stored, setStored] = useState(false);
  const fromLoad = useRef(false);   // the stored value arriving is placed, not animated
  const reduceMotion = useReduceMotion();
  useEffect(() => {
    let on = true;
    AsyncStorage.getItem(KEY).then((v) => { if (on) { fromLoad.current = true; setStored(v === '1'); } }).catch(() => {});
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
    settle(target, !first.current && !fromLoad.current);
    first.current = false;
    fromLoad.current = false;
  }, [target, settle]);
  const choose = useCallback((reach: boolean) => {
    setStored(reach);
    AsyncStorage.setItem(KEY, reach ? '1' : '0').catch(() => {});
    if ((reach ? 1 : 0) === target) settle(reach ? 1 : 0);   // same target: the effect will not run — land here
  }, [settle, target]);
  return { progress, reach: target === 1, choose, settle };
}
