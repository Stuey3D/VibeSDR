/**
 * ★★★ THE APP'S PERSISTENT STATION-LOGO STORE — DAB and FM, the BYTES on disk (Stuart, 2026-10-30: "cache the logos and
 * only change them when a new one is published"). The policy is logoCachePolicy.ts (shared with the web client); this
 * file is only its I/O: the index in AsyncStorage, each picture a file under documentDirectory/stationlogos/.
 *
 * ★★ WHY BYTES AND NOT URLS. A cached URL still needs the network — and for an over-the-air logo, the SERVER, tuned to
 *    that multiplex — before anything draws. A file draws at once, offline, from any server, and the lock screen and the
 *    watch read the same file (setStationLogo / readAsStringAsync), so all three agree.
 * ★★ A NEW PICTURE GETS A NEW FILE NAME (key + md5). React Native's image pipeline caches by URI; overwriting a file in
 *    place could leave the old picture on screen. The old file is deleted once the index points at the new one.
 * ★ Every network step has a deadline. A stalled tunnel used to leave a lookup pending for ever, and a pending lookup is
 *   a key that is never asked about again — the shape of the blank-logos fault this store exists to end.
 */
import * as FileSystem from 'expo-file-system/legacy';
import AsyncStorage from '@react-native-async-storage/async-storage';
import React from 'react';
import { LogoIndex, APP_LIMITS, outranked, type LogoEntry, type LogoSource } from './logoCachePolicy';
import { limiter } from '../utils/limit';

const DIR = (FileSystem.documentDirectory ?? '') + 'stationlogos/';
const TMP = DIR + 'tmp/';
const INDEX_KEY = 'lsv_logo_cache_v2';
/** ★ The FM-only store this replaces (stationLogoCache.ts before V12): ISO|PI → {path,url,ts}. Migrated once. */
const OLD_INDEX_KEY = 'lsv_logo_cache_v1';
/** ★ A logo is a few KB to a few hundred; anything past a megabyte is not a logo (and 1 GB phones run this app). */
const MAX_FILE = 1024 * 1024;
const DL_MS = 20000;

const index = new LogoIndex({}, APP_LIMITS);
/** ★ This session only: a URL that resolved but whose bytes could not be kept (download failed) — still worth drawing. */
const remoteOnly = new Map<string, string>();
const listeners = new Set<() => void>();
/** ★ Pictures (by md5) that would not draw this session — never filed again until the next launch, or a damaged file
 *  would be re-downloaded and re-filed on every rung of the retry ladder. */
const deadSigs = new Set<string>();
let version = 0;
let saveTimer: ReturnType<typeof setTimeout> | null = null;
/** ★ Three downloads at a time — a DAB list of 40 services must not open 40 sockets down one tunnel. */
const dlQueue = limiter(3);

function changed() {
  version++;
  for (const f of listeners) { try { f(); } catch { /* a listener's fault is its own */ } }
}
function saveSoon() {
  if (saveTimer) return;
  saveTimer = setTimeout(() => {
    saveTimer = null;
    AsyncStorage.setItem(INDEX_KEY, JSON.stringify(index.entries)).catch(() => {});
  }, 1500);
}
async function rm(path?: string) {
  if (path && path.startsWith(DIR)) await FileSystem.deleteAsync(path, { idempotent: true }).catch(() => {});
}

let loaded = false;
const ready: Promise<void> = (async () => {
  try {
    const raw = await AsyncStorage.getItem(INDEX_KEY);
    if (raw) {
      const j = JSON.parse(raw) as Record<string, LogoEntry>;
      for (const [k, e] of Object.entries(j)) if (e && e.show && e.src) index.entries[k] = e;
    } else {
      /* ★ MIGRATE the FM store: its keys were ISO|PI (no frequency) and its source unrecorded — kept as fm:ISO|PI| with
       *  source 'name', the lowest real rank, so the first identity answer replaces it rather than being refused. */
      const old = await AsyncStorage.getItem(OLD_INDEX_KEY);
      if (old) {
        const j = JSON.parse(old) as Record<string, { path: string; url: string; ts: number }>;
        for (const [k, e] of Object.entries(j)) {
          if (!e?.path) continue;
          const [iso, pi] = k.split('|');
          index.entries[`fm:${iso || ''}|${pi || ''}|`] = { url: e.url, src: 'name', sig: '', show: e.path, bytes: 0,
            savedAt: e.ts, checkedAt: e.ts, usedAt: e.ts };
        }
        saveSoon();
      }
    }
  } catch { /* a corrupt index is an empty one */ }
  // ★ Half-finished downloads from a run that was killed or timed out.
  FileSystem.deleteAsync(TMP, { idempotent: true }).catch(() => {});
  loaded = true;
  changed();
})();

export function logoStoreReady(): Promise<void> { return ready; }

/** ★ Cheap, synchronous: would resolveLogo ask anything for `key` now? Lets a list that renders every second skip
 *  building a lookup it would refuse. True until the index has loaded (resolveLogo then waits and decides). */
