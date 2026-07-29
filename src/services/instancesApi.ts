import { NativeModules, PermissionsAndroid, Platform } from 'react-native';

export interface SDRInstance {
  uuid:          string | null;  // collector `id` (UUIDv4) — used for vibesdr:// deep links
  name:          string;
  url:           string;
  location:      string;
  callsign:      string;
  users:         number;
  maxUsers:      number;
  online:        boolean;
  version:       string | null;
  latitude:      number | null;
  longitude:     number | null;
  countryCode:   string | null;  // ISO 3166-1 alpha-2 (directory country_code)
  distance:      number | null;   // km, populated when user location is known
  bestSnr:       number | null;   // best band-condition SNR across all bands
  serverType?:   'ubersdr' | 'owrx' | 'kiwi' | 'fmdx' | 'spyserver';  // directory-tagged backend (skips re-detect)
  // SpyServer only: the protocol is host:port, not a URL. `url` carries
  // spyserver://host:port so the rest of the list plumbing (keys, favourites)
  // keeps working unchanged.
  deviceType?:   string;   // 'RTL-SDR' | 'AirspyHF+' | 'AirspyOne'
  full?:         boolean;  // every client slot taken right now
  sessionLimitMins?: number;  // 0/undefined = no limit; else the server kicks you after this
}

const BASE_URL = 'https://instances.ubersdr.org/api/instances?conditions=true';

// Minimum recommended version — instances older than this show a warning.
export const MIN_RECOMMENDED_VERSION = '0.1.51';

/** Semver-style comparison: returns true if `ver` is older than `min`. */
export function isVersionOld(ver: string | null): boolean {
  if (!ver) return false;
  const parse = (v: string) => v.split('.').map(n => parseInt(n, 10) || 0);
  const a = parse(ver);
  const b = parse(MIN_RECOMMENDED_VERSION);
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    const diff = (a[i] ?? 0) - (b[i] ?? 0);
    if (diff < 0) return true;
    if (diff > 0) return false;
  }
  return false; // equal
}

/** Normalised key for matching a connected URL back to a directory entry:
 *  host[:port] only, so http/https, a trailing slash or a ws:// rewrite all agree. */
