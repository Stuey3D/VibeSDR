/**
 * Optional online enrichment for stations — genre, logo, homepage — from the
 * Radio-Browser API (addendum §7). STRICTLY a nice-to-have layered on top of the
 * offline FCC data:
 *   - Never blocks the nearby list; callers read the cache and fetch in the
 *     background.
 *   - Everything degrades silently to null when offline.
 *   - Radio-Browser catalogs internet streams contributed by users, so coverage
 *     is spotty and its geo/frequency are the STREAM's, not the transmitter's —
 *     we only ever take genre/logo/homepage from it, never location/frequency.
 *   - Join is fuzzy: the FCC callsign must appear in the stream's name.
 *   - Polite: descriptive User-Agent, rate-limited, cached aggressively to disk.
 *
 * Cache lives in its OWN runtime SQLite db (enrichment.sqlite) so refreshing the
 * bundled station DB never wipes the cache.
 */

import * as FileSystem from 'expo-file-system/legacy';
import * as SQLite from 'expo-sqlite';

import type { Enrichment } from './stationTypes';

const USER_AGENT = 'VibeSDR-CarFM/1.0';           // Radio-Browser asks for this
const MIN_REQUEST_INTERVAL_MS = 1000;             // be a good citizen
const CACHE_TTL_MS = 30 * 24 * 60 * 60 * 1000;    // 30 days
const LOGO_DIR = `${FileSystem.documentDirectory}stationlogos/`;
const FALLBACK_SERVERS = [
  'https://de1.api.radio-browser.info',
  'https://nl1.api.radio-browser.info',
  'https://at1.api.radio-browser.info',
];

// ── cache db ─────────────────────────────────────────────────────────────────
let cachePromise: Promise<SQLite.SQLiteDatabase | null> | null = null;
async function openCache(): Promise<SQLite.SQLiteDatabase | null> {
  try {
    const d = await SQLite.openDatabaseAsync('enrichment.sqlite');
    await d.execAsync(
      `CREATE TABLE IF NOT EXISTS enrichment (
         callsign_base TEXT PRIMARY KEY,
         genre TEXT, logo_path TEXT, homepage TEXT, fetched_at INTEGER
       )`,
    );
    return d;
  } catch (e) {
    console.warn('[radioBrowser] cache open failed', e);
    return null;
  }
}
function cache() { return (cachePromise ??= openCache()); }

interface CacheRow { genre: string | null; logo_path: string | null; homepage: string | null; fetched_at: number | null; }
const toEnrichment = (r: CacheRow): Enrichment => ({
  genre: r.genre, logoUri: r.logo_path, homepage: r.homepage, fetchedAt: r.fetched_at,
});

/** Read cached enrichment (no network). Safe to call for every list row. */
export async function getCachedEnrichment(base: string): Promise<Enrichment | null> {
  const d = await cache();
  if (!d || !base) return null;
  try {
    const r = await d.getFirstAsync<CacheRow>(
      `SELECT genre, logo_path, homepage, fetched_at FROM enrichment WHERE callsign_base = ?`,
      [base.toUpperCase()],
    );
    return r ? toEnrichment(r) : null;
  } catch { return null; }
}

async function putCache(base: string, e: Enrichment): Promise<void> {
  const d = await cache();
  if (!d) return;
  try {
    await d.runAsync(
      `INSERT INTO enrichment(callsign_base, genre, logo_path, homepage, fetched_at)
       VALUES (?,?,?,?,?)
       ON CONFLICT(callsign_base) DO UPDATE SET
         genre=excluded.genre, logo_path=excluded.logo_path,
         homepage=excluded.homepage, fetched_at=excluded.fetched_at`,
      [base.toUpperCase(), e.genre, e.logoUri, e.homepage, e.fetchedAt],
    );
  } catch (err) { console.warn('[radioBrowser] cache write failed', err); }
}

// ── network (rate-limited, deduped) ──────────────────────────────────────────
let lastRequestAt = 0;
let serverBase: string | null = null;
const inFlight = new Map<string, Promise<Enrichment | null>>();

async function throttle() {
  const wait = MIN_REQUEST_INTERVAL_MS - (Date.now() - lastRequestAt);
  if (wait > 0) await new Promise((r) => setTimeout(r, wait));
  lastRequestAt = Date.now();
}

