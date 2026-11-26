/* ★★ V12 beta96 SERVER-LEVEL AIRCRAFT FEEDS on the Android server screen (docs/v12/ADSB-FEED.md §Feed sources) — the Setup
 * page's #feedsCard rules (vibe_setup_page.h feedsFromForm / feedsTitle / beastAddrWhy), the same words, for the phone's
 * Server mode and VibeServer Lite. Stored as ONE JSON list, exactly ServerConfig::aircraftFeeds (vibe_feeds.h parseSources —
 * the engine checks it again and refuses in words). Pure: tested by scripts/test_aircraft_feeds_app.ts. */

export type FeedFmt = 'beast' | 'sbs';
export type FeedDir = 'connect' | 'listen';
export type FeedLabel = 'adsb' | 'hfdl' | 'vdl2' | 'acars';
export interface FeedSource { fmt: FeedFmt; dir: FeedDir; host?: string; port: number; label: FeedLabel; name?: string }
export const FEED_MAX = 8;
export const FEED_LABEL_WORD: Record<FeedLabel, string> = { adsb: 'ADS-B', hfdl: 'HFDL', vdl2: 'VDL2', acars: 'ACARS' };

/** The form's defaults for a format (Beast: connect to :30005; BaseStation: dumphfdl connects in to :32010, HFDL). */
export function feedDefaults(fmt: FeedFmt): { dir: FeedDir; port: number; label: FeedLabel } {
  return fmt === 'sbs' ? { dir: 'listen', port: 32010, label: 'hfdl' } : { dir: 'connect', port: 30005, label: 'adsb' };
}

/** The stored text → the list ('' or junk = none). */
export function parseFeeds(json: string | null | undefined): FeedSource[] {
  if (!json) return [];
  let j: unknown;
  try { j = JSON.parse(json); } catch { return []; }
  if (!Array.isArray(j)) return [];
  const out: FeedSource[] = [];
  for (const e of j.slice(0, FEED_MAX)) {
    if (!e || typeof e !== 'object') continue;
    const o = e as Record<string, unknown>;
    const fmt: FeedFmt = o.fmt === 'sbs' ? 'sbs' : 'beast';
    const dir: FeedDir = o.dir === 'listen' ? 'listen' : 'connect';
    const port = Number(o.port);
    if (!(port >= 1 && port <= 65535)) continue;
    const label: FeedLabel = fmt === 'beast' ? 'adsb' : (['adsb', 'hfdl', 'vdl2', 'acars'].includes(String(o.label)) ? String(o.label) as FeedLabel : 'hfdl');
    const f: FeedSource = { fmt, dir, port, label };
    if (dir === 'connect') { const h = typeof o.host === 'string' ? o.host.trim().slice(0, 253) : ''; if (!h) continue; f.host = h; }
    if (typeof o.name === 'string' && o.name.trim()) f.name = o.name.trim().slice(0, 40);
    out.push(f);
  }
  return out;
}
/** The list → the stored text ('' = none, as the Setup page writes it). */
export function feedsBlob(list: FeedSource[]): string { return list.length ? JSON.stringify(list) : ''; }

export function feedTitle(f: FeedSource): string {
  const lbl = FEED_LABEL_WORD[f.fmt === 'beast' ? 'adsb' : f.label] || 'ADS-B';
  return f.name || (f.dir === 'listen' ? `${lbl} feed on port ${f.port}` : `${lbl} from ${f.host}:${f.port}`);
}

/** The Setup page's address rule (beastAddrWhy): '' = fine, else why not. */
export function feedAddrWhy(t: string): string {
  const v = String(t || '').trim();
  if (!v) return 'give the other receiver’s address, e.g. 192.168.1.20:30005';
  let host = v, port = '';
  const m6 = v.match(/^\[([0-9a-fA-F:.]+)\](?::(\d+))?$/);
  if (m6) { host = m6[1]; port = m6[2] || ''; }
  else if ((v.match(/:/g) || []).length === 1) { host = v.split(':')[0]; port = v.split(':')[1]; }
  if (port !== '' && !(/^\d{1,5}$/.test(port) && +port >= 1 && +port <= 65535)) return 'the port must be 1 to 65535';
  if (/^[\d.]+$/.test(host) && !/^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$/.test(host)) return `“${host}” is not an IPv4 address`;
  if (!/^[A-Za-z0-9.:-]{1,253}$/.test(host)) return `“${host}” is not a host name or address`;
  return '';
}

