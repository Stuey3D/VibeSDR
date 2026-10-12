import { useEffect, useRef, useState } from 'react';

const SETTLE_MS = 1000;

/* ★★★ THE TOP INSET A ROTATION WILL SETTLE ON, FROM ITS FIRST RENDER (Stuart, 2026-10-12, Moto: the waterfall "having
 *  to shift about and it is clunky" on every turn). Android delivers the safe-area inset LATE and IN STEPS after a
 *  rotation — measured on the Moto G35 (logcat, five turns): the render carrying the new window still has the OLD
 *  orientation's inset, then 51 → 24 → 0 pt for landscape over the next ~300 ms (0 → 51 for portrait). The waterfall's
 *  height is the window less this inset, so it was laid out THREE times in ~0.6 s, its top edge and the spectrum split
 *  moving each time. An orientation's inset is the same every time it is entered, so it is remembered: from the
 *  rotation's own render the remembered value is used, and the late steps land on a value already in place. Live
 *  again SETTLE_MS after the turn, so a real change (a status bar shown or hidden) is still followed. The first turn
 *  into an orientation has nothing remembered and steps as before. */
export function useOrientationInset(inset: number, landscape: boolean): number {
  const r = useRef({ landscape, at: 0, seen: {} as Record<'l' | 'p', number | undefined> });
  const [, wake] = useState(0);
  if (landscape !== r.current.landscape) {
    // The render that carries a rotation still holds the orientation being LEFT's inset — its settled value.
    r.current.seen[r.current.landscape ? 'l' : 'p'] = inset;
    r.current.landscape = landscape;
    r.current.at = Date.now();
  }
  const at = r.current.at;
  useEffect(() => {
    if (!at) return;
    const t = setTimeout(() => wake((n) => n + 1), SETTLE_MS);   // hand back to the live inset once it has settled
    return () => clearTimeout(t);
  }, [at]);
  const held = r.current.seen[landscape ? 'l' : 'p'];
  return held !== undefined && Date.now() - at < SETTLE_MS ? held : inset;
}
