/**
 * ★★★ THE STATION-LOGO CACHE'S RULES — ONE COPY FOR THE APP AND THE WEB CLIENT (Stuart, 2026-10-30).
 *
 *   "cache the logos and only change them when a new one is published"
 *   "if the DAB ensemble also has the logo then that takes over … same as now, also that needs to be cached too"
 *
 * Pure: no React Native, no DOM, no storage. The app (logoStore.ts — bytes on disk, index in AsyncStorage) and the web
 * client (main.ts — localStorage) each wrap a LogoIndex with their own I/O, so the two cannot drift on the policy.
 *
 * ★★★ THE FAULT THIS REPLACES. DabPanel's useServiceLogo claimed a service with `null` BEFORE its fetch and never asked
 *     again — one fetch lost in a Cloudflare quick-tunnel stall (Stuart's Sony TV server, 2026-10-30, while RadioDNS was
 *     answering perfectly) blanked that logo for the whole session. A MISS IS NOW A RETRY WITH BACKOFF (1 min, 5 min,
 *     30 min, then daily), never a permanent "no logo"; and a cached logo is never removed because a refresh failed.
 *
 * ★★ PRIORITY IS UNCHANGED, IT IS ONLY REMEMBERED NOW:
 *     air (the multiplex's own SPI logo) > radiodns (the broadcaster's file, by identity) > name (a search: usually right)
 *     > slide (programme artwork — the web's last resort, kept only while nothing better exists).
 *     A lower source never displaces a higher one, with ONE exception: an AIR logo the multiplex has not been seen
 *     carrying for AIR_STALE_MS. The air logo comes only while that block is tuned, so "not seen" is normal for weeks at
 *     a time — but a station that left the carousel for a month must not keep its old picture for ever.
 * ★★ REPLACE ONLY ON A REAL CHANGE. Same source: a content signature (md5 of the bytes in the app, the data: URL itself
 *    on the web) decides when both sides have one; otherwise the URL does. An unchanged answer only stamps `checkedAt`.
 * ★ BOUNDED (LRU by last use, entries AND bytes) — 1 GB 32-bit Android phones run this app.
 */

export type LogoSource = 'air' | 'radiodns' | 'name' | 'slide';
export const SOURCE_RANK: Record<LogoSource, number> = { air: 3, radiodns: 2, name: 1, slide: 0 };

/** ★ A day between background checks of a logo we already hold — Stuart: "only change them when a new one is published",
 *  and broadcasters publish rarely; a daily look is what notices it without asking on every tune. */
export const REFRESH_MS = 24 * 3600 * 1000;
/** ★★ The retry ladder for a key that has nothing (or whose refresh failed): 1 min, 5 min, 30 min, then daily for ever.
 *  The first rung is short because the commonest failure is a link stall that is over in seconds. */
export const RETRY_STEPS_MS = [60 * 1000, 5 * 60 * 1000, 30 * 60 * 1000, 24 * 3600 * 1000];
/** ★ An air logo not seen on the multiplex for this long may be displaced by RadioDNS (see the header). */
export const AIR_STALE_MS = 30 * 24 * 3600 * 1000;

export interface LogoEntry {
  /** where the picture was published — a broadcaster's URL, or the server's dablogoair route for an air logo */
  url: string;
  src: LogoSource;
  /** content signature — md5 (app) or the data: URL (web); '' = unknown, the URL then stands in for it */
  sig: string;
  /** what to DRAW: the app's local file:// path, or the web's data:/https: URL */
  show: string;
  bytes: number;
  /** when these bytes were stored — i.e. the last time the logo actually CHANGED */
  savedAt: number;
  /** when the source was last asked and answered (same or new) */
  checkedAt: number;
  /** LRU */
  usedAt: number;
  /** air logos: when the multiplex was last seen carrying it */
  airAt?: number;
}

export interface MissState { fails: number; nextAt: number }

export interface LogoLimits { maxEntries: number; maxBytes: number }
/** ★ App: a few hundred logos, a few MB of files. Web: localStorage is ~5 MB shared with everything else. */
export const APP_LIMITS: LogoLimits = { maxEntries: 400, maxBytes: 8 * 1024 * 1024 };
export const WEB_LIMITS: LogoLimits = { maxEntries: 300, maxBytes: 1536 * 1024 };

/** The wait after the `fails`-th consecutive failure (1-based). */
export function retryDelay(fails: number): number {
  const i = Math.max(1, Math.floor(fails)) - 1;
  return RETRY_STEPS_MS[Math.min(i, RETRY_STEPS_MS.length - 1)];
}