export function logoDue(key: string): boolean {
  return !!key && (!loaded || index.shouldAsk(key, Date.now()));
}

/** What to draw for `key` right now (a local file), or null. Synchronous — safe in render. */
export function peekLogo(key: string): string | null {
  if (!key) return null;
  const e = index.get(key);
  if (e) {
    if (index.touch(key, Date.now())) saveSoon();
    return e.show;
  }
  return remoteOnly.get(key) ?? null;
}
export function peekLogoEntry(key: string): LogoEntry | undefined { return key ? index.get(key) : undefined; }

/** Re-render when the store changes (a logo arrived or changed). */
export function useLogoStoreVersion(): number {
  return React.useSyncExternalStore(
    (f) => { listeners.add(f); return () => { listeners.delete(f); }; },
    () => version, () => version);
}

function safeName(key: string) { return key.replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 80); }

/** Why `take` drew nothing: the bytes did not come (`net` — worth retrying soon) or they came and are not a usable
 *  logo (`bad` — an SVG, an error page, a 5 MB file: the next source down should be tried instead). */
type TakeFail = 'net' | 'bad';
/** Download `url` and file it under `key` per the policy. Returns what to draw, or why there is nothing. */
async function take(key: string, src: LogoSource, url: string, headers?: Record<string, string>): Promise<string | TakeFail> {
  if (!/^(https?:)\/\//i.test(url)) return 'bad';
  const tmp = `${TMP}${safeName(key)}-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`;
  let res: FileSystem.FileSystemDownloadResult | undefined;
  try {
    await FileSystem.makeDirectoryAsync(TMP, { intermediates: true }).catch(() => {});
    const task = FileSystem.createDownloadResumable(url, tmp, { md5: true, ...(headers ? { headers } : {}) });
    let timer: ReturnType<typeof setTimeout> | undefined;
    res = await dlQueue(() => Promise.race([
      task.downloadAsync(),
      new Promise<undefined>((r) => { timer = setTimeout(() => { task.cancelAsync().catch(() => {}); r(undefined); }, DL_MS); }),
    ]).finally(() => clearTimeout(timer)));
  } catch { res = undefined; }
  const ok = !!res && res.status === 200;
  const type = String(res?.mimeType || res?.headers?.['Content-Type'] || res?.headers?.['content-type'] || '').toLowerCase();
  // ★ An HTML error page served with a 200 is not a logo; nor is an SVG — React Native's <Image> cannot draw one, and a
  //   lower source (a name match, usually a PNG) is better than a picture that renders as nothing.
  const image = !type || (type.startsWith('image/') && !type.includes('svg'));
  const info = ok ? await FileSystem.getInfoAsync(tmp).catch(() => null) : null;
  const size = info && info.exists ? (info as { size?: number }).size ?? 0 : 0;
  if (!ok || !image || !size || size > MAX_FILE || !res?.md5) {
    await rm(tmp);
    // ★ A 4xx is the publisher's answer about this file, not a stall: treat it as unusable, not as "try again soon".
    return !res || res.status >= 500 || res.status === 0 ? 'net' : 'bad';
  }
  if (deadSigs.has(res.md5)) { await rm(tmp); return 'bad'; }
  const now = Date.now();
  const v = index.verdict(key, { src, url, sig: res.md5 }, now);
  if (v !== 'store') {
    await rm(tmp);
    if (v === 'same') { index.same(key, now, { url, src }); saveSoon(); }
    else index.confirm(key, now);
    return index.get(key)?.show ?? 'bad';
  }
  const ext = (type.match(/image\/(png|jpe?g|webp|gif)/)?.[1] || url.match(/\.(png|jpe?g|webp|gif)(\?|$)/i)?.[1] || 'img')
    .toLowerCase().replace('jpeg', 'jpg');
  const path = `${DIR}${safeName(key)}-${res.md5.slice(0, 10)}.${ext}`;
  try {
    await rm(path);
    await FileSystem.moveAsync({ from: tmp, to: path });
  } catch { await rm(tmp); return 'net'; }
  const { evicted, replaced } = index.store(key, { url, src, sig: res.md5, show: path, bytes: size }, now);
  remoteOnly.delete(key);
  for (const e of evicted) void rm(e.show);
  if (replaced) void rm(replaced.show);
  saveSoon();
  changed();
  return path;
}

/** A source to ask. `run` resolves the logo's URL, or null for "this source has none"; it THROWS for "could not ask"
 *  (a stall, a 5xx) — which the store retries on the ladder instead of believing for a day. */
export interface LogoResolver { src: LogoSource; run: () => Promise<string | null | undefined> }

/**
 * ★★★ THE ONE ENTRY POINT. Returns what to draw for `key` — at once from disk when held (and a background refresh is
 * started if its day is up), else after asking `resolvers` IN ORDER (highest source first). Safe to call on every
 * render: the index refuses a key that is being asked, is inside a miss's backoff, or was checked within the day.
 * ★ A resolver whose source cannot displace what is held is not even asked (a name search under a RadioDNS logo).
 */
export async function resolveLogo(key: string, resolvers: LogoResolver[]): Promise<string | null> {
  if (!key) return null;
  await ready;
  const held = peekLogo(key);
  const now = Date.now();
  if (!index.shouldAsk(key, now) || !index.begin(key)) return held;
  const job = (async (): Promise<string | null> => {
    let remote: string | null = null;
    let asked = 0;
    /** ★★ The highest source that could not be ASKED (it threw: a stall, a 5xx) or whose bytes would not come. A lower
     *  answer is still drawn, but this one is retried on the ladder rather than in a day (LogoIndex.failedAbove). */
    let stalled: LogoSource | null = null;
    try {
      for (const r of resolvers) {
        if (outranked(index.get(key), r.src, Date.now())) continue;
        asked++;
        let url: string | null | undefined = null;
        try { url = await r.run(); } catch { stalled = stalled ?? r.src; continue; }
        if (!url) continue;
        const shown = await take(key, r.src, url);
        if (shown !== 'net' && shown !== 'bad') {
          if (stalled) index.failedAbove(key, stalled, Date.now());
          return shown;
        }
        if (shown === 'bad') continue;       // not a drawable logo — the next source down may have one
        // ★ Resolved, but the bytes did not come: draw it from the network this session, retry on the ladder.
        stalled = stalled ?? r.src;
        remote = remote ?? url;
      }
      // ★ Every source is outranked by what is held (an air logo, say): nothing to ask — look again in a day.
      if (!asked && index.get(key)) { index.confirm(key, Date.now()); return peekLogo(key); }
      if (stalled && index.get(key)) { index.failedAbove(key, stalled, Date.now()); return peekLogo(key); }
      if (remote && !index.get(key)) {
        remoteOnly.set(key, remote);
        changed();
      }
      // ★ Nothing better and nothing new: back off (the held logo, if any, stays exactly as it is).
      index.miss(key, Date.now());
      return peekLogo(key);
    } finally {
      index.end(key);
    }
  })();
  // ★ Held: draw it now, refresh behind it. Not held: the caller waits for the answer.
  if (held) { job.catch(() => {}); return held; }
  return job;
}

/** File one answer found OUTSIDE the ladder (a later PS fragment's name match, say) — the policy still decides whether
 *  it may displace what is held. Returns what to draw, or null. */
export async function offerLogo(key: string, src: LogoSource, url: string): Promise<string | null> {
  if (!key) return null;
  await ready;
  if (outranked(index.get(key), src, Date.now())) return peekLogo(key);
  if (!index.begin(key)) return peekLogo(key);
  try {
    const r = await take(key, src, url);
    return r === 'net' || r === 'bad' ? peekLogo(key) : r;
  } finally { index.end(key); }
}

/**
 * ★★★ THE MULTIPLEX'S OWN LOGO, KEPT (Stuart: "that needs to be cached too"). Called while a service carries `logoAir`.
 * Once per session per key (and again after a day), the server's dablogoair is downloaded and compared by md5: the
 * same picture only stamps "seen on air"; a different one replaces the old file. Air outranks every other source, so
 * this is what the row draws from then on — offline, and on any server.
 */
const airAsked = new Map<string, number>();
export function offerAirLogo(key: string, url: string): void {
  if (!key) return;
  const now = Date.now();
  const last = airAsked.get(key);
  if (last !== undefined && now - last < 24 * 3600 * 1000) return;
  airAsked.set(key, now);
  void (async () => {
    await ready;
    if (!index.begin(key)) { airAsked.delete(key); return; }   // a lookup is running — try on a later render
    try {
      const shown = await take(key, 'air', url);
      // ★ A failed fetch is retried on the ladder's first rung, not tomorrow.
      if (shown === 'net') airAsked.set(key, Date.now() - 24 * 3600 * 1000 + 60 * 1000);
    } finally { index.end(key); }
  })();
}

/** The picture would not load: forget it (file and entry) and back off, so it is re-resolved later — never blanked. */
export function dropLogo(key: string, shown?: string | null): void {
  const e = index.get(key);
  if (e && shown && e.show !== shown) return;           // a newer picture already replaced it
  if (!e) { if (remoteOnly.get(key) === shown) remoteOnly.delete(key); index.miss(key, Date.now()); changed(); return; }
  if (e.sig) deadSigs.add(e.sig);
  index.drop(key, Date.now());
  void rm(e.show);
  saveSoon();
  changed();
}

/** The same, found by what was drawn — for an <Image> that only knows its URI. */
export function forgetShownLogo(shown: string | null | undefined): void {
  if (!shown) return;
  for (const [k, e] of Object.entries(index.entries)) if (e.show === shown) { dropLogo(k, shown); return; }
  for (const [k, u] of remoteOnly) if (u === shown) { dropLogo(k, shown); return; }
}
