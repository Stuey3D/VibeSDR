/* ★★★ V12 beta96 THE STANDALONE AIR & SEA MAP — the half the web page and the app share (docs/v12/ADSB-FEED.md §The
 * standalone AIR & SEA map). Pure: every request goes through the caller's `get` (the web's fetch with P()/galUrl, the app's
 * fetch with the read auth), so it is tested in node (scripts/test_airsea_viewer.ts).
 *
 *   airSeaOffer  — is there anything to show (the landing button's gate: aircraft OR ships OR sondes / VOR OR a feed), and
 *                  which tab the map opens on (the most populated; a feed's label when it is all there is — "HFDL").
 *   AirSeaViewer — the viewer's session with the server: a TICKET on open (vibe_airsea_viewers.h — the owner's "Map viewers"
 *                  limit counts these), the aircraft as DELTAS (aircraft.json?d=<cursor>&vt=<ticket>, gzip on the wire), the
 *                  time limit's countdown and its end. An older server (no tickets, no deltas) still works: no ticket, the
 *                  whole aircraft.json every poll.
 */
import { AircraftDeltaStore } from './aircraftDelta';
import { avShown, avLayersOf, sharedSanitise } from './aviationMap';
import { SHIP_KEEP_SEC } from './shippingMap';
import { STORED_SPEC } from './storedLayers';

export type AirSeaTab = 'aircraft' | 'ships' | 'sondes' | 'navaids';
export interface AirSeaOffer {
  show: boolean;
  tab: AirSeaTab;
  /** what there is, for the button's line ("34 aircraft now · 12 ships") — aircraft = what the map draws NOW (avShown) */
  counts: { aircraft: number; ships: number; sondes: number; vor: number; feeds: number };
  /** the one feed label when every aircraft comes from feeds of one kind ("HFDL"), else '' */
  label: string;
  /** the button's caption under its title — '' when nothing worth saying */
  line: string;
}
export type Getter = (path: string) => Promise<any | null>;

const n0 = (v: unknown) => (typeof v === 'number' && isFinite(v) && v > 0 ? Math.floor(v) : 0);

/** ★★ THE LANDING BUTTON'S GATE (AGENTS.md: no dead controls) — four small reads, once when the landing draws, never polled.
 *  `get(path)` returns parsed JSON or null. A server that says nothing anywhere: no button. */
