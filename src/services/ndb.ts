/* ndb.ts — V12 NDB on a VibeServer (docs/v12/NDB-SKIMMER.md), the pure part of the app's NDB mode.
 *
 * ★★ ONE RULE, TWO READERS: the web client's NDB view (web/client/src/ndb.ts) is the reference, and every rule here is
 *    that one — the report's sanitiser, the ident cell (✓ match · ≈ one letter off · plain unlisted · "?" candidate),
 *    the 3-bar meter, nautical miles, the list's order, the tiles, what goes on the map. scripts/test_ndb_app.ts feeds
 *    the same reports through both and fails on any difference. (The web file draws HTML; this one returns data for
 *    React Native — the two cannot share a module without rebuilding the web page, so the rules are mirrored and
 *    held together by the test.)
 *
 * Wire (spectrum socket, vibe_ndb_service.h; the mode is `{"type":"ndb","on":1|0}`):
 *   {"type":"ndb", centre, lo, hi, rate, now, carriers, identified, published, heardIn, pos, far, bin, source, starting,
 *     beacons:[{id,hz,snr,lvl,tone,tsnr,a1a,wpm,ident,votes,ballots,read,active,first,last,idAt,lastId,db,alt,also,harm,copy}],
 *     log:[…], copy:[{s,t,hz,id,text,q}]}   once a second · `ndb_off` · `ndb_error {why}`
 *   {"type":"ndb_find","q","limit"} → {"type":"ndb_found", q, pos, list:[{ident,hz,name,cc,lat,lon,km,brg,heard}]}
 *   {"type":"ndb_stats"} → {"type":"ndb_stats", since, hours:[{t,heard,far}], beacons:[{ident,hz,name,cc,km,minutes,mean,best,last,snr}]}
 */

export interface NdbDb { k: 'match' | 'near' | 'cand'; ident: string; name: string; cc: string; hz: number; lat: number; lon: number; km: number; brg: number }
export interface NdbBeacon {
  id: number; hz: number; snr: number; lvl: number; tone: number; tsnr: number; a1a: boolean; wpm: number;
  ident: string; votes: number; ballots: number; read: string; active: boolean; first: number; last: number; idAt: number; lastId: number;
  db?: NdbDb; alt?: { ident: string; name: string; cc: string; km: number }[]; also?: string[];
  harm?: { id: number; ident: string; hz: number }; copy: string;
}
export interface NdbLog { ident: string; hz: number; name: string; cc: string; km: number; brg: number; lat: number; lon: number; best: number; first: number; last: number }
export type NdbQ = 'high' | 'medium' | 'low' | 'poor';
export interface NdbCopy { s: number; t: number; hz: number; id: number; text: string; q: NdbQ }
export interface NdbReport {
  type: 'ndb'; centre: number; lo: number; hi: number; rate?: number; now?: number; carriers: number; identified: number;
  published?: number; heardIn?: number; pos?: boolean; far?: { ident: string; km: number }; bin?: number; source?: string;
  starting?: boolean; beacons: NdbBeacon[]; log: NdbLog[]; copy: NdbCopy[];
}
export interface NdbFound { ident: string; hz: number; name: string; cc: string; lat: number; lon: number; km: number; brg: number; heard: boolean }

const num = (v: unknown, d = 0) => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const str = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');
const ident = (v: unknown) => str(v, 8).replace(/[^A-Z0-9?]/g, '');

