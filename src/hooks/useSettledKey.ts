import { useEffect, useRef, useState } from 'react';

const SETTLE_GRACE_MS = 900;

/* ★★ A REMOUNT KEY THAT ONLY CHANGES ONCE A RESIZE HAS SETTLED (moved out of ChassisPlate, 2026-10-11, so SpriteView
 *  can share it). On the Mac a Skia <Canvas> whose size changes can keep showing its OLD surface — half a plate after a
 *  window grew, and a key's light drawn at the key's old size (a ghost outline beside the key, Stuart 2026-10-11, build
 *  445, default deck). Keying the canvas by its LIVE size fixes that but rebuilds it every frame of a window drag, which
 *  paused the waterfall and spectrum (2026-10-03). So: follow the size live, rebuild ONCE, 250 ms after it stops. */
export function useSettledKey(w: number, h: number): string {
  const [key, setKey] = useState(`${w}x${h}`);
  /* ★★ THE FIRST REAL SIZE IS ADOPTED AT ONCE (Stuart, 2026-10-11, the Moto: every popup "flicker[s] when it hits its
   *  topmost extension, almost like the lighting is rendering in late"). A sheet's plate mounts unmeasured (0 × 0),
   *  is measured, draws — and 250 ms later the key changed from "0x0" to its size and the canvas was rebuilt, just as
   *  the sheet's slide finished. Only a CHANGE from one real size to another waits to settle. */
  const born = useRef(Date.now());
  const live = `${w}x${h}`;
  /* ★ …and while the thing is still ARRIVING (its first SETTLE_GRACE_MS) every size is adopted at once: the audio sheet
   *  grows once its contents load, a moment after it opens, and that second size was being rebuilt 250 ms later — the
   *  one menu that still flickered as it landed. After that, a change waits to settle as before. */
  const unmeasured = (key.startsWith('0x') || key.endsWith('x0') || Date.now() - born.current < SETTLE_GRACE_MS)
    && key !== live;
  if (unmeasured && w > 0 && h > 0) setKey(live);   // ★ no early return — the hooks below must always run
  useEffect(() => {
    const t = setTimeout(() => setKey(`${w}x${h}`), 250);
    return () => clearTimeout(t);
  }, [w, h]);
  return unmeasured && w > 0 && h > 0 ? live : key;
}

/** ★ The VALUE as it was when `key` last stopped changing for 250 ms (the first value at once). A drawing that is
 *  expensive to rebuild — a key's light, its pressed wash — is built from this, and while a resize is still moving
 *  the caller STRETCHES the settled picture to the live box instead of rebuilding it on every step. */
export function useSettled<T>(value: T, key: string): T {
  const [s, setS] = useState({ key, value });
  useEffect(() => {
    if (key === s.key) return;
    const t = setTimeout(() => setS({ key, value }), 250);
    return () => clearTimeout(t);
  }, [key]);   // eslint-disable-line react-hooks/exhaustive-deps
  return s.value;
}