export async function airSeaOffer(get: Getter, nowSec = Math.floor(Date.now() / 1000)): Promise<AirSeaOffer> {
  const [ac, sh, sd, vm] = await Promise.all([
    get('/vibeserver/aircraft.json?since=3600').catch(() => null),   // ★ the table's whole hour — filtered below exactly as the map filters it
    get(`/vibeserver/gallery/ais-ships.json?since=${SHIP_KEEP_SEC}&limit=1`).catch(() => null),   // ★ "total" = heard in the map's ship window
    get('/vibeserver/gallery.json?kind=sondes&limit=4').catch(() => null),
    get('/vibeserver/gallery.json?kind=vormap&limit=4').catch(() => null),
  ]);
  /* ★★★ V12 11-27 THE AIRCRAFT THE MAP WILL DRAW WHEN IT OPENS, not the hour's table (Stuart: "lies on the landing
   *  screen" — the Sony's button said 243 aircraft, the map 72): the standalone map's own filter (aviationMap.ts avShown,
   *  as airSeaStandalone.ts / the web's asRender compose it — no live report, the ADS-B focus, every layer on). */
  const shared = ac && !ac.off ? sharedSanitise(ac) : null;
  const aircraftList = shared ? avShown(null, shared, 'adsb', avLayersOf(null)).shown : [];
  const feeds: any[] = ac && Array.isArray(ac.feeds) ? ac.feeds : [];
  const ships = sh && !sh.off ? (n0(sh.total) || n0(sh.count) || (Array.isArray(sh.vessels) ? sh.vessels.length : 0)) : 0;
  const stored = (j: any, kind: 'sondes' | 'vormap') => {
    const items: any[] = j && Array.isArray(j.items) ? j.items : [];
    // ★ a day card younger than the layer's keep (storedLayers.ts STORED_SPEC: sondes 24 h, VOR stations 7 days) — what the map would draw
    return items.filter((it) => typeof it?.t === 'number' && nowSec - it.t <= STORED_SPEC[kind].keepSec).length;
  };
  const counts = { aircraft: aircraftList.length, ships, sondes: stored(sd, 'sondes'), vor: stored(vm, 'vormap'), feeds: feeds.length };
  const show = counts.aircraft > 0 || counts.ships > 0 || counts.sondes > 0 || counts.vor > 0 || counts.feeds > 0;
  // ── the most populated layer ──
  const ranked: Array<[AirSeaTab, number]> = [['aircraft', counts.aircraft], ['ships', counts.ships], ['sondes', counts.sondes], ['navaids', counts.vor]];
  ranked.sort((a, b) => b[1] - a[1]);
  const tab: AirSeaTab = ranked[0][1] > 0 ? ranked[0][0] : 'aircraft';
  // ── one feed label when it is all there is: every aircraft heard only by that label's source, and no radio of ours ──
  let label = '';
  const labels = new Set(feeds.map((f) => String(f?.label || '').toUpperCase()).filter(Boolean));
  if (labels.size === 1) {
    const only = [...labels][0];
    const srcName = only === 'ADSB' ? 'ADS-B' : only;
    if (aircraftList.length && aircraftList.every((a) => Array.isArray(a?.src) && a.src.length === 1 && a.src[0] === srcName)) label = srcName;
    else if (!aircraftList.length) label = srcName;
  }
  const bits: string[] = [];
  if (counts.aircraft) bits.push(`${counts.aircraft} ${label && label !== 'ADS-B' ? label + ' ' : ''}aircraft now`);
  if (counts.ships) bits.push(`${counts.ships} ship${counts.ships === 1 ? '' : 's'}`);
  if (counts.sondes) bits.push(`radiosondes`);
  if (counts.vor) bits.push(`VOR stations`);
  if (!bits.length && counts.feeds) bits.push(`${label || 'aircraft'} feed waiting for aircraft`);
  return { show, tab, counts, label, line: bits.join(' · ') };
}

// ── the session ─────────────────────────────────────────────────────────────────────────────────────────────────────
export type ViewerState = 'joining' | 'live' | 'full' | 'ended' | 'gone';
export interface ViewerStatus {
  state: ViewerState;
  /** the banner over the map ('' = none): the last minute's countdown, the end */
  banner: string;
  /** the status line's part ('' = no limit): "Map time ends in 29:39" */
  clock: string;
  /** full: "The map is full (20/20) — try again shortly" */
  refusal: string;
}
/** ★ The words, in one place (the web and the app say the same). */
export function mapFullText(n: number, max: number): string { return `The map is full (${n}/${max}) — try again shortly`; }
export function mapEndedText(limitSec: number, backSec = 0): string {
  const m = Math.max(1, Math.round(limitSec / 60));
  const back = backSec > 90 ? ` You can open it again in about ${Math.ceil(backSec / 60)} minutes.` : backSec > 0 ? ' You can open it again in a minute or two.' : ' Try again later.';
  return `Your time on the map is up — the owner allows ${m} minute${m === 1 ? '' : 's'} each.${back}`;
}
export function mapClockText(leftSec: number): string {
  const l = Math.max(0, Math.floor(leftSec)), mm = Math.floor(l / 60), ss = l % 60;
  return `Map time ends in ${mm}:${String(ss).padStart(2, '0')}`;
}

export class AirSeaViewer {
  private store = new AircraftDeltaStore();
  private vt = '';
  private deadline = 0;        // ms; 0 = no limit
  private warnSec = 60;
  private limitSec = 0;
  private deltas = true;       // ★ off once an older server answers the whole table
  state: ViewerState = 'joining';
  refusal = '';
  endedText = '';
  private get: Getter;
  private now: () => number;
  constructor(get: Getter, now: () => number = () => Date.now()) { this.get = get; this.now = now; }