/** Trust nothing: numbers are numbers, strings are bounded, idents are A–Z / 0–9. (web ndbSanitise, rule for rule) */
export function ndbSanitise(m: any): NdbReport | null {
  if (!m || m.type !== 'ndb') return null;
  const db = (d: any): NdbDb | undefined => d && typeof d === 'object' && (d.k === 'match' || d.k === 'near' || d.k === 'cand')
    ? { k: d.k, ident: ident(d.ident), name: str(d.name, 60), cc: str(d.cc, 2).replace(/[^A-Z]/g, ''), hz: num(d.hz), lat: num(d.lat), lon: num(d.lon), km: num(d.km, -1), brg: num(d.brg) }
    : undefined;
  const beacons: NdbBeacon[] = (Array.isArray(m.beacons) ? m.beacons : []).slice(0, 200).map((b: any) => ({
    id: num(b.id), hz: num(b.hz), snr: num(b.snr), lvl: num(b.lvl), tone: num(b.tone), tsnr: num(b.tsnr), a1a: b.a1a === true, wpm: num(b.wpm),
    ident: ident(b.ident), votes: num(b.votes), ballots: num(b.ballots), read: ident(b.read), active: b.active === true,
    first: num(b.first), last: num(b.last), idAt: num(b.idAt), lastId: num(b.lastId), db: db(b.db),
    alt: Array.isArray(b.alt) ? b.alt.slice(0, 6).map((a: any) => ({ ident: ident(a.ident), name: str(a.name, 60), cc: str(a.cc, 2), km: num(a.km, -1) })) : undefined,
    also: Array.isArray(b.also) ? b.also.slice(0, 4).map(ident) : undefined,
    harm: b.harm && typeof b.harm === 'object' ? { id: num(b.harm.id), ident: ident(b.harm.ident), hz: num(b.harm.hz) } : undefined,
    copy: str(b.copy, 80).replace(/[^A-Z0-9? ]/g, ''),
  }));
  const log: NdbLog[] = (Array.isArray(m.log) ? m.log : []).slice(0, 300).map((e: any) => ({
    ident: ident(e.ident), hz: num(e.hz), name: str(e.name, 60), cc: str(e.cc, 2), km: num(e.km, -1), brg: num(e.brg),
    lat: num(e.lat), lon: num(e.lon), best: num(e.best), first: num(e.first), last: num(e.last),
  }));
  const copy: NdbCopy[] = (Array.isArray(m.copy) ? m.copy : []).slice(-200).map((c: any) => ({ s: num(c.s), t: num(c.t), hz: num(c.hz), id: num(c.id), text: str(c.text, 12).replace(/[^A-Z0-9? ]/g, ''),
    q: (c.q === 'high' || c.q === 'medium' || c.q === 'poor' ? c.q : 'low') as NdbQ }));
  return {
    type: 'ndb', centre: num(m.centre), lo: num(m.lo), hi: num(m.hi), rate: num(m.rate), now: num(m.now), carriers: num(m.carriers), identified: num(m.identified),
    published: num(m.published), heardIn: num(m.heardIn), pos: m.pos === true ? true : m.pos === false ? false : undefined, far: m.far && typeof m.far === 'object' ? { ident: ident(m.far.ident), km: num(m.far.km) } : undefined,
    bin: num(m.bin), source: str(m.source, 160), starting: m.starting === 1 || m.starting === true, beacons, log, copy,
  };
}

const COMPASS = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
export function compass(deg: number): string { return COMPASS[Math.round(((deg % 360) + 360) % 360 / 22.5) % 16]; }
export function flag(cc: string): string {
  if (!/^[A-Z]{2}$/.test(cc)) return '';
  return String.fromCodePoint(0x1f1e6 + cc.charCodeAt(0) - 65, 0x1f1e6 + cc.charCodeAt(1) - 65);
}
export function khz(hz: number): string { return (hz / 1e3).toFixed(1); }
/** ★ Distances in NAUTICAL MILES, as on the ADS-B map (its rings are these rings): "94 NM S". */
export function nmText(km: number, brg?: number): string {
  if (!(km >= 0)) return '';
  const nm = km / 1.852;
  return `${nm < 10 ? nm.toFixed(1) : Math.round(nm)} NM${brg !== undefined ? ` ${compass(brg)}` : ''}`;
}
/** ★ The 3-bar meter from the live SNR: < 10 dB one, < 20 dB two, else three; 0 = not heard in the last minute.
 *  ONE RULE, THREE READERS: web ndb.ts ndbBars, web/mapkit/ndblayer.js barsFor, this. */
export function ndbBars(snr: number, live: boolean): number { if (!live) return 0; return snr < 10 ? 1 : snr < 20 ? 2 : 3; }
/** The meter's lit colours — the map layer's BAR_ON and ADS-B's list bars: one red, two amber, three green. */
export const NDB_BAR_ON: Record<number, string> = { 1: '#e04040', 2: '#e0b020', 3: '#33cc44' };

