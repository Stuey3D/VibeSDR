/**
 * adsbFeed — V12 beta96 SHARE ADS-B WITH FLIGHT TRACKERS: the settings model for the app's server screen (VibeServer on
 * the phone, and Lite). Exactly RadioConfig::adsbFeed's JSON (android/app/src/main/cpp/vibe_beast.h parseConfig) and the
 * Setup page's afRowsFrom / afBlob (vibe_setup_page.h) — the same presets, the same refusals. docs/v12/ADSB-FEED.md.
 * Tested by scripts/test_adsb_feed.ts.
 */

export interface AdsbFeedPreset { name: string; host: string; port: number; uuid: 'auto' | 'paste' }
/** ★ The sites whose published feed set-ups give a host and port taking a plain Beast stream (docs/v12/ADSB-FEED.md: where
 *  each came from, read 2026-11-26). ★ ADS-B Exchange added 11-26: its feed client's own target (feed1.adsbexchange.com:30004,
 *  reduced Beast, a UUID); MLAT and its stats page still need its client, which can read our Beast port. */
export const ADSB_FEED_PRESETS: AdsbFeedPreset[] = [
  { name: 'adsb.lol', host: 'feed.adsb.lol', port: 30004, uuid: 'auto' },
  { name: 'adsb.fi', host: 'feed.adsb.fi', port: 30004, uuid: 'auto' },
  { name: 'airplanes.live', host: 'feed.airplanes.live', port: 30004, uuid: 'auto' },
  { name: 'map.flights', host: 'feed.map.flights', port: 30004, uuid: 'paste' },
  { name: 'ADS-B Exchange', host: 'feed1.adsbexchange.com', port: 30004, uuid: 'auto' },
];
const UUID_RE = /^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$/;

export interface AdsbFeedRow { preset: AdsbFeedPreset | null; on: boolean; host: string; port: number; uuid: string }
export interface AdsbFeedModel { serve: boolean; port: number; open: boolean; push: boolean; rows: AdsbFeedRow[]; wasSet: boolean }

export function newUuid(rand: () => number = Math.random): string {
  const b = new Array(16).fill(0).map(() => Math.floor(rand() * 256) & 255);
  b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
  const h = b.map((x) => x.toString(16).padStart(2, '0')).join('');
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}

export function parseAdsbFeed(json: string): AdsbFeedModel {
  let c: any = null;
  try { c = json ? JSON.parse(json) : null; } catch { c = null; }
  if (!c || typeof c !== 'object') c = {};
  const feeds: any[] = Array.isArray(c.feeds) ? c.feeds : [];
  const isPreset = (f: any, p: AdsbFeedPreset) => String(f.host || '').toLowerCase() === p.host && Number(f.port) === p.port;
  const rows: AdsbFeedRow[] = ADSB_FEED_PRESETS.map((p) => {
    const f = feeds.find((x) => isPreset(x, p));
    return { preset: p, on: !!(f && f.on !== false), host: p.host, port: p.port, uuid: f && f.uuid ? String(f.uuid) : '' };
  });
  for (const f of feeds) {
    if (ADSB_FEED_PRESETS.some((p) => isPreset(f, p))) continue;
    rows.push({ preset: null, on: f.on !== false, host: String(f.host || ''), port: Number(f.port) || 30004, uuid: f.uuid ? String(f.uuid) : '' });
  }
  return { serve: c.serve === true, port: c.port >= 1024 && c.port <= 65535 ? Number(c.port) : 30005, open: c.open === true,
           push: c.push === true, rows, wasSet: !!json };
}

/** The model → the blob the server stores ('' = never set and everything off), or the refusal in words. */
export function collectAdsbFeed(m: AdsbFeedModel): { blob: string; error?: undefined } | { blob?: undefined; error: string } {
  const feeds: { host: string; port: number; on: boolean; uuid?: string }[] = [];
  for (const r of m.rows) {
    if (r.preset && !r.on) continue;
    const host = String(r.host || '').trim();
    if (!host) continue;
    if (!/^[A-Za-z0-9.:-]{1,253}$/.test(host)) return { error: `“${host}” is not a host name or address` };
    const pt = Number(r.port);
    if (!(pt >= 1 && pt <= 65535 && Math.floor(pt) === pt)) return { error: `the port for ${host} must be 1 to 65535` };
    const u = String(r.uuid || '').trim();
    if (u && !UUID_RE.test(u)) return { error: `the station UUID for ${host} is not a UUID (8-4-4-4-12 hex)` };
    if (m.push && r.on && r.preset && r.preset.uuid === 'paste' && !u) return { error: `${r.preset.name} needs the station UUID from your station page there` };
    const f: { host: string; port: number; on: boolean; uuid?: string } = { host, port: pt, on: !!r.on };
    if (u) f.uuid = u.toLowerCase();
    feeds.push(f);
  }
  if (feeds.length > 8) return { error: 'at most 8 flight-tracking sites' };
  const p = Number(m.port);
  if (m.serve && !(p >= 1024 && p <= 65535 && Math.floor(p) === p)) return { error: 'the Beast port must be a whole number from 1024 to 65535' };
  if (!m.serve && !m.push && !feeds.length && !m.wasSet) return { blob: '' };
  return { blob: JSON.stringify({ serve: !!m.serve, port: p >= 1024 && p <= 65535 ? p : 30005, open: !!m.open, push: !!m.push, feeds }) };
}

/** Could this radio feed at all? Only where it can run ADS-B: never an Airspy HF+ (it stops near 260 MHz) or an Icom (a
 *  transceiver's audio) — AGENTS.md, no dead controls. The engine's own `available` answer outranks this when it has one. */
export function adsbFeedOffered(driver: string): boolean { return driver !== 'airspyhf' && driver !== 'icom'; }

/** The status line from GET /vibeserver/adsb-feed (vibe_beast.h statusJson). '' when off. */
export function adsbFeedStatusLine(j: any): string {
  if (!j || !j.on) return '';
  return 'Now: ' + String(j.line || '') + (j.frames ? ` · ${j.frames} messages decoded since the server started` : '');
}
