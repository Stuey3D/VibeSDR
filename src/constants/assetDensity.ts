/**
 * assetDensity — how many device pixels per point the app's GENERATED ARTWORK is drawn at: the key lights, glow
 * sprites, drum pictures. ★ Not the live canvases (waterfall, spectrum, the plate), not text.
 *
 * ★★ HIGH-RESOLUTION TEXTURES (Stuart, 2026-10-11): "we maybe have a high resolution assets option … that way we can use
 *   lower resolution stuff upscaled to hopefully save memory and processor". These are soft glows and brushed metal:
 *   drawn at HALF density and scaled up they lose almost nothing ("you have to be really looking for it"). Measured on
 *   the Moto G35 (metal, Nixie, analogue): janky frames 4.1 % → 0.06 %, worst frames 97 → 17 ms, idle 842 → 761 MB with
 *   swap 176 → 75 MB. "It feels a lot smoother."
 *   AUTO (no pick made) is STANDARD — half density — on a dense Android screen (≥ 2.5 device pixels per point: the
 *   1080p phones) and on any device with 3 GB or less (an iPhone SE, the 1 GB phones); HIGH everywhere else.
 */
import { PixelRatio, Platform } from 'react-native';
import { LOW_MEMORY_BYTES } from './transparency';

export type Textures = 'auto' | 'high' | 'standard';

/** What AUTO resolves to on this device (pure: the inputs are passed in, for the test). */
export function autoTextures(o: { os: string; pixelRatio: number; totalMemoryBytes: number | null }): 'high' | 'standard' {
  if (o.totalMemoryBytes != null && o.totalMemoryBytes < LOW_MEMORY_BYTES) return 'standard';
  if (o.os === 'android' && o.pixelRatio >= 2.5) return 'standard';
  return 'high';
}

/** The fraction of the device's density for each resolved setting. */
export const TEXTURE_SCALE: Record<'high' | 'standard', number> = { high: 1, standard: 0.5 };

let scale = 1;
/** Set from FaceplateContext as the setting resolves — before anything draws. */
export function setAssetScale(s: number) { scale = s > 0 ? s : 1; }

/** Device pixels per point for generated artwork — never below 1. Part of every sprite's cache key. */
export function assetPixelRatio(): number {
  return Math.max(1, Math.round(PixelRatio.get() * scale * 4) / 4);
}

export const OS = Platform.OS;