export function serverKey(url: string): string {
  return url.replace(/^\w+:\/\//, '').replace(/\/+$/, '').toLowerCase();
}

/**
 * The owner's per-listener limit for a server we are ALREADY connected to, in
 * minutes — or undefined if the directory doesn't list one.
 *
 * ★★ Looked up from the URL rather than threaded through navigation on purpose.
 * The limit used to travel as a route param and reached the receiver on ONE of
 * seven `navigate('SDR')` paths: connect from a favourite, the default server on
 * launch, a deep link, a custom URL or the watch's Reopen and it was silently
 * dropped, so we sat past the owner's limit and were cut off with no warning.
 * A new call site would have re-broken it. The receiver knows its own URL, so it
 * can just ask. Cache-only — never blocks a connect on a network round trip.
 */
export function sessionLimitForUrl(url: string): number | undefined {
  if (!_cache || !url) return undefined;
  const k = serverKey(url);
  return _cache.find(i => i.url && serverKey(i.url) === k)?.sessionLimitMins;
}

// Module-level cache
let _cache:     SDRInstance[] | null = null;
let _cacheTime  = 0;
let _cacheLat:  number | null = null;
let _cacheLon:  number | null = null;
const CACHE_TTL_MS = 60_000;

export async function getUserLocation(): Promise<{ lat: number; lon: number } | null> {
  // navigator.geolocation does NOT exist in React Native — the old code
  // always resolved null, never prompted, and the directory fell back to
  // IP geolocation (wildly wrong on cellular). Native one-shot instead:
  // iOS prompts via CoreLocation; Android needs the runtime permission.
  try {
    if (Platform.OS === 'android') {
      const granted = await PermissionsAndroid.request(
        PermissionsAndroid.PERMISSIONS.ACCESS_COARSE_LOCATION,
        {
          title: 'Location for nearest servers',
          message: 'VibeSDR uses your approximate location to sort the server list by distance and to align the map for local SDR decodes. It is coarsened and only used on-device for sorting.',
          buttonPositive: 'OK',
        },
      );
      if (granted !== PermissionsAndroid.RESULTS.GRANTED) return null;
    }
    const mod = NativeModules.VibePowerModule as
      | { getLocation?: () => Promise<{ lat: number; lon: number } | null> }
      | undefined;
    const res = await mod?.getLocation?.();
    // Coarsen to ~1 km (2 dp) before it leaves this function — the app only
    // needs rough distance to sort servers, never a precise fix. This keeps
    // what we use and transmit to the instance directory "coarse" (matches
    // the App Store privacy declaration + Android's COARSE_LOCATION request).
    return res && typeof res.lat === 'number' && typeof res.lon === 'number'
      ? { lat: Math.round(res.lat * 100) / 100, lon: Math.round(res.lon * 100) / 100 } : null;
  } catch {
    return null;
  }
}

export async function fetchInstances(
  userLat?: number | null,
  userLon?: number | null,
): Promise<SDRInstance[]> {
  const lat = userLat ?? null;
  const lon = userLon ?? null;

  // Reuse cache if location unchanged and within TTL
  const sameLocation = lat === _cacheLat && lon === _cacheLon;
  if (_cache && Date.now() - _cacheTime < CACHE_TTL_MS && sameLocation) return _cache;

  let url = BASE_URL;
  if (lat != null && lon != null) url += `&lat=${lat.toFixed(6)}&lon=${lon.toFixed(6)}`;

  const resp = await fetch(url, { signal: AbortSignal.timeout(10000) });
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  const data = await resp.json();
  const items: any[] = data.instances ?? [];

  const result = items.map((item: any): SDRInstance => {
    let publicUrl: string = item.public_url ?? '';
    if (!publicUrl && item.host) {
      const tls    = item.tls ?? false;
      const port   = item.port ?? (tls ? 443 : 80);
      const scheme = tls ? 'https' : 'http';
      publicUrl    = port === (tls ? 443 : 80)
        ? `${scheme}://${item.host}`
        : `${scheme}://${item.host}:${port}`;
    }
    publicUrl = publicUrl.replace(/\/+$/, '');

    // Best SNR across all reported band conditions
    let bestSnr: number | null = null;
    const bc = item.band_conditions;
    if (bc && typeof bc === 'object') {
      for (const v of Object.values(bc)) {
        if (typeof v === 'number' && (bestSnr === null || v > bestSnr)) bestSnr = v;
      }
    }

    return {
      uuid:      typeof item.id === 'string' && item.id ? item.id : null,
      name:      item.name     || item.callsign || item.host || 'Unknown',
      url:       publicUrl,
      location:  item.location  ?? '',
      callsign:  item.callsign  ?? '',
      users:     item.available_clients ?? 0,
      maxUsers:  item.max_clients       ?? 0,
      online:    item.is_online         ?? true,
      version:   item.version           ?? null,
      latitude:  item.latitude          ?? null,
      longitude: item.longitude         ?? null,
      countryCode: typeof item.country_code === 'string' && item.country_code.length === 2
        ? item.country_code : null,
      distance:  item.distance          ?? null,
      bestSnr,
      // ★ The owner's per-listener time limit, in SECONDS from the directory.
      //   Every UberSDR entry carries it (seen: 3600–43200) and we ignored it
      //   entirely until 2026-07-29 — so we sat past the limit and were kicked
      //   without warning, on someone else's receiver. Round DOWN: a limit we
      //   over-state would have us still showing time when the server cuts us.
      sessionLimitMins: typeof item.max_session_time === 'number' && item.max_session_time > 0
        ? Math.max(1, Math.floor(item.max_session_time / 60))
        : undefined,
    };
  });

  _cache     = result;
  _cacheTime = Date.now();
  _cacheLat  = lat;
  _cacheLon  = lon;
  return result;
}
