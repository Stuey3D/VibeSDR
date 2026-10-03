/**
 * RecElapsed — the recording timer, ticking where it is SHOWN instead of in SDRScreen.
 *
 * ★★ WHY (efficiency audit 2026-10-03). SDRScreen ran `setInterval(() => setRecSeconds(s => s + 1), 1000)`
 *   for as long as a recording ran: a render of the WHOLE receiver screen every second — screen locked
 *   included — and a count of interval firings, which runs SLOW (a timer delayed by a busy JS thread or a
 *   suspended app never catches up), so a long recording read short.
 * ★ Now SDRScreen stores only the START TIME (it changes twice per recording) and this wrapper derives the
 *   elapsed seconds from the clock, on its own tick, paused while the app is not active and re-read the
 *   moment it is — SessionClock's pattern (SDRScreen.tsx). Only the wrapped element re-renders per second.
 *   The number handed down is the same whole-seconds integer as before, so every display formats it
 *   identically.
 */
import React, { useEffect, useState } from 'react';
import { AppState } from 'react-native';

const elapsedOf = (startedAt: number | null) =>
  startedAt == null ? 0 : Math.max(0, Math.floor((Date.now() - startedAt) / 1000));

/** Whole seconds since `startedAt` (ms since epoch), ticking once a second while the app is active. 0 when null. */
export function useRecSeconds(startedAt: number | null): number {
  const [secs, setSecs] = useState(() => elapsedOf(startedAt));
  useEffect(() => {
    const read = () => setSecs(elapsedOf(startedAt));
    read();
    if (startedAt == null) return;
    const t = setInterval(() => { if (AppState.currentState === 'active') read(); }, 1000);
    // ★ Re-read on the way back in, rather than showing a stale figure for up to a second.
    const sub = AppState.addEventListener('change', (st: string) => { if (st === 'active') read(); });
    return () => { clearInterval(t); sub.remove(); };
  }, [startedAt]);
  return secs;
}

/** Render-prop wrapper: `render(seconds)` is re-run once a second while recording; the parent is not. */
export function RecElapsed({ startedAt, render }: {
  startedAt: number | null;
  render: (recSeconds: number) => React.ReactElement | null;
}) {
  const secs = useRecSeconds(startedAt);
  return render(secs);
}