  /** Open: ask for a ticket. false = refused (full, or this address's time is used up) — `refusal` says why. An older server
   *  (no /airsea/join) is not a refusal: the map opens without a ticket. */
  async join(): Promise<boolean> {
    const j = await this.get('/vibeserver/airsea/join').catch(() => null);
    if (j && j.ok === true && typeof j.vt === 'string') {
      this.vt = j.vt.slice(0, 40);
      this.limitSec = n0(j.limit);
      this.warnSec = n0(j.warn) || 60;
      this.deadline = typeof j.left === 'number' && j.left >= 0 ? this.now() + j.left * 1000 : 0;
      this.state = 'live';
      return true;
    }
    if (j && j.ok === false && j.full) { this.state = 'full'; this.refusal = mapFullText(n0(j.n), n0(j.max)); return false; }
    if (j && j.ok === false && j.ended) { this.state = 'ended'; this.refusal = mapEndedText(n0(j.limit), n0(j.back)); this.endedText = this.refusal; return false; }
    this.vt = ''; this.deadline = 0; this.state = 'live';   // ★ an older server: no tickets — the map opens as before
    return true;
  }
  /** One poll: aircraft.json's object (whole, for sharedSanitise), or null (nothing new to draw / an error / the end). */
  async poll(): Promise<Record<string, unknown> | null> {
    if (this.state !== 'live') return null;
    const q: string[] = [];
    if (this.deltas) q.push(`d=${encodeURIComponent(this.store.cursor)}`);
    if (this.vt) q.push(`vt=${this.vt}`);
    const j = await this.get(`/vibeserver/aircraft.json${q.length ? '?' + q.join('&') : ''}`).catch(() => null);
    if (!j) return null;
    if (j.viewer === 'unknown') {
      // ★ the server forgot the ticket (a restart, a long sleep): join again — a full map now refuses honestly
      this.store.reset();
      const ok = await this.join();
      return ok ? this.poll() : null;
    }
    if (j.viewer === 'ended') { this.end(n0(j.limit) || this.limitSec); return null; }
    if (j.viewer && typeof j.viewer === 'object' && typeof j.viewer.left === 'number') {
      this.deadline = j.viewer.left >= 0 ? this.now() + j.viewer.left * 1000 : 0;
    }
    if (j.d !== 1) { this.deltas = false; return Array.isArray(j.aircraft) ? j : null; }   // ★ an older server
    const r = this.store.apply(j);
    if (!r.json) { this.store.reset(); return null; }   // ★ lost its place: the next poll asks for everything
    return r.json;
  }
  /** The time limit reached (the server said so, or the clock did): the viewer stops. */
  end(limitSec: number) { this.state = 'ended'; this.endedText = mapEndedText(limitSec); }
  /** Close the map: the ticket ends at once (else it lapses in 30 s). */
  async leave(): Promise<void> {
    const vt = this.vt; this.vt = ''; this.state = 'gone'; this.store.reset();
    if (vt) await this.get(`/vibeserver/airsea/leave?vt=${vt}`).catch(() => null);
  }
  status(): ViewerStatus {
    if (this.state === 'full') return { state: 'full', banner: '', clock: '', refusal: this.refusal };
    if (this.state === 'ended') return { state: 'ended', banner: this.endedText, clock: '', refusal: this.endedText };
    if (!this.deadline) return { state: this.state, banner: '', clock: '', refusal: '' };
    const left = Math.max(0, Math.round((this.deadline - this.now()) / 1000));
    const clock = mapClockText(left);
    return { state: this.state, banner: left <= this.warnSec ? `${clock} — the owner allows ${Math.max(1, Math.round(this.limitSec / 60))} minutes each` : '', clock, refusal: '' };
  }
  /** Seconds left (-1 = no limit). */
  left(): number { return this.deadline ? Math.max(0, Math.round((this.deadline - this.now()) / 1000)) : -1; }
  ticket(): string { return this.vt; }
}
