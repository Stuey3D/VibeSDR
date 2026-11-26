/**
 * ★★★ THE WINDOW'S RESERVED REGIONS — what iOS draws over the app, and where it folds (iPhone Duo, Stuart 2026-11-25).
 *
 *  Apple, HIG "Designing for iPhone Duo" › Reserved regions: "In addition to standard considerations for safe areas,
 *  available space on iPhone Duo is shaped by reserved regions" — the outer front camera (always present, with the
 *  status bar beside it on the trailing edge), the inner camera (only while it is active) and the folding region
 *  ("conditional … When the device is partially open, the folding region divides the inner display"). "Use the
 *  ReservedRegion API to keep important elements clear of the center if the system doesn't move them automatically."
 *
 *  ★ Safe-area insets do NOT carry the outer camera: on the folded front screen insets.top is 0 and the app draws
 *    under the camera and the clock — which is the design (Stuart: the spectrum runs full-bleed and the icons float
 *    over it). Only OUR readouts and controls have to stay out of the way, and these rects say where "out of the way"
 *    is. They come from UIView -reservedRegionsOfKind: on the key window (modules/vibe-icloud-kvs/
 *    VibeInterfaceOrientation.mm), frame INCLUDING the margins Apple wants interactive content to keep.
 *  ★ Empty everywhere else: Android, iOS before 27.1, an app built with an older SDK, any iPhone or iPad without such a
 *    region. Every caller then keeps exactly what it did before, so no other device moves.
 */
import { useEffect, useState } from 'react';
import { NativeEventEmitter, NativeModules, Platform, useWindowDimensions } from 'react-native';

import { NO_REGIONS as NONE, type HingeState, type RegionRect, type ReservedRegions } from '../utils/duoGeometry';
export type { HingeState, RegionRect, ReservedRegions } from '../utils/duoGeometry';
export { avoidRects, clearBelow, rightLimitAt, deckClearance, namePlacement } from '../utils/duoGeometry';

const M: { getReservedRegions?(): Promise<any> } | undefined =
  Platform.OS === 'ios' ? (NativeModules as any).VibeInterfaceOrientation : undefined;

function clean(list: any): RegionRect[] {
  if (!Array.isArray(list)) return [];
  return list
    .map((r: any) => ({ x: Number(r?.x), y: Number(r?.y), w: Number(r?.w), h: Number(r?.h) }))
    .filter((r) => [r.x, r.y, r.w, r.h].every(Number.isFinite) && r.w > 0 && r.h > 0);
}

function parse(body: any): ReservedRegions {
  const occlusion = clean(body?.occlusion), division = clean(body?.division), statusBar = clean(body?.statusBar);
  const h = body?.hinge;
  const hinge: HingeState | null = h && typeof h === 'object' && typeof h.status === 'string'
    ? { status: (['closed', 'partiallyOpen', 'fullyOpen'].includes(h.status) ? h.status : 'unknown') as HingeState['status'],
        angleDeg: Number.isFinite(Number(h.angleDeg)) ? Number(h.angleDeg) : 0 }
    : null;
  return occlusion.length || division.length || statusBar.length || hinge
    ? { occlusion, division, statusBar, hinge } : NONE;
}

function same(a: ReservedRegions, b: ReservedRegions): boolean {
  if (a === b) return true;
  const eq = (p: RegionRect[], q: RegionRect[]) => p.length === q.length && p.every((r, i) =>
    r.x === q[i].x && r.y === q[i].y && r.w === q[i].w && r.h === q[i].h);
  return eq(a.occlusion, b.occlusion) && eq(a.division, b.division) && eq(a.statusBar, b.statusBar)
    && (a.hinge?.status ?? null) === (b.hinge?.status ?? null) && (a.hinge?.angleDeg ?? null) === (b.hinge?.angleDeg ?? null);
}

export function useReservedRegions(): ReservedRegions {
  const [regions, setRegions] = useState<ReservedRegions>(NONE);
  const { width, height } = useWindowDimensions();
  useEffect(() => {
    if (!M?.getReservedRegions) return;
    let live = true;
    const take = (body: any) => {
      if (!live) return;
      const next = parse(body);
      setRegions((prev) => (same(prev, next) ? prev : next));
    };
    const emitter = new NativeEventEmitter(M as any);
    const sub = emitter.addListener('reservedRegions', take);
    return () => { live = false; sub.remove(); };
  }, []);
  /* ★ And ask on every window size: a fold, an unfold or a Split View resize re-lays the window, and the event that
   *  follows it may land a frame after the size does. Asking here closes that gap. */
  useEffect(() => {
    if (!M?.getReservedRegions) return;
    let live = true;
    M.getReservedRegions()
      .then((body) => { if (live) setRegions((prev) => { const next = parse(body); return same(prev, next) ? prev : next; }); })
      .catch(() => {});
    return () => { live = false; };
  }, [width, height]);
  return regions;
}

