/* adsbDelta.ts — ★★ V12 beta96 THE ADS-B DELTA PROTOCOL on the CLIENTS (the app's VibeServerClient and the web
 * client's spectrum.ts both import this). The server's side and the wire format are in
 * android/app/src/main/cpp/vibe_adsb_delta.h; the design and the measured saving in docs/v12/ADSB-DELTA.md.
 *
 * A client that puts `adsbd=1` on its spectrum socket gets `adsb_k` (the whole table) and `adsb_d` (one second's
 * changes) instead of the whole `adsb` list every second. This store keeps the table and hands back, after each message,
 * an object in exactly the shape of an `adsb` message — the same field names, the same presence rules — so everything
 * downstream (parseAdsbMessage, adsb_info merging, the list, the map layer, the cards, the range outline) is untouched.
 * An older server never sends these; the client handles `adsb` exactly as before.
 *
 * ★ Somebody else's server's JSON: every value is checked (a number is finite, an id is a small integer, a string is a
 *   string), and the table is capped as the full list is. Anything that does not fit = ask for a keyframe.
 */

/** Columns, in the server's order (vibe_adsb_delta.h Col). The first five are differences. */
const C_LAT = 0, C_LON = 1, C_POSAT = 2, C_SEENAT = 3, C_MSGS = 4, C_ALT = 5, C_RSSI = 6, C_GS = 7, C_TRACK = 8,
  C_VR = 9, C_GEOM = 10, C_HDG = 11, C_TAS = 12, C_IAS = 13, C_SQUAWK = 14, C_EMERG = 15, C_TRACKED = 16,
  C_SNR = 17,   // ★ beta96 SNR over the 1090 MHz floor, a 3 dB band (vibe_adsb_delta.h cSnr / snrBand)
  C_FLIGHT = 18, C_CAT = 19, C_COMMB = 20, N_COLS = 21, N_NUM = 18;
const DIFF_COLS = 5;
const MAX_ROWS = 2000;   // the server's kMaxRows (and the web client's own cap on the full list)
const MAX_ID = 1_000_000;
const RESYNC_GAP_MS = 2000;

type Cell = number | string | Record<string, unknown> | undefined;
interface Row { hex: string; v: Cell[] }

export interface AdsbDeltaResult {
  /** The table as an `adsb` message ({type:'adsb', now, …, aircraft:[…]}), or null when there is nothing new to draw. */
  report: Record<string, unknown> | null;
  /** Send {"type":"adsb_resync"}: the client missed something (a gap in `s`, a row it does not hold). */
  resync: boolean;
}

const isInt = (x: unknown): x is number => typeof x === 'number' && Number.isInteger(x) && Math.abs(x) < 2 ** 52;

export class AdsbDeltaStore {
  private seq = -1;                 // the tick the table is at; -1 = none yet
  private t = 0;                    // that tick's T (vibe_adsb_delta.h)
  private header: Record<string, unknown> = {};
  private rows = new Map<number, Row>();
  private resyncAt = -Infinity;

  /** ADS-B ended (adsb_off), or a new socket: forget the table. */
  clear(): void { this.seq = -1; this.header = {}; this.rows.clear(); }
  /** How many aircraft are held (tests). */
  get size(): number { return this.rows.size; }
  get at(): number { return this.seq; }

  /** Apply one `adsb_k` / `adsb_d`. `nowMs` only paces the resync requests. */
  take(msg: Record<string, unknown>, nowMs = Date.now()): AdsbDeltaResult {
    const none = (resync: boolean): AdsbDeltaResult => {
      if (!resync || nowMs - this.resyncAt < RESYNC_GAP_MS) return { report: null, resync: false };
      this.resyncAt = nowMs;
      return { report: null, resync: true };
    };
    const s = msg.s, t = msg.t;
    if (!isInt(s) || s < 0 || !isInt(t)) return none(true);
    if (msg.type === 'adsb_k') {
      const a = Array.isArray(msg.a) ? msg.a : null;
      const h = msg.h && typeof msg.h === 'object' && !Array.isArray(msg.h) ? msg.h as Record<string, unknown> : null;
      if (!a || !h) return none(true);
      const rows = new Map<number, Row>();
      for (let i = 0; i < a.length && rows.size < MAX_ROWS;) {
        const got = this.newRow(a, i, t);
        if (!got) return none(true);
        rows.set(got[0], got[1]); i = got[2];
      }
      this.rows = rows; this.header = { ...h }; this.seq = s; this.t = t;
      this.resyncAt = -Infinity;   // ★ answered: the next gap may ask again at once
      return { report: this.report(), resync: false };
    }
    if (msg.type !== 'adsb_d') return { report: null, resync: false };
    if (this.seq < 0) return none(true);                      // a delta with no table under it
    if (s <= this.seq) return { report: null, resync: false };   // already in the keyframe we hold
    if (s !== this.seq + 1) return none(true);                // missed one
    // ── validate everything before touching the table, so a bad message leaves it whole ──
    const h = msg.h === undefined ? {} : msg.h, hx = msg.hx === undefined ? [] : msg.hx;
    const g = msg.g === undefined ? [] : msg.g, n = msg.n === undefined ? [] : msg.n, u = msg.u === undefined ? [] : msg.u;
    if (!h || typeof h !== 'object' || Array.isArray(h) || !Array.isArray(hx) || !Array.isArray(g) || !Array.isArray(n)
        || !Array.isArray(u)) return none(true);
    for (const k of hx) delete this.header[String(k)];
    Object.assign(this.header, h);
    for (const id of g) if (isInt(id)) this.rows.delete(id);
    for (let i = 0; i < n.length && this.rows.size < MAX_ROWS;) {
      const got = this.newRow(n, i, t);
      if (!got) { this.seq = -1; return none(true); }
      this.rows.set(got[0], got[1]); i = got[2];
    }
    for (let i = 0; i < u.length;) {
      const row = isInt(u[i]) ? this.rows.get(u[i] as number) : undefined;
      const next = row ? this.apply(row, u, i + 1, t) : -1;
      if (next < 0) { this.seq = -1; return none(true); }
      i = next;
    }
    this.seq = s; this.t = t;
    return { report: this.report(), resync: false };
  }

