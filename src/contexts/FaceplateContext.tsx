/**
 * FaceplateContext — the faceplate settings, stored, and resolved ONCE for the whole tree.
 *
 * `useFaceplate()` is what the deck draws with (ControlsBar, DrumWheel, TunerKeys, VTSBar,
 * DecoderShell). `useFaceplateSettings()` is for whatever edits them — the CONTROL CUSTOMISATION
 * pane in MenuSheet (brief §1). Stored as `lsv_faceplate`; with nothing stored the app renders
 * exactly as it did.
 *
 * ★★ App-wide, not per server: a faceplate is the hardware in your hand, not a property of the
 *   receiver you are listening to. (Display prefs are per server; this deliberately is not.)
 * ★ Resolution is memoised on the settings object, so a meter update never re-resolves anything —
 *   the theme changes only when a setting does.
 * ★★★ TRANSPARENCY EFFECTS: until the user picks ON / OFF, the DEVICE decides (low-end detection,
 *   src/constants/transparency.ts). This file only GATHERS the signals; the decision is the pure
 *   `autoTransparency`. The theme is resolved with the EFFECTIVE value in `settings.transparency`,
 *   and the auto value is never stored — only a pick from the pane is (`setTransparency`).
 */

import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState,
  type ReactNode } from 'react';
import { AccessibilityInfo, Platform } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import {
  DEFAULT_SETTINGS, FACEPLATE_STORAGE_KEY, parseSettings, resolveFaceplate, withDisplay, withText,
  withTransparency,
  type FaceplateSettings, type FaceplateTheme, type DisplayStyle, type TextColour, type Transparency,
  type SurfaceTokens,
} from '../constants/faceplate';
import {
  autoTransparency, effectiveTransparency, parseDeviceClass, parseOsVersion,
  type AutoTransparency, type DeviceSignals,
} from '../constants/transparency';
import { installNativeTransliterator } from '../services/transliterator';
import { explainMeterFallback, takeUncleanMeterExit } from '../services/meterGuard';
import { meterAfterUncleanExit } from '../constants/meters';
import { readNativeDeviceClass } from '../services/deviceClass';

// ★ The dot-matrix / 14-segment displays transliterate non-Latin names with the platform's ICU
//   (brief §7). Installed once, when the faceplate owner loads — before any display draws.
installNativeTransliterator();

interface FaceplateContextValue {
  theme:      FaceplateTheme;
  /** ★ What is ON SCREEN: `transparency` here is the effective value (the user's pick, or the
   *  device's default while `transparencyExplicit` is false) — what the pane must show lit. */
  settings:   FaceplateSettings;
  /** The device's own default and why — the pane says so under the row until the user picks. */
  autoTransparency: AutoTransparency;
  /** A pick from the pane: ON / OFF, stored as the user's choice from then on. */
  setTransparency: (t: Transparency) => void;
  /** Display, with its side effects (controls → neon for Nixie, back to amber leaving it; text
   *  colour clamped / restored per display). */
  setDisplay: (d: DisplayStyle) => void;
  setText:    (t: TextColour) => void;
  /** The rest have no side effects. */
  set:        (patch: Partial<Pick<FaceplateSettings, 'chassis' | 'controls' | 'meter' | 'steadyLeds'>>) => void;
}

const DEFAULT_THEME = resolveFaceplate(DEFAULT_SETTINGS);
const AUTO_ON: AutoTransparency = { transparency: 'on', reason: null };

const FaceplateContext = createContext<FaceplateContextValue>({
  theme: DEFAULT_THEME, settings: DEFAULT_SETTINGS, autoTransparency: AUTO_ON,
  setTransparency: () => {}, setDisplay: () => {}, setText: () => {}, set: () => {},
});

/**
 * The signals low-end detection reads (transparency.ts header says where each comes from).
 * Synchronous — RAM / model / Mac come from one blocking native getter — so the first frame already
 * has the device's default; only Reduce Transparency arrives a moment later, from a promise.
 */
function baseSignals(): DeviceSignals {
  const os = Platform.OS === 'ios' ? 'ios' : Platform.OS === 'android' ? 'android' : 'other';
  return {
    os, osVersion: parseOsVersion(Platform.Version as string | number),
    isPad: Platform.OS === 'ios' && !!(Platform as { isPad?: boolean }).isPad,
    isTV: !!Platform.isTV,
    reduceTransparency: false,
    // ★ No getter (old binary, Expo Go, web) → null / null / false: decides nothing.
    ...parseDeviceClass(readNativeDeviceClass()),
  };
}

/**
 * @param legacyThemeName  ThemeContext's font choice ('amber' = Nixie One, 'white' = Atkinson),
 *   used ONCE to seed Display when nothing has ever been stored (§1 migration).
 */
