/**
 * portraitScheme — WIDE or TALL controls on a tablet held upright (Stuart, 2026-11-08).
 *
 *   WIDE = the landscape bar: tune drum at the left edge, zoom at the right, the frequency between — under the
 *          thumbs, and a shorter deck, so more of the waterfall is clear.
 *   TALL = the portrait deck: bigger keys, the frequency over the drums.
 *
 * ★★ THE DEFAULT IS BY WIDTH: a window 800 pt across or more (10" Android tablets, every full-size iPad) starts WIDE;
 *    a narrower tablet window (the Tab A9's 601, the iPad mini's 744) starts TALL. The row in CONTROL CUSTOMISATION
 *    overrides it: wide keys too small → TALL; too much waterfall covered → WIDE.
 * ★★ A CHOICE BELONGS TO THE WINDOW WIDTH IT WAS MADE AT ("When the screen space is modified due to split screen or
 *    windowing the controls will switch to best fit the window size"). Split screen or a freeform window of another
 *    width gets that width's best fit; back to full screen, the choice made there is back. A window under 600 pt
 *    across is not a tablet window at all and takes the phone deck (useUiScale TABLET_MIN_SIDE).
 * ★ App-wide (the device in your hand, not the server), kept in AsyncStorage.
 */
import AsyncStorage from '@react-native-async-storage/async-storage';
import { useSyncExternalStore } from 'react';
import { UPRIGHT_BAND_MIN_W } from '../hooks/useUiScale';

export type PortraitScheme = 'wide' | 'tall';

const KEY = 'lsv_portrait_scheme';
let byWidth: Record<string, PortraitScheme> = {};
const subs = new Set<() => void>();
let version = 0;

AsyncStorage.getItem(KEY).then(v => {
  if (!v) return;
  try {
    const o = JSON.parse(v);
    if (o && typeof o === 'object') {
      const clean: Record<string, PortraitScheme> = {};
      for (const [k, s] of Object.entries(o)) if (s === 'wide' || s === 'tall') clean[k] = s;
      byWidth = clean; version++; subs.forEach(f => f());
    }
  } catch { /* a bad blob is "no choice made" */ }
}).catch(() => {});

const widthKey = (W: number) => String(Math.round(W));

/** The best fit for a window this wide, with no choice made. */
export function portraitSchemeDefault(W: number): PortraitScheme {
  return W >= UPRIGHT_BAND_MIN_W ? 'wide' : 'tall';
}

/** The scheme for this window width: the choice made at this width, else the best fit. */
export function portraitSchemeFor(W: number): PortraitScheme {
  return byWidth[widthKey(W)] ?? portraitSchemeDefault(W);
}

export function setPortraitScheme(W: number, s: PortraitScheme): void {
  const k = widthKey(W);
  if (s === portraitSchemeDefault(W)) delete byWidth[k]; else byWidth[k] = s;   // the best fit needs no entry
  byWidth = { ...byWidth };
  version++;
  subs.forEach(f => f());
  AsyncStorage.setItem(KEY, JSON.stringify(byWidth)).catch(() => {});
}

function subscribe(f: () => void) { subs.add(f); return () => { subs.delete(f); }; }
const snap = () => version;

/** The scheme for a window `W` wide, re-read whenever a choice is made or loaded. */
export function usePortraitScheme(W: number): PortraitScheme {
  useSyncExternalStore(subscribe, snap, snap);
  return portraitSchemeFor(W);
}
