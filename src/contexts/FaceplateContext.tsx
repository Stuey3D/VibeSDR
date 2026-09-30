/**
 * FaceplateContext — the faceplate settings, stored, and resolved ONCE for the whole tree.
 *
 * `useFaceplate()` is what the deck draws with (ControlsBar, DrumWheel, TunerKeys, VTSBar,
 * DecoderShell). `useFaceplateSettings()` is for whatever edits them — the CONTROL CUSTOMISATION
 * pane (brief §1), not built yet: until it is, the settings are reachable only as stored prefs
 * (`lsv_faceplate`), and with nothing stored the app renders exactly as it did.
 *
 * ★★ App-wide, not per server: a faceplate is the hardware in your hand, not a property of the
 *   receiver you are listening to. (Display prefs are per server; this deliberately is not.)
 * ★ Resolution is memoised on the settings object, so a meter update never re-resolves anything —
 *   the theme changes only when a setting does.
 */

import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState,
  type ReactNode } from 'react';
import AsyncStorage from '@react-native-async-storage/async-storage';
import {
  DEFAULT_SETTINGS, FACEPLATE_STORAGE_KEY, parseSettings, resolveFaceplate, withDisplay, withText,
  type FaceplateSettings, type FaceplateTheme, type DisplayStyle, type TextColour,
} from '../constants/faceplate';

interface FaceplateContextValue {
  theme:      FaceplateTheme;
  settings:   FaceplateSettings;
  /** Display, with its side effects (controls → neon for Nixie, back to amber leaving it; text
   *  colour clamped / restored per display). */
  setDisplay: (d: DisplayStyle) => void;
  setText:    (t: TextColour) => void;
  /** The rest have no side effects. */
  set:        (patch: Partial<Pick<FaceplateSettings, 'chassis' | 'controls' | 'meter' | 'decoderBg'>>) => void;
}

const DEFAULT_THEME = resolveFaceplate(DEFAULT_SETTINGS);

const FaceplateContext = createContext<FaceplateContextValue>({
  theme: DEFAULT_THEME, settings: DEFAULT_SETTINGS,
  setDisplay: () => {}, setText: () => {}, set: () => {},
});

/**
 * @param legacyThemeName  ThemeContext's font choice ('amber' = Nixie One, 'white' = Atkinson),
 *   used ONCE to seed Display when nothing has ever been stored (§1 migration).
 */
export function FaceplateProvider({ children, legacyThemeName = 'white' }:
    { children: ReactNode; legacyThemeName?: string }) {
  const [settings, setSettings] = useState<FaceplateSettings>(() => parseSettings(null, legacyThemeName));
  // ★ A change made before the stored copy has loaded must not be overwritten by that load.
  const touched = useRef(false);

  useEffect(() => {
    let live = true;
    AsyncStorage.getItem(FACEPLATE_STORAGE_KEY)
      .then((j: string | null) => {
        if (!live || touched.current) return;
        const s = parseSettings(j, legacyThemeName);
        setSettings(s);
        // ★ Write the migrated result back, so the migration runs once and a later change to the
        //   legacy default cannot re-seed Display under a user who never touched it.
        if (!j) AsyncStorage.setItem(FACEPLATE_STORAGE_KEY, JSON.stringify(s)).catch(() => {});
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

  const theme = useMemo(() => resolveFaceplate(settings), [settings]);
  const value = useMemo(() => ({ theme, settings, setDisplay, setText, set }),
                        [theme, settings, setDisplay, setText, set]);
  return <FaceplateContext.Provider value={value}>{children}</FaceplateContext.Provider>;
}

/** The resolved faceplate — what components draw with. */
export function useFaceplate(): FaceplateTheme {
  return useContext(FaceplateContext).theme;
}

/** Settings + setters — for the settings pane (and ThemeContext's legacy setTheme). */
export function useFaceplateSettings(): FaceplateContextValue {
  return useContext(FaceplateContext);
}