export function FaceplateProvider({ children, legacyThemeName = 'white' }:
    { children: ReactNode; legacyThemeName?: string }) {
  const [settings, setSettings] = useState<FaceplateSettings>(() => parseSettings(null, legacyThemeName));
  // ★ A change made before the stored copy has loaded must not be overwritten by that load.
  const touched = useRef(false);

  // ★★ The OS Reduce Transparency switch (iOS; Android has none and always reports false). Followed
  //   LIVE — but it can only move what is on screen while the user has not chosen, because
  //   effectiveTransparency() lets a stored choice win. Nothing here is ever stored.
  const [reduceTransparency, setReduceTransparency] = useState(false);
  useEffect(() => {
    if (Platform.OS !== 'ios') return;
    let live = true;
    AccessibilityInfo.isReduceTransparencyEnabled()
      .then((on: boolean) => { if (live) setReduceTransparency(on); })
      .catch(() => {});
    const sub = AccessibilityInfo.addEventListener('reduceTransparencyChanged',
      (on: boolean) => setReduceTransparency(on));
    return () => { live = false; sub.remove(); };
  }, []);
  const base = useMemo(baseSignals, []);
  const auto = useMemo(() => autoTransparency({ ...base, reduceTransparency }), [base, reduceTransparency]);

  useEffect(() => {
    let live = true;
    Promise.all([AsyncStorage.getItem(FACEPLATE_STORAGE_KEY), takeUncleanMeterExit()])
      .then(([j, armed]: [string | null, string | null]) => {
        if (!live || touched.current) return;
        let s = parseSettings(j, legacyThemeName);
        // ★★★ THE METER CANNOT LOCK YOU OUT (services/meterGuard.ts): the last run died while this
        //   meter was starting — come back on the bar, stored, and say so once.
        const meter = meterAfterUncleanExit(s.meter, armed);
        const fellBack = meter !== s.meter;
        if (fellBack) s = { ...s, meter };
        setSettings(s);
        // ★ Write the migrated result back, so the migration runs once and a later change to the
        //   legacy default cannot re-seed Display under a user who never touched it.
        if (!j || fellBack) AsyncStorage.setItem(FACEPLATE_STORAGE_KEY, JSON.stringify(s)).catch(() => {});
        if (fellBack && armed) explainMeterFallback(armed, 'crash');
      })
      .catch(() => {});
    return () => { live = false; };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const commit = useCallback((f: (s: FaceplateSettings) => FaceplateSettings) => {
    touched.current = true;
    setSettings((prev: FaceplateSettings) => {
      const next = f(prev);
      if (next !== prev) AsyncStorage.setItem(FACEPLATE_STORAGE_KEY, JSON.stringify(next)).catch(() => {});
      return next;
    });
  }, []);

  const setDisplay = useCallback((d: DisplayStyle) => commit(s => withDisplay(s, d)), [commit]);
  const setText    = useCallback((t: TextColour)   => commit(s => withText(s, t)), [commit]);
  const set        = useCallback((patch: Partial<FaceplateSettings>) =>
    commit(s => ({ ...s, ...patch })), [commit]);
  const setTransparency = useCallback((t: Transparency) => commit(s => withTransparency(s, t)), [commit]);

  // ★ The effective settings: what the theme is resolved from and what the pane shows lit. The
  //   STORED object (`settings`) keeps the user's own `transparency` / `transparencyExplicit`.
  const transparency = effectiveTransparency(settings, auto);
  const onScreen = useMemo(() => settings.transparency === transparency ? settings : { ...settings, transparency },
                           [settings, transparency]);
  const theme = useMemo(() => resolveFaceplate(onScreen), [onScreen]);
  const value = useMemo(() => ({ theme, settings: onScreen, autoTransparency: auto, setTransparency,
                                 setDisplay, setText, set }),
                        [theme, onScreen, auto, setTransparency, setDisplay, setText, set]);
  return <FaceplateContext.Provider value={value}>{children}</FaceplateContext.Provider>;
}

/** The resolved faceplate — what components draw with. */
export function useFaceplate(): FaceplateTheme {
  return useContext(FaceplateContext).theme;
}

/**
 * ★★★ Transparency OFF — for anything that draws a see-through surface: the deck, DecoderShell,
 * MenuSheet and row 10's PopupShell (menus, sheets, chat, modals). True means: alpha 1.0 EXACTLY
 * (`solidOver()` the colour you had — faceplate.ts), the colour ON the view that carries the shadow,
 * and no BlurView. One switch, one reader of it.
 */
export function useSurfaceOpaque(): boolean {
  return useContext(FaceplateContext).theme.opaque;
}

/**
 * ★★★ The whole OFF contract for a surface that draws itself (row 10's PopupShell: menus, sheets,
 * chat, modals): `fill()` its colours, BlurView only if `blur`, the scrim's opacity × `scrimOpacity`
 * (0 = no dim — keep the invisible tap-to-close view), shadows only if `dropShadow`
 * (NO_DROP_SHADOW otherwise). See SurfaceTokens in faceplate.ts.
 */
export function useSurface(): SurfaceTokens {
  return useContext(FaceplateContext).theme.surface;
}

/** Settings + setters — for the settings pane (and ThemeContext's legacy setTheme). */
export function useFaceplateSettings(): FaceplateContextValue {
  return useContext(FaceplateContext);
}
