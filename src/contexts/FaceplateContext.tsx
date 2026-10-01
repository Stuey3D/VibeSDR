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
import { AccessibilityInfo, AppState, Platform } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import {
  decideLaunch, DEFAULT_SETTINGS, FACEPLATE_STORAGE_KEY, frameRateCapHz, parseSettings, resolveFaceplate, withDisplay, withText,
  withTransparency,
  type FaceplateSettings, type FaceplateTheme, type DisplayStyle, type TextColour, type Transparency,
  type SurfaceTokens,
} from '../constants/faceplate';
import {
  autoTransparency, effectiveTransparency, parseDeviceClass, parseOsVersion,
  type AutoTransparency, type DeviceSignals,
} from '../constants/transparency';
import { installNativeTransliterator } from '../services/transliterator';
import { explainFaceplateReset, faceplateGuard, LAST_CRASHED_KEY, launchMarkOnce } from '../services/faceplateGuard';
import { readNativeDeviceClass } from '../services/deviceClass';
import { applyFrameRateCap, readMaxRefreshRate } from '../services/frameRate';

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
  /** ★ The panel's top refresh rate (Hz), or null until known / when the binary cannot say — the
   *  FRAME RATE row is shown only above 60 (faceplate.ts `frameRateChoices`). */
  maxRefreshHz: number | null;
  /** A pick from the pane: ON / OFF, stored as the user's choice from then on. */
  setTransparency: (t: Transparency) => void;
  /** Display, with its side effects (controls → neon for Nixie, back to amber leaving it; text
   *  colour clamped / restored per display). */
  setDisplay: (d: DisplayStyle) => void;
  setText:    (t: TextColour) => void;
  /** The rest have no side effects. */
  set:        (patch: Partial<Pick<FaceplateSettings, 'chassis' | 'controls' | 'meter' | 'steadyLeds' | 'frameRate'>>) => void;
}

const DEFAULT_THEME = resolveFaceplate(DEFAULT_SETTINGS);
const AUTO_ON: AutoTransparency = { transparency: 'on', reason: null };

const FaceplateContext = createContext<FaceplateContextValue>({
  theme: DEFAULT_THEME, settings: DEFAULT_SETTINGS, autoTransparency: AUTO_ON, maxRefreshHz: null,
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

  // ★★★ A FACEPLATE CANNOT LOCK YOU OUT (constants/faceplate.ts CRASH SAFETY). The previous run's crash mark
  //   is read SYNCHRONOUSLY, here, before anything can arm — and the stored settings are applied only
  //   through decideLaunch, so a faceplate the last run died drawing is never drawn again unasked.
  //   (Until the stored copy loads, the tree draws DEFAULT_SETTINGS, which is the safe faceplate.)
  const launchMark = useMemo(launchMarkOnce, []);
  // ★ FRAME RATE is pushed to the native side only once the STORED copy is known (or the user has
  //   picked): the defaults drawn before the load would otherwise lift a stored 60 Hz cap — which the
  //   native side already applied at launch from its own copy — for the moment the load takes.
  const [storedKnown, setStoredKnown] = useState(false);
  useEffect(() => {
    let live = true;
    AsyncStorage.getItem(FACEPLATE_STORAGE_KEY)
      .then((j: string | null) => {
        if (live) setStoredKnown(true);
        if (!live || touched.current) return;
        const { settings: s, crashed } = decideLaunch(parseSettings(j, legacyThemeName), launchMark);
        // ★ Written BEFORE the stored faceplate is drawn: this launch is now the one on trial.
        faceplateGuard.arm(s);
        setSettings(s);
        // ★ Write the migrated result back, so the migration runs once and a later change to the
        //   legacy default cannot re-seed Display under a user who never touched it.
        if (!j || crashed) AsyncStorage.setItem(FACEPLATE_STORAGE_KEY, JSON.stringify(s)).catch(() => {});
        if (crashed) {
          AsyncStorage.setItem(LAST_CRASHED_KEY, JSON.stringify(crashed)).catch(() => {});
          explainFaceplateReset();
        }
      })
      .catch(() => { if (live) setStoredKnown(true); });
    return () => { live = false; };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // ★ Going to the BACKGROUND ends the trial (a swipe-away from the switcher is not a crash);
  //   coming back puts the faceplate on trial again for a few seconds (surfaces are rebuilt).
  // ★★ NOT 'inactive': iOS goes inactive for a system prompt at launch (local network permission),
  //   Control Centre or an incoming call — still drawing, still able to crash. Clearing there let a
  //   crashing faceplate through for a launch (B8, iPhone: the reset came on the THIRD attempt). A
  //   swipe-away straight from inactive costs at worst one needless reset, never a lock-out.
  const settingsRef = useRef(settings);
  settingsRef.current = settings;
  useEffect(() => {
    const sub = AppState.addEventListener('change', (st: string) => {
      if (st === 'active') faceplateGuard.arm(settingsRef.current);
      else if (st === 'background') faceplateGuard.clear();
    });
    return () => sub.remove();
  }, []);

  // ★★ FRAME RATE: live, no restart (services/frameRate.ts). Re-sent whenever it changes.
  const capHz = frameRateCapHz(settings);
  const capKnown = storedKnown || touched.current;
  useEffect(() => { if (capKnown) applyFrameRateCap(capHz); }, [capKnown, capHz]);
  const [maxRefreshHz, setMaxRefreshHz] = useState<number | null>(null);
  useEffect(() => {
    let live = true;
    readMaxRefreshRate().then((hz: number | null) => { if (live) setMaxRefreshHz(hz); });
    return () => { live = false; };
  }, []);

  const commit = useCallback((f: (s: FaceplateSettings) => FaceplateSettings) => {
    touched.current = true;
    setSettings((prev: FaceplateSettings) => {
      const next = f(prev);
      if (next !== prev) {
        // ★ A new faceplate from the pane is on trial from before its first frame.
        faceplateGuard.arm(next);
        AsyncStorage.setItem(FACEPLATE_STORAGE_KEY, JSON.stringify(next)).catch(() => {});
      }
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
  const value = useMemo(() => ({ theme, settings: onScreen, autoTransparency: auto, maxRefreshHz,
                                 setTransparency, setDisplay, setText, set }),
                        [theme, onScreen, auto, maxRefreshHz, setTransparency, setDisplay, setText, set]);
  return <FaceplateContext.Provider value={value}>{children}</FaceplateContext.Provider>;
}

/**
 * ★★★ The deck (and the VTS strip) put the faceplate on trial as they MOUNT — entering a receiver is
 * where a faceplate is first really drawn, often long after launch. Runs during the first render, so
 * the mark is on disk before the first frame (the write is synchronous and idempotent).
 */
export function useFaceplateOnTrial(): void {
  const { settings } = useContext(FaceplateContext);
  useState(() => { faceplateGuard.arm(settings); return 0; });
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