/** ★ DAB: ECC + EId + SId — the service's identity, the same triple RadioDNS and the bookmarks are keyed on. Hex, so
 *  it reads like the codes on the signal pane. An unknown ECC (-1) stays in the key: the EId still pins the multiplex. */
export function dabLogoKey(ecc: number, eid: number, sid: number): string {
  const h = (n: number) => (n >= 0 ? n.toString(16).toUpperCase() : '-');
  return `dab:${h(ecc)}|${h(eid)}|${h(sid)}`;
}
/** ★ FM: country + PI + the bearer's frequency (to 10 kHz) — RadioDNS answers per BEARER, and a PI is unique only within
 *  a country. The frequency is optional because the name-only callers (bookmarks) do not have one. */
export function fmLogoKey(pi: string, iso: string, freqHz?: number): string {
  const f = freqHz && freqHz > 0 ? String(Math.round(freqHz / 10000)) : '';
  return `fm:${(iso || '').toUpperCase()}|${pi.toUpperCase()}|${f}`;
}
/** ★ A name search with no identity at all (bookmark rows, EiBi) — persisted too, so it shows offline. */
export function nameLogoKey(name: string, iso?: string): string {
  return `nm:${(iso || '').toUpperCase()}|${name.trim().toLowerCase()}`;
}

/** True when a candidate from `src` may NOT displace `old` (so it need not even be downloaded). */
export function outranked(old: LogoEntry | undefined, src: LogoSource, now: number): boolean {
  if (!old) return false;
  const a = SOURCE_RANK[src], b = SOURCE_RANK[old.src];
  if (a >= b) return false;
  // ★ The one exception — an air logo the multiplex has stopped carrying (see the header).
  if (old.src === 'air' && now - (old.airAt ?? old.savedAt) > AIR_STALE_MS && src !== 'slide') return false;
  return true;
}

export type Verdict = 'store' | 'same' | 'keep';
/** What to do with a candidate answer for a key that holds `old`. */
export function verdict(old: LogoEntry | undefined, cand: { src: LogoSource; url: string; sig?: string }, now: number): Verdict {
  if (!old) return 'store';
  if (outranked(old, cand.src, now)) return 'keep';
  if (SOURCE_RANK[cand.src] !== SOURCE_RANK[old.src]) return 'store';     // a better source (or a stale air logo)
  // ★★ Same source: the CONTENT decides when both sides know it — a logo replaced in place at the same URL is a new
  //    logo; the same bytes moved to a new URL are not.
  if (cand.sig && old.sig) return cand.sig === old.sig ? 'same' : 'store';
  return cand.url === old.url ? 'same' : 'store';
}

/** Keys to drop, least recently used first, until both limits hold. `keep` is never dropped (the entry just stored). */
export function evictions(entries: Record<string, LogoEntry>, lim: LogoLimits, keep?: string): string[] {
  const keys = Object.keys(entries);
  let n = keys.length;
  let bytes = keys.reduce((s, k) => s + (entries[k].bytes || 0), 0);
  if (n <= lim.maxEntries && bytes <= lim.maxBytes) return [];
  const order = keys.filter(k => k !== keep).sort((a, b) => entries[a].usedAt - entries[b].usedAt);
  const out: string[] = [];
  for (const k of order) {
    if (n <= lim.maxEntries && bytes <= lim.maxBytes) break;
    out.push(k);
    n--; bytes -= entries[k].bytes || 0;
  }
  return out;
}

/**
 * The index both clients wrap. Holds the persisted entries plus the SESSION state — misses with their backoff, and
 * which keys are being asked about right now (so a list re-rendered every second asks once, not once per render).
 * ★ Misses are not persisted on purpose: a new session asks again at once, which is the cheapest correct retry.
 */
export class LogoIndex {
  entries: Record<string, LogoEntry>;
  private misses = new Map<string, MissState>();
  /** ★★ A HIGHER SOURCE COULD NOT BE ASKED (a stall) while a lower one answered — the lower answer is drawn, and the
   *  higher one is retried on the ladder rather than after a day. Without this, one tunnel stall during the RadioDNS
   *  lookup filed a name match for 24 hours in place of the broadcaster's own file. */
  private upgrades = new Map<string, MissState & { src: LogoSource }>();
  private busy = new Set<string>();
  limits: LogoLimits;
  // ★ No parameter properties: the tests run this file under plain `node` (type stripping only).
  constructor(entries: Record<string, LogoEntry> = {}, limits: LogoLimits = APP_LIMITS) {
    this.entries = entries;
    this.limits = limits;
  }

