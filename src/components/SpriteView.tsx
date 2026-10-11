/**
 * SpriteView — a ready-made Skia image (a glow sprite, a key's light) shown as a static picture.
 *
 * ★★★ ON ANDROID, NOT A CANVAS (the diet, 2026-10-11, Moto G35). There every Skia <Canvas> is its own TextureView:
 *   a GPU surface with 2–3 buffers, composited on EVERY frame and rebuilt on rotation. The Nixie skin had 28 of them,
 *   most only ever showing one fixed image (KeyLight's rest light, its pressed light, the pressed wash — three per
 *   key). An image does not need a canvas: Android gets it encoded ONCE to PNG and shown in a plain React Native
 *   Image, an ordinary view with no surface of its own. iOS keeps the canvas — Skia's Metal layers are cheap there,
 *   and nothing about the iPhone's look or timing changes.
 * ★ The PNG is made once per SkImage (WeakMap — it goes when the image does) and decoded once by Android.
 */
import React, { useMemo } from 'react';
import { Image, Platform, type StyleProp, type ViewStyle } from 'react-native';
import { Canvas, Image as SkImageNode, type SkImage } from '@shopify/react-native-skia';
import { useSettledKey } from '../hooks/useSettledKey';

const uris = new WeakMap<SkImage, string>();
function dataUri(img: SkImage): string | null {
  const have = uris.get(img);
  if (have) return have;
  try {
    const b64 = img.encodeToBase64();
    if (!b64) return null;
    const uri = 'data:image/png;base64,' + b64;
    uris.set(img, uri);
    return uri;
  } catch { return null; }
}

export function SpriteView({ image, width, height, style }: {
  image: SkImage; width: number; height: number; style?: StyleProp<ViewStyle>;
}) {
  const uri = useMemo(() => (Platform.OS === 'android' ? dataUri(image) : null), [image]);
  // ★★ iOS / Mac: a canvas that changes size can keep its OLD surface — a key's light left at the key's previous size,
  //   a ghost outline beside it after a window resize (Stuart, 2026-10-11). Rebuilt once the size has settled.
  const settled = useSettledKey(Math.round(width), Math.round(height));
  if (uri) {
    return (
      <Image source={{ uri }} fadeDuration={0} resizeMode="stretch"
             style={[{ width, height }, style as any]} />
    );
  }
  return (
    <Canvas key={settled} pointerEvents="none" style={[{ width, height }, style]}>
      <SkImageNode image={image} x={0} y={0} width={width} height={height} />
    </Canvas>
  );
}
