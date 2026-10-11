/**
 * rotateReveal — ONE signal for everything that leaves on a rotation and comes back with the control bar.
 *
 * ★ Stuart, 2026-10-11 (the Moto): "the VTS is staying on screen during the rotation it needs to pop up or fade in with
 *   the controls, as well as disappear on rotation". ControlsBar (useRotateSlide) decides when the bar comes back; the
 *   station strip follows the same moment and the same slide, so the two always move together.
 * ★ A rotation is the window's width and height SWAPPING — see isRotation. Never a resize.
 */
import { useEffect, useReducer, useRef } from 'react';
import { Animated } from 'react-native';

/** The shared slide: 0 = in place; SLIDE_OFF = parked below the screen. Native-driven by ControlsBar. */
export const rotSlide = new Animated.Value(0);

const revealSubs = new Set<() => void>();
/** ControlsBar calls this as the bar starts back up. */
export function notifyRevealed() { revealSubs.forEach(f => f()); }

/** The window's size swapped — a rotation, not a resize (a Mac window, a fold opening never swaps them). */
export function isRotation(p: { W: number; H: number }, W: number, H: number) {
  return Math.abs(W - p.H) < 2 && Math.abs(H - p.W) < 2 && Math.abs(W - p.W) >= 2;
}

/** For a FOLLOWER (the station strip): true from the very render that carries the rotated size until the bar is back. */
export function useRotateAway(W: number, H: number): boolean {
  const prev = useRef({ W, H });
  const away = useRef(false);
  const p = prev.current;
  if (p.W !== W || p.H !== H) {
    if (isRotation(p, W, H)) away.current = true;
    prev.current = { W, H };
  }
  const [, force] = useReducer((n: number) => n + 1, 0);
  useEffect(() => {
    const f = () => { if (away.current) { away.current = false; force(); } };
    revealSubs.add(f);
    return () => { revealSubs.delete(f); };
  }, []);
  return away.current;
}
