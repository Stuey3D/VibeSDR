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

import React, { useEffect, useMemo, useState } from 'react';
import { Image, View } from 'react-native';
import Animated from 'react-native-reanimated';
import { Skia, type SkImage } from '@shopify/react-native-skia';
import { SpriteView, spriteUri } from './SpriteView';
import { imageBuild, makeSprite, useSharedSprite } from './glowSprite';
import { hotA, ledA, type LedColour } from '../constants/faceplate';
import { drawKeyLight, drawKeyWash, keyLightSprite, lightNine, ninePieces, NINE_STRIP, type LightNine, type RR } from '../constants/keyLight';
import { useSettled } from '../hooks/useSettledKey';

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
export const KeyLight = React.memo(function KeyLight(props: {
  cut: RR; cap: RR; reach: number; led: LedColour; chassis: string;
  /** How far the cap sinks when clicked (DomeKey's DOME_TRAVEL; 0 for the outline key). */
  travel?: number;
  /** The key's press as an animated opacity (0 at rest → 1 clicked): fades the OPENED gap in. Absent:
   *  no press light (a key whose press the caller does not hand over). */
  pressStyle?: PressStyle;
}) {
  /* ★★★ THE FIXED BACKLIGHT (constants/keyLight.ts lightNine; Stuart, 2026-10-11: "that is what real hardware would have
   *  anyway"). One picture per light — built at a small canonical size, keyed by nothing that changes with the key's
   *  size — cut into pieces that are stretched onto the key. A rotation or a window resize only re-places them.
   *  The pressed FLOOD (the gap opening round the sunk cap) is sliced the same way and fades in exactly as before.
   *  Proven against the exact per-size light in pixels (scripts/test_keylight_nine.ts: no seams, mean ≤ 2.2 / 255). */
  const travel = props.travel ?? 0;
  const restNine = lightNine(props.cut, props.cap, props.reach, 0);
  const openNine = props.pressStyle ? lightNine(props.cut, props.cap, props.reach, travel) : null;
  if (restNine && (!props.pressStyle || openNine)) return <KeyLightNine {...props} travel={travel} rest={restNine} open={openNine} />;
  return <KeyLightExact {...props} />;
});

/** The sliced light: two canonical pictures (rest, opened), placed as pieces. */
function KeyLightNine({ cut, reach, led, chassis, pressStyle, travel, rest, open }: {
  cut: RR; reach: number; led: LedColour; chassis: string; pressStyle?: PressStyle; travel: number;
  rest: LightNine; open: LightNine | null;
}) {
  const img = useSharedSprite(keyLightKey(rest.cut, rest.cap, reach, led, chassis, false, 0),
    () => keyLightBuild(rest.cut, rest.cap, reach, led, false, 0));
  const openImg = useSharedSprite(open ? keyLightKey(open.cut, open.cap, reach, led, chassis, true, travel) : null,
    () => keyLightBuild(open!.cut, open!.cap, reach, led, true, travel));
  if (!img) return null;
  const sp = keyLightSprite(cut, reach);
  const box = { position: 'absolute' as const, left: sp.x, top: sp.y, width: sp.w, height: sp.h };
  return (
    <>
      <View pointerEvents="none" style={box}><NinePieces image={img} n={rest} reach={reach} W={sp.w} H={sp.h} /></View>
      {open && openImg && (
        <Animated.View pointerEvents="none" style={[box, pressStyle]}>
          <NinePieces image={openImg} n={open} reach={reach} W={sp.w} H={sp.h} />
        </Animated.View>
      )}
    </>
  );
}

/** The pieces of one canonical picture on a W × H sprite: each a clipping view with the whole picture inside it,
 *  scaled so that the piece's part of the picture fills it. Plain Images on every platform — no canvas per piece. */
function NinePieces({ image, n, reach, W, H }: { image: SkImage; n: LightNine; reach: number; W: number; H: number }) {
  const uri = spriteUri(image);
  const pieces = useMemo(() => ninePieces(n, reach, W, H), [n, reach, W, H]);
  if (!uri) return null;
  const W0 = 2 * n.cx + NINE_STRIP, H0 = 2 * n.cy + 2 * NINE_STRIP;
  return (
    <>
      {pieces.map((p, k) => {
        const sx = p.dst.w / p.src.w, sy = p.dst.h / p.src.h;
        return (
          <View key={k} pointerEvents="none"
                style={{ position: 'absolute', left: p.dst.x, top: p.dst.y, width: p.dst.w, height: p.dst.h, overflow: 'hidden' }}>
            <Image source={{ uri }} fadeDuration={0} resizeMode="stretch"
                   style={{ position: 'absolute', left: -p.src.x * sx, top: -p.src.y * sy, width: W0 * sx, height: H0 * sy }} />
          </View>
        );
      })}
    </>
  );
}

