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

import { PixelRatio } from 'react-native';
import { spritePixels } from '../constants/spriteSizing';
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
  const img = surf.makeImageSnapshot();
  return img.makeNonTextureImage() ?? img;
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