/** ★ The ident cell: what it says, its class (how it is drawn) and why (the title / accessibility). */
export function ndbIdentCell(b: NdbBeacon): { text: string; cls: string; title: string } {
  if (b.harm) return { text: b.ident || b.harm.ident || '—', cls: 'harm', title: `2nd harmonic of ${b.harm.ident || 'the carrier'} on ${khz(b.harm.hz)} kHz` };
  if (b.ident) {
    if (b.db?.k === 'match') return { text: `✓ ${b.ident}`, cls: 'conf', title: `${b.ident} read by ${b.votes || 'folded'} agreeing repeats — matches the published beacon` };
    if (b.db?.k === 'near') return { text: `≈ ${b.ident}`, cls: 'near', title: `${b.ident} read by consensus — the published beacon here is ${b.db.ident}` };
    return { text: b.ident, cls: 'unl', title: `${b.ident} read by consensus — not in the published list` };
  }
  if (b.db?.k === 'cand') return { text: `${b.db.ident}?`, cls: 'cand', title: 'Not read yet — the published beacon nearest you on this frequency (a guess)' };
  return { text: '—', cls: 'none', title: b.read ? `Reading so far: ${b.read} (not yet confirmed)` : 'Carrier — no ident read yet' };
}
/** Station · distance + bearing, from the published match (or the candidate's). */
export function ndbStation(b: NdbBeacon): { name: string; dist: string } {
  const d = b.db;
  if (!d) return { name: b.harm ? 'harmonic' : '', dist: '' };
  const fl = flag(d.cc);
  return { name: `${fl ? fl + ' ' : ''}${d.name}${d.k === 'cand' ? '?' : ''}`, dist: nmText(d.km, d.brg) };
}
/** The four tiles. */
export function ndbTiles(r: NdbReport): { carriers: string; identified: string; furthest: string; coverage: string } {
  return {
    carriers: String(r.carriers),
    identified: String(r.identified),
    furthest: r.far ? `${r.far.ident} ${nmText(r.far.km)}` : '—',
    coverage: r.published && r.published > 0 ? `${Math.round(100 * (r.heardIn ?? 0) / r.published)} %` : '—',
  };
}
export function ndbAgo(nowS: number, tS: number): string {
  const d = Math.max(0, Math.round(nowS - tS));
  return d < 60 ? `${d} s` : d < 3600 ? `${Math.round(d / 60)} min` : `${(d / 3600).toFixed(1)} h`;
}
export function utc(tS: number): string { const d = new Date(tS * 1000); return d.toISOString().slice(11, 19); }
/** CSV from rows (quoted where needed). */
export function csv(head: string[], rows: (string | number)[][]): string {
  const q = (v: string | number) => { const s = String(v); return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s; };
  return [head.map(q).join(','), ...rows.map((r) => r.map(q).join(','))].join('\n') + '\n';
}
/** ★ The list's rows, LOWEST FREQUENCY FIRST (the CW skimmer's order): every carrier being skimmed, and the identified
 *  ones that have gone quiet (dimmed). */
export function ndbRows(r: NdbReport): NdbBeacon[] {
  return r.beacons.filter((b) => b.active || b.ident).sort((a, b) => a.hz - b.hz);
}
/** Is this beacon on the map? Only where the published list puts it (✓, ≈, or the candidate). */
export function ndbOnMap(b: NdbBeacon): boolean { return !!b.db && Number.isFinite(b.db.lat) && Number.isFinite(b.db.lon) && !(b.db.lat === 0 && b.db.lon === 0); }

/** ★ Live = heard in the last minute (the web's row rule: an unidentified carrier is live while it is active). */
export function ndbRowLive(b: NdbBeacon, nowMs: number): boolean {
  return b.active && (b.lastId === 0 ? true : nowMs - b.last < 60000);
}

