/* community-map.js — THE VibeSDR COMMUNITY MAP's logic: read, merge, filter. No DOM here (the page is community-map.html),
 * so directory/scripts/test-community.mjs runs exactly this file against fixtures.
 *
 * ★★★ STUART (2026-11-25): "a combined world map that everyone can contribute to with a simple toggle … one map that a user
 *     can have both maritime and aviation together or filter by mode".
 * ★★★ EACH SERVER IS READ DIRECTLY, never through the directory's Worker: https://<its own tunnel>/vibeserver/community.json
 *     (vibe_community.h on the server — the server's own formats: aircraft.json, ais-ships.json, gallery.json?kind=dsc,
 *     navtex-stations.json, the NDB report's log). The Worker only said who contributes (/api/community).
 * ★★ ONE AIRCRAFT, ONE SHIP, ONE MARKER: the same ICAO address (or registration / flight where there is none) and the same
 *    MMSI heard by several servers merge into one item that says "heard by N receivers"; the newest position wins.
 * ★★ NO DEAD CONTROLS: the page offers a mode filter only for a mode somebody actually shares. */
(function (root) {
  'use strict';

  // ── the modes ────────────────────────────────────────────────────────────────────────────────────────────────────────
  /* ★ NDB is AVIATION: the beacons are placed from the aeronautical navaid list (OurAirports) and almost all of them are
   *   aviation beacons now — the maritime radiobeacons went with DGPS. */
  const MODES = [
    { id: 'adsb',   label: 'ADS-B',  group: 'aviation', colour: '#ffb000', words: 'Aircraft positions from ADS-B (1090 MHz)' },
    { id: 'acars',  label: 'ACARS',  group: 'aviation', colour: '#f472b6', words: 'Aircraft positions from VHF ACARS' },
    { id: 'vdl2',   label: 'VDL2',   group: 'aviation', colour: '#e879f9', words: 'Aircraft positions from VDL Mode 2' },
    { id: 'hfdl',   label: 'HFDL',   group: 'aviation', colour: '#c084fc', words: 'Aircraft positions from HF datalink' },
    { id: 'ndb',    label: 'NDB',    group: 'aviation', colour: '#a3e635', words: 'Non-directional beacons heard, placed from the published list' },
    { id: 'ais',    label: 'AIS',    group: 'maritime', colour: '#22d3ee', words: 'Ships, from AIS' },
    { id: 'dsc',    label: 'DSC',    group: 'maritime', colour: '#ef4444', words: 'Positions sent in DSC calls' },
    { id: 'navtex', label: 'NAVTEX', group: 'maritime', colour: '#4ade80', words: 'NAVTEX stations heard' },
  ];
  const MODE = Object.fromEntries(MODES.map((m) => [m.id, m]));
  const SRC_MODE = { 'ADS-B': 'adsb', ACARS: 'acars', VDL2: 'vdl2', HFDL: 'hfdl' };   // ★ ADS-C rides a bearer: no mode of its own
  /** The age choices (seconds). Aircraft are only ever sent for 15 minutes; ships 6 h; NDB a day; DSC and NAVTEX a week. */
  const AGES = [{ s: 900, label: '15 MIN' }, { s: 3600, label: '1 H' }, { s: 6 * 3600, label: '6 H' }, { s: 86400, label: '24 H' }, { s: 7 * 86400, label: '7 DAYS' }];
  const ADSB_STALE_SEC = 120;   // ★ an ADS-B aircraft squitters twice a second: two minutes unheard = drawn faded

  // ── small, defensive readers ─────────────────────────────────────────────────────────────────────────────────────────
  const num = (x) => (typeof x === 'number' && isFinite(x) ? x : (typeof x === 'string' && x.trim() !== '' && isFinite(Number(x)) ? Number(x) : null));
  const str = (x, n = 80) => (typeof x === 'string' ? x.replace(/[\u0000-\u001f\u007f]/g, ' ').trim().slice(0, n) : '');
  const placed = (lat, lon) => lat !== null && lon !== null && Math.abs(lat) <= 90 && Math.abs(lon) <= 180 && !(lat === 0 && lon === 0);
  const arr = (x, n) => (Array.isArray(x) ? x.slice(0, n) : []);

  /** ★ One server's community.json → items, each with its own server's sighting. `sv` = the contributor ({id,name,…}). */
  function itemsOf(c, sv) {
    const out = [];
    if (!c || typeof c !== 'object' || c.on !== true) return out;
    const now = num(c.now) || Math.floor(Date.now() / 1000);
    const seen = (t) => ({ sid: sv.id, name: sv.name, t });
    // aircraft — /vibeserver/aircraft.json rows
    const ac = c.aircraft && typeof c.aircraft === 'object' ? c.aircraft : null;
    if (ac) {
      const at = num(ac.now) || now;
      for (const r of arr(ac.aircraft, 1000)) {
        if (!r || typeof r !== 'object') continue;
        const lat = num(r.lat), lon = num(r.lon);
        if (!placed(lat, lon)) continue;
        const icao = /^[0-9A-F]{6}$/i.test(String(r.icao || '')) ? String(r.icao).toUpperCase() : '';
        const key = 'a:' + (icao || str(r.key, 24));
        const modes = [...new Set(arr(r.src, 8).map((s) => SRC_MODE[s]).filter(Boolean))];
        if (!modes.length) continue;
        const t = at - Math.max(0, num(r.posAge) || 0);
        out.push({ key, kind: 'aircraft', modes, lat, lon, t, icao,
          title: str(r.flight, 12) || str(r.reg, 12) || icao || 'Aircraft',
          d: { reg: str(r.reg, 12), flight: str(r.flight, 12), type: str(r.type, 12), alt: num(r.alt), gs: num(r.gs), trk: num(r.trk),
               posSrc: str(r.posSrc, 8), ground: r.ground === 1, operator: str(r.operator || r.op, 40) },
          heard: [seen(t)] });
      }
    }
    // ships — /vibeserver/gallery/ais-ships.json vessels
    const sh = c.ships && typeof c.ships === 'object' ? c.ships : null;
    if (sh) {
      const at = num(sh.now) || now;
      for (const v of arr(sh.vessels, 2048)) {
        if (!v || typeof v !== 'object') continue;
        const mmsi = num(v.mmsi), lat = num(v.lat), lon = num(v.lon);
        if (!(mmsi > 0) || !placed(lat, lon)) continue;
        const t = at - Math.max(0, num(v.seen) || 0);
        out.push({ key: 's:' + mmsi, kind: 'ship', modes: ['ais'], lat, lon, t,
          title: str(v.name, 30) || 'MMSI ' + mmsi,
          d: { mmsi: String(mmsi), call: str(v.call, 10), dest: str(v.dest, 24), type: str(v.stypeName || v.atonName, 40), flag: str(v.flag, 3),
               country: str(v.country, 40), sog: num(v.sog), cog: num(v.cog), vkind: str(v.kind, 6), line: str(v.line, 160) },
          heard: [seen(t)] });
      }
    }
    // DSC — /vibeserver/gallery.json?kind=dsc items with a pinned position
    const ds = c.dsc && typeof c.dsc === 'object' ? c.dsc : null;
    if (ds) {
      for (const it of arr(ds.items, 1000)) {
        if (!it || it.k !== 'dsc') continue;
        const lat = num(it.plat), lon = num(it.plon), t = num(it.t);
        if (!placed(lat, lon) || !(t > 0)) continue;
        const title = str(it.title, 200), m = /\b(\d{9})\b/.exec(title);
        const lv = Math.max(0, Math.min(3, num(it.lv) || 0));
        out.push({ key: 'd:' + (m ? m[1] : '') + ':' + lat.toFixed(3) + ',' + lon.toFixed(3), kind: 'dsc', modes: ['dsc'], lat, lon, t,
          title: lv >= 3 ? 'DSC DISTRESS' : lv === 2 ? 'DSC URGENCY' : lv === 1 ? 'DSC safety' : 'DSC call',
          d: { mmsi: m ? m[1] : '', level: lv, line: title }, heard: [seen(t)] });
      }
    }
    // NAVTEX — /vibeserver/gallery/navtex-stations.json stations
    const nv = c.navtex && typeof c.navtex === 'object' ? c.navtex : null;
    if (nv) {
      for (const s of arr(nv.stations, 200)) {
        if (!s || typeof s !== 'object') continue;
        const lat = num(s.lat), lon = num(s.lon), t = num(s.last);
        if (!placed(lat, lon) || !(t > 0)) continue;
        const b1 = /^[A-Z]$/.test(String(s.b1 || '')) ? s.b1 : '?';
        const name = str(s.name, 60);
        out.push({ key: 'n:' + b1 + ':' + name, kind: 'navtex', modes: ['navtex'], lat, lon, t,
          title: `NAVTEX ${b1} · ${name || 'station'}`,
          d: { b1, name, iso: str(s.iso, 3), country: str(s.country, 40), n: num(s.n) || 0, conf: str(s.conf, 12),
               khz: arr(s.khz, 4).map(num).filter((k) => k !== null) },
          heard: [seen(t)] });
      }
    }
    // NDB — the NDB report's log shape
    const nd = c.ndb && typeof c.ndb === 'object' ? c.ndb : null;
    if (nd) {
      for (const b of arr(nd.log, 500)) {
        if (!b || typeof b !== 'object') continue;
        const lat = num(b.lat), lon = num(b.lon), t = num(b.last), hz = num(b.hz);
        const ident = str(b.ident, 6).toUpperCase();
        if (!placed(lat, lon) || !(t > 0) || !ident || !(hz > 0)) continue;
        out.push({ key: 'b:' + ident + ':' + Math.round(hz / 500), kind: 'ndb', modes: ['ndb'], lat, lon, t,
          title: `NDB ${ident} · ${(hz / 1e3).toFixed(1)} kHz`,
          d: { ident, name: str(b.name, 60), cc: str(b.cc, 3), best: num(b.best) }, heard: [seen(t)] });
      }
    }
    return out;
  }

  /** ★★ The merge: one item per key (and a DSC call heard by two servers — same position, within two minutes — once). The
   *  newest sighting's position and details win; every server that heard it is kept, newest first. */
  function merge(lists) {
    const by = new Map();
    const dsc = [];
    for (const list of lists) for (const it of list) {
      let have = by.get(it.key);
      if (!have && it.kind === 'dsc') have = dsc.find((x) => Math.abs(x.t - it.t) <= 120 && Math.abs(x.lat - it.lat) < 1e-3 && Math.abs(x.lon - it.lon) < 1e-3);
      if (!have) { const c = { ...it, heard: it.heard.slice(), modes: it.modes.slice() }; by.set(it.key, c); if (it.kind === 'dsc') dsc.push(c); continue; }
      for (const h of it.heard) {
        const k = have.heard.findIndex((x) => x.sid === h.sid);
        if (k < 0) have.heard.push(h); else if (h.t > have.heard[k].t) have.heard[k] = h;
      }
      for (const m of it.modes) if (!have.modes.includes(m)) have.modes.push(m);
      if (it.t > have.t) { have.lat = it.lat; have.lon = it.lon; have.t = it.t; have.title = it.title; have.d = { ...have.d, ...it.d }; }
    }
    const out = [...by.values()];
    for (const it of out) it.heard.sort((a, b) => b.t - a.t);
    return out;
  }

  /** ★ The filter: scope ('both' | 'aviation' | 'maritime'), the modes on (a Set), the age (s), and one receiver or none. */
  function filter(items, f, now) {
    const t0 = now - (f.maxAge || 86400);
    return items.filter((it) => it.t >= t0
      && it.modes.some((m) => f.modes.has(m) && (f.scope === 'both' || !f.scope || MODE[m].group === f.scope))
      && (!f.server || it.heard.some((h) => h.sid === f.server)));
  }

  /** The modes somebody shares: from the contributors' layers and what actually arrived. */
  function modesPresent(servers, items) {
    const s = new Set();
    for (const v of servers) for (const l of v.layers || []) if (MODE[l]) s.add(l);
    for (const it of items) for (const m of it.modes) s.add(m);
    return MODES.filter((m) => s.has(m.id)).map((m) => m.id);
  }

  /** How many items each receiver is providing (in the current filter, or all). */
  function perServer(items) {
    const n = new Map();
    for (const it of items) for (const h of it.heard) n.set(h.sid, (n.get(h.sid) || 0) + 1);
    return n;
  }

  // ── addresses: the directory's own rules ───────────────────────────────────────────────────────────────────────────
  /** ★ The directory's ADDRESS_OK: only <slug>.vibeserver.vibesdr.net is a receiver address we will aim a tab at. */
  const ADDRESS_OK = /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.vibeserver\.vibesdr\.net$/;
  /** The LISTEN link — the directory's listenUrl(): the slug page where there is one, else the server's own address. */
  function listenUrl(s) {
    if (s && typeof s.address === 'string' && ADDRESS_OK.test(s.address)) return `https://${s.address}/`;
    return s && typeof s.url === 'string' && /^https:\/\/[A-Za-z0-9.-]+(:\d{1,5})?$/.test(s.url) ? s.url + '/' : '';
  }
  /** ★★ The DATA address: the server's own https URL, never the slug (that is the directory's Worker). On an http page (a
   *  local test) an http URL is allowed too — an https page could not load it anyway. */
  function dataUrl(s, pageProto) {
    const u = s && typeof s.url === 'string' ? s.url.replace(/\/$/, '') : '';
    const ok = /^https:\/\/[A-Za-z0-9.-]+(:\d{1,5})?$/.test(u) || (pageProto === 'http:' && /^http:\/\/[A-Za-z0-9.-]+(:\d{1,5})?$/.test(u));
    return ok ? u + '/vibeserver/community.json' : '';
  }
  /** ★ The directory's state() words for a receiver, from what the listing says (and whether it answered us). */
  function stateOf(s) {
    if (s.unreachable) return { cls: 'down', label: 'not responding' };
    if (s.maxListeners > 0 && s.listeners >= s.maxListeners) return { cls: 'busy', label: 'no free slot' };
    if (s.temporary) return { cls: 'temp', label: 'temporary share' };
    return { cls: 'free', label: 'free slot' };
  }

  // ── the polling rule ─────────────────────────────────────────────────────────────────────────────────────────────────
  /* ★★★ EVERY POLLED FETCH HAS A FLOOR (Stuart, 2026-11-24: the Workers limit incident). Each server is asked at most once
   *     per REFRESH_SEC while the tab is visible and never while it is hidden; a server that fails is left alone for
   *     RETRY_SEC, doubling to RETRY_MAX_SEC. The contributors list (the Worker) is asked once, then every LIST_SEC. */
  const REFRESH_SEC = 60, RETRY_SEC = 300, RETRY_MAX_SEC = 1800, LIST_SEC = 600, FETCH_TIMEOUT_MS = 8000;
  /** Is server `s` due a fetch at `nowMs`? (its own `next` stamp) */
  const due = (s, nowMs) => !(s._next > nowMs);
  function noteResult(s, ok, nowMs) {
    if (ok) { s._fails = 0; s.unreachable = false; s._next = nowMs + REFRESH_SEC * 1000; }
    else { s._fails = (s._fails || 0) + 1; s.unreachable = true; s._next = nowMs + Math.min(RETRY_MAX_SEC, RETRY_SEC * 2 ** (s._fails - 1)) * 1000; }
  }

  // ── words ──────────────────────────────────────────────────────────────────────────────────────────────────────────
  function ago(sec) {
    sec = Math.max(0, Math.round(sec));
    if (sec < 60) return sec + ' s';
    if (sec < 3600) return Math.round(sec / 60) + ' min';
    if (sec < 86400) return (sec / 3600).toFixed(sec < 36000 ? 1 : 0) + ' h';
    return Math.round(sec / 86400) + ' d';
  }
  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  const api = { MODES, MODE, AGES, ADSB_STALE_SEC, REFRESH_SEC, RETRY_SEC, RETRY_MAX_SEC, LIST_SEC, FETCH_TIMEOUT_MS,
                itemsOf, merge, filter, modesPresent, perServer, listenUrl, dataUrl, stateOf, due, noteResult, ago, esc, ADDRESS_OK };
  root.CommunityMap = api;
  if (typeof module === 'object' && module && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
