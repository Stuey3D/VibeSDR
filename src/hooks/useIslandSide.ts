/**
 * ★★★ WHICH SIDE IS THE DYNAMIC ISLAND / NOTCH ON? (Stuart, 2026-10-06, RC16 iPhone landscape: "the health and timer
 * pills are huge when collapsed, bigger than when they are open").
 *
 *  iOS reports the landscape safe-area inset on BOTH sides, whichever side the island is really on, so an edge chip
 *  that pads by `insets.right` carries ~59 pt of empty frame on the side without one. The interface orientation
 *  (modules/vibe-icloud-kvs/VibeInterfaceOrientation.mm, KVO on UIWindowScene.effectiveGeometry) says which side:
 *
 *    landscapeRight — Apple: "the Home button on the right" ⇒ the top of the phone, and the island, on the LEFT.
 *    landscapeLeft  — Home button on the left ⇒ the island on the RIGHT.
 *
 *  ★ null = do not know (Android — its cutout insets are already asymmetric — the native module missing from an older
 *    build, or the first value not in yet). Callers then keep the safe behaviour: the full inset.
 *  ★ 'none' = portrait / upside down: neither long edge has the island.
 *  ★ The event also fires on the 180° flip between the two landscapes, which changes neither the window size nor the
 *    insets — nothing else in JS sees that rotation.
 */
import { useEffect, useState } from 'react';
import { NativeEventEmitter, NativeModules, Platform } from 'react-native';
import { islandSideFor, type IslandSide } from '../components/edgeChipGeometry';

const M: { get(): Promise<string> } | undefined =
  Platform.OS === 'ios' ? (NativeModules as any).VibeInterfaceOrientation : undefined;

export function useIslandSide(): IslandSide | null {
  const [side, setSide] = useState<IslandSide | null>(null);
  useEffect(() => {
    if (!M) return;
    let live = true;
    const emitter = new NativeEventEmitter(M as any);
    const sub = emitter.addListener('interfaceOrientation', (e: { orientation?: string }) => {
      if (live) setSide(islandSideFor(e?.orientation));
    });
    M.get().then((o) => { if (live) setSide(islandSideFor(o)); }).catch(() => {});
    return () => { live = false; sub.remove(); };
  }, []);
  return side;
}
