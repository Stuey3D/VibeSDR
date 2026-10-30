/**
 * ★★★ THE WEB CLIENT'S PERSISTENT STATION-LOGO INDEX (V12, Stuart, 2026-10-30: "cache the logos and only change them
 * when a new one is published"). The rules are src/services/logoCachePolicy.ts — the SAME copy the app uses — and this
 * file is only their storage: one localStorage record, written a second after the last change.
 *
 * ★★ URLS FOR THE BROADCASTERS' FILES, BYTES ONLY FOR OUR OWN. A page may not read a third-party picture's bytes without
 *    CORS headers, which logo hosts do not send — the browser's HTTP cache keeps those. The multiplex's own logo and the
 *    slideshow come from this server, so they are kept as data: URLs (≤ 24 KB) and draw with the server on any block.
 * ★ Every storage call is wrapped: a private window, a full quota or blocked site data is an empty cache, never a fault.
 */
import { LogoIndex, WEB_LIMITS, dabLogoKey, outranked, type LogoEntry, type LogoSource } from '../../../src/services/logoCachePolicy.ts';

const STORE = 'vibe.logos.v2';
/** ★ The DAB-only store this replaces: `${ecc}|${eid}|${sid}` (decimal) → { u, t }. Migrated once, then removed. */
const OLD_DAB = 'vibe.dabLogos.v1';

/** The web's DAB key (`${ecc}|${eid}|${sid}`, decimal — what the station list and the bookmark rows use) as the index's. */
export function dabIdxKey(webKey: string): string {
  const [e, i, s] = webKey.split('|').map(Number);
  return dabLogoKey(Number.isFinite(e) ? e : -1, Number.isFinite(i) ? i : 0, Number.isFinite(s) ? s : 0);
}
/** …and back. */
export function dabWebKey(idxKey: string): string | null {
  const m = /^dab:([0-9A-F]+|-)\|([0-9A-F]+|-)\|([0-9A-F]+|-)$/.exec(idxKey);
  if (!m) return null;
  const n = (h: string) => (h === '-' ? -1 : parseInt(h, 16));
  return `${n(m[1])}|${n(m[2]) < 0 ? 0 : n(m[2])}|${n(m[3])}`;
}

function load(): Record<string, LogoEntry> {
  const out: Record<string, LogoEntry> = {};
  try {
    const raw = localStorage.getItem(STORE);
    if (raw) {
      const j = JSON.parse(raw) as Record<string, LogoEntry>;
      for (const [k, e] of Object.entries(j)) if (e && typeof e.show === 'string' && e.src) out[k] = e;
      return out;
    }
    /* ★ MIGRATE: the old store did not record a source. A data: URL was the multiplex's picture or a slide — filed as
     *  'slide' so a real air logo or RadioDNS replaces it; a URL as 'name', so an identity answer replaces it. */
    const old = localStorage.getItem(OLD_DAB);
    if (old) {
      const j = JSON.parse(old) as Record<string, { u: string; t: number }>;
      for (const [k, v] of Object.entries(j)) {
        if (!v?.u) continue;
        const data = v.u.startsWith('data:image/');
        out[dabIdxKey(k)] = { url: data ? '' : v.u, src: data ? 'slide' : 'name', sig: data ? v.u : '', show: v.u,
          bytes: v.u.length, savedAt: v.t, checkedAt: v.t, usedAt: v.t };
      }
      setTimeout(() => { try { localStorage.removeItem(OLD_DAB); } catch { /* ignore */ } save(); }, 0);
    }
  } catch { /* storage unavailable or corrupt: start empty */ }
  return out;
}

export const webLogos = new LogoIndex(load(), WEB_LIMITS);

let timer: ReturnType<typeof setTimeout> | null = null;
function save() {
  try { localStorage.setItem(STORE, JSON.stringify(webLogos.entries)); }
  catch {
    /* ★ Over quota (localStorage is shared with everything else this client keeps): drop the oldest half of the
     *  pictures we hold as bytes and try once more. */
    try {
      const big = Object.entries(webLogos.entries).filter(([, e]) => e.show.startsWith('data:'))
        .sort((a, b) => a[1].usedAt - b[1].usedAt);
      for (const [k] of big.slice(0, Math.ceil(big.length / 2))) delete webLogos.entries[k];
      localStorage.setItem(STORE, JSON.stringify(webLogos.entries));
    } catch { /* give up: the session still has its in-memory copy */ }
  }
}
export function webLogoSave(): void {
  if (timer) return;
  timer = setTimeout(() => { timer = null; save(); }, 1000);
}

/** ★ URLs that failed to draw this session — never filed again until the next visit, or a dead favicon would be
 *  re-resolved, re-filed and re-requested on every rung of the retry ladder. */
const dead = new Set<string>();

/**
 * Offer an answer for `key`. Applies the policy and returns what to DRAW for the key afterwards (the new picture, or the
 * held one when the offer was outranked or unchanged), or '' when there is nothing.
 * `sig` is the content when we have it (a data: URL); `show` defaults to the URL.
 */
export function webLogoOffer(key: string, src: LogoSource, url: string, opts: { sig?: string; show?: string } = {}): string {
  const show = opts.show ?? url;
  if (!show || dead.has(show)) return webLogos.get(key)?.show ?? '';
  const now = Date.now();
  const v = webLogos.verdict(key, { src, url, sig: opts.sig }, now);
  if (v === 'keep') { webLogos.confirm(key, now); return webLogos.get(key)?.show ?? ''; }
  if (v === 'same') { webLogos.same(key, now, { url, src }); webLogoSave(); return webLogos.get(key)?.show ?? ''; }
  webLogos.store(key, { url, src, sig: opts.sig ?? '', show, bytes: show.length }, now);
  webLogoSave();
  return show;
}

/** Would a candidate from `src` be refused for `key`? (So a fetch can be skipped.) */
export function webLogoOutranked(key: string, src: LogoSource): boolean {
  return outranked(webLogos.get(key), src, Date.now());
}

/** A picture that would not draw: forget every key holding it and back off. Returns the keys forgotten. */
export function webLogoForget(shown: string): string[] {
  if (!shown) return [];
  dead.add(shown);
  const out: string[] = [];
  for (const [k, e] of Object.entries(webLogos.entries)) if (e.show === shown) { webLogos.drop(k, Date.now()); out.push(k); }
  if (out.length) webLogoSave();
  return out;
}
export function webLogoDead(url: string): boolean { return dead.has(url); }
