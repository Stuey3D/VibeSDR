/**
 * KeyLight — the light coming up out of the panel gap around a front-panel key (constants/keyLight.ts
 * says what the light IS; this file only puts it on screen).
 *
 * ★★★ Stuart, 2026-10-01: "replace it with a glow coming up in the panel gap around each button, also
 *   apply that same lighting to all the buttons". DomeKey draws one round every front-panel key that
 *   asks for it (`lightReach`; round its dark slot when it sits in one, `lightSlot` — TunerKeys on the
 *   default chassis): the four main keys and the tuner keys, one implementation.
 * ★★ PERF: ONE IMAGE PER KEY SHAPE, made once and shared. keyLightBuild() rasterises the light into a
 *   sprite (glowSprite makeSprite — sizes vetted by spriteSizing) and keeps it in the shared sprite cache keyed by
 *   the cut-out, the cap, the reach, the colour and the chassis; the four main keys are one size, so they
 *   share one image. The canvas that shows it is static — it redraws only when that key changes. A press
 *   moves the cap OVER the light and only fades a ready-made image in (below): it never rebuilds one.
 * ★★ PRESSED (constants/keyLight.ts KEY_PRESS_LIGHT): the gap opens and the lamp floods over the key.
 *   Two more images, made with the rest one, faded in by the key's OWN press animation (DomeKey's `dim`
 *   style — a 45 ms withTiming that settles and stops). No frame callback, no derived value; the
 *   press never rebuilds an image.
 * ★ Transparency effects OFF / low-end: no light, no canvas, nothing — the keys exactly as before.
 */

import React from 'react';
import { StyleSheet } from 'react-native';
import Animated from 'react-native-reanimated';
import { Canvas, Image as SkImageNode, Skia } from '@shopify/react-native-skia';
import { imageBuild, makeSprite, useSharedSprite } from './glowSprite';
import { hotA, ledA, type LedColour } from '../constants/faceplate';
import { drawKeyLight, drawKeyWash, keyLightSprite, type RR } from '../constants/keyLight';

const q = (n: number) => Math.round(n * 2) / 2;
const rrKey = (b: RR) => `${q(b.x)},${q(b.y)},${q(b.w)},${q(b.h)},${q(b.r)}`;
const colourKey = (led: LedColour) => `${led.rgb}|${led.hsl?.join(',') ?? ''}`;

/* ★★ THE SHARED SPRITE CACHE, NOT A PRIVATE ONE (2026-10-03). This file kept its own 24-entry Map and
 *   evicted by insertion order WITHOUT disposing — an evicted light waited for a GC Hermes never feels,
 *   and disposing on eviction would have freed an image a mounted key was still drawing. useSharedSprite
 *   (glowSprite.ts) is the app's one bounded cache: LRU, RETAINED while a key is mounted, disposed when
 *   evicted and nobody draws it. Same keys as before (quantised to half a point), so the same sharing. */

/**
 * The cache key for the light round `cut` (with `cap` kept dark) at `reach`. null when there is nothing
 * sane to draw (unmeasured, no reach). `chassis` is part of the key: the same shape lit on two chassis
 * is two images.
 */
function keyLightKey(cut: RR, cap: RR, reach: number, led: LedColour, chassis: string,
                     pressed: boolean, travel: number): string | null {
  if (!(reach > 0) || !(cut.w > 1) || !(cut.h > 1)) return null;
  return `keylight|${pressed ? `open${travel}` : 'rest'}|${chassis}|${colourKey(led)}|${reach}|${rrKey(cut)}|${rrKey(cap)}`;
}

/** The light as an image the size of keyLightSprite(cut, reach), as a cache build. */
const keyLightBuild = (cut: RR, cap: RR, reach: number, led: LedColour, pressed: boolean, travel: number) => {
  const sp = keyLightSprite(cut, reach);
  return imageBuild(makeSprite(sp.w, sp.h, (c) => drawKeyLight(Skia, c, cut, cap, reach,
                               a => ledA(led, a), a => hotA(led, a), pressed, travel)));
};

/** The key's press as an animated style (DomeKey's `dim`: opacity 0 at rest → 1 clicked). */
type PressStyle = React.ComponentProps<typeof Animated.View>['style'];

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
  const img = useSharedSprite(keyLightKey(cut, cap, reach, led, chassis, false, 0),
    () => keyLightBuild(cut, cap, reach, led, false, 0));
  const open = useSharedSprite(pressStyle ? keyLightKey(cut, cap, reach, led, chassis, true, travel) : null,
    () => keyLightBuild(cut, cap, reach, led, true, travel));
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
  const img = useSharedSprite(
    cap.w > 1 && cap.h > 1 ? `keywash|${chassis}|${colourKey(led)}|${rrKey({ ...cap, x: 0, y: 0 })}` : null,
    () => imageBuild(makeSprite(cap.w, cap.h, (c) => drawKeyWash(Skia, c, cap, a => ledA(led, a)))));
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
