/**
 * glowSprite — draw something that GLOWS once, into an image, and blit the image thereafter.
 *
 * ★★ PERF (faceplates brief §4.3 / §7, Xcover 4S): a glow is a blur, and a blur is the expensive
 *   thing to rasterise. Tube digits, segment digits and the RDS annunciator glow; they change at the
 *   tuning rate, which during a fast spin IS the frame rate. So the glow is rasterised ONCE per
 *   glyph (at device pixel ratio) and every later frame only draws an image — no live BlurMask.
 * ★ The snapshot is converted to a non-texture (raster) image so it can be drawn on any canvas; an
 *   offscreen GPU texture belongs to the offscreen context.
 */

import { useEffect, useMemo } from 'react';
import { PixelRatio } from 'react-native';
import { spritePixels } from '../constants/spriteSizing';
import { SpriteCache, type Built } from '../constants/spriteCache';
import { BlurStyle, Skia, type SkCanvas, type SkImage, type SkPaint } from '@shopify/react-native-skia';

/** Rasterise `draw` into a w × h (pt) image at the screen's pixel ratio. null if no surface. */
export function makeSprite(w: number, h: number, draw: (c: SkCanvas) => void): SkImage | null {
  const pr = PixelRatio.get();
  const px = spritePixels(w, h, pr);
  if (!px) { console.warn(`[sprite] refused ${w} x ${h} pt at ${pr}x`); return null; }
  const { pw, ph } = px;
  const surf = Skia.Surface.MakeOffscreen(pw, ph);
  if (!surf) return null;
  const c = surf.getCanvas();
  c.scale(pr, pr);
  draw(c);
  surf.flush();
  /* ★★★ DISPOSE THE GPU SIDE AT ONCE. The offscreen surface and its texture snapshot were left for Hermes to
   *  collect, and Hermes feels no pressure from native memory — on Stuart's Mac that was most of 370–460 MB of
   *  graphics in ~900 regions (audit, 2026-10-02). The raster copy is all anyone draws; the surface and the
   *  snapshot are scaffolding. If the raster copy fails, the snapshot is kept (it holds its own reference to
   *  the pixels, so the surface can still go). */
  const snap = surf.makeImageSnapshot();
  const img = snap.makeNonTextureImage();
  if (img) snap.dispose();
  surf.dispose();
  return img ?? snap;
}

// ── One shared, bounded sprite cache (constants/spriteCache.ts) ───────────────

/** ~40 MB of raster sprites: every glyph set on screen at once with room for a colour or size change. */
const SPRITE_BUDGET_BYTES = 40 * 1024 * 1024;
const SPRITE_GRACE_MS = 5000;
const shared = new SpriteCache<unknown>(SPRITE_BUDGET_BYTES, SPRITE_GRACE_MS, () => Date.now());

/** A build of one or more images, ready for the cache: the value to share and the images it owns. */
export function spriteBuild<T>(value: T | null, images: Array<SkImage | null | undefined>): Built<T> | null {
  if (value == null) return null;
  const own = images.filter((i): i is SkImage => !!i);
  return {
    value,
    bytes: own.reduce((n, i) => n + i.width() * i.height() * 4, 0),
    dispose: () => { for (const i of own) i.dispose(); },
  };
}

/** One image as a cache build. */
export const imageBuild = (img: SkImage | null): Built<SkImage> | null => spriteBuild(img, [img]);

/**
 * ★★ THE ONE WAY A COMPONENT GETS A SPRITE: shared across every instance with the same `key`, retained while
 * mounted, freed when the cache needs the room and nobody draws it. `key` MUST name everything `make` reads
 * (glyph, size, colours, style) — the pixel ratio is added here. null key = nothing to draw.
 */
export function useSharedSprite<T>(key: string | null, make: () => Built<T> | null): T | null {
  const full = key == null ? null : `${PixelRatio.get()}|${key}`;
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const value = useMemo(() => (full == null ? null : (shared.get(full, make as () => Built<unknown> | null) as T | null)), [full]);
  useEffect(() => {
    if (full == null) return;
    shared.retain(full);
    return () => shared.release(full);
  }, [full]);
  return value;
}

/** A fill paint, optionally blurred. `blurPx` is the CSS blur RADIUS (sigma = radius / 2). */
export function glowPaint(color: string, blurPx = 0): SkPaint {
  const p = Skia.Paint();
  p.setAntiAlias(true);
  p.setColor(Skia.Color(color));
  if (blurPx > 0) p.setMaskFilter(Skia.MaskFilter.MakeBlur(BlurStyle.Normal, blurPx / 2, true));
  return p;
}

/** A CSS glow stack, outermost first when drawn: `[[radius, colour], …]` then the core on top. */
export type GlowStack = Array<[number, string]>;