/** The ADD form → {src} or {error} (feedsFromForm). */
export function feedFromForm(fmt: FeedFmt, dir: FeedDir, addr: string, port: string | number, label: FeedLabel, name: string, list: FeedSource[]):
    { src: FeedSource; error?: undefined } | { src?: undefined; error: string } {
  const f: FeedSource = { fmt, dir, port: 0, label: fmt === 'beast' ? 'adsb' : label };
  if (list.length >= FEED_MAX) return { error: `at most ${FEED_MAX} aircraft feeds` };
  if (dir === 'connect') {
    const why = feedAddrWhy(addr);
    if (why) return { error: why };
    const v = String(addr).trim();
    const m6 = v.match(/^\[([0-9a-fA-F:.]+)\](?::(\d+))?$/);
    let host = v, pt = '';
    if (m6) { host = m6[1]; pt = m6[2] || ''; }
    else if ((v.match(/:/g) || []).length === 1) { host = v.split(':')[0]; pt = v.split(':')[1]; }
    f.host = host; f.port = pt ? Number(pt) : feedDefaults(fmt).port;
  } else {
    const p = Number(port);
    if (!(p >= 1024 && p <= 65535 && Math.floor(p) === p)) return { error: 'a feed listens on a port from 1024 to 65535' };
    if (list.some((x) => x.dir === 'listen' && x.port === p)) return { error: `another feed already listens on ${p}` };
    f.port = p;
  }
  const n = String(name || '').trim().slice(0, 40);
  if (n) f.name = n;
  return { src: f };
}

/** The running server's word on each source (/vibeserver/adsb-feed `sources`: {title, label, line|state}) → one line each. */
export function feedStatusLines(j: any): string[] {
  const out: string[] = [];
  for (const s of (j && Array.isArray(j.sources) ? j.sources : []).slice(0, FEED_MAX)) {
    if (!s || typeof s !== 'object') continue;
    out.push(`${String(s.title || 'feed').slice(0, 40)} (${String(s.label || '').slice(0, 6)}): ${String(s.line || s.state || '').slice(0, 200)}`);
  }
  return out;
}

/* ── the standalone AIR & SEA map's limits (ServerConfig::mapViewers / mapTimeMin — vibe_airsea_viewers.h) ── */
export const MAP_VIEWERS_DEFAULT = 20;
export function mapViewersOf(v: string | null | undefined): number { const n = parseInt(String(v ?? ''), 10); return Number.isFinite(n) ? Math.max(0, Math.min(1000, n)) : MAP_VIEWERS_DEFAULT; }
export function mapTimeMinOf(v: string | null | undefined): number { const n = parseInt(String(v ?? ''), 10); return Number.isFinite(n) ? Math.max(0, Math.min(1440, n)) : 0; }
/** ★ The measured cost per viewer (docs/v12/ADSB-FEED.md — the Setup page's AS_MAP_KBS): KB/s at ~150 aircraft. */
export const MAP_KBS_PER_VIEWER = 1.6;
export function mapCostLine(viewers: number): string {
  const base = `Each viewer uses about ${MAP_KBS_PER_VIEWER} KB/s with 150 aircraft on the map, plus a one-off download as the map opens.`;
  if (!viewers) return `${base} With no limit, every extra viewer adds that much.`;
  const kbs = viewers * MAP_KBS_PER_VIEWER;
  return `${base} At ${viewers} viewers that is about ${Math.round(kbs)} KB/s (${(kbs * 8 / 1000).toFixed(1)} Mbit/s) of upload — on mobile data, about ${Math.round(kbs * 3600 / 1024)} MB an hour if all ${viewers} stay.`;
}
/** The running server's viewers (/vibeserver/adsb-feed `mapViewers`) → the status line ('' = none open). */
export function mapViewersLine(j: any): string {
  const m = j?.mapViewers;
  if (!m || typeof m.now !== 'number') return '';
  return `${m.now} ${m.now === 1 ? 'viewer' : 'viewers'} on the map now${Number(m.max) ? ` of ${m.max}` : ''}`;
}