/* ── The map's feed: what the shared layer (web/mapkit/ndblayer.js) is given — the web's renderMap, as data ── */
export interface NdbMapBeacon { key: string; ident: string; label: string; lat: number; lon: number; bars: number; stale: boolean; kind: 'match' | 'near' | 'cand' }
export interface NdbMapUnheard { lat: number; lon: number; ident: string }
export function ndbMapFeed(r: NdbReport, found: readonly NdbFound[]): { beacons: NdbMapBeacon[]; unheard: NdbMapUnheard[] } {
  const nowMs = (r.now || Date.now() / 1000) * 1000;
  const placed = new Set<string>();
  const beacons = ndbRows(r).filter(ndbOnMap).map((b) => {
    placed.add(`${b.db!.ident}@${Math.round(b.db!.hz / 500)}`);
    const live = b.active && nowMs - b.last < 60000;
    return { key: String(b.id), ident: b.ident || b.db!.ident, label: b.ident ? b.ident : `${b.db!.ident}?`, lat: b.db!.lat, lon: b.db!.lon,
             bars: ndbBars(b.snr, live), stale: !live || !b.ident, kind: (b.ident ? (b.db!.k === 'near' ? 'near' : 'match') : 'cand') as NdbMapBeacon['kind'] };
  });
  const unheard = found.filter((f) => !placed.has(`${f.ident}@${Math.round(f.hz / 500)}`) && f.hz >= r.lo && f.hz <= r.hi)
    .map((f) => ({ lat: f.lat, lon: f.lon, ident: f.ident }));
  return { beacons, unheard };
}

/** The server's answer to Find NDB (web found_'s parse). An empty query also feeds the map's faint unheard beacons. */
export function parseNdbFound(m: any): { q: string; pos?: boolean; list: NdbFound[] } {
  const list: NdbFound[] = (Array.isArray(m?.list) ? m.list : []).slice(0, 400).map((f: any) => ({
    ident: ident(f.ident), hz: num(f.hz), name: str(f.name, 60), cc: str(f.cc, 2), lat: num(f.lat), lon: num(f.lon), km: num(f.km, -1), brg: num(f.brg), heard: f.heard === true }));
  return { q: typeof m?.q === 'string' ? m.q.slice(0, 40) : '', pos: m?.pos === false ? false : m?.pos === true ? true : undefined, list };
}
/** A heard Find row's key — the same one the map feed places by. */
export function ndbFoundKey(f: { ident: string; hz: number }): string { return `${f.ident}@${Math.round(f.hz / 500)}`; }

export interface NdbStatsBeacon { ident: string; hz: number; name: string; cc: string; km: number; minutes: number; mean: number; best: number; last: number; snr: [number, number][] }
export interface NdbStats { since: number; hours: { t: number; heard: number; far: number }[]; beacons: NdbStatsBeacon[] }
/** The server's stats (web stats_'s parse): per hour, and each beacon's SNR — least heard first. */
export function parseNdbStats(m: any): NdbStats {
  const hours = (Array.isArray(m?.hours) ? m.hours : []).map((h: any) => ({ t: num(h?.t), heard: num(h?.heard), far: num(h?.far) }));
  const beacons: NdbStatsBeacon[] = (Array.isArray(m?.beacons) ? m.beacons : []).slice(0, 400).map((b: any) => ({
    ident: ident(b?.ident), hz: num(b?.hz), name: str(b?.name, 60), cc: str(b?.cc, 2), km: num(b?.km, -1), minutes: num(b?.minutes), mean: num(b?.mean), best: num(b?.best), last: num(b?.last),
    snr: (Array.isArray(b?.snr) ? b.snr : []).slice(-300).map((p: any) => [num(p?.[0]), num(p?.[1])] as [number, number]) }));
  beacons.sort((a, b) => a.minutes - b.minutes);
  return { since: num(m?.since), hours, beacons };
}
/** By hour of day (UTC): the average beacons heard. */
export function ndbByHourOfDay(hours: readonly { t: number; heard: number }[]): number[] {
  const byHod = new Array(24).fill(0).map(() => ({ n: 0, sum: 0 }));
  for (const h of hours) { const d = byHod[new Date(h.t * 1000).getUTCHours()]; d.n++; d.sum += h.heard; }
  return byHod.map((d) => (d.n ? d.sum / d.n : 0));
}
/** The beacons' CSV — the web's 'beacons' download, the same columns. */
export function ndbStatsCsv(s: NdbStats): string {
  return csv(['ident', 'khz', 'station', 'country', 'nm', 'minutes_heard', 'mean_snr_db', 'best_snr_db', 'last_heard_utc'],
    s.beacons.map((x) => [x.ident, (x.hz / 1e3).toFixed(1), x.name, x.cc, x.km >= 0 ? Math.round(x.km / 1.852) : '', x.minutes, x.mean.toFixed(1), x.best.toFixed(1), new Date(x.last * 1000).toISOString()]));
}

