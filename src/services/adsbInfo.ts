// adsbInfo.ts — what each aircraft IS, as the server sends it once (V12 aircraft data, docs/v12/ADSB.md):
// `{"type":"adsb_info","aircraft":{"<hex>":{r, t, desc, wtc, eng, species, country, countryName, mil, op, opName,
// logo, sil, route{…}}}}` — new or changed aircraft only; `full:1` = the whole set (a socket that just joined).
//
// ★★★ ONE FILE, BOTH CLIENTS: the app imports it, and the web client too (web/client/src/spectrum.ts, as it already
//     does dabAssemble and tunePace) — one sanitiser, one merge, one rule for the list's second line.
// ★★ MERGED INTO THE REPORT, so every reader downstream (the lists, the shared map layer's card) sees a.r, a.t, a.op …
//    on the aircraft itself. Kept per hex until the aircraft leaves the report (the server forgets it then too, and
//    sends it again if it comes back).
// ★ Somebody else's server's JSON, drawn into lists and injected into a WebView: every field is checked for its
//   TYPE and SHAPE, strings are bounded and stripped of markup, image keys must be the server index's keys.
// ★ PURE: no React Native, no DOM — node runs it in the test.

export interface AdsbRoute {
  from: string; to: string; fromIata?: string; toIata?: string;
  fromName?: string; toName?: string; fromPlace?: string; toPlace?: string;
  stops?: string[]; plausible?: boolean;
}
export interface AdsbInfo {
  r?: string; t?: string; desc?: string; wtc?: string; eng?: string; species?: string;
  country?: string; countryName?: string; mil?: 1; op?: string; opName?: string;
  logo?: string; sil?: string; route?: AdsbRoute;
}

function txt(v: unknown, max: number): string {
  if (typeof v !== 'string') return '';
  // eslint-disable-next-line no-control-regex
  return v.replace(/[\u0000-\u001f\u007f<>]/g, '').trim().slice(0, max);
}
function code(v: unknown, re: RegExp): string { const s = txt(v, 12); return re.test(s) ? s : ''; }

/** One aircraft's details, cleaned. Unknown or malformed fields are dropped, never passed on. */
export function cleanAdsbInfo(v: unknown): AdsbInfo {
  const o: AdsbInfo = {};
  if (!v || typeof v !== 'object') return o;
  const a = v as Record<string, unknown>;
  const put = <K extends keyof AdsbInfo>(k: K, val: AdsbInfo[K] | '') => { if (val) o[k] = val as AdsbInfo[K]; };
  put('r', txt(a.r, 12));
  put('t', code(a.t, /^[A-Z0-9]{2,4}$/));
  put('desc', txt(a.desc, 60));
  put('wtc', code(a.wtc, /^[LMHJ]$/));
  put('eng', code(a.eng, /^[1-8C][JTPER]?$/));
  put('species', code(a.species, /^[LHSAGT]$/));
  put('country', code(a.country, /^[A-Z]{2}$/));
  put('countryName', txt(a.countryName, 48));
  if (a.mil === 1 || a.mil === true) o.mil = 1;
  put('op', code(a.op, /^[A-Z0-9]{2,4}$/));
  put('opName', txt(a.opName, 60));
  put('logo', code(a.logo, /^[A-Za-z0-9_-]{1,40}$/));
  put('sil', code(a.sil, /^[A-Za-z0-9_-]{1,40}$/));
  const r = a.route as Record<string, unknown> | undefined;
  if (r && typeof r === 'object') {
    const ap = /^[A-Z0-9]{3,4}$/, ia = /^[A-Z0-9]{3}$/;
    const from = code(r.from, ap), to = code(r.to, ap);
    if (from && to) {
      const rt: AdsbRoute = { from, to };
      const fi = code(r.fromIata, ia), ti = code(r.toIata, ia);
      if (fi) rt.fromIata = fi;
      if (ti) rt.toIata = ti;
      for (const k of ['fromName', 'toName', 'fromPlace', 'toPlace'] as const) { const s = txt(r[k], 70); if (s) rt[k] = s; }
      if (Array.isArray(r.stops)) { const st = r.stops.slice(0, 16).map((x) => code(x, ap)).filter(Boolean); if (st.length > 2) rt.stops = st; }
      if (typeof r.plausible === 'boolean') rt.plausible = r.plausible;
      o.route = rt;
    }
  }
  return o;
}

/** The per-hex store a client keeps while it is in ADS-B. */
export class AdsbInfoStore {
  private m = new Map<string, AdsbInfo>();
  /** An `adsb_info` message. */
  take(msg: unknown) {
    if (!msg || typeof msg !== 'object') return;
    const j = msg as Record<string, unknown>;
    const list = j.aircraft as Record<string, unknown> | undefined;
    if (!list || typeof list !== 'object') return;
    if (j.full === 1 || j.full === true) this.m.clear();
    let n = 0;
    for (const hex of Object.keys(list)) {
      if (++n > 2000) break;                                   // ★ bounded, like the report
      const h = hex.toLowerCase();
      if (!/^[0-9a-f]{6}$/.test(h)) continue;
      const info = cleanAdsbInfo(list[hex]);
      if (Object.keys(info).length) this.m.set(h, info); else this.m.delete(h);
    }
  }
  /** Each aircraft of a report, with its details merged in (the report's own fields win). Forgets aircraft that
   *  have left the report. Returns NEW objects; the report's are not changed. */
  apply<A extends { hex: string }>(aircraft: A[]): (A & AdsbInfo)[] {
    const live = new Set<string>();
    const out = aircraft.map((a) => {
      const h = String(a.hex || '').toLowerCase();
      live.add(h);
      const i = this.m.get(h);
      return i ? { ...i, ...a } : { ...a };
    });
    for (const h of [...this.m.keys()]) if (!live.has(h)) this.m.delete(h);
    return out as (A & AdsbInfo)[];
  }
  get(hex: string): AdsbInfo | undefined { return this.m.get(String(hex || '').toLowerCase()); }
  clear() { this.m.clear(); }
  get size() { return this.m.size; }
}

/** A flag from an ISO alpha-2 code (regional-indicator emoji); '' for anything else. */
export function flagEmoji(iso: unknown): string {
  const c = String(iso ?? '').toUpperCase();
  if (!/^[A-Z]{2}$/.test(c)) return '';
  return String.fromCodePoint(0x1F1E6 + c.charCodeAt(0) - 65, 0x1F1E6 + c.charCodeAt(1) - 65);
}
/** "AMS → STN" (IATA when both ends have one, else ICAO); '' when there is none or the server judged it NOT plausible. */
export function routeShort(r: AdsbRoute | undefined): string {
  if (!r || r.plausible === false) return '';
  const iata = !!(r.fromIata && r.toIata);
  return `${iata ? r.fromIata : r.from} → ${iata ? r.toIata : r.to}`;
}
/** The list row's second line: operator · type · route · registration — what there is, in that order. ★ The route
 *  before the registration: on a narrow list the end is what gets cut, and the card carries the registration anyway. */
export function adsbInfoLine(a: AdsbInfo): string {
  return [a.op || (a.opName === 'Private' ? 'Private' : ''), a.t, routeShort(a.route), a.r].filter(Boolean).join(' · ');
}
