/* community-map.js — THE VibeSDR COMMUNITY MAP's logic: read, merge, filter, schedule. No DOM here (the page is
 * community-map.html), so directory/scripts/test-community.mjs runs exactly this file against fixtures.
 *
 * ★★★ STUART (2026-11-25): "a combined world map that everyone can contribute to with a simple toggle" · "anything that we
 *     map now should be able to be contributed" · "all layers can be on at once aviation/maritime/misc" · "optimise
 *     everything so it doesn't get too overloaded and takes an eternity to load".
 * ★★★ EACH SERVER IS READ DIRECTLY, never through the directory's Worker: https://<its own tunnel>/vibeserver/community.json
 *     ?s=<the layers switched on> (vibe_community.h — compact rows; NAVTEX in the NAVTEX map's own shapes). The Worker only
 *     said who contributes (/api/community).
 * ★★ ONE THING, ONE MARKER: the same ICAO address, MMSI, sonde serial, APRS call / object, MeshCore node, NAVTEX station,
 *    warning and message (station + id) heard by several servers is one item that says "heard by N receivers"; the newest
 *    sighting's position wins — a NAVTEX message keeps its CLEANEST copy (fewest lost characters).
 * ★★ NO DEAD CONTROLS: a mode chip only for a mode somebody shares. Uses window.CommunityKit (community-kit.js, the server
 *    maps' own sanitisers, icons and cards) for NAVTEX where it is loaded. */
