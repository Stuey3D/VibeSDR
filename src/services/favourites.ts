import type { BackendType } from './sdrTypes';
import AsyncStorage from '@react-native-async-storage/async-storage';

const KEY = 'vsdr_favourites';

// SpyServer favourites carry url = spyserver://host:port (the protocol has no
// web UI to point a browser at); the picker routes on serverType.
//
// visits    = Most-Used tally: bumped every time the user connects to this favourite, on the
//             phone OR the watch (the watch's counts fold in via sync). Default sort is by this.
// lat/lon   = snapshot of the receiver's location (from the directory entry when favourited, and
//             refreshed when a directory loads) → the Nearest sort.
// bestSnr   = snapshot of the last-seen reported SNR → the SNR sort (missing ⇒ sorts to the bottom).
// order is the array position itself = the Manual (drag) order.
export type Favourite = {
  name: string; url: string; serverType?: BackendType;
  visits?: number; latitude?: number; longitude?: number; bestSnr?: number;
  // custom = a server the user TYPED IN (vs one saved from a directory listing). Drives the picker's
  // two subheadings and the edit affordance (only custom servers are editable). Optional: a LEGACY
  // favourite saved before this flag has it undefined — favIsCustom() falls back to a coords heuristic
  // (directory entries carry lat/lon; typed ones don't), so no migration write is needed.
  custom?: boolean;
};

/** Is this a manually-added (typed) server, vs one saved from a directory? Handles legacy entries. */
export function favIsCustom(f: Favourite): boolean {
  return f.custom ?? (f.latitude == null && f.longitude == null);
}

export type FavSort = 'used' | 'alpha' | 'nearest' | 'snr' | 'type' | 'manual';
const FAV_SORT_KEY = 'vibe.fav.sort';
const FAV_SORTS: FavSort[] = ['used', 'alpha', 'nearest', 'snr', 'type', 'manual'];
export async function getFavSort(): Promise<FavSort> {
  try { const v = await AsyncStorage.getItem(FAV_SORT_KEY) as FavSort;
    return FAV_SORTS.includes(v) ? v : 'used';
  } catch { return 'used'; }
}
export async function setFavSort(v: FavSort): Promise<void> {
  try { await AsyncStorage.setItem(FAV_SORT_KEY, v); } catch { /* best effort */ }
}

/** Bump the Most-Used tally for whichever favourite matches this url (server, custom URL, local,
 *  TCP, spyserver — the url is the key). No-op if it isn't a favourite. Returns the updated list. */
export async function registerFavouriteVisit(url: string): Promise<Favourite[]> {
  const favs = await getFavourites();
  let changed = false;
  const next = favs.map(f => {
    if (f.url === url) { changed = true; return { ...f, visits: (f.visits ?? 0) + 1 }; }
    return f;
  });
  if (changed) await saveFavourites(next);
  return next;
}

export async function getFavourites(): Promise<Favourite[]> {
  try {
    const raw = await AsyncStorage.getItem(KEY);
    return raw ? JSON.parse(raw) : [];
  } catch {
    return [];
  }
}

export async function saveFavourites(favs: Favourite[]): Promise<void> {
  await AsyncStorage.setItem(KEY, JSON.stringify(favs));
}

export async function toggleFavourite(fav: Favourite, current: Favourite[]): Promise<Favourite[]> {
  const exists = current.some(f => f.url === fav.url);
  const next = exists
    ? current.filter(f => f.url !== fav.url)
    : [...current, { name: fav.name, url: fav.url, serverType: fav.serverType,
                     latitude: fav.latitude, longitude: fav.longitude, bestSnr: fav.bestSnr,
                     visits: fav.visits ?? 0, custom: fav.custom }];
  await saveFavourites(next);
  return next;
}

/** Edit an existing favourite in place (keeps its slot + visits). Matches by the OLD url, since
 *  editing the address changes the key. Used by the custom-server edit sheet. */
export async function updateFavourite(oldUrl: string, patch: Partial<Favourite>): Promise<Favourite[]> {
  const favs = await getFavourites();
  const next = favs.map(f => (f.url === oldUrl ? { ...f, ...patch } : f));
  await saveFavourites(next);
  return next;
}

// ── RTL-TCP named favourites (host:port + friendly name) ──────────────────────
const TCP_KEY = 'vsdr_rtltcp_favs';

// `proto` is optional for backwards compatibility: favourites saved before
// SpyServer support existed have no field and must keep resolving to rtl_tcp.
export type TcpFav = { name: string; host: string; port: number; proto?: BackendType };

export async function getTcpFavs(): Promise<TcpFav[]> {
  try {
    const raw = await AsyncStorage.getItem(TCP_KEY);
    return raw ? JSON.parse(raw) : [];
  } catch {
    return [];
  }
}

export async function saveTcpFavs(favs: TcpFav[]): Promise<void> {
  await AsyncStorage.setItem(TCP_KEY, JSON.stringify(favs));
}

/**
 * One-shot repair for the v8.0.0 mis-detection.
 *
 * detectServerType() matched "vibesdr" as well as "vibeserver" — but "vibesdr"
 * is the CLIENT's name: UberSDR instances carry vibesdr:// deep-link banners, so
 * genuine UberSDR pages matched the VibeServer rule. The picker treats detection
 * as authoritative, so it wrote 'vibeserver' back over the saved favourite: the
 * corruption is PERSISTED, and fixing the detector alone would not undo it.
 *
 * So: strip the type from any favourite v8 marked 'vibeserver'. We clear rather
 * than force to 'ubersdr' because a few of them may be real VibeServers — the
 * (now fixed) detector re-derives the correct type on the next connect, and an
 * unreachable host falls back to 'ubersdr', which is right for the vast majority.
 */
const VIBESERVER_FIX_KEY = 'vsdr_fav_vibeserver_fix_v1';

export async function repairVibeserverFavourites(): Promise<void> {
  try {
    if (await AsyncStorage.getItem(VIBESERVER_FIX_KEY)) return;   // already run
    const favs = await getFavourites();
    if (favs.some(f => f.serverType === 'vibeserver')) {
      await saveFavourites(favs.map(f =>
        f.serverType === 'vibeserver' ? { name: f.name, url: f.url } : f));
    }
    await AsyncStorage.setItem(VIBESERVER_FIX_KEY, '1');
  } catch {
    // Never block startup on the repair — a failed pass retries next launch.
  }
}

/** Persist a learned serverType onto an existing favourite (after detection). */
export async function setFavouriteServerType(url: string, serverType: BackendType): Promise<void> {
  const favs = await getFavourites();
  let changed = false;
  const next = favs.map(f => {
    if (f.url === url && f.serverType !== serverType) { changed = true; return { ...f, serverType }; }
    return f;
  });
  if (changed) await saveFavourites(next);
}