/** The exact light, drawn to the key's own size — only for a key too small for the pieces' corners. */
const KeyLightExact = function KeyLightExact({ cut: liveCut, cap: liveCap, reach, led, chassis, pressStyle, travel = 0 }: {
  cut: RR; cap: RR; reach: number; led: LedColour; chassis: string; travel?: number; pressStyle?: PressStyle;
}) {
  // Built at the settled size, stretched onto the live box while a resize moves (3bf5e2c6).
  const { cut, cap } = useSettled({ cut: liveCut, cap: liveCap }, `${rrKey(liveCut)}|${rrKey(liveCap)}`);
  const img = useSharedSprite(keyLightKey(cut, cap, reach, led, chassis, false, 0),
    () => keyLightBuild(cut, cap, reach, led, false, 0));
  const open = useSharedSprite(pressStyle ? keyLightKey(cut, cap, reach, led, chassis, true, travel) : null,
    () => keyLightBuild(cut, cap, reach, led, true, travel));
  if (!img) return null;
  const sp = keyLightSprite(cut, reach);
  const box = stretchOnto(sp, keyLightSprite(liveCut, reach));
  return (
    <>
      <SpriteView image={img} width={sp.w} height={sp.h} style={box} />
      {open && (
        <Animated.View pointerEvents="none" style={[box, pressStyle]}>
          <SpriteView image={open} width={sp.w} height={sp.h} />
        </Animated.View>
      )}
    </>
  );
};

/** A `from`-sized box placed and scaled so it covers `to` (both in the key's coordinates). RN scales about the
 *  centre, so the box is centred on `to` first. Equal sizes ⇒ no transform at all. */
function stretchOnto(from: { w: number; h: number }, to: { x: number; y: number; w: number; h: number }) {
  const sx = from.w > 0 ? to.w / from.w : 1, sy = from.h > 0 ? to.h / from.h : 1;
  return {
    position: 'absolute' as const, width: from.w, height: from.h,
    left: to.x + (to.w - from.w) / 2, top: to.y + (to.h - from.h) / 2,
    ...(sx !== 1 || sy !== 1 ? { transform: [{ scaleX: sx }, { scaleY: sy }] } : null),
  };
}

/**
 * The pressed wash over the cap, as a view filling its parent (the cap, or an outline key's face): put
 * it OVER the face and UNDER the legend. Faded in by `pressStyle`; invisible — and drawn once — at rest.
 */
export const KeyWash = React.memo(function KeyWash({ cap: liveCap, led, chassis, pressStyle, radius }: {
  cap: RR; led: LedColour; chassis: string; pressStyle: PressStyle; radius: number;
}) {
  // ★ As KeyLight: built at the settled size, stretched onto the live cap while a resize moves.
  const cap = useSettled(liveCap, rrKey(liveCap));
  /* ★★ NOT ON THE TURN'S CLOCK (2026-10-11): the wash is seen only while a key is held, and on a small key its softness
   *  follows the key's size, so it cannot be one fixed picture. It is built a moment AFTER the key appears instead of
   *  during the rotation or resize that made the key — long before anyone can press it. */
  const [ready, setReady] = useState(false);
  useEffect(() => { const t = setTimeout(() => setReady(true), 300); return () => clearTimeout(t); }, []);
  const img = useSharedSprite(
    ready && cap.w > 1 && cap.h > 1 ? `keywash|${chassis}|${colourKey(led)}|${rrKey({ ...cap, x: 0, y: 0 })}` : null,
    () => imageBuild(makeSprite(cap.w, cap.h, (c) => drawKeyWash(Skia, c, cap, a => ledA(led, a)))));
  if (!img) return null;
  return (
    <Animated.View pointerEvents="none"
      style={[{ ...stretchOnto(cap, { x: 0, y: 0, w: liveCap.w, h: liveCap.h }), borderRadius: radius }, pressStyle]}>
      <SpriteView image={img} width={cap.w} height={cap.h} />
    </Animated.View>
  );
});
