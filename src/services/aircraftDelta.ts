/* ★★ V12 beta96 THE SHARED AIRCRAFT TABLE AS DELTAS — the clients' half (the server's: android/app/src/main/cpp/
 * vibe_aircraft_delta.h; docs/v12/ADSB-FEED.md §Feed aircraft as deltas).
 *
 * The server sends aircraft.json as a delta from a cursor: rows whose static part changed ("s" — what the aircraft is) and
 * rows whose dynamic part changed ("y" — where it is, with ABSOLUTE times), the keys gone, and the small header, stations and
 * feeds whole. This store keeps the rows and hands back an ordinary aircraft.json object — times turned back into ages
 * against the answer's `now` — so sharedSanitise and everything after it read exactly what they always did.
 *
 * Where the deltas come from: GET /vibeserver/aircraft.json?d=<cursor> (the standalone AIR & SEA viewer, web and app) and
 * the spectrum socket's {"type":"acs"} (a radio's map, `acd=1`). Pure: no fetch, no timers (tested in node,
 * scripts/test_aircraft_delta.ts). */

const MAX_ROWS = 2048;

export interface AircraftDeltaResult {
  /** aircraft.json's shape, ready for sharedSanitise — null when the message is not a delta this store could use */
  json: Record<string, unknown> | null;
  /** the cursor to come back with ("" = ask for everything) */
  cursor: string;
}

export class AircraftDeltaStore {
  private s = new Map<string, Record<string, unknown>>();
  private y = new Map<string, Record<string, unknown>>();
  private order: string[] = [];
  cursor = '';

  /** Forget everything (the next answer must be the whole table: ask with an empty cursor). */
  reset(): void { this.s.clear(); this.y.clear(); this.order = []; this.cursor = ''; }

  /** One delta document (parsed JSON). Returns aircraft.json's shape, or null for something that is not a delta (an older
   *  server's plain aircraft.json is passed through unchanged as `json`). */
  apply(j: any): AircraftDeltaResult {
    if (!j || typeof j !== 'object') return { json: null, cursor: this.cursor };
    if (j.d !== 1) {
      // ★ an older server ignores ?d= and answers the whole table — use it as it is, and keep asking the old way
      return { json: Array.isArray(j.aircraft) ? j : null, cursor: '' };
    }
    if (j.off === 1) { this.reset(); return { json: { now: j.now, count: 0, aircraft: [], stations: [], off: 1 }, cursor: '' }; }
    const full = j.full === 1;
    if (full) { this.s.clear(); this.y.clear(); }
    if (!full && !this.cursor) {
      // ★ a delta with nothing to apply it to (should not happen: the server answers "" with the whole table) — start over
      this.reset();
      return { json: null, cursor: '' };
    }
    if (Array.isArray(j.g)) for (const k of j.g.slice(0, MAX_ROWS)) { if (typeof k === 'string') { this.s.delete(k); this.y.delete(k); } }
    const seen: string[] = [];
    if (Array.isArray(j.a)) for (const r of j.a.slice(0, MAX_ROWS)) {
      if (!r || typeof r !== 'object' || typeof r.k !== 'string' || !r.k || r.k.length > 24) continue;
      if (r.s && typeof r.s === 'object') this.s.set(r.k, r.s);
      if (r.y && typeof r.y === 'object') this.y.set(r.k, r.y);
      seen.push(r.k);
    }
    // ★ rows the delta never described (a static part with no dynamic one) are not drawn — never half an aircraft
    for (const k of Array.from(this.s.keys())) if (!this.y.has(k)) this.s.delete(k);
    for (const k of Array.from(this.y.keys())) if (!this.s.has(k)) this.y.delete(k);
    while (this.y.size > MAX_ROWS) { const k = this.y.keys().next().value as string; this.y.delete(k); this.s.delete(k); }
    // ★ newest first, as aircraft.json reads: the rows this answer moved, then the rest in their old order
    const fresh = new Set(seen);
    this.order = [...seen.filter((k) => this.y.has(k)), ...this.order.filter((k) => !fresh.has(k) && this.y.has(k))];
    for (const k of this.y.keys()) if (!this.order.includes(k)) this.order.push(k);
    this.cursor = typeof j.c === 'string' ? j.c.slice(0, 40) : '';
    const now = typeof j.now === 'number' && isFinite(j.now) ? j.now : Math.floor(Date.now() / 1000);
    const age = (t: unknown) => (typeof t === 'number' && isFinite(t) ? Math.max(0, now - t) : undefined);
    const aircraft: Record<string, unknown>[] = [];
    for (const k of this.order) {
      const st = this.s.get(k) ?? {}, dy = this.y.get(k);
      if (!dy) continue;
      const row: Record<string, unknown> = { key: k, ...st };
      for (const [f, v] of Object.entries(dy)) {
        if (f === 'pt') row.posAge = age(v);
        else if (f === 'lt') row.age = age(v);
        else if (f === 'st') {
          const sa: Record<string, number> = {};
          if (v && typeof v === 'object') for (const [n, t] of Object.entries(v as Record<string, unknown>)) { const a = age(t); if (a != null) sa[n] = a; }
          row.srcAge = sa;
        } else row[f] = v;
      }
      aircraft.push(row);
    }
    const out: Record<string, unknown> = { ...j, aircraft };
    delete out.d; delete out.c; delete out.full; delete out.a; delete out.g; delete out.type;
    return { json: out, cursor: this.cursor };
  }
  size(): number { return this.y.size; }
}
