/**
 * ★★ THE STATUS BAR STEPS ASIDE IN LANDSCAPE ON THE iPHONE DUO (Stuart, 2026-11-25 — folded, open and in Split View).
 *
 *  A window UIKit reports a hinge for (useReservedRegions) and that is wider than it is tall asks iOS to hide the
 *  clock and Wi-Fi (VibePowerModule.setStatusBarHidden → VibeRootViewController.prefersStatusBarHidden; React Native's
 *  own StatusBar.setHidden is a no-op from iOS 27). Portrait, and every device without a hinge, leave it alone.
 *  ★ iOS may still draw it in a Split View share: the camera / status-pill avoidance stays in place as the fallback,
 *    and the camera is reserved regardless.
 */
import { useEffect } from 'react';
import { NativeModules, Platform, useWindowDimensions } from 'react-native';
import { useReservedRegions } from './useReservedRegions';

const VP: { setStatusBarHidden?: (hidden: boolean) => void } | undefined =
  Platform.OS === 'ios' ? (NativeModules as any).VibePowerModule : undefined;

export function useDuoStatusBar(): void {
  const { width, height } = useWindowDimensions();
  const reserved = useReservedRegions();
  const hide = reserved.hinge != null && width > height;
  useEffect(() => { VP?.setStatusBarHidden?.(hide); }, [hide]);
  useEffect(() => () => { VP?.setStatusBarHidden?.(false); }, []);
}