/* ── The page's per-beacon memory (the web NdbView.update): the live readings, each word with its confidence, and the
 *    SNR every 10 s for the card's sparkline. Kept while the mode runs; forgotten on leaving it. ── */
export class NdbTracks {
  private t = new Map<number, { words: NdbCopy[]; snr: [number, number][] }>();
  private seenSeq = 0;
  update(r: NdbReport): void {
    const nowS = r.now || Date.now() / 1000;
    for (const c of r.copy) {
      if (c.s <= this.seenSeq) continue;
      const k = this.track(c.id); k.words.push(c); if (k.words.length > 12) k.words.shift();
    }
    this.seenSeq = Math.max(this.seenSeq, ...r.copy.map((c) => c.s), 0);
    for (const b of r.beacons) if (b.active) {
      const k = this.track(b.id); const lastT = k.snr.length ? k.snr[k.snr.length - 1][0] : 0;
      if (nowS - lastT >= 10) { k.snr.push([nowS, b.snr]); if (k.snr.length > 90) k.snr.shift(); }
    }
  }
  words(id: number): readonly NdbCopy[] { return this.t.get(id)?.words ?? []; }
  snr(id: number): readonly [number, number][] { return this.t.get(id)?.snr ?? []; }
  clear(): void { this.t.clear(); this.seenSeq = 0; }
  private track(id: number) { let k = this.t.get(id); if (!k) { k = { words: [], snr: [] }; this.t.set(id, k); } return k; }
}

/** ★ The readings' colours — the health pill's (CW_Q_COLOURS): red poor, orange low, amber medium, green high. */
export const NDB_Q_COLOURS: Record<NdbQ, string> = { poor: '#FF4B4B', low: '#FF8A3D', medium: '#E8C547', high: '#5BE36B' };

/* ── The capability (vibeserver.json, the RADIO's own): `ndb` true / false + `ndbWhy`, and `ndbLo`/`ndbHi` — ABSENT where
 *    the radio reaches no LF (an RTL without direct sampling, a HackRF, an R2, an Icom): then no control at all. ── */
export interface NdbCap { ok: boolean; why: string; lo: number; hi: number }
export function parseNdbCapability(j: any): NdbCap | undefined {
  if (!j || (j.ndb !== true && j.ndb !== false)) return undefined;
  const why = typeof j.ndbWhy === 'string' ? j.ndbWhy.trim().slice(0, 200) : '';
  return { ok: j.ndb === true, why: j.ndb === true ? '' : (why || 'NDB is not available on this receiver'), lo: num(j.ndbLo), hi: num(j.ndbHi) };
}

/** ★ The map's toggles, remembered per device (the web's vibesdr.ndbMapPrefs): rings, the unheard beacons, autofit. */
export interface NdbMapPrefs { rings: boolean; unheard: boolean; autofit: boolean }
export function parseNdbMapPrefs(v: string | null): NdbMapPrefs {
  const d = { rings: true, unheard: true, autofit: true };
  try {
    const j = JSON.parse(v || '{}');
    return { rings: typeof j?.rings === 'boolean' ? j.rings : d.rings, unheard: typeof j?.unheard === 'boolean' ? j.unheard : d.unheard,
             autofit: typeof j?.autofit === 'boolean' ? j.autofit : d.autofit };
  } catch { return d; }
}

/** The header line under the title: "255.0–526.5 kHz · 43 carriers" (web render's #ndbStat). */
export function ndbStatText(r: NdbReport): string {
  return `${khz(r.lo)}–${khz(r.hi)} kHz · ${r.carriers} carriers${r.starting ? ' · starting…' : ''}`;
}
/** The list's empty line (web renderList). */
export function ndbEmptyText(r: NdbReport | null): string {
  return !r || r.starting || !r.beacons.length
    ? 'Skimming the whole beacon band — carriers appear as they are found; an ident once several of its repeats agree (about half a minute for a clear one).'
    : 'No beacons heard yet.';
}
export const NDB_NO_POS = 'This server has no position set — beacons are placed, but distances are unknown.';
