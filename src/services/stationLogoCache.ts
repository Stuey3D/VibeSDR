// On-device station-logo cache, keyed by RDS country+PI. A logo discovered
// online (radio-browser, on ANY backend — FM-DX or local RTL-SDR) is downloaded
// to disk and remembered against the station's PI code, so it displays OFFLINE
// later — e.g. discover Pride Radio's logo on an FM-DX server, then tune it
// locally on an offline RTL-SDR (the PI + ECC decode locally, no network). Stale
// entries refresh in the background when online so logo changes are picked up.

import * as FileSystem from 'expo-file-system/legacy';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { lookupStationLogo } from './stationLogo';
import { radioDnsLogo } from './radiodns';
import { receiverIso, eccForIso } from './rdsCountry';

const DIR = FileSystem.documentDirectory + 'stationlogos/';
const INDEX_KEY = 'lsv_logo_cache_v1';
const REFRESH_MS = 14 * 24 * 3600 * 1000;   // re-fetch a cached logo after ~2 weeks

interface Entry { path: string; url: string; ts: number }
let index: Record<string, Entry> | null = null;

async function loadIndex(): Promise<Record<string, Entry>> {
  if (index) return index;
  try { index = JSON.parse((await AsyncStorage.getItem(INDEX_KEY)) || '{}'); }
  catch { index = {}; }
  return index!;
}
async function saveIndex(): Promise<void> {
  try { await AsyncStorage.setItem(INDEX_KEY, JSON.stringify(index || {})); } catch {}
}

/** Country+PI identifies a station uniquely (same PI = different station across
 *  countries). Both come from RDS, so this key is available offline too. */
function keyFor(pi?: string, iso?: string): string {
  return pi ? `${(iso || '').toUpperCase()}|${pi.toUpperCase()}` : '';
}

async function ensureDir(): Promise<void> {
  const info = await FileSystem.getInfoAsync(DIR);
  if (!info.exists) await FileSystem.makeDirectoryAsync(DIR, { intermediates: true });
}

async function download(url: string, key: string): Promise<string | null> {
  try {
    await ensureDir();
    const ext = (url.match(/\.(png|jpe?g|webp|gif)/i)?.[1] || 'img').toLowerCase();
    const path = `${DIR}${key.replace(/[^A-Za-z0-9_-]/g, '_')}.${ext}`;
    const res = await FileSystem.downloadAsync(url, path);
    return res.status === 200 ? path : null;
  } catch { return null; }
}

async function refresh(key: string, name: string, iso?: string,
                       id?: { pi?: string; ecc?: string; freqHz?: number }): Promise<void> {
  // ★★★ THE REFRESH COULD NOT CORRECT THE THING MOST LIKELY TO BE WRONG. It re-ran ONLY the name
  //     search, so a logo that got here by a bad PS decode — the Radio 1 roundel on Radio 3 — was
  //     re-derived from a name every fortnight and confirmed itself for ever. The identity path is
  //     the one that can overrule it, so it runs FIRST here too, exactly as on the cold path.
  let url = '';
  if (id?.pi && id.ecc && id.freqHz) {
    url = await radioDnsLogo(id.pi, id.ecc, id.freqHz).catch(() => '');
  }
  // receiverIso() is a PREFERENCE, not a filter: it searches the receiver's own
  // country first (a global name search buries the local station — "Kiss" found a
  // Greek one because the UK one was nowhere near the top by votes) but still falls
  // back worldwide, so a sporadic-E catch from abroad is not excluded.
  if (!url && name) url = (await lookupStationLogo(name, iso, receiverIso() || undefined)) || '';   // no-op offline
  if (!url) return;
  const path = await download(url, key);
  if (path) { (await loadIndex())[key] = { path, url, ts: Date.now() }; await saveIndex(); }
}

/**
 * Resolve a station logo, cache-first (offline-capable):
 *  1. Cached (by country+PI) → return the local file URI; refresh in the
 *     background if stale + online.
 *  2. Not cached → online radio-browser lookup, download, cache by PI.
 *  3. Offline + not cached → null (caller shows a monogram, no placeholder).
 */
export async function resolveStationLogo(
  opts: { pi?: string; name: string; iso?: string; ecc?: string; freqHz?: number },
): Promise<string | null> {
  const { pi, name, iso, ecc, freqHz } = opts;
  // ★★★ A NAME IS NOT AN IDENTITY, AND A WRONG LOGO LOCKS. The cache is keyed on the PI — right,
  //     because the PS rotates — so whatever lands first is kept until the PI changes. One bad PS
  //     decode ("BBC R1" is two bits from "BBC R3") therefore sticks for good, which is exactly
  //     what Stuart photographed on 2026-08-11: a weak Radio 3 wearing the Radio 1 roundel.
  //     ▶ So ask RadioDNS FIRST, which is keyed on the PI + ECC + frequency the transmitter
  //       error-protects, and returns the broadcaster's OWN artwork. The name search stays as the
  //       fallback for the many stations that publish no SPI at all (Heart, FLEX — verified).
  //     ★ This is the app's PRIMARY case, not an edge one: driving local hardware there is no
  //       server to ask, so the in-app lookup is the only route to identity-keyed artwork.
  if (!name && !(pi && ecc && freqHz)) return null;
  const key = keyFor(pi, iso);
  const idx = await loadIndex();

  if (key && idx[key]) {
    const e = idx[key];
    const info = await FileSystem.getInfoAsync(e.path);
    if (info.exists) {
      if (Date.now() - e.ts > REFRESH_MS)
        refresh(key, name, iso, { pi, ecc: ecc || eccForIso(iso, pi), freqHz }).catch(() => {});
      return e.path;
    }
    delete idx[key];   // file vanished — fall through to re-fetch
  }

  // ★ Identity first, name second. Both are cached under the same PI key, so a station that
  //   publishes an SPI is looked up once and then comes off disk — offline included.
  let url = '';
  // ★ The ECC is derived from the country when the station did not transmit one — which is most
  //   of them. See eccForIso: it is a table lookup against an ISO that has already been validated,
  //   not an assumption about where the signal came from.
  const gccEcc = ecc || eccForIso(iso, pi);
  if (pi && gccEcc && freqHz) url = await radioDnsLogo(pi, gccEcc, freqHz).catch(() => '');
  if (!url && name) url = (await lookupStationLogo(name, iso, receiverIso() || undefined)) || '';
  if (!url) return null;
  if (key) {
    const path = await download(url, key);
    if (path) { idx[key] = { path, url, ts: Date.now() }; await saveIndex(); return path; }
  }
  return url;   // no PI to key on / download failed → remote URL (needs network)
}