(function (root) {
  'use strict';
  const K = () => root.CommunityKit || null;

  // ── the groups and the modes ────────────────────────────────────────────────────────────────────────────────────────
  /* ★ Each mode: its group, its colour, the section it comes in (vibe_community.h sectionsOfLayer), its window (how far back
   *   the server shares it — the chip's tooltip says so) and whether it is a SLOW layer (asked every 5 min, not every minute). */
  const GROUPS = [
    { id: 'aviation', label: 'AVIATION' },
    { id: 'maritime', label: 'MARITIME' },
    { id: 'misc', label: 'MISC' },
  ];
  const H = 3600;
  const MODES = [
    { id: 'adsb',   label: 'ADS-B',  group: 'aviation', colour: '#ffb000', sec: 'a', win: 300,     slow: false, words: 'Aircraft positions from ADS-B (1090 MHz)' },
    { id: 'acars',  label: 'ACARS',  group: 'aviation', colour: '#f472b6', sec: 'a', win: 1800,    slow: false, words: 'Aircraft positions from VHF ACARS' },
    { id: 'vdl2',   label: 'VDL2',   group: 'aviation', colour: '#e879f9', sec: 'a', win: 1800,    slow: false, words: 'Aircraft positions from VDL Mode 2' },
    { id: 'hfdl',   label: 'HFDL',   group: 'aviation', colour: '#c084fc', sec: 'a', win: 1800,    slow: false, words: 'Aircraft positions from HF datalink' },
    { id: 'ndb',    label: 'NDB',    group: 'aviation', colour: '#a3e635', sec: 'b', win: 24 * H,  slow: true,  words: 'Non-directional beacons heard, placed from the published list' },
    { id: 'vor',    label: 'VOR',    group: 'aviation', colour: '#93c5fd', sec: 'o', win: 24 * H,  slow: true,  words: 'VOR / ILS beacons identified on the VOR map, placed from the published list' },
    { id: 'ais',    label: 'AIS',    group: 'maritime', colour: '#22d3ee', sec: 's', win: 6 * H,   slow: false, words: 'Ships, from AIS' },
    { id: 'dsc',    label: 'DSC',    group: 'maritime', colour: '#ef4444', sec: 'd', win: 24 * H,  slow: false, words: 'Positions sent in DSC calls' },
    { id: 'navtex', label: 'NAVTEX', group: 'maritime', colour: '#4fc3f7', sec: 'n', win: 24 * H,  slow: true,  words: 'NAVTEX: the stations heard, warnings and their areas, forecasts, the messages' },
    { id: 'sondes', label: 'SONDES', group: 'misc',     colour: '#7fd4ff', sec: 'z', win: 12 * H,  slow: true,  words: 'Radiosondes: the current and the last flight, with tracks' },
    { id: 'aprs',   label: 'APRS',   group: 'misc',     colour: '#facc15', sec: 'p', win: 6 * H,   slow: false, words: 'APRS stations, objects and weather with a position' },
    { id: 'mesh',   label: 'MESHCORE', group: 'misc',   colour: '#34d399', sec: 'm', win: 24 * H,  slow: false, words: 'MeshCore nodes, from their own public adverts (a person’s node rounded to ~1 km)' },
  ];
  const MODE = Object.fromEntries(MODES.map((m) => [m.id, m]));
  const SRC_BITS = [[1, 'adsb'], [2, 'acars'], [4, 'vdl2'], [8, 'hfdl']];   // ★ 16 = ADS-C: it rides a bearer
  const AGES = [{ s: 900, label: '15 MIN' }, { s: H, label: '1 H' }, { s: 6 * H, label: '6 H' }, { s: 24 * H, label: '24 H' }];
  const ADSB_STALE_SEC = 120;
  const winWords = (s) => (s < H ? `${Math.round(s / 60)} min` : s < 86400 ? `${s / H} h` : `${s / 86400} d`);
  /** The chip's tooltip: what it is and how far back servers share it. */
  const modeTip = (m) => `${m.words} — shared for the last ${winWords(m.win)}`;

  // ── small, defensive readers ─────────────────────────────────────────────────────────────────────────────────────────
  const num = (x) => (typeof x === 'number' && isFinite(x) ? x : null);
  const str = (x, n = 80) => (typeof x === 'string' ? x.replace(/[\u0000-\u001f\u007f]/g, ' ').trim().slice(0, n) : '');
  const placed = (lat, lon) => lat !== null && lon !== null && Math.abs(lat) <= 90 && Math.abs(lon) <= 180 && !(lat === 0 && lon === 0);
  const arr = (x, n) => (Array.isArray(x) ? x.slice(0, n) : []);

  /** ★ One server's answer (v2, compact) → items, each with that server's sighting. `sv` = the contributor ({id, name}). */
  function itemsOf(c, sv) {
    const out = [];
    if (!c || typeof c !== 'object' || c.on !== true) return out;
    const now = num(c.now) || Math.floor(Date.now() / 1000);
    const seen = (t, extra) => Object.assign({ sid: sv.id, name: sv.name, t }, sv.rid ? { rid: sv.rid } : {}, extra || {});   // ★ rid: the radio that answered (a message's text is read there)
    for (const r of arr(c.a, 1000)) {                       // aircraft [k, lat, lon, age, alt, trk, gs, mask, flight, reg, type]
      if (!Array.isArray(r)) continue;
      const lat = num(r[1]), lon = num(r[2]), mask = num(r[7]) || 0;
      if (!placed(lat, lon)) continue;
      const modes = SRC_BITS.filter(([b]) => mask & b).map(([, m]) => m);
      if (!modes.length) continue;
      const k = str(r[0], 32), t = now - Math.max(0, num(r[3]) || 0);
      out.push({ key: 'a:' + k, kind: 'aircraft', modes, lat, lon, t, title: str(r[8], 10) || str(r[9], 10) || k,
        d: { icao: /^[0-9A-F]{6}$/.test(k) ? k : '', alt: num(r[4]), trk: num(r[5]), gs: num(r[6]), flight: str(r[8], 10), reg: str(r[9], 10), type: str(r[10], 8), adsc: !!(mask & 16) },
        heard: [seen(t)] });
    }
    for (const r of arr(c.s, 1000)) {                       // ships [mmsi, lat, lon, seen UTC, sog, cog, family, kind, name, call]
      if (!Array.isArray(r)) continue;
      const mmsi = num(r[0]), lat = num(r[1]), lon = num(r[2]), t = num(r[3]);
      if (!(mmsi > 0) || !placed(lat, lon) || !(t > 0)) continue;
      out.push({ key: 's:' + mmsi, kind: 'ship', modes: ['ais'], lat, lon, t, title: str(r[8], 24) || 'MMSI ' + mmsi,
        d: { mmsi: String(mmsi), sog: num(r[4]), cog: num(r[5]), family: str(r[6], 12) || 'other', vkind: ['A', 'B', 'base', 'aton', 'sar', 'sart'].includes(r[7]) ? r[7] : 'A', name: str(r[8], 24), call: str(r[9], 8) },
        heard: [seen(t)] });
    }
    for (const r of arr(c.d, 500)) {                        // DSC [t, lat, lon, level, title, file]
      if (!Array.isArray(r)) continue;
      const t = num(r[0]), lat = num(r[1]), lon = num(r[2]), lv = Math.max(0, Math.min(3, num(r[3]) || 0));
      if (!(t > 0) || !placed(lat, lon)) continue;
      const line = str(r[4], 200), m = /\b(\d{9})\b/.exec(line);
      out.push({ key: 'd:' + (m ? m[1] : '') + ':' + lat.toFixed(3) + ',' + lon.toFixed(3), kind: 'dsc', modes: ['dsc'], lat, lon, t,
        title: lv >= 3 ? 'DSC DISTRESS' : lv === 2 ? 'DSC URGENCY' : lv === 1 ? 'DSC safety' : 'DSC call', d: { mmsi: m ? m[1] : '', level: lv, line }, heard: [seen(t)] });
    }
    for (const r of arr(c.b, 500)) {                        // NDB [ident, hz, lat, lon, last, name, cc]
      if (!Array.isArray(r)) continue;
      const lat = num(r[2]), lon = num(r[3]), t = num(r[4]), hz = num(r[1]), id = str(r[0], 6).toUpperCase();
      if (!placed(lat, lon) || !(t > 0) || !id || !(hz > 0)) continue;
      out.push({ key: 'b:' + id + ':' + Math.round(hz / 500), kind: 'ndb', modes: ['ndb'], lat, lon, t, title: `NDB ${id} · ${(hz / 1e3).toFixed(1)} kHz`,
        d: { ident: id, name: str(r[5], 60), cc: str(r[6], 3) }, heard: [seen(t)] });
    }
    for (const r of arr(c.o, 500)) {                        // VOR [ident, hz, lat, lon, last, name, cc, kind]
      if (!Array.isArray(r)) continue;
      const lat = num(r[2]), lon = num(r[3]), t = num(r[4]), hz = num(r[1]), id = str(r[0], 5).toUpperCase();
      if (!placed(lat, lon) || !(t > 0) || !id || !(hz > 0)) continue;
      out.push({ key: 'o:' + id + ':' + Math.round(hz / 1e4), kind: 'vor', modes: ['vor'], lat, lon, t,
        title: `${r[7] === 'loc' ? 'ILS' : 'VOR'} ${id} · ${(hz / 1e6).toFixed(2)} MHz`, d: { ident: id, name: str(r[5], 60), cc: str(r[6], 3), loc: r[7] === 'loc' }, heard: [seen(t)] });
    }
    for (const r of arr(c.z, 64)) {                         // sondes [serial, type, lat, lon, alt, climb, last, maxAlt, temp, track]
      if (!Array.isArray(r)) continue;
      const lat = num(r[2]), lon = num(r[3]), t = num(r[6]), serial = str(r[0], 16);
      if (!placed(lat, lon) || !(t > 0) || !serial) continue;
      const track = arr(r[9], 200).filter((p) => Array.isArray(p) && placed(num(p[0]), num(p[1]))).map((p) => [p[0], p[1]]);
      out.push({ key: 'z:' + serial, kind: 'sonde', modes: ['sondes'], lat, lon, t, title: `${str(r[1], 16) || 'Sonde'} ${serial}`,
        d: { serial, type: str(r[1], 16), alt: num(r[4]), climb: num(r[5]), maxAlt: num(r[7]), temp: num(r[8]), track }, heard: [seen(t)] });
    }
    for (const r of arr(c.p, 1000)) {                       // APRS [key, from, lat, lon, t, sym, kind, line]
      if (!Array.isArray(r)) continue;
      const lat = num(r[2]), lon = num(r[3]), t = num(r[4]), k = str(r[0], 24);
      if (!placed(lat, lon) || !(t > 0) || !k) continue;
      out.push({ key: 'p:' + k, kind: 'aprs', modes: ['aprs'], lat, lon, t, title: k.replace(/^obj:/, ''),
        d: { from: str(r[1], 12), sym: str(r[5], 2), akind: str(r[6], 12), line: str(r[7], 160) }, heard: [seen(t)] });
    }
    for (const r of arr(c.m, 1000)) {                       // MeshCore [id, name, type, lat, lon, last, count, approx]
      if (!Array.isArray(r)) continue;
      const lat = num(r[3]), lon = num(r[4]), t = num(r[5]), id = str(r[0], 16), name = str(r[1], 40);
      if (!placed(lat, lon) || !(t > 0) || !(id || name)) continue;
      out.push({ key: 'm:' + (id || 'n:' + name), kind: 'mesh', modes: ['mesh'], lat, lon, t, title: name || id,
        d: { node: id, name, ntype: ['repeater', 'companion', 'room server', 'sensor'].includes(r[2]) ? r[2] : 'companion', approx: r[7] === 1, count: num(r[6]) || 0 }, heard: [seen(t)] });
    }
    // ── NAVTEX, in the NAVTEX map's own shapes (sanitised by its own code when the kit is loaded) ──
    const k = K();
    if (c.ns && k) {
      const rep = k.navSanitise(Object.assign({}, c.ns, { rx: null }));
      for (const s of rep ? rep.stations : []) {
        if (!placed(s.lat, s.lon) || !(s.last > 0)) continue;
        out.push({ key: 'ns:' + s.b1 + ':' + s.name, kind: 'navst', modes: ['navtex'], lat: s.lat, lon: s.lon, t: s.last, title: `NAVTEX ${s.b1} · ${s.name}`,
          d: s, heard: [seen(s.last)] });
      }
    }
    if (c.nw && k) {
      const rep = k.navWarnSanitise(c.nw);
      for (const w of rep ? rep.list : []) {
        out.push({ key: 'nw:' + (w.st || '') + ':' + w.id, kind: 'navwarn', modes: ['navtex'], lat: w.lat, lon: w.lon, t: w.last, title: w.id, b2: w.id[1],
          station: w.st, d: w, heard: [seen(w.last, { f: w.f })] });
      }
    }
    if (c.nm && k) {
      const rep = k.navMetSanitise(c.nm);
      for (const a of rep ? rep.areas : []) {
        if (!a.fc.length) continue;
        out.push({ key: 'nm:' + a.id, kind: 'navmet', modes: ['navtex'], lat: a.lat, lon: a.lon, t: a.fc[0].t, title: a.name, b2: 'E',
          station: a.fc[0].st, d: a, heard: [seen(a.fc[0].t, { f: a.fc[0].f })] });
      }
    }
    for (const r of arr(c.nx, 1000)) {                      // NAVTEX messages [id, kHz, first, last, n, lost, station, conf, iso, file, title]
      if (!Array.isArray(r)) continue;
      const t = num(r[3]), f = str(r[9], 128);
      if (!(t > 0) || !f) continue;
      const id = /^[A-Z]{2}\d{2}$/.test(r[0]) ? r[0] : '';
      const st = str(r[6], 40);
      out.push({ key: 'nx:' + (id ? st + ':' + id + ':' + Math.round(num(r[1]) || 0) : sv.id + ':' + f), kind: 'navmsg', modes: ['navtex'], lat: null, lon: null, t,
        title: str(r[10], 80), b2: id ? id[1] : '', station: st,
        d: { id, khz: num(r[1]) || 0, first: num(r[2]) || t, n: num(r[4]) || 1, lost: Math.max(0, num(r[5]) || 0), st, sc: str(r[7], 12), iso: str(r[8], 2) },
        heard: [seen(t, { f, lost: Math.max(0, num(r[5]) || 0) })] });
    }
    return out;
  }

  /** ★★ The merge: one item per key (a DSC call heard by two servers — same position, within two minutes — once). The newest
   *  sighting's position and details win; a NAVTEX message keeps its CLEANEST copy as the text; every server is kept. */
  function merge(lists) {
    const by = new Map(), dsc = [];
    for (const list of lists) for (const it of list) {
      let have = by.get(it.key);
      if (!have && it.kind === 'dsc') have = dsc.find((x) => Math.abs(x.t - it.t) <= 120 && Math.abs(x.lat - it.lat) < 1e-3 && Math.abs(x.lon - it.lon) < 1e-3);
      if (!have) { const c = Object.assign({}, it, { heard: it.heard.slice(), modes: it.modes.slice() }); by.set(it.key, c); if (it.kind === 'dsc') dsc.push(c); continue; }
      for (const h of it.heard) {
        const k = have.heard.findIndex((x) => x.sid === h.sid);
        if (k < 0) have.heard.push(h); else if (h.t > have.heard[k].t) have.heard[k] = h;
      }
      for (const m of it.modes) if (!have.modes.includes(m)) have.modes.push(m);
      if (it.kind === 'navmsg') {
        have.d = Object.assign({}, have.d, { n: have.d.n + it.d.n, first: Math.min(have.d.first, it.d.first) });
        if (it.d.lost < have.d.lost || (it.d.lost === have.d.lost && it.t > have.t)) { have.d = Object.assign({}, it.d, { n: have.d.n, first: have.d.first }); have.title = it.title; }
        have.t = Math.max(have.t, it.t);
        continue;
      }
      if (it.t > have.t) { have.lat = it.lat; have.lon = it.lon; have.t = it.t; have.title = it.title; have.d = it.kind === 'navwarn' || it.kind === 'navmet' || it.kind === 'navst' ? it.d : Object.assign({}, have.d, it.d); }
    }
    const out = [...by.values()];
    for (const it of out) it.heard.sort((a, b) => b.t - a.t);
    return out;
  }
  /** ★ Where to read a NAVTEX message's (or warning's, or forecast's) text: the receiver that holds its cleanest copy. */
  function textSource(it) {
    const hs = it.heard.filter((h) => h.f);
    if (!hs.length) return null;
    hs.sort((a, b) => (a.lost ?? 0) - (b.lost ?? 0) || b.t - a.t);
    return { sid: hs[0].sid, f: hs[0].f, rid: hs[0].rid || '' };
  }
  /** ★★ ONE SERVER'S RADIOS, MERGED (a multi-radio server — several radios often answer the same server-wide stores, the
   *  Pi 500's three RTLs alike): one item per key, the newest copy wins; nothing summed (it is one receiver, not two). */
  function mergeRadios(lists) {
    const by = new Map();
    for (const list of lists) for (const it of list) { const h = by.get(it.key); if (!h || it.t > h.t) by.set(it.key, it); }
    return [...by.values()];
  }

  /** ★ The filter: the groups on, the chips on, the age, one receiver or none, and NAVTEX's own (its layers, message types,
   *  station). Each mode also keeps to its own window. A group switched off hides its chips' layers without forgetting them. */
  function filter(items, f, now) {
    const nav = f.nav || {};
    return items.filter((it) => {
      if (!it.modes.some((m) => MODE[m] && f.groups.has(MODE[m].group) && f.modes.has(m) && it.t >= now - Math.min(f.maxAge || 86400, MODE[m].win))) return false;
      if (f.server && !it.heard.some((h) => h.sid === f.server)) return false;
      if (it.modes[0] === 'navtex') {
        const layer = it.kind === 'navst' ? 'stations' : it.kind === 'navwarn' ? 'warnings' : it.kind === 'navmet' ? 'weather' : 'messages';
        if (nav.layers && !nav.layers.has(layer)) return false;
        if (nav.types && it.b2 && !nav.types.has(it.b2)) return false;
        if (nav.station && it.kind !== 'navst' && it.station !== nav.station) return false;
        if (nav.station && it.kind === 'navst' && it.d.name !== nav.station) return false;
        if (it.kind === 'navwarn' && (it.d.expired || it.d.cancelled || it.d.superseded)) return false;   // ★ the NAVTEX map's rule
      }
      return true;
    });
  }
  /** ★ THE PILLS' TOTALS (Stuart 11-25: "maybe a little total in the pills for each displayed mode"): per mode, the items it
   *  draws on the map under the HEARD IN window, ONLY THIS and NAVTEX's type / station filters — counted as if its chip and
   *  group were on, so a pill switched off still says what it would show; NAVTEX's messages (a list, not the map) apart. Per
   *  NAVTEX layer too. The same numbers as the status line's when everything is on. */
  /** The mode an item is drawn (and counted) as: its first source not switched off, else its first. */
  const primaryMode = (it, off) => it.modes.find((x) => !(off && off.has(x))) || it.modes[0];
  function modeCounts(items, f, now) {
    const all = { groups: new Set(GROUPS.map((g) => g.id)), modes: new Set(MODES.map((m) => m.id)), maxAge: f.maxAge, server: f.server,
                  nav: Object.assign({}, f.nav || {}, { layers: null }) };
    const by = Object.fromEntries(MODES.map((m) => [m.id, 0]));
    const nav = { stations: 0, warnings: 0, weather: 0, messages: 0 };
    for (const it of filter(items, all, now)) {
      if (it.kind === 'navmsg') { nav.messages++; continue; }
      if (it.kind === 'navst') nav.stations++; else if (it.kind === 'navwarn') nav.warnings++; else if (it.kind === 'navmet') nav.weather++;
      // ★★ ONE ITEM, ONE PILL (Stuart 11-25: the pills are the BREAKDOWN of the status line's totals): an aircraft heard on
      //    ADS-B and VDL2 counts once, under the source it is DRAWN in — the first of its sources switched on (ADS-B, ACARS,
      //    VDL2, HFDL: iconOf's colour) — so the aviation pills add up to the aircraft on the map.
      if (it.kind === 'dsc') continue;   // ★ counted below: by MMSI, with the ships
      const m = primaryMode(it, f.off);
      if (m in by) by[m]++;
    }
    // ★★ A SHIP HEARD ON AIS AND DSC IS ONE SHIP: the DSC pill counts the MMSIs AIS has not drawn (AIS's position is the
    //    current one), each MMSI once however many calls — so AIS + DSC is the status line's combined ships
    const sh = shipsCombined(filter(items, all, now), !(f.off && f.off.has('ais')));
    by.dsc = sh.dscOnly;
    return { by, nav };
  }
  /** ★ The ships on the map, AIS and DSC COMBINED by MMSI (a DSC call with no MMSI is its own). `aisOn`: AIS's ships count. */
  function shipsCombined(items, aisOn = true) {
    const ais = new Set(), dsc = new Set();
    for (const it of items) {
      if (it.kind === 'ship' && aisOn) ais.add(it.d.mmsi);
      else if (it.kind === 'dsc') dsc.add(it.d.mmsi || it.key);
    }
    let dscOnly = 0;
    for (const k of dsc) if (!ais.has(k)) dscOnly++;
    return { ships: ais.size + dscOnly, ais: ais.size, dscOnly, both: dsc.size - dscOnly };
  }
  /** The modes somebody shares: from the contributors' layers and what actually arrived. */
  function modesPresent(servers, items) {
    const s = new Set();
    for (const v of servers) for (const l of serverLayers(v)) if (MODE[l]) s.add(l);
    for (const it of items) for (const m of it.modes) s.add(m);
    return MODES.filter((m) => s.has(m.id)).map((m) => m.id);
  }
  /** What a server shares: its listing's words (its front door's radio) and what each of its radios said it shares. */
  function serverLayers(v) {
    const s = new Set(v.layers || []);
    for (const r of v._rx || []) for (const l of r.layers || []) s.add(l);
    return [...s].filter((l) => MODE[l]);
  }
  /** NAVTEX's own filters, from what arrived: the message types (B2) and the stations named. */
  function navtexFacets(items) {
    const types = new Set(), stations = new Set();
    for (const it of items) if (it.modes[0] === 'navtex') {
      if (it.b2) types.add(it.b2);
      if (it.kind === 'navst') stations.add(it.d.name); else if (it.station) stations.add(it.station);
    }
    return { types: [...types].sort(), stations: [...stations].sort() };
  }
  /** How many items each receiver is providing. */
  function perServer(items) {
    const n = new Map();
    for (const it of items) for (const h of it.heard) n.set(h.sid, (n.get(h.sid) || 0) + 1);
    return n;
  }

  // ── addresses: the directory's own rules ───────────────────────────────────────────────────────────────────────────
  const ADDRESS_OK = /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.vibeserver\.vibesdr\.net$/;
  /** The LISTEN link — the directory's listenUrl(): the slug page where there is one, else the server's own address. */
  function listenUrl(s) {
    if (s && typeof s.address === 'string' && ADDRESS_OK.test(s.address)) return `https://${s.address}/`;
    return s && typeof s.url === 'string' && /^https:\/\/[A-Za-z0-9.-]+(:\d{1,5})?$/.test(s.url) ? s.url + '/' : '';
  }
  /** ★★ The DATA address: the server's own https URL, never the slug (that is the directory's Worker). On an http page (a
   *  local test) an http URL is allowed too. `path` defaults to the summary. */
  function dataBase(s, pageProto) {
    const u = s && typeof s.url === 'string' ? s.url.replace(/\/$/, '') : '';
    const ok = /^https:\/\/[A-Za-z0-9.-]+(:\d{1,5})?$/.test(u) || (pageProto === 'http:' && /^http:\/\/[A-Za-z0-9.-]+(:\d{1,5})?$/.test(u));
    return ok ? u : '';
  }
  const RID_OK = /^[A-Za-z0-9_-]{1,24}$/;
  /** ★ `rid`: one radio of a multi-radio server — its own /r/<id>/ on the SAME tunnel (the front door answers for one radio). */
  const radioPath = (rid) => (rid && RID_OK.test(rid) ? '/r/' + encodeURIComponent(rid) : '');
  function dataUrl(s, pageProto, layers, rid) {
    const b = dataBase(s, pageProto);
    if (!b) return '';
    return b + radioPath(rid) + '/vibeserver/community.json' + (layers && layers.length ? '?s=' + layers.filter((l) => MODE[l]).join(',') : '');
  }
  function textUrl(s, pageProto, f, rid) {
    const b = dataBase(s, pageProto);
    return b && /^[A-Za-z0-9._ -]{1,128}$/.test(f) ? b + radioPath(rid) + '/vibeserver/community/navtex?f=' + encodeURIComponent(f) : '';
  }
  /** ★★ WHAT TO ASK, PER RADIO. A single-radio server (no `radios` in its listing): ONE target, the server itself, exactly as
   *  before. A multi-radio one: a target per radio (kept across list reloads — each with its OWN floor and back-off), asked at
   *  /r/<id>/. A radio not yet heard from is asked for every layer switched on (`want`: the listing names the front door's
   *  only); after that, only what its answer said it shares. */
  function targets(s) {
    const ids = Array.isArray(s.radios) ? s.radios.filter((x) => typeof x === 'string' && RID_OK.test(x)).slice(0, 8) : [];
    if (ids.length < 2) { s._rx = null; return [s]; }
    const old = new Map((s._rx || []).map((r) => [r.rid, r]));
    s._rx = ids.map((rid) => old.get(rid) || { rid, layers: null });
    return s._rx;
  }
  /** The layers target `t` (of server `s`) should be asked for now, out of `want` (every layer switched on). */
  function askFor(t, s, want, nowMs) {
    const can = t === s ? want.filter((l) => (s.layers || []).includes(l)) : t.layers ? want.filter((l) => t.layers.includes(l)) : want;
    return dueLayers(t, can, nowMs);
  }
  /** ★ The directory's state() words for a receiver. */
  function stateOf(s) {
    if (s.unreachable) return { cls: 'down', label: 'not responding' };
    if (s.maxListeners > 0 && s.listeners >= s.maxListeners) return { cls: 'busy', label: 'no free slot' };
    if (s.temporary) return { cls: 'temp', label: 'temporary share' };
    return { cls: 'free', label: 'free slot' };
  }

  // ── the polling rule ─────────────────────────────────────────────────────────────────────────────────────────────────
  /* ★★★ EVERY POLLED FETCH HAS A FLOOR (Stuart 11-24/25). While the tab is visible: the live layers every FAST_SEC, the slow
   *     ones (NAVTEX, NDB, VOR, sondes) every SLOW_SEC — each request asking only for the layers switched on; nothing while
   *     the tab is hidden. A layer switched on asks for itself at once. A receiver that fails is left alone RETRY_SEC,
   *     doubling to RETRY_MAX_SEC. At most CONCURRENT requests in flight; each gives up after FETCH_TIMEOUT_MS. */
  const FAST_SEC = 60, SLOW_SEC = 300, RETRY_SEC = 300, RETRY_MAX_SEC = 1800, LIST_SEC = 600, FETCH_TIMEOUT_MS = 8000, CONCURRENT = 6;
  /** What receiver `s` should be asked now: the fast and the slow layers due, out of `on` (the layers switched on). */
  function dueLayers(s, on, nowMs) {
    if (s._retryAt > nowMs) return [];
    const fast = on.filter((l) => !MODE[l].slow), slow = on.filter((l) => MODE[l].slow);
    const want = [];
    if (fast.length && !(s._fastAt > nowMs - FAST_SEC * 1000)) want.push(...fast);
    if (slow.length && !(s._slowAt > nowMs - SLOW_SEC * 1000)) want.push(...slow);
    return want;
  }
  function noteResult(s, ok, nowMs, layers) {
    if (ok) {
      s._fails = 0; s.unreachable = false; s._retryAt = 0;
      if (layers.some((l) => !MODE[l].slow)) s._fastAt = nowMs;
      if (layers.some((l) => MODE[l].slow)) s._slowAt = nowMs;
    } else {
      s._fails = (s._fails || 0) + 1; s.unreachable = true;
      s._retryAt = nowMs + Math.min(RETRY_MAX_SEC, RETRY_SEC * 2 ** (s._fails - 1)) * 1000;
    }
  }
  /** The sections a layer brings (so an answer for some layers replaces only those). */
  const SECTIONS = { a: ['a'], s: ['s'], d: ['d'], b: ['b'], o: ['o'], z: ['z'], p: ['p'], m: ['m'], n: ['ns', 'nw', 'nm', 'nx'] };
  const sectionsOf = (layers) => [...new Set(layers.flatMap((l) => SECTIONS[MODE[l].sec] || []))];

  // ── words ──────────────────────────────────────────────────────────────────────────────────────────────────────────
  function ago(sec) {
    sec = Math.max(0, Math.round(sec));
    if (sec < 60) return sec + ' s';
    if (sec < 3600) return Math.round(sec / 60) + ' min';
    if (sec < 86400) return (sec / 3600).toFixed(sec < 36000 ? 1 : 0) + ' h';
    return Math.round(sec / 86400) + ' d';
  }
  /** ★ Old things fade, as the servers' own maps do: full for the first fifth of the mode's window, down to 35 % at its end. */
  function fade(it, now) {
    const m = MODE[it.modes[0]];
    if (!m) return 1;
    const a = Math.max(0, now - it.t);
    // ★ Aircraft (Stuart 11-26: datalink positions "hold them for 30 … faded to show older"): full strength under 5 min,
    //   fading to 35 % at 30 min — by the POSITION's age, whichever source gave it (the server keeps ADS-B 5 min, datalink 30).
    if (it.kind === 'aircraft') return a <= 300 ? 1 : Math.max(0.35, 1 - 0.65 * (a - 300) / 1500);
    const w = m.id === 'adsb' ? ADSB_STALE_SEC * 5 : m.win;
    return a <= w / 5 ? 1 : Math.max(0.35, 1 - 0.65 * (a - w / 5) / (w * 0.8));
  }
  // ── the SEA AREAS (Stuart 11-25: "can we draw all the shipping areas onto it like we do for navtex?") ─────────────────
  /* ★ The server's NAVTEX / shipping map's own layer (V12 src/services/seaAreas.ts), its data one static file beside the page
   *   (community-sea-areas.json — directory/scripts/build-sea-areas.mjs, from V12's seaAreas.generated.ts), fetched ONCE, only
   *   while drawn. The same look: a thin outline, the name in small capitals at its label point; tinted AMBER at force 7 and
   *   RED at a gale while a current forecast for it is on the map — and then no name of its own (the forecast's arrow names
   *   it). DECLUTTER as the server's: names before outlines. ★ The zooms are the server map's Leaflet zooms less one
   *   (MapLibre's 512 px tiles): a UK / offshore area's name from 3 / 4, a coastal zone's outline from 5 and its name from 7. */
  const SEA = {
    url: '/community-sea-areas.json',
    colours: { line: '#8fb4d4', amber: '#ffb74d', red: '#ff5a4a', name: '#a9c7e2', halo: '#04101c' },
    nameZoom: { s: 3, o: 4, c: 7 }, lineZoom: { s: 0, o: 2, c: 5 },
  };
  /** seaAreas.ts seaSeverity: 2 = a gale (8+, forecast or warned), 1 = force 7, 0 = no tint. */
  function seaSeverity(f) {
    if (!f) return 0;
    if (f.gale >= 8 || f.fmax >= 8 || (f.wind && (f.wind.sev === 2 || f.wind.sev2 === 2))) return 2;
    if (f.fmax === 7 || (f.wind && (f.wind.sev === 1 || f.wind.sev2 === 1))) return 1;
    return 0;
  }
  /** ★ From the items SHOWN: the forecasts drawn (area id → severity). Each such area loses its own name — the forecast's
   *  arrow label names it, so the name is never on the map twice — and is tinted by its forecast; a stale one (over a day
   *  old, the NAVTEX map's rule: never shown as the conditions now) tints nothing (0). */
  function seaForecasts(shown) {
    const by = new Map();
    for (const it of shown) if (it.kind === 'navmet' && it.d && it.d.fc && it.d.fc.length) by.set(it.d.id, it.d.stale ? 0 : seaSeverity(it.d.fc[0]));
    return by;
  }
  /** SEA AREAS is drawn: MARITIME on, its row present (somebody shares a maritime mode — the toggle lives there), and the
   *  viewer has not switched it off (ON by default). */
  const seaShown = (groups, present, pref) => pref !== false && groups.has('maritime') && present.some((m) => MODE[m] && MODE[m].group === 'maritime');
  /** The map's credit line for the file's sources (seaAreas.ts seaAttribution, one "Sea areas:" however many sources). */
  function seaAttribution(j) {
    const a = j && j.attribution && typeof j.attribution === 'object' ? Object.values(j.attribution).filter((x) => typeof x === 'string') : [];
    return a.length ? 'Sea areas: ' + a.map((x) => x.replace(/^Sea areas:\s*/, '')).join(' · ') : '';
  }

  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  const api = { GROUPS, MODES, MODE, AGES, ADSB_STALE_SEC, FAST_SEC, SLOW_SEC, RETRY_SEC, RETRY_MAX_SEC, LIST_SEC, FETCH_TIMEOUT_MS, CONCURRENT,
                itemsOf, merge, mergeRadios, modeCounts, primaryMode, shipsCombined, serverLayers, targets, askFor, textSource, filter, modesPresent, navtexFacets, perServer, listenUrl, dataBase, dataUrl, textUrl, stateOf,
                dueLayers, noteResult, sectionsOf, ago, fade, esc, modeTip, ADDRESS_OK, SEA, seaSeverity, seaForecasts, seaShown, seaAttribution };
  root.CommunityMap = api;
  if (typeof module === 'object' && module && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