  /** One `id,"hex",mask,v…` row at r[i] → [id, row, the index after it], or null. */
  private newRow(r: unknown[], i: number, t: number): [number, Row, number] | null {
    const id = r[i], hex = r[i + 1];
    if (!isInt(id) || id < 0 || id > MAX_ID || typeof hex !== 'string' || hex.length > 8) return null;
    const row: Row = { hex, v: new Array(N_COLS) };
    const next = this.apply(row, r, i + 2, t);
    return next < 0 ? null : [id, row, next];
  }

  /** `r[at]` is the mask, the values follow in column order (vibe_adsb_delta.h encodeChange). Returns the index after
   *  the row, or -1 when it does not read. */
  private apply(row: Row, r: unknown[], at: number, t: number): number {
    const mask = r[at];
    if (!isInt(mask) || mask < 0 || mask >= 2 ** N_COLS) return -1;
    let i = at + 1;
    for (let c = 0; c < N_COLS; c++) {
      if (!(mask & (1 << c))) continue;
      if (i >= r.length) return -1;
      const x = r[i++];
      if (x === null) { row.v[c] = undefined; continue; }
      if (c < N_NUM) {
        if (c === C_ALT && x === 'g') { row.v[c] = 'g'; continue; }
        if (!isInt(x)) return -1;
        const held = row.v[c];
        row.v[c] = c < DIFF_COLS && typeof held === 'number' ? held + x : x;
      } else if (c === C_COMMB) {
        if (!x || typeof x !== 'object' || Array.isArray(x)) return -1;
        row.v[c] = x as Record<string, unknown>;
      } else {
        if (typeof x !== 'string') return -1;
        row.v[c] = x;
      }
    }
    // ★ the two times the server leaves out when they are this tick's T (the header's "TWO TIMES LEFT OUT")
    const latLon = (1 << C_LAT) | (1 << C_LON);
    if (!(mask & (1 << C_POSAT)) && (mask & latLon) && row.v[C_LAT] !== undefined && row.v[C_LON] !== undefined) row.v[C_POSAT] = t;
    if (!(mask & (1 << C_SEENAT)) && (mask & (1 << C_MSGS)) && row.v[C_MSGS] !== undefined) row.v[C_SEENAT] = t;
    return i;
  }

  /** The table in the `adsb` report's own shape (vibe_adsb.h Tracker::json), in id order. */
  private report(): Record<string, unknown> {
    const T = this.t;
    const age = (x: Cell) => Math.max(0, Math.round(((T - (x as number)) + 0.35) * 10) / 10);
    const ids = [...this.rows.keys()].sort((a, b) => a - b);
    const aircraft: Record<string, unknown>[] = [];
    for (const id of ids) {
      const v = this.rows.get(id)!.v;
      const a: Record<string, unknown> = { hex: this.rows.get(id)!.hex };
      if (v[C_FLIGHT] !== undefined) a.flight = v[C_FLIGHT];
      if (v[C_CAT] !== undefined) a.category = v[C_CAT];
      if (v[C_LAT] !== undefined && v[C_LON] !== undefined) {
        a.lat = (v[C_LAT] as number) / 1e5; a.lon = (v[C_LON] as number) / 1e5;
        if (v[C_POSAT] !== undefined) a.seen_pos = age(v[C_POSAT]);
      }
      if (v[C_ALT] === 'g') a.alt_baro = 'ground';
      else if (v[C_ALT] !== undefined) a.alt_baro = (v[C_ALT] as number) * 25;
      if (v[C_GEOM] !== undefined) a.alt_geom = (v[C_GEOM] as number) * 25;
      if (v[C_GS] !== undefined) a.gs = v[C_GS];
      if (v[C_TRACK] !== undefined) a.track = v[C_TRACK];
      if (v[C_HDG] !== undefined) a.mag_heading = v[C_HDG];
      if (v[C_TAS] !== undefined) a.tas = v[C_TAS];
      if (v[C_IAS] !== undefined) a.ias = v[C_IAS];
      if (v[C_VR] !== undefined) a.baro_rate = v[C_VR];
      if (v[C_SQUAWK] !== undefined) a.squawk = String(v[C_SQUAWK]).padStart(4, '0');
      if (v[C_EMERG] !== undefined) a.emergency = v[C_EMERG];
      if (v[C_TRACKED] !== undefined) a.tracked_nm = v[C_TRACKED];
      if (v[C_COMMB] !== undefined) a.commb = v[C_COMMB];
      if (v[C_SEENAT] !== undefined) a.seen = age(v[C_SEENAT]);
      if (v[C_MSGS] !== undefined) a.messages = v[C_MSGS];
      if (v[C_RSSI] !== undefined) a.rssi = v[C_RSSI];
      // ★ beta96 the SNR comes in 3 dB bands on ADS-B's colour edges (vibe_adsb_delta.h snrBand): band k = 11.5 + 3k dB
      if (v[C_SNR] !== undefined) a.snr = 11.5 + 3 * (v[C_SNR] as number);
      aircraft.push(a);
    }
    return { type: 'adsb', ...this.header, aircraft };
  }
}
