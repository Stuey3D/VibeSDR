/**
 * KeyLight — the light coming up out of the panel gap around a front-panel key (constants/keyLight.ts
 * says what the light IS; this file only puts it on screen).
 *
 * ★★★ Stuart, 2026-10-01: "replace it with a glow coming up in the panel gap around each button, also
 *   apply that same lighting to all the buttons". DomeKey draws one round every front-panel key that
 *   asks for it (`lightReach`; round its dark slot when it sits in one, `lightSlot` — TunerKeys on the
 *   default chassis): the four main keys and the tuner keys, one implementation.
 * ★★ PERF: ONE IMAGE PER KEY SHAPE, made once and shared. keyLightImage() rasterises the light into a
 *   sprite (glowSprite makeSprite — sizes vetted by spriteSizing) and keeps it in a small cache keyed by
 *   the cut-out, the cap, the reach, the colour and the chassis; the four main keys are one size, so they
 *   share one image. The canvas that shows it is static — it redraws only when that key changes. A press
 *   moves the cap OVER the light and only fades a ready-made image in (below): it never rebuilds one.
 * ★★ PRESSED (constants/keyLight.ts KEY_PRESS_LIGHT): the gap opens and the lamp floods over the key.
 *   Two more images, made with the rest one, faded in by the key's OWN press animation (DomeKey's `dim`
 *   style — a 45 ms withTiming that settles and stops). No frame callback, no derived value; the
 *   press never rebuilds an image.
 * ★ Transparency effects OFF / low-end: no light, no canvas, nothing — the keys exactly as before.
 */

import React, { useMemo } from 'react';
import { StyleSheet } from 'react-native';
import Animated from 'react-native-reanimated';
import { Canvas, Image as SkImageNode, Skia, type SkImage } from '@shopify/react-native-skia';
import { makeSprite } from './glowSprite';
import { hotA, ledA, type LedColour } from '../constants/faceplate';
import { drawKeyLight, drawKeyWash, keyLightSprite, type RR } from '../constants/keyLight';

const cache = new Map<string, SkImage>();
/** Plenty for every key shape on screen at once (main + tuner keys, rest + open + wash, a chassis or colour change). */
const CACHE_MAX = 24;

const q = (n: number) => Math.round(n * 2) / 2;
const rrKey = (b: RR) => `${q(b.x)},${q(b.y)},${q(b.w)},${q(b.h)},${q(b.r)}`;

/**
 * The light round `cut` (with `cap` kept dark) at `reach`, as an image the size of keyLightSprite(cut,
 * reach). null when there is nothing sane to draw (unmeasured, no reach) or no surface.
 * `chassis` is part of the key: the same shape lit on two chassis is two images.
 */
export function keyLightImage(cut: RR, cap: RR, reach: number, led: LedColour, chassis: string,
                              pressed = false, travel = 0): SkImage | null {
  if (!(reach > 0) || !(cut.w > 1) || !(cut.h > 1)) return null;
  const k = `${pressed ? `open${travel}` : 'rest'}|${chassis}|${colourKey(led)}|${reach}|${rrKey(cut)}|${rrKey(cap)}`;
  const sp = keyLightSprite(cut, reach);
  return cached(k, () => makeSprite(sp.w, sp.h, (c) => drawKeyLight(Skia, c, cut, cap, reach,
                                    a => ledA(led, a), a => hotA(led, a), pressed, travel)));
}

/** The pressed wash over a cap, the cap's own size (constants/keyLight.ts drawKeyWash). */
export function keyWashImage(cap: RR, led: LedColour, chassis: string): SkImage | null {
  if (!(cap.w > 1) || !(cap.h > 1)) return null;
  return cached(`wash|${chassis}|${colourKey(led)}|${rrKey({ ...cap, x: 0, y: 0 })}`,
                () => makeSprite(cap.w, cap.h, (c) => drawKeyWash(Skia, c, cap, a => ledA(led, a))));
}

/** The key's press as an animated style (DomeKey's `dim`: opacity 0 at rest → 1 clicked). */
type PressStyle = React.ComponentProps<typeof Animated.View>['style'];

const colourKey = (led: LedColour) => `${led.rgb}|${led.hsl?.join(',') ?? ''}`;
function cached(k: string, make: () => SkImage | null): SkImage | null {
  const hit = cache.get(k);
  if (hit) return hit;
  const img = make();
  if (!img) return null;   // ★ never cache a failed surface — the next render tries again
  if (cache.size >= CACHE_MAX) cache.delete(cache.keys().next().value as string);
  cache.set(k, img);
  return img;
}

/**
 * The light as a view: an absolutely placed, untouchable canvas reaching `reach` past `cut` on every
 * side, in the parent's coordinates (the key's). Put it UNDER the cap and OVER the slot's own fill.
 */
export const KeyLight = React.memo(function KeyLight({ cut, cap, reach, led, chassis, pressStyle, travel = 0 }: {
  cut: RR; cap: RR; reach: number; led: LedColour; chassis: string;
  /** How far the cap sinks when clicked (DomeKey's DOME_TRAVEL; 0 for the outline key). */
  travel?: number;
  /** The key's press as an animated opacity (0 at rest → 1 clicked): fades the OPENED gap in. Absent:
   *  no press light (a key whose press the caller does not hand over). */
  pressStyle?: PressStyle;
}) {
  const ck = rrKey(cut), pk = rrKey(cap);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const img = useMemo(() => keyLightImage(cut, cap, reach, led, chassis), [ck, pk, reach, led, chassis]);
  const open = useMemo(() => pressStyle ? keyLightImage(cut, cap, reach, led, chassis, true, travel) : null,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [ck, pk, reach, led, chassis, !!pressStyle, travel]);
  if (!img) return null;
  const sp = keyLightSprite(cut, reach);
  const box = { position: 'absolute' as const, left: sp.x, top: sp.y, width: sp.w, height: sp.h };
  return (
    <>
      <Canvas pointerEvents="none" style={box}>
        <SkImageNode image={img} x={0} y={0} width={sp.w} height={sp.h} />
      </Canvas>
      {open && (
        <Animated.View pointerEvents="none" style={[box, pressStyle]}>
          <Canvas pointerEvents="none" style={StyleSheet.absoluteFill}>
            <SkImageNode image={open} x={0} y={0} width={sp.w} height={sp.h} />
          </Canvas>
        </Animated.View>
      )}
    </>
  );
});

/**
 * The pressed wash over the cap, as a view filling its parent (the cap, or an outline key's face): put
 * it OVER the face and UNDER the legend. Faded in by `pressStyle`; invisible — and drawn once — at rest.
 */
export const KeyWash = React.memo(function KeyWash({ cap, led, chassis, pressStyle, radius }: {
  cap: RR; led: LedColour; chassis: string; pressStyle: PressStyle; radius: number;
}) {
  const pk = rrKey({ ...cap, x: 0, y: 0 });
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const img = useMemo(() => keyWashImage(cap, led, chassis), [pk, led, chassis]);
  if (!img) return null;
  return (
    <Animated.View pointerEvents="none"
      style={[{ position: 'absolute', left: 0, top: 0, width: cap.w, height: cap.h, borderRadius: radius }, pressStyle]}>
      <Canvas pointerEvents="none" style={StyleSheet.absoluteFill}>
        <SkImageNode image={img} x={0} y={0} width={cap.w} height={cap.h} />
      </Canvas>
    </Animated.View>
  );
});
