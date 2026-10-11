import { useEffect, useState } from 'react';

/* ★★ A REMOUNT KEY THAT ONLY CHANGES ONCE A RESIZE HAS SETTLED (moved out of ChassisPlate, 2026-10-11, so SpriteView
 *  can share it). On the Mac a Skia <Canvas> whose size changes can keep showing its OLD surface — half a plate after a
 *  window grew, and a key's light drawn at the key's old size (a ghost outline beside the key, Stuart 2026-10-11, build
 *  445, default deck). Keying the canvas by its LIVE size fixes that but rebuilds it every frame of a window drag, which
 *  paused the waterfall and spectrum (2026-10-03). So: follow the size live, rebuild ONCE, 250 ms after it stops. */
export function useSettledKey(w: number, h: number): string {
  const [key, setKey] = useState(`${w}x${h}`);
  useEffect(() => {
    const t = setTimeout(() => setKey(`${w}x${h}`), 250);
    return () => clearTimeout(t);
  }, [w, h]);
  return key;
}