async function resolveServer(): Promise<string> {
  if (serverBase) return serverBase;
  try {
    const res = await fetch('https://all.api.radio-browser.info/json/servers', {
      headers: { 'User-Agent': USER_AGENT, Accept: 'application/json' },
    });
    const list = (await res.json()) as { name: string }[];
    const names = list.map((s) => s?.name).filter(Boolean);
    if (names.length) {
      serverBase = `https://${names[Math.floor(Math.random() * names.length)]}`;
      return serverBase;
    }
  } catch { /* fall through to a fixed mirror */ }
  serverBase = FALLBACK_SERVERS[0];
  return serverBase;
}

interface RbStation { name?: string; tags?: string; favicon?: string; homepage?: string; votes?: number; }

/** First tag as a tidy genre (tags are freeform/messy — keep it short). */
function pickGenre(tags?: string): string | null {
  if (!tags) return null;
  const first = tags.split(',').map((t) => t.trim()).filter(Boolean)[0];
  if (!first || first.length > 24) return null; // some entries are whole sentences
  return first.replace(/\b\w/g, (c) => c.toUpperCase());
}

async function downloadLogo(base: string, url: string): Promise<string | null> {
  try {
    if (!/^https?:\/\//i.test(url)) return null;
    const info = await FileSystem.getInfoAsync(LOGO_DIR);
    if (!info.exists) await FileSystem.makeDirectoryAsync(LOGO_DIR, { intermediates: true });
    const ext = (url.split('?')[0].match(/\.(png|jpg|jpeg|gif|webp|ico|svg)$/i)?.[1] ?? 'img').toLowerCase();
    const path = `${LOGO_DIR}${base.toUpperCase()}.${ext}`;
    const res = await FileSystem.downloadAsync(url, path);
    return res?.uri ?? null;
  } catch { return null; }
}

/**
 * Fetch + cache enrichment for a station. Returns cached data (even if stale) on
 * any network failure. `nameHint` (e.g. the RDS PS) can improve the fuzzy match.
 * Rate-limited and deduped; never throws.
 */
export async function enrichStation(base: string, nameHint?: string): Promise<Enrichment | null> {
  const key = base.toUpperCase();
  if (!key) return null;

  const cached = await getCachedEnrichment(key);
  if (cached?.fetchedAt && Date.now() - cached.fetchedAt < CACHE_TTL_MS) return cached;

  if (inFlight.has(key)) return inFlight.get(key)!;
  const task = (async (): Promise<Enrichment | null> => {
    try {
      await throttle();
      const server = await resolveServer();
      const url = `${server}/json/stations/search?name=${encodeURIComponent(key)}` +
        `&countrycode=US&limit=15&hidebroken=true&order=votes&reverse=true`;
      const res = await fetch(url, { headers: { 'User-Agent': USER_AGENT, Accept: 'application/json' } });
      const rows = (await res.json()) as RbStation[];

      // Fuzzy join: the FCC callsign base must appear in the stream name.
      const hint = (nameHint ?? '').toUpperCase();
      const match = rows
        .filter((r) => (r.name ?? '').toUpperCase().includes(key) ||
                       (hint && (r.name ?? '').toUpperCase().includes(hint)))
        .sort((a, b) => (b.votes ?? 0) - (a.votes ?? 0))[0];

      if (!match) {
        // Record the miss so we don't hammer the API for an uncatalogued station.
        const miss: Enrichment = { genre: null, logoUri: null, homepage: null, fetchedAt: Date.now() };
        await putCache(key, miss);
        return cached ?? miss;
      }

      const logoUri = match.favicon ? await downloadLogo(key, match.favicon) : (cached?.logoUri ?? null);
      const e: Enrichment = {
        genre: pickGenre(match.tags),
        logoUri,
        homepage: match.homepage?.trim() || null,
        fetchedAt: Date.now(),
      };
      await putCache(key, e);
      return e;
    } catch (err) {
      console.warn('[radioBrowser] enrich failed (offline?)', err);
      return cached ?? null; // silent degrade
    } finally {
      inFlight.delete(key);
    }
  })();
  inFlight.set(key, task);
  return task;
}
