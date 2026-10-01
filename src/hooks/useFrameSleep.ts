/**
 * useFrameSleep — lets a meter's Reanimated frame callback STOP when it has nothing left to draw, and
 * start again when a new sample arrives.
 *
 * ★★★ WHY. An ACTIVE useFrameCallback runs on EVERY display frame — 120 a second on a ProMotion
 *   iPhone — for as long as it is mounted, and it keeps the UI thread's display link running, which
 *   PINS the panel's refresh rate: the screen can never fall to its low idle rate. The LED strip's and
 *   the analogue meter's callbacks never stopped. Paused, disconnected, a steady carrier, a closed
 *   squelch on a quiet channel — 120 wake-ups a second to compute that nothing had changed. The
 *   waterfall's own callbacks already release the display when they settle (WaterfallView specTween /
 *   revealCb: Stuart, "if we can maintain the VRR on promotion screens that is a big win for battery
 *   life"); the meters were the ones left holding it. Power audit 2026-10-01.
 * ★★ THE HANDSHAKE. The worklet decides it is settled and ASKS (scheduleOnRN(sleep, gen)) — it cannot
 *   deactivate itself. The bus subscription bumps `gen` on every sample that changes anything (wake).
 *   The JS side sleeps the callback ONLY if no sample arrived after the worklet looked (gen unchanged):
 *   otherwise a sample landing in the hop would be dropped and the LEDs would freeze on the old level
 *   until the next one. Both halves run on the JS thread, so they are ordered.
 * ★★ COVERED (useScreenCovered): an opaque full-screen overlay hides the meter — the callback sleeps
 *   and no sample wakes it until the overlay goes.
 */
import { useCallback, useEffect, useRef } from 'react';
import { useSharedValue, type SharedValue } from 'react-native-reanimated';
import { useScreenCovered } from './useScreenCovered';

type FrameCallbackHandle = { setActive: (on: boolean) => void; isActive: boolean };

export interface FrameSleep {
  /** The sample generation, as the worklet last saw it — passed back to `sleep`. */
  gen: SharedValue<number>;
  /** 1 = the worklet has asked to sleep for this generation; do not ask again every frame. */
  asked: SharedValue<number>;
  /** JS: a sample changed something — run the callback (no-op while it already runs). */
  wake: () => void;
  /** JS, from the worklet via scheduleOnRN: stop, unless a sample arrived since `seen`. */
  sleep: (seen: number) => void;
  /** Give it the frame callback's handle (the return of useFrameCallback), every render. */
  attach: (fc: FrameCallbackHandle) => void;
}

export function useFrameSleep(): FrameSleep {
  const genRef = useRef(0);
  const gen = useSharedValue(0);
  const asked = useSharedValue(0);
  const fcRef = useRef<FrameCallbackHandle | null>(null);
  const covered = useScreenCovered();
  const coveredRef = useRef(covered);
  coveredRef.current = covered;

  const wake = useCallback(() => {
    genRef.current += 1;
    gen.value = genRef.current;
    asked.value = 0;
    const fc = fcRef.current;
    if (fc && !fc.isActive && !coveredRef.current) fc.setActive(true);
  }, [gen, asked]);

  const sleep = useCallback((seen: number) => {
    if (seen !== genRef.current) return;   // a sample arrived after the worklet looked — stay awake
    const fc = fcRef.current;
    if (fc && fc.isActive) fc.setActive(false);
  }, []);

  useEffect(() => {
    const fc = fcRef.current;
    if (covered) { if (fc && fc.isActive) fc.setActive(false); }
    else wake();
  }, [covered, wake]);

  const attach = useCallback((fc: FrameCallbackHandle) => { fcRef.current = fc; }, []);
  return { gen, asked, wake, sleep, attach };
}
