/**
 * ★★ beta94 LOCAL HARDWARE's POSITION (Stuart 2026-11-25: "in local hardware mode the location goes nowhere anyway") —
 * the town or locator the user entered (kept on this phone so it is not asked for again), the phone's own location
 * without a second permission prompt, and the hand-over to THIS app's satellite engine (setSatObserver).
 *
 * ★★★ NEVER PUBLISHED. Not the server location (vibeServer.ts LOC_KEY / publishLocation are untouched), not
 *     setLocationJson, not the directory. The native call reaches this process's engine only (VibeLocalSdrModule
 *     .setSatObserver — the server process is never told). Grid-square centre before it leaves JS (grid.ts gridCentre).
 * The pure rules and words are in satPlace.ts.
 * ★★ ONE value for the whole app: the server list's MY LOCATION row (InstancePickerScreen — its distances and NEAR sort;
 *    rules in myPlace.ts), the SATELLITE box and the spots map's city picker all read and write it, and hear each other
 *    through onEnteredPlaceChange.
 */
import AsyncStorage from '@react-native-async-storage/async-storage';
import { NativeModules, PermissionsAndroid, Platform } from 'react-native';
import { gridCentre } from './grid';
import { getUserLocation } from './instancesApi';

/** ★ Its own key — NOT the server's location (LOC_KEY 'lsv_server_location'). */
export const LOCAL_PLACE_KEY = 'vibe.localPlace';

export type EnteredPlace = { lat: number; lon: number; label?: string };

export async function getEnteredPlace(): Promise<EnteredPlace | null> {
  try {
    const v = await AsyncStorage.getItem(LOCAL_PLACE_KEY);
    if (!v) return null;
    const o = JSON.parse(v) as EnteredPlace;
    return Number.isFinite(o?.lat) && Number.isFinite(o?.lon) && Math.abs(o.lat) <= 90 && Math.abs(o.lon) <= 180
      ? { lat: o.lat, lon: o.lon, ...(typeof o.label === 'string' && o.label ? { label: o.label.slice(0, 60) } : {}) } : null;
  } catch { return null; }
}

/** ★ ONE value, three places to change it (the server list's MY LOCATION, the SATELLITE box, the spots map's city
 *  picker): whoever is on screen hears a change made in another, with no restart. null = cleared. */
type PlaceListener = (p: EnteredPlace | null) => void;
const placeListeners = new Set<PlaceListener>();
export function onEnteredPlaceChange(fn: PlaceListener): () => void {
  placeListeners.add(fn);
  return () => { placeListeners.delete(fn); };
}
function tellPlace(p: EnteredPlace | null): void {
  for (const fn of [...placeListeners]) { try { fn(p); } catch {} }
}

/** Kept snapped to the grid-square centre — what is stored is no finer than what is used. null = CLEAR. */
export async function setEnteredPlace(p: EnteredPlace | null): Promise<EnteredPlace | null> {
  try {
    if (!p) { await AsyncStorage.removeItem(LOCAL_PLACE_KEY); tellPlace(null); return null; }
    const c = gridCentre(p.lat, p.lon);
    const v: EnteredPlace = { lat: c.lat, lon: c.lon, ...(p.label ? { label: p.label.slice(0, 60) } : {}) };
    await AsyncStorage.setItem(LOCAL_PLACE_KEY, JSON.stringify(v));
    tellPlace(v);
    return v;
  } catch { return null; }
}

/** The phone's location only where it is ALREADY allowed — no prompt (CLEAR falls back to it quietly). */
export async function phoneLocationIfAllowed(): Promise<{ lat: number; lon: number } | null> {
  try {
    if (Platform.OS === 'android') {
      const ok = await PermissionsAndroid.check(PermissionsAndroid.PERMISSIONS.ACCESS_COARSE_LOCATION);
      if (!ok) return null;
    }
    return await getUserLocation();
  } catch { return null; }
}

type SatNative = { setSatObserver?: (lat: number, lon: number) => void; clearSatObserver?: () => void };
const native = (): SatNative | undefined => (NativeModules as { VibeLocalSDR?: SatNative }).VibeLocalSDR;

/** Has this build the native call? (Android; an older native side or iOS has not.) */
export function satObserverNative(): boolean { return typeof native()?.setSatObserver === 'function'; }

/** This app's satellite engine (Local Hardware) — grid-square centre; null clears the phone's own position. */
export function sendSatObserver(p: { lat: number; lon: number } | null): void {
  const n = native();
  try {
    if (!p) { n?.clearSatObserver?.(); return; }
    const c = gridCentre(p.lat, p.lon);
    n?.setSatObserver?.(c.lat, c.lon);
  } catch {}
}