  get(key: string): LogoEntry | undefined { return this.entries[key]; }
  missOf(key: string): MissState | undefined { return this.misses.get(key); }
  upgradeOf(key: string): (MissState & { src: LogoSource }) | undefined { return this.upgrades.get(key); }
  isBusy(key: string): boolean { return this.busy.has(key); }

  /** Mark used (LRU). Returns true when the stamp moved enough to be worth saving (an hour — not every render). */
  touch(key: string, now: number): boolean {
    const e = this.entries[key];
    if (!e) return false;
    const was = e.usedAt;
    e.usedAt = now;
    return now - was > 3600 * 1000;
  }

  /** ★★ May we ask the sources about `key` now? Not while asking already; not inside a miss's backoff; and for a key we
   *  already hold, only once its daily refresh is due. */
  shouldAsk(key: string, now: number): boolean {
    if (this.busy.has(key)) return false;
    const m = this.misses.get(key);
    if (m && now < m.nextAt) return false;
    const e = this.entries[key];
    if (!e) return true;
    const up = this.upgrades.get(key);
    if (up && now >= up.nextAt) return true;
    return now - e.checkedAt >= REFRESH_MS;
  }
  /** Claim the key for one round of asking — call only after shouldAsk said yes. Returns false if already claimed. */
  begin(key: string): boolean {
    if (this.busy.has(key)) return false;
    this.busy.add(key);
    return true;
  }
  end(key: string): void { this.busy.delete(key); }

  /** Nothing usable came back (or the refresh failed): back off. ★ The cached entry, if any, is KEPT. */
  miss(key: string, now: number): MissState {
    const prev = this.misses.get(key);
    const fails = (prev?.fails ?? 0) + 1;
    const m = { fails, nextAt: now + retryDelay(fails) };
    this.misses.set(key, m);
    return m;
  }
  /** A source at `src` could not be asked (a stall) though something lower may have answered: retry it on the ladder. */
  failedAbove(key: string, src: LogoSource, now: number): MissState {
    const prev = this.upgrades.get(key);
    const fails = (prev?.fails ?? 0) + 1;
    const top = prev && SOURCE_RANK[prev.src] > SOURCE_RANK[src] ? prev.src : src;
    const m = { fails, nextAt: now + retryDelay(fails), src: top };
    this.upgrades.set(key, m);
    return m;
  }

  /** An answer arrived: the backoff resets — and a pending upgrade too, once the source it waits for has answered. */
  hit(key: string, src?: LogoSource): void {
    this.misses.delete(key);
    const up = this.upgrades.get(key);
    if (up && src && SOURCE_RANK[src] >= SOURCE_RANK[up.src]) this.upgrades.delete(key);
  }

  verdict(key: string, cand: { src: LogoSource; url: string; sig?: string }, now: number): Verdict {
    return verdict(this.entries[key], cand, now);
  }

  /** The source re-published the same logo: stamp it checked (and seen on air). */
  same(key: string, now: number, cand?: { url?: string; src?: LogoSource }): void {
    const e = this.entries[key];
    if (!e) return;
    e.checkedAt = now;
    if (cand?.url) e.url = cand.url;
    if (cand?.src === 'air' || e.src === 'air') e.airAt = now;
    this.hit(key, cand?.src ?? e.src);
  }

  /** A higher source answered nothing new but a lower one did and was outranked: the entry stays; stamp it checked so
   *  the lower source is not re-asked for a day. */
  confirm(key: string, now: number, src?: LogoSource): void {
    const e = this.entries[key];
    if (e) e.checkedAt = now;
    this.hit(key, src);
  }

  /** Store a new or changed logo. Returns the entries evicted to stay within the limits (and the replaced one's `show`
   *  if it differs, so the app can delete the old file) — the caller removes their bytes. */
  store(key: string, e: Omit<LogoEntry, 'savedAt' | 'checkedAt' | 'usedAt'>, now: number): { evicted: LogoEntry[]; replaced?: LogoEntry } {
    const old = this.entries[key];
    this.entries[key] = { ...e, savedAt: now, checkedAt: now, usedAt: now, ...(e.src === 'air' ? { airAt: now } : {}) };
    this.hit(key, e.src);
    const evicted: LogoEntry[] = [];
    for (const k of evictions(this.entries, this.limits, key)) { evicted.push(this.entries[k]); delete this.entries[k]; }
    return { evicted, replaced: old && old.show !== e.show ? old : undefined };
  }

  /** The picture would not load (a dead link): forget it and back off, so it is re-resolved later — not never. */
  drop(key: string, now: number): LogoEntry | undefined {
    const e = this.entries[key];
    delete this.entries[key];
    this.upgrades.delete(key);
    this.miss(key, now);
    return e;
  }
}
