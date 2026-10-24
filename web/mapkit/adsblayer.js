/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * adsblayer.js — THE ADS-B AIRCRAFT LAYER, in ONE place: the server's web client and the app's map
 * WebView draw the same aircraft, the same glide and the same three toggles from this file.
 *
 * ★★★ THE HFDL MAP IS THE REFERENCE FOR FEEL. Stuart, 2026-10-24: "Take inspiration from our HFDL map
 *     as that is ASMR grade sublime, I could sit and watch that all day." So the constants below are
 *     the HFDL map's, not new ones: the 2.2 s glide (vibemapgl-compat.js GLIDE_MS), the aircraft's
 *     fuselage-and-swept-wing silhouette and its age tones (MapOverlay.tsx acSVG / acTone), the 1.4 s
 *     ring ×3 and the 1.2 s breathing glow (.ring / .pglow), the dashed amber range rings of the spots
 *     map, the receiver's little house, the dark monospace aircraft card. Where ADS-B needs something
 *     HFDL never did (a fix every second, an auto-zoom), it is built to the same calm: slow eased
 *     moves, nothing snaps, nothing flickers, colour only where it means something.
 *
 * ★★ PLAIN JAVASCRIPT, NO BUILD STEP — the same rule as vibemap.js: the web client carries it as a
 *    string into its map window, the app injects it into its WebView, the server serves it at
 *    /mapgl/vendor/adsblayer.js. scripts/gen-adsblayer-source.mjs writes those copies. ✗ Never edit
 *    a copy.
 *
 * USE (the contract — docs/v12/ADSB.md "The shared map layer"):
 *     const layer = VibeAdsbLayer.create(map, { receiver: {lat, lon} | null,
 *                                               prefs: {autoZoom, fit, rings} | null,
 *                                               onPrefs: (prefs) => void, theme: 'dark' | 'light',
 *                                               drawReceiver: true });   // false = the host draws its own
 *     layer.update(aircraftArray)    // ADSB.md aircraft objects, the server's whole list each time
 *     layer.setReceiver(lat, lon)
 *     layer.setPrefs({autoZoom, fit, rings})
 *     layer.focus(hex)               // select an aircraft and open its card (a list row was tapped)
 *     layer.destroy()
 *
 *   map — a MapLibre map (VibeMapGL's vm.map), the Leaflet-shaped adapter vibemapgl-compat.js
 *           installs (its ._gl is used), or a real Leaflet map (the no-WebGL-2 fallback: DOM markers,
 *           a lower frame rate, the same behaviour). The HOST creates the map with the 'aero' profile
 *           (VibeMapGL createNow profile:'aero', or VibeMap.attach profile:'aero') — that is where the
 *           airports and runways come from, exactly as on the HFDL map. This layer draws only aircraft.
 *
 * ★ THE PURE MATHS is VibeAdsbLayer.maths — no DOM, no map — so scripts/test_adsb_layer.mjs can check
 *   the fit, the auto-zoom decisions, the ring spacing, the dead-reckoning and the icon table in node.
 *
 * ✗ NO BACKTICKS IN THIS FILE. The web client embeds it with JSON.stringify (safe), but the app's page
 *   is a template literal and a backtick anywhere in an injected source has broken a build before.
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
(function (global) {
  'use strict';

  /* ══ CONSTANTS — the HFDL map's, named where they came from ═════════════════════════════════════ */
  /** ★ vibemapgl-compat.js GLIDE_MS: how long a correction takes to ease out. The HFDL map glides a
   *  marker to each new fix over this long; here the fixes come every second, so it is the time a
   *  DIFFERENCE between where we guessed and where the aircraft said it was is spread over. */
  const GLIDE_MS = 2200;
  /** ★ The HFDL .ring: 1.4 s, ease-out, three times — the new-aircraft ring. */
  const RING_MS = 1400, RING_REPEATS = 3;
  /** ★ The HFDL .pglow: a 1.2 s breath, faint at the ends, full in the middle. */
  const BREATHE_MS = 1200;
  /** ★ The camera's ease: as long as a glide, so the view and the aircraft move to one rhythm. */
  const CAMERA_MS = GLIDE_MS;
  /** Dead-reckon at most this far past a fix. The server keeps a position up to 60 s; guessing a
   *  straight line for a whole minute would fly an aircraft through its own turn. Past this it holds. */
  const MAX_DR_S = 20;
  /** A correction bigger than this is not a correction, it is a different answer (a bad CPR decode, or
   *  an aircraft back after a gap): the icon fades across instead of sliding miles over the map. */
  const SNAP_NM = 5;
  /** ★ No animation for aircraft that were already there: the first batch, and everything that turns up
   *  in the first seconds after the mode starts (positions arrive over the first few CPR pairs). */
  const SETTLE_MS = 10000;
  const FADE_IN_MS = 600, FADE_OUT_MS = 900;
  /** ★ MapOverlay.tsx AC_GLIDE_MAX: past this many moving aircraft, honest steps beat a stutter. Here
   *  that means a lower frame rate for the moving layer rather than none — see frameGap(). */
  const AC_GLIDE_MAX = 220;
  /** Transition altitude for flight levels — AircraftPanel.tsx's rule (ONE RULE, TWO READERS). */
  const TRANSITION_FT = 18000;
  const ICON_BOX = 40;               // CSS px, every icon drawn in the same square, nose up
  const LABEL_EVERY_MS = 500;        // label decluttering is decided twice a second, never per frame
  const LABEL_MAX = 60;

  /* ══ PURE MATHS ═══════════════════════════════════════════════════════════════════════════════ */
  const NM_M = 1852, RE_M = 6371008.8, D2R = Math.PI / 180;
  const EARTH_CIRC_M = 40075016.686;

  function wrapLon(lon, ref) {
    let d = lon - (ref || 0);
    d = ((d + 180) % 360 + 360) % 360 - 180;
    return (ref || 0) + d;
  }
  function distNm(aLat, aLon, bLat, bLon) {
    const p1 = aLat * D2R, p2 = bLat * D2R, dp = (bLat - aLat) * D2R, dl = (bLon - aLon) * D2R;
    const h = Math.sin(dp / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
    return 2 * RE_M * Math.asin(Math.min(1, Math.sqrt(h))) / NM_M;
  }
  /** Initial great-circle bearing a → b, degrees true, 0–360. */
  function bearingDeg(aLat, aLon, bLat, bLon) {
    const p1 = aLat * D2R, p2 = bLat * D2R, dl = (bLon - aLon) * D2R;
    const y = Math.sin(dl) * Math.cos(p2);
    const x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
    return (Math.atan2(y, x) / D2R + 360) % 360;
  }
  function destination(lat, lon, brgDeg, nm) {
    const d = nm * NM_M / RE_M, b = brgDeg * D2R, p1 = lat * D2R, l1 = lon * D2R;
    const p2 = Math.asin(Math.sin(p1) * Math.cos(d) + Math.cos(p1) * Math.sin(d) * Math.cos(b));
    const l2 = l1 + Math.atan2(Math.sin(b) * Math.sin(d) * Math.cos(p1), Math.cos(d) - Math.sin(p1) * Math.sin(p2));
    return { lat: p2 / D2R, lon: wrapLon(l2 / D2R, lon) };
  }
  const CARD16 = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
  function cardinal(deg) { return CARD16[Math.round((((deg % 360) + 360) % 360) / 22.5) % 16]; }

  /** Where a fix would be at time t (ms, the same clock as fix.t) on its ground speed and track.
   *  ★ Capped at MAX_DR_S, then it holds — and nothing moves without a speed and a track. */
  function deadReckon(fix, t) {
    if (!(fix.gs > 0) || fix.track == null || !isFinite(fix.track)) return { lat: fix.lat, lon: fix.lon };
    const dt = Math.max(0, Math.min(MAX_DR_S, (t - fix.t) / 1000));
    if (dt === 0) return { lat: fix.lat, lon: fix.lon };
    return destination(fix.lat, fix.lon, fix.track, fix.gs * dt / 3600);
  }

  /** ★ DO-260B emitter category → icon (docs/v12/ADSB.md). A0 / none / anything unknown = the generic
   *  jet: most of the sky that does not say what it is, is an airliner. */
  const CAT_ICON = { A1: 'light', A2: 'regional', A3: 'jet', A4: 'b757', A5: 'heavy', A6: 'fighter',
                     A7: 'heli', B1: 'glider', B2: 'balloon', B3: 'parachute', B4: 'ultralight',
                     B6: 'uav', B7: 'space', C1: 'ground', C2: 'ground', C3: 'obstacle' };
  const CAT_NAME = { A1: 'Light aircraft', A2: 'Small aircraft', A3: 'Large aircraft', A4: 'High-vortex large (B757)',
                     A5: 'Heavy', A6: 'High performance', A7: 'Rotorcraft', B1: 'Glider', B2: 'Lighter than air',
                     B3: 'Parachutist', B4: 'Ultralight', B6: 'Unmanned aerial vehicle', B7: 'Space vehicle',
                     C1: 'Emergency vehicle', C2: 'Service vehicle', C3: 'Obstacle' };
  function iconFor(category) {
    const c = typeof category === 'string' ? category.trim().toUpperCase() : '';
    return CAT_ICON[c] || 'jet';
  }

  function onGround(a) { return a && a.alt_baro === 'ground'; }
  /** Flight levels above the transition altitude, feet below — what you would hear on air. */
  function altText(alt) {
    if (alt === 'ground') return 'GND';
    const ft = Number(alt);
    if (alt == null || alt === '' || !isFinite(ft)) return '—';
    if (ft >= TRANSITION_FT) return 'FL' + String(Math.round(ft / 100)).padStart(3, '0');
    return (Math.round(ft / 100) * 100).toLocaleString('en-GB') + ' ft';
  }
  /** The short form for a map label: FL350, 3400, GND. */
  function altShort(alt) {
    if (alt === 'ground') return 'GND';
    const ft = Number(alt);
    if (alt == null || alt === '' || !isFinite(ft)) return '';
    return ft >= TRANSITION_FT ? 'FL' + String(Math.round(ft / 100)).padStart(3, '0') : String(Math.round(ft / 100) * 100);
  }
  function vsText(rate) {
    const r = Number(rate);
    if (rate == null || !isFinite(r)) return '';
    if (Math.abs(r) < 100) return 'level';
    return (r > 0 ? '↑ +' : '↓ −') + Math.abs(Math.round(r / 64) * 64).toLocaleString('en-GB') + ' ft/min';
  }
  function emergencyOf(a) {
    const e = a && a.emergency;
    const sq = a && String(a.squawk || '');
    if (sq === '7500') return 'HIJACK 7500';
    if (sq === '7600') return 'RADIO FAILURE 7600';
    if (sq === '7700') return 'EMERGENCY 7700';
    if (e && e !== 'none' && e !== 0 && e !== '0') return 'EMERGENCY';
    return '';
  }

  /** Nautical miles per CSS pixel at a latitude and a zoom. tileSize 512 = MapLibre, 256 = Leaflet. */
  function nmPerPx(lat, zoom, tileSize) {
    return EARTH_CIRC_M * Math.cos(lat * D2R) / ((tileSize || 512) * Math.pow(2, zoom)) / NM_M;
  }
  /** ★ A ring every N nautical miles, N from a radar's own ladder, chosen so the rings sit about
   *  targetPx apart on screen. Log-nearest, so the step changes at the geometric midpoints. */
  const RING_LADDER = [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000];
  function ringStep(npp, targetPx) {
    const want = npp * (targetPx || 110);
    let best = RING_LADDER[0], bd = Infinity;
    for (const v of RING_LADDER) { const d = Math.abs(Math.log(v / want)); if (d < bd) { bd = d; best = v; } }
    return best;
  }

  /** Web Mercator in world units 0..1 (x east, y south). */
  function merc(lat, lon) {
    const s = Math.sin(Math.max(-85.05, Math.min(85.05, lat)) * D2R);
    return { x: (lon + 180) / 360, y: 0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI) };
  }
  function unmerc(x, y) {
    return { lat: Math.atan(Math.sinh(Math.PI * (1 - 2 * y))) / D2R, lon: x * 360 - 180 };
  }

  /** ★★★ THE VIEW THAT JUST CONTAINS EVERY AIRCRAFT.
   *   fit:true (or no receiver) — the bounding box of the receiver AND the aircraft, centred on the BOX:
   *     traffic mostly to the south-east puts the receiver towards the top-left, and the zoom is as close
   *     as that box allows.
   *   fit:false — the receiver dead centre, zoomed just far enough out for the furthest aircraft on
   *     either axis.
   *  o = { points:[{lat,lon}], rx:{lat,lon}|null, fit, w, h, pad, tileSize, minZoom, maxZoom, emptyZoom }.
   *  Returns { center:{lat,lon}, zoom } or null when there is nothing to look at. */
  function cameraFor(o) {
    const ts = o.tileSize || 512, pad = o.pad != null ? o.pad : 48;
    const minZ = o.minZoom != null ? o.minZoom : 2, maxZ = o.maxZoom != null ? o.maxZoom : 10;
    const pts = o.points || [], rx = o.rx || null;
    const clampZ = (z) => Math.max(minZ, Math.min(maxZ, z));
    if (!pts.length) {
      if (!rx) return null;
      return { center: { lat: rx.lat, lon: rx.lon }, zoom: clampZ(o.emptyZoom != null ? o.emptyZoom : maxZ - 2) };
    }
    const ref = rx ? rx.lon : pts[0].lon;
    const ms = pts.map((p) => merc(p.lat, wrapLon(p.lon, ref)));
    const availW = Math.max(40, o.w - 2 * pad), availH = Math.max(40, o.h - 2 * pad);
    if (rx && !o.fit) {
      const c = merc(rx.lat, rx.lon);
      let hx = 0, hy = 0;
      for (const m of ms) { hx = Math.max(hx, Math.abs(m.x - c.x)); hy = Math.max(hy, Math.abs(m.y - c.y)); }
      const zx = hx > 0 ? Math.log2(availW / 2 / (hx * ts)) : Infinity;
      const zy = hy > 0 ? Math.log2(availH / 2 / (hy * ts)) : Infinity;
      return { center: { lat: rx.lat, lon: rx.lon }, zoom: clampZ(Math.min(zx, zy)) };
    }
    if (rx) ms.push(merc(rx.lat, rx.lon));
    let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
    for (const m of ms) { x0 = Math.min(x0, m.x); x1 = Math.max(x1, m.x); y0 = Math.min(y0, m.y); y1 = Math.max(y1, m.y); }
    const dx = x1 - x0, dy = y1 - y0;
    const zx = dx > 0 ? Math.log2(availW / (dx * ts)) : Infinity;
    const zy = dy > 0 ? Math.log2(availH / (dy * ts)) : Infinity;
    const c = unmerc((x0 + x1) / 2, (y0 + y1) / 2);
    return { center: { lat: c.lat, lon: wrapLon(c.lon, ref) }, zoom: clampZ(Math.min(zx, zy)) };
  }

  /** Screen offset (CSS px) of a point from a camera's centre. */
  function screenOffset(cam, lat, lon, tileSize) {
    const ws = (tileSize || 512) * Math.pow(2, cam.zoom);
    const c = merc(cam.center.lat, cam.center.lon), p = merc(lat, wrapLon(lon, cam.center.lon));
    return { x: (p.x - c.x) * ws, y: (p.y - c.y) * ws };
  }
  /** Is any point outside the view cam would show, less a margin of pad px? */
  function anyOutside(points, cam, w, h, pad, tileSize) {
    const hw = w / 2 - pad, hh = h / 2 - pad;
    for (const p of points) {
      const s = screenOffset(cam, p.lat, p.lon, tileSize);
      if (Math.abs(s.x) > hw || Math.abs(s.y) > hh) return true;
    }
    return false;
  }

  /** ★★★ AUTO-ZOOM, THE DECISIONS — calm by construction, never per frame.
   *   OUT at once when an aircraft leaves the view (a new one 200 NM out pulls the view out), and to a
   *     little FURTHER than strictly needed (slack), so the next second's drift does not ask again.
   *   IN only when the view is clearly too wide (inHyst zoom levels) and has stayed so for inHold:
   *     an aircraft dropping off brings the view back in to the next furthest, once, not in steps.
   *   RE-CENTRE (fit mode) when the box's centre has wandered centreFrac of the screen for centreHold.
   *   Never twice inside minGap, never while a move is still running (busy).
   *  decide(now, cur, target, outside, w, h, tileSize, busy) → null | { center, zoom } */
  function AutoZoom(o) {
    o = o || {};
    this.slack = o.slack != null ? o.slack : 0.3;
    this.inHyst = o.inHyst != null ? o.inHyst : 0.6;
    this.inHold = o.inHold != null ? o.inHold : 4000;
    this.centreFrac = o.centreFrac != null ? o.centreFrac : 0.12;
    this.centreHold = o.centreHold != null ? o.centreHold : 3000;
    this.minGap = o.minGap != null ? o.minGap : 1500;
    this.reset();
  }
  AutoZoom.prototype.reset = function () { this.inSince = null; this.centreSince = null; this.lastMoveAt = -Infinity; };
  AutoZoom.prototype.decide = function (now, cur, tgt, outside, w, h, tileSize, busy) {
    if (!tgt || !cur) return null;
    if (busy || now - this.lastMoveAt < this.minGap) return null;
    const self = this;
    const goal = { center: tgt.center, zoom: tgt.zoom - this.slack };
    const go = (g) => { self.lastMoveAt = now; self.inSince = null; self.centreSince = null; return g; };
    if (outside || tgt.zoom < cur.zoom - 0.02) return go(goal);
    if (tgt.zoom > cur.zoom + this.inHyst) {
      if (this.inSince == null) this.inSince = now;
      if (now - this.inSince >= this.inHold) return go(goal);
    } else this.inSince = null;
    const off = screenOffset(cur, tgt.center.lat, tgt.center.lon, tileSize);
    if (Math.hypot(off.x, off.y) > this.centreFrac * Math.min(w, h)) {
      if (this.centreSince == null) this.centreSince = now;
      if (now - this.centreSince >= this.centreHold) return go({ center: tgt.center, zoom: cur.zoom });
    } else this.centreSince = null;
    return null;
  };

  /** ★★ LABELS THAT DO NOT CLUTTER, AND DO NOT FLICKER. MapLibre's own collision runs every frame, so a
   *  moving label pops in and out as its neighbours slide past. Decided here instead, twice a second:
   *  a label sits right of its icon and is kept only if it overlaps no kept label and no OTHER
   *  aircraft. Labels already shown go first (sticky), then by priority (lower = more important).
   *  items: [{ id, x, y, w, h, pri }] in CSS px (x,y = the icon's centre). Returns a Set of ids. */
  function placeLabels(items, prev, vw, vh, max) {
    const ICON_HALF = 9, GAP = 12;
    const order = items.slice().sort((a, b) => {
      const pa = prev && prev.has(a.id) ? 0 : 1, pb = prev && prev.has(b.id) ? 0 : 1;
      return pa - pb || a.pri - b.pri;
    });
    const kept = [], out = new Set();
    for (const it of order) {
      if (out.size >= (max || LABEL_MAX)) break;
      const box = { x0: it.x + GAP, x1: it.x + GAP + it.w, y0: it.y - it.h / 2, y1: it.y + it.h / 2 };
      if (box.x1 > vw || box.y0 < 0 || box.y1 > vh || it.x < 0) continue;
      let hit = false;
      for (const k of kept) if (box.x0 < k.x1 && box.x1 > k.x0 && box.y0 < k.y1 && box.y1 > k.y0) { hit = true; break; }
      if (!hit) for (const o of items) {
        if (o.id === it.id) continue;
        if (o.x + ICON_HALF > box.x0 && o.x - ICON_HALF < box.x1 && o.y + ICON_HALF > box.y0 && o.y - ICON_HALF < box.y1) { hit = true; break; }
      }
      if (hit) continue;
      kept.push(box); out.add(it.id);
    }
    return out;
  }

  /** Correction easing: smooth at both ends, so a corrected aircraft neither lurches off nor stops dead. */
  function easeInOut(k) { return k < 0.5 ? 4 * k * k * k : 1 - Math.pow(-2 * k + 2, 3) / 2; }
  function easeOut(k) { return 1 - Math.pow(1 - k, 3); }
  /** The HFDL pglow curve: 0.15 at the ends of each breath, 1 in the middle. */
  function breath(tMs) { const k = (tMs % BREATHE_MS) / BREATHE_MS; return 0.15 + 0.85 * (0.5 - 0.5 * Math.cos(2 * Math.PI * k)); }
  function angleTo(from, to) { return ((to - from + 540) % 360) - 180; }

  /** The age tone — MapOverlay.tsx acTone's colours, judged on the POSITION's age (ADS-B drops an
   *  aircraft at 60 s, so its ladder is seconds, not HFDL's minutes). Ground is grey, an emergency red. */
  const TONES = {
    dark:  { fresh: '80,255,120', aging: '180,255,80', stale: '255,190,40', ground: '150,160,170', alert: '255,80,60' },
    light: { fresh: '20,140,60', aging: '110,150,20', stale: '190,120,0', ground: '90,100,110', alert: '210,40,30' },
  };
  function toneKey(a) {
    if (emergencyOf(a)) return 'alert';
    if (onGround(a)) return 'ground';
    const age = Number(a.seen_pos) || 0;
    return age < 10 ? 'fresh' : age < 30 ? 'aging' : 'stale';
  }

  const maths = { GLIDE_MS, RING_MS, RING_REPEATS, BREATHE_MS, MAX_DR_S, SNAP_NM, SETTLE_MS, TRANSITION_FT,
                  wrapLon, distNm, bearingDeg, destination, cardinal, deadReckon, iconFor, altText, altShort,
                  vsText, emergencyOf, nmPerPx, ringStep, RING_LADDER, merc, unmerc, cameraFor, screenOffset,
                  anyOutside, AutoZoom, placeLabels, easeInOut, easeOut, breath, angleTo, toneKey };

  /* ══ ICONS — one geometry, two renderers ═══════════════════════════════════════════════════════
   * Every icon is a list of parts in a 40×40 box, nose up (north), centre (20,20): f = filled path,
   * s = stroked path (width), o = opacity. The GPU map rasterises them with Path2D onto a canvas at
   * the screen's pixel ratio (crisp, synchronous — no image decode to wait for); the Leaflet fallback
   * writes the same paths as an SVG.
   * ★ The jets are the HFDL map's own silhouette (acSVG: fuselage ellipse, swept wings, tail), scaled by
   *   class, so a heavy reads bigger than a regional at a glance and the family looks like one map. */
  function ell(cx, cy, rx, ry) {
    return 'M' + cx + ',' + (cy - ry) + 'A' + rx + ',' + ry + ' 0 0,1 ' + cx + ',' + (cy + ry) + 'A' + rx + ',' + ry + ' 0 0,1 ' + cx + ',' + (cy - ry) + 'Z';
  }
  function poly(pts) { return 'M' + pts.map((p) => p[0] + ',' + p[1]).join('L') + 'Z'; }
  /** The HFDL airliner, scaled k about the centre (HFDL drew it in 26 px with its centre at 13,13). */
  function hfdlJet(k, opts) {
    opts = opts || {};
    const S = (x, y) => [+(20 + (x - 13) * k).toFixed(2), +(20 + (y - 13) * k).toFixed(2)];
    const fusRx = (opts.slim ? 2.0 : 2.5) * k, fusRy = (opts.long ? 10 : 9) * k;
    const parts = [
      { f: ell(20, 20, +fusRx.toFixed(2), +fusRy.toFixed(2)) },
      { f: poly([S(13, 10), S(24, 17), S(24, 19), S(13, 15), S(2, 19), S(2, 17)]) },
      { f: poly([S(13, 20), S(17, 24), S(17, 25), S(13, 23), S(9, 25), S(9, 24)]) },
    ];
    if (opts.engines) {
      for (const x of [6.5, 9.8, 16.2, 19.5]) {
        const p = S(x, 14.8 + Math.abs(x - 13) * 0.32);
        parts.push({ f: ell(p[0], p[1], +(0.9 * k).toFixed(2), +(1.5 * k).toFixed(2)) });
      }
    }
    return parts;
  }
  const ICONS = {
    /* ★ The A3 jet is the HFDL icon at its own 26 px; the classes step either side of it. Larger was tried
     *  first (×1.15) and London's stack read as a green blot on the bench recording (2026-10-24). */
    jet: hfdlJet(1.0),
    regional: hfdlJet(0.82),
    b757: hfdlJet(1.1, { slim: true, long: true }),
    heavy: hfdlJet(1.3, { engines: true }),
    light: [
      { f: ell(20, 21, 1.8, 7) },
      { f: 'M8,17.5H32V20.5H8Z' },
      { f: 'M15.5,26.5H24.5V28.5H15.5Z' },
      { s: 'M16.5,13.2H23.5', w: 1.4 },
    ],
    fighter: [
      { f: 'M20,5L23,17L31,27L31,29L22.5,27L21.5,31L18.5,31L17.5,27L9,29L9,27L17,17Z' },
    ],
    heli: [
      { f: ell(20, 19, 4, 5.2) },
      { f: 'M19.2,23H20.8V33H19.2Z' },
      { f: 'M16.5,32H23.5V33.8H16.5Z' },
      { s: 'M9,8L31,30M31,8L9,30', w: 1.5, o: 0.75 },
    ],
    glider: [
      { f: ell(20, 21, 1.4, 8) },
      { f: 'M3,18.2H37V20.2H3Z' },
      { f: 'M16,27.6H24V29H16Z' },
    ],
    balloon: [
      { f: ell(20, 15, 7.5, 8.5) },
      { s: 'M14,20L18.4,27M26,20L21.6,27', w: 1 },
      { f: 'M17.8,27H22.2V31H17.8Z' },
    ],
    parachute: [
      { f: 'M8,17C8,8 32,8 32,17C28,15 24,15 20,17C16,15 12,15 8,17Z' },
      { s: 'M9,17L20,28M31,17L20,28M20,17V28', w: 0.9, o: 0.8 },
      { f: ell(20, 29.5, 1.8, 1.8) },
    ],
    ultralight: [
      { f: 'M20,10L35,23L33,24.5L20,20L7,24.5L5,23Z' },
      { f: ell(20, 24, 1.6, 4) },
    ],
    uav: [
      { s: 'M12,12L28,28M28,12L12,28', w: 2 },
      { f: ell(12, 12, 3.4, 3.4), o: 0.8 }, { f: ell(28, 12, 3.4, 3.4), o: 0.8 },
      { f: ell(12, 28, 3.4, 3.4), o: 0.8 }, { f: ell(28, 28, 3.4, 3.4), o: 0.8 },
      { f: ell(20, 20, 3, 3) },
    ],
    space: [
      { f: 'M20,4C23,9 23.5,14 23.5,24L28,32H12L16.5,24C16.5,14 17,9 20,4Z' },
    ],
    ground: [
      { f: 'M15,12Q15,10 17,10H23Q25,10 25,12V29Q25,31 23,31H17Q15,31 15,29Z' },
      { f: 'M16.5,13H23.5V16H16.5Z', dark: true },
    ],
    obstacle: [
      { f: 'M20,7L28,32H12Z' },
    ],
  };
  /** Icons that keep their own "up": a balloon or a mast has no nose to point. */
  const NO_ROTATE = { balloon: 1, parachute: 1, obstacle: 1 };

  function iconSvg(kind, rgb, alpha, rot) {
    const parts = ICONS[kind] || ICONS.jet;
    const c = 'rgba(' + rgb + ',' + alpha + ')';
    let s = '<svg xmlns="http://www.w3.org/2000/svg" width="' + ICON_BOX + '" height="' + ICON_BOX + '" viewBox="0 0 40 40">'
          + '<g transform="rotate(' + (NO_ROTATE[kind] ? 0 : (rot || 0).toFixed(1)) + ',20,20)">';
    for (const p of parts) {
      const op = p.o != null ? ' opacity="' + p.o + '"' : '';
      if (p.f) s += '<path d="' + p.f + '" fill="' + (p.dark ? 'rgba(0,0,0,0.55)' : c) + '" stroke="rgba(0,0,0,0.3)" stroke-width="0.6"' + op + '/>';
      else s += '<path d="' + p.s + '" fill="none" stroke="' + c + '" stroke-width="' + p.w + '" stroke-linecap="round"' + op + '/>';
    }
    return s + '</g></svg>';
  }
  /** Rasterise an icon for the GPU map. glow = the HFDL selection glow baked in (as compat's symImage). */
  function iconImage(kind, rgb, alpha, glow, dpr) {
    const parts = ICONS[kind] || ICONS.jet;
    const px = Math.ceil(ICON_BOX * dpr);
    const cv = document.createElement('canvas'); cv.width = px; cv.height = px;
    const x = cv.getContext('2d');
    x.scale(dpr, dpr);
    const c = 'rgba(' + rgb + ',' + alpha + ')';
    const draw = () => {
      for (const p of parts) {
        x.globalAlpha = p.o != null ? p.o : 1;
        if (p.f) {
          const path = new Path2D(p.f);
          x.fillStyle = p.dark ? 'rgba(0,0,0,0.55)' : c; x.fill(path);
          if (!glow) { x.lineWidth = 0.6; x.strokeStyle = 'rgba(0,0,0,0.3)'; x.stroke(path); }
        } else {
          x.lineWidth = p.w; x.lineCap = 'round'; x.strokeStyle = c; x.stroke(new Path2D(p.s));
        }
      }
      x.globalAlpha = 1;
    };
    if (glow) { x.shadowColor = 'rgba(' + rgb + ',0.85)'; x.shadowBlur = 5 * dpr; draw(); x.shadowBlur = 0; x.shadowColor = 'transparent'; }
    draw();
    return x.getImageData(0, 0, px, px);
  }

  const RX_SVG = '<svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 22 22"><polygon points="11,1 21,10 17,10 17,21 13,21 13,15 9,15 9,21 5,21 5,10 1,10" fill="rgba(255,200,60,0.85)" stroke="rgba(0,0,0,0.5)" stroke-width="1"/></svg>';

  /* ══ STYLE — the toggles and the card, injected once per theme ══════════════════════════════════ */
  const THEME = {
    dark:  { bg: '9,6,2', ink: '255,184,51', hi: '255,229,102', border: '255,160,0', ring: 'rgba(255,160,0,0.30)', ringLbl: 'rgba(255,184,51,0.6)',
             label: 'rgba(225,238,228,0.88)', halo: 'rgba(0,0,0,0.9)' },
    light: { bg: '250,247,240', ink: '120,78,0', hi: '60,40,0', border: '160,100,0', ring: 'rgba(150,90,0,0.40)', ringLbl: 'rgba(120,78,0,0.75)',
             label: 'rgba(30,30,30,0.9)', halo: 'rgba(255,255,255,0.95)' },
  };
  function css(th) {
    const T = THEME[th];
    const p = '.vsadsb-' + th;
    return p + '.vsadsb-ctl{position:absolute;top:10px;right:10px;z-index:5;display:flex;gap:0;'
      + 'background:rgba(' + T.bg + ',0.92);border:1px solid rgba(' + T.border + ',0.30);border-radius:8px;overflow:hidden;'
      + 'font:11px ui-monospace,"SF Mono",Menlo,Consolas,monospace;letter-spacing:1px;user-select:none;-webkit-user-select:none}'
      + p + ' button{all:unset;box-sizing:border-box;min-width:44px;height:32px;padding:0 9px;display:flex;align-items:center;justify-content:center;'
      + 'color:rgba(' + T.ink + ',0.45);cursor:pointer;border-left:1px solid rgba(' + T.border + ',0.18);-webkit-tap-highlight-color:transparent}'
      + p + ' button:first-child{border-left:none}'
      + p + ' button.on{color:rgba(' + T.hi + ',0.95);background:rgba(' + T.border + ',0.12)}'
      + p + ' button.paused{color:rgba(' + T.ink + ',0.75);background:repeating-linear-gradient(135deg,rgba(' + T.border + ',0.10) 0 4px,transparent 4px 8px)}'
      + p + ' button:disabled{opacity:0.35;cursor:default}'
      + p + ' button:focus-visible{outline:1px solid rgba(' + T.hi + ',0.8);outline-offset:-2px}'
      + '.vsadsb-pop-' + th + ' .maplibregl-popup-content,.vsadsb-pop-' + th + ' .leaflet-popup-content-wrapper{background:rgba(' + T.bg + ',0.96);'
      + 'border:1px solid rgba(' + T.border + ',0.30);color:rgba(' + T.ink + ',0.90);font:13px ui-monospace,"SF Mono",Menlo,Consolas,monospace;'
      + 'border-radius:8px;padding:10px 12px;box-shadow:0 6px 24px rgba(0,0,0,0.5)}'
      + '.vsadsb-pop-' + th + ' .leaflet-popup-content{margin:0}'
      + '.vsadsb-pop-' + th + ' .leaflet-popup-tip{background:rgba(' + T.bg + ',0.96)}'
      + '.vsadsb-pop-' + th + '.maplibregl-popup-anchor-bottom .maplibregl-popup-tip{border-top-color:rgba(' + T.bg + ',0.96)}'
      + '.vsadsb-pop-' + th + '.maplibregl-popup-anchor-top .maplibregl-popup-tip{border-bottom-color:rgba(' + T.bg + ',0.96)}'
      + '.vsadsb-pop-' + th + '.maplibregl-popup-anchor-left .maplibregl-popup-tip{border-right-color:rgba(' + T.bg + ',0.96)}'
      + '.vsadsb-pop-' + th + '.maplibregl-popup-anchor-right .maplibregl-popup-tip{border-left-color:rgba(' + T.bg + ',0.96)}'
      + '.vsadsb-pop-' + th + ' .maplibregl-popup-close-button,.vsadsb-pop-' + th + ' .leaflet-popup-close-button{color:rgba(' + T.ink + ',0.5);font-size:16px}'
      + '.vsadsb-pop-' + th + ' .t{font-size:15px;font-weight:600;color:rgba(' + T.hi + ',1);margin-bottom:6px;letter-spacing:0.8px}'
      + '.vsadsb-pop-' + th + ' .t i{font-style:normal;font-size:12px;font-weight:400;color:rgba(' + T.ink + ',0.65);margin-left:6px}'
      + '.vsadsb-pop-' + th + ' table{border-collapse:collapse}'
      + '.vsadsb-pop-' + th + ' td{padding:1px 0;vertical-align:top}'
      + '.vsadsb-pop-' + th + ' td:first-child{color:rgba(' + T.ink + ',0.65);padding-right:10px;white-space:nowrap}'
      + '.vsadsb-pop-' + th + ' td:last-child{color:rgba(' + T.ink + ',0.95)}'
      + '.vsadsb-pop-' + th + ' .em{color:#ff6a50;font-weight:600;letter-spacing:1px;margin-bottom:4px}'
      // ★ The Leaflet fallback's icons, labels and the HFDL ring (keyframes verbatim).
      + '.vsadsb-ll{position:relative;width:40px;height:40px}'
      + '.vsadsb-ll svg{position:absolute;left:0;top:0}'
      + '.vsadsb-ll .lb{position:absolute;left:31px;top:12px;font:10px ui-monospace,Menlo,monospace;line-height:11px;white-space:pre;'
      + 'color:' + T.label + ';text-shadow:0 0 3px ' + T.halo + ',0 0 2px ' + T.halo + ';pointer-events:none;transition:opacity .4s}'
      + '.vsadsb-ll .rg{position:absolute;left:20px;top:20px;width:26px;height:26px;border-radius:50%;border:2px solid;pointer-events:none;'
      + 'transform:translate(-50%,-50%);animation:vsadsbRing ' + RING_MS + 'ms ease-out ' + RING_REPEATS + ';opacity:0}'
      + '@keyframes vsadsbRing{0%{transform:translate(-50%,-50%) scale(1);opacity:0.8}100%{transform:translate(-50%,-50%) scale(3);opacity:0}}'
      + '.vsadsb-rl{font:10px ui-monospace,Menlo,monospace;color:' + T.ringLbl + ';white-space:nowrap;transform:translate(-50%,-120%);'
      + 'text-shadow:0 0 3px ' + T.halo + ';pointer-events:none}';
  }
  const cssDone = {};
  function injectCss(th, doc) {
    if (cssDone[th]) return;
    cssDone[th] = true;
    const el = doc.createElement('style');
    el.textContent = css(th);
    doc.head.appendChild(el);
  }

  function esc(v) {
    return String(v == null ? '' : v).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }
  function num(v) { const n = Number(v); return v == null || v === '' || !isFinite(n) ? null : n; }
  /** ★ Off-air text is whatever a transponder sent: a callsign is kept to the characters a callsign has. */
  function cleanFlight(v) { return String(v == null ? '' : v).replace(/[^A-Za-z0-9 -]/g, '').trim().slice(0, 10); }
  function cleanHex(v) { return String(v == null ? '' : v).replace(/[^~0-9A-Fa-f]/g, '').slice(0, 7).toLowerCase(); }

  /* ══ THE LAYER ═══════════════════════════════════════════════════════════════════════════════ */
  function create(mapIn, opts) {
    opts = opts || {};
    const th = opts.theme === 'light' ? 'light' : 'dark';
    const T = THEME[th], TONE = TONES[th];
    const doc = (typeof document !== 'undefined') ? document : null;
    if (!doc) throw new Error('adsblayer: needs a document');
    injectCss(th, doc);

    // ── Which map is this? ─────────────────────────────────────────────────────────────────────
    let gl = null, lm = null;
    if (mapIn && mapIn._gl && typeof mapIn._gl.addLayer === 'function') gl = mapIn._gl;            // compat adapter
    else if (mapIn && mapIn.map && typeof mapIn.map.addLayer === 'function' && mapIn.map.getStyle) gl = mapIn.map; // a vm
    else if (mapIn && typeof mapIn.getStyle === 'function' && typeof mapIn.project === 'function') gl = mapIn;  // MapLibre
    else if (mapIn && typeof mapIn.latLngToContainerPoint === 'function') lm = mapIn;               // Leaflet
    else throw new Error('adsblayer: not a MapLibre or Leaflet map');
    const LL = lm ? global.L : null;
    const ML = global.maplibregl || null;
    const TILE = gl ? 512 : 256;
    const ZMAX = gl ? 10 : 11, ZMIN = gl ? 1.5 : 2.5;
    const container = gl ? gl.getContainer() : lm.getContainer();
    const DPR = Math.min(4, Math.max(1, global.devicePixelRatio || 1));
    const now = () => (global.performance && performance.now ? performance.now() : Date.now());

    let rx = opts.receiver && isFinite(opts.receiver.lat) && isFinite(opts.receiver.lon) ? { lat: +opts.receiver.lat, lon: +opts.receiver.lon } : null;
    if (rx && rx.lat === 0 && rx.lon === 0) rx = null;      // ★ Null Island is "unknown", as on the HFDL map
    const prefs = Object.assign({ autoZoom: true, fit: true, rings: true }, opts.prefs || {});
    const onPrefs = typeof opts.onPrefs === 'function' ? opts.onPrefs : null;
    /* ★ drawReceiver:false — the host draws its own receiver marker (the app's page already has the HFDL
     *  house); the layer still uses the position for rings, distances and the camera. Default true. */
    const drawReceiver = opts.drawReceiver !== false;

    const ac = new Map();          // hex → state
    let firstUpdateAt = 0, firstPosBatchSeen = false;
    let selected = null;
    let paused = false;            // ★ the user moved the map: auto-zoom waits until they say otherwise
    let ourMove = 0;               // camera moves we started, still running
    let destroyed = false;
    const az = new AutoZoom();
    let labelled = new Set(), labelAt = 0;
    let rafId = 0, lastFlush = 0;
    let firstCamera = true;

    /* ── the control: AUTO · FIT · RINGS · ⌖, inside the map ──────────────────────────────────── */
    const ctl = doc.createElement('div');
    ctl.className = 'vsadsb-ctl vsadsb-' + th;
    const mk = (txt, title) => { const b = doc.createElement('button'); b.type = 'button'; b.textContent = txt; b.title = title; ctl.appendChild(b); return b; };
    const bAuto = mk('AUTO', 'Auto-zoom: keep every aircraft in view');
    const bFit = mk('FIT', 'Fit to aircraft: the receiver need not be in the middle');
    const bRings = mk('RINGS', 'Range rings in nautical miles around the receiver');
    const bCentre = mk('⌖', 'Recentre on the aircraft');
    bCentre.setAttribute('aria-label', 'Recentre');
    // ★ The map must not take the control's taps as map gestures (a drag that starts on a button).
    for (const ev of ['pointerdown', 'mousedown', 'touchstart', 'dblclick', 'wheel']) ctl.addEventListener(ev, (e) => e.stopPropagation(), { passive: true });
    container.appendChild(ctl);
    function paintCtl() {
      bAuto.classList.toggle('on', !!prefs.autoZoom && !paused);
      bAuto.classList.toggle('paused', !!prefs.autoZoom && paused);
      bAuto.title = !prefs.autoZoom ? 'Auto-zoom is off — tap to keep every aircraft in view'
        : paused ? 'Auto-zoom paused because you moved the map — tap to resume' : 'Auto-zoom on — tap to turn it off';
      bFit.classList.toggle('on', !!prefs.fit || !rx);
      bRings.classList.toggle('on', !!prefs.rings && !!rx);
      // ★ A control that cannot do anything is shown disabled with the reason, never left inert (AGENTS.md).
      bFit.disabled = !rx; bRings.disabled = !rx;
      if (!rx) { bFit.title = bRings.title = 'The receiver’s position is not known on this server'; }
      else { bFit.title = prefs.fit ? 'Fit to aircraft — tap to keep the receiver in the middle' : 'Receiver centred — tap to fit the view to the aircraft instead';
             bRings.title = 'Range rings in nautical miles around the receiver'; }
    }
    function setPref(k, v) {
      if (prefs[k] === v) return;
      prefs[k] = v;
      if (onPrefs) { try { onPrefs(Object.assign({}, prefs)); } catch (e) { console.error('adsblayer: onPrefs threw', e); } }
    }
    bAuto.addEventListener('click', () => {
      if (prefs.autoZoom && paused) { paused = false; az.reset(); cameraStep(true); }
      else { setPref('autoZoom', !prefs.autoZoom); paused = false; az.reset(); if (prefs.autoZoom) cameraStep(true); }
      paintCtl();
    });
    bFit.addEventListener('click', () => { setPref('fit', !prefs.fit); az.reset(); paintCtl(); if (prefs.autoZoom && !paused) cameraStep(true); });
    bRings.addEventListener('click', () => { setPref('rings', !prefs.rings); paintCtl(); drawRings(); });
    bCentre.addEventListener('click', () => { paused = false; az.reset(); paintCtl(); cameraStep(true); });
    paintCtl();

    /* ── camera ─────────────────────────────────────────────────────────────────────────────── */
    function viewSize() { return { w: container.clientWidth || 600, h: container.clientHeight || 400 }; }
    function curCam() {
      if (gl) { const c = gl.getCenter(); return { center: { lat: c.lat, lon: c.lng }, zoom: gl.getZoom() }; }
      const c = lm.getCenter(); return { center: { lat: c.lat, lon: c.lng }, zoom: lm.getZoom() };
    }
    function positioned() {
      const out = [];
      for (const s of ac.values()) if (!s.goneAt && s.disp) out.push({ lat: s.disp.lat, lon: s.disp.lon });
      return out;
    }
    /* ★ The padding leaves room for the control (top right) and the icons' own size at the edges. */
    function padPx() { const v = viewSize(); return Math.max(36, Math.min(64, Math.min(v.w, v.h) * 0.08)); }
    function target() {
      const v = viewSize();
      return cameraFor({ points: positioned(), rx, fit: prefs.fit || !rx, w: v.w, h: v.h, pad: padPx(), tileSize: TILE,
                         minZoom: ZMIN, maxZoom: ZMAX, emptyZoom: gl ? 7 : 8 });
    }
    function moveCamera(cam, ms) {
      ourMove++;
      /* ★ Counted down on moveend — or by the timer, because a move to where the map already is may
       *  never send one, and a count stuck above zero would hold auto-zoom "busy" for ever. */
      let fin = false, tid = 0;
      const done = () => { if (fin) return; fin = true; clearTimeout(tid); ourMove = Math.max(0, ourMove - 1); };
      if (ms > 0) tid = setTimeout(done, ms + 600);
      try {
        if (gl) {
          if (ms > 0) { gl.once('moveend', done); gl.easeTo({ center: [cam.center.lon, cam.center.lat], zoom: cam.zoom, duration: ms, easing: easeInOut }, { vsadsb: true }); }
          else { gl.jumpTo({ center: [cam.center.lon, cam.center.lat], zoom: cam.zoom }, { vsadsb: true }); done(); }
        } else {
          if (ms > 0) { lm.once('moveend', done); lm.flyTo([cam.center.lat, cam.center.lon], cam.zoom, { duration: ms / 1000, easeLinearity: 0.25 }); }
          else { lm.setView([cam.center.lat, cam.center.lon], cam.zoom, { animate: false }); done(); }
        }
      } catch (e) { done(); console.error('adsblayer: camera move failed', e); }
    }
    function busy() { return ourMove > 0 || (gl ? gl.isMoving() : false); }
    /** force = a person asked (a toggle, recentre, resume): go now. */
    function cameraStep(force) {
      const tgt = target();
      if (!tgt) return;
      if (firstCamera) {
        firstCamera = false;
        moveCamera({ center: tgt.center, zoom: tgt.zoom - az.slack }, 0);
        az.lastMoveAt = now();
        return;
      }
      if (force) { az.reset(); az.lastMoveAt = now(); moveCamera({ center: tgt.center, zoom: tgt.zoom - az.slack }, CAMERA_MS); return; }
      if (!prefs.autoZoom || paused) return;
      const v = viewSize();
      let cur;
      try { cur = curCam(); } catch (e) { return; }   // ★ a Leaflet map with no view yet
      const out = anyOutside(positioned(), cur, v.w, v.h, padPx() * 0.5, TILE);
      const g = az.decide(now(), cur, tgt, out, v.w, v.h, TILE, busy());
      if (g) moveCamera(g, CAMERA_MS);
    }
    /* ★★★ A PERSON WHO MOVES THE MAP HAS TAKEN IT. Any camera move this layer did not start pauses
     *  auto-zoom (a drag, a pinch, the wheel, the zoom buttons — and a host's own fly, which is the same
     *  wish). It stays paused until they tap AUTO or ⌖: a view that drifts back under the finger that
     *  just put it somewhere is a view fighting its user. */
    /* ★★ ONLY A PERSON'S MOVE COUNTS: one carrying MapLibre's originalEvent, or any move within a second of
     *  a touch, a click, the wheel or a key on the map (the compat adapter's zoom buttons send no
     *  originalEvent). A host that places its map in code after creating the layer (MapOverlay's addRx
     *  does a setView) must not leave auto-zoom paused before anyone has touched anything. */
    let lastInputAt = -Infinity;
    const onInput = () => { lastInputAt = now(); };
    for (const ev of ['pointerdown', 'touchstart', 'wheel', 'keydown']) container.addEventListener(ev, onInput, { passive: true, capture: true });
    function onMoveStart(e) {
      if (e && e.vsadsb) return;
      const person = !!(e && e.originalEvent) || now() - lastInputAt < 1000;
      if (!person) return;
      if (ourMove > 0) ourMove = 0;   // ★ a person interrupted our ease — theirs now
      if (prefs.autoZoom && !paused) { paused = true; paintCtl(); }
    }
    const onMoveEnd = () => { drawRings(); labelAt = 0; kick(); };
    if (gl) { gl.on('movestart', onMoveStart); gl.on('moveend', onMoveEnd); }
    else { lm.on('movestart', onMoveStart); lm.on('moveend', onMoveEnd); }

    /* ── range rings ────────────────────────────────────────────────────────────────────────── */
    let llRings = null;
    function ringGeo() {
      if (!rx || !prefs.rings) return null;
      const z = gl ? gl.getZoom() : lm.getZoom();
      const v = viewSize();
      const step = ringStep(nmPerPx(rx.lat, z, TILE), Math.max(70, Math.min(v.w, v.h) / 4.5));
      // ★ Enough rings to reach the furthest corner of the view from the receiver, at most twelve.
      const cur = curCam();
      const o = screenOffset(cur, rx.lat, rx.lon, TILE);
      const far = Math.max(Math.hypot(v.w / 2 + Math.abs(o.x), v.h / 2 + Math.abs(o.y)), 1) * nmPerPx(rx.lat, z, TILE);
      const n = Math.max(1, Math.min(12, Math.ceil(far / step)));
      const rings = [];
      for (let i = 1; i <= n; i++) {
        const nm = i * step, pts = [];
        for (let k = 0; k <= 128; k++) { const d = destination(rx.lat, rx.lon, k * 360 / 128, nm); pts.push([d.lon, d.lat]); }
        rings.push({ nm, pts, lbl: destination(rx.lat, rx.lon, 0, nm) });
      }
      return rings;
    }
    function drawRings() {
      if (destroyed) return;
      const rings = ringGeo();
      if (gl) {
        const s1 = gl.getSource('vsadsb-rings'), s2 = gl.getSource('vsadsb-ringlbl');
        if (!s1 || !s2) return;
        s1.setData({ type: 'FeatureCollection', features: (rings || []).map((r) => ({ type: 'Feature', properties: {}, geometry: { type: 'LineString', coordinates: r.pts } })) });
        s2.setData({ type: 'FeatureCollection', features: (rings || []).map((r) => ({ type: 'Feature', properties: { t: r.nm + ' NM' }, geometry: { type: 'Point', coordinates: [r.lbl.lon, r.lbl.lat] } })) });
      } else {
        if (llRings) { lm.removeLayer(llRings); llRings = null; }
        if (!rings) return;
        llRings = LL.layerGroup();
        for (const r of rings) {
          LL.circle([rx.lat, rx.lon], { radius: r.nm * NM_M, color: T.ring, weight: 1, fill: false, dashArray: '4 6', interactive: false }).addTo(llRings);
          LL.marker([r.lbl.lat, r.lbl.lon], { interactive: false, icon: LL.divIcon({ className: '', html: '<div class="vsadsb-rl">' + r.nm + ' NM</div>', iconSize: [0, 0] }) }).addTo(llRings);
        }
        llRings.addTo(lm);
      }
    }

    /* ── GPU layers ─────────────────────────────────────────────────────────────────────────── */
    const imgs = new Set();
    let glReady = false;
    function imgId(kind, tone, glow) {
      const id = 'vsadsb-' + th + '-' + kind + '-' + tone + (glow ? '-g' : '');
      if (!imgs.has(id) || !gl.hasImage(id)) {
        try { if (!gl.hasImage(id)) gl.addImage(id, iconImage(kind, TONE[tone], tone === 'ground' ? 0.8 : 0.92, glow, DPR), { pixelRatio: DPR }); imgs.add(id); }
        catch (e) { console.error('adsblayer: icon not added', id, e); }
      }
      return id;
    }
    function rxImage() {
      if (gl.hasImage('vsadsb-rx')) return;
      const im = new Image();
      im.onload = () => {
        const cv = doc.createElement('canvas'); cv.width = 22 * DPR; cv.height = 22 * DPR;
        const x = cv.getContext('2d'); x.scale(DPR, DPR); x.drawImage(im, 0, 0, 22, 22);
        try { if (!gl.hasImage('vsadsb-rx')) gl.addImage('vsadsb-rx', x.getImageData(0, 0, cv.width, cv.height), { pixelRatio: DPR }); } catch (e) { console.error('adsblayer: receiver icon', e); }
      };
      im.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(RX_SVG);
    }
    function glSetup() {
      if (destroyed || gl.getSource('vsadsb-ac')) return;
      const empty = { type: 'FeatureCollection', features: [] };
      for (const id of ['vsadsb-rings', 'vsadsb-ringlbl', 'vsadsb-ac', 'vsadsb-rx']) gl.addSource(id, { type: 'geojson', data: empty });
      gl.addLayer({ id: 'vsadsb-rings', type: 'line', source: 'vsadsb-rings',
                    paint: { 'line-color': T.ring, 'line-width': 1, 'line-dasharray': [4, 6] } });
      gl.addLayer({ id: 'vsadsb-ringlbl', type: 'symbol', source: 'vsadsb-ringlbl',
                    layout: { 'text-field': ['get', 't'], 'text-font': ['JetBrains Mono Regular'], 'text-size': 10, 'text-anchor': 'bottom',
                              'text-offset': [0, -0.3], 'text-allow-overlap': true, 'text-ignore-placement': true },
                    paint: { 'text-color': T.ringLbl, 'text-halo-color': T.halo, 'text-halo-width': 1.2 } });
      gl.addLayer({ id: 'vsadsb-rx', type: 'symbol', source: 'vsadsb-rx',
                    layout: { 'icon-image': 'vsadsb-rx', 'icon-anchor': 'bottom', 'icon-allow-overlap': true, 'icon-ignore-placement': true } });
      // ★ The breath (HFDL .pglow) and the ring (HFDL .ring), drawn only for the aircraft that are new.
      gl.addLayer({ id: 'vsadsb-halo', type: 'circle', source: 'vsadsb-ac', filter: ['has', 'ho'],
                    paint: { 'circle-radius': 15, 'circle-blur': 1, 'circle-color': ['get', 'col'], 'circle-opacity': ['get', 'ho'],
                             'circle-pitch-alignment': 'map' } });
      gl.addLayer({ id: 'vsadsb-ring', type: 'circle', source: 'vsadsb-ac', filter: ['has', 'rr'],
                    paint: { 'circle-radius': ['get', 'rr'], 'circle-opacity': 0, 'circle-stroke-width': 2,
                             'circle-stroke-color': ['get', 'col'], 'circle-stroke-opacity': ['get', 'ro'] } });
      gl.addLayer({ id: 'vsadsb-icons', type: 'symbol', source: 'vsadsb-ac',
                    layout: { 'icon-image': ['get', 'img'], 'icon-rotate': ['get', 'rot'], 'icon-rotation-alignment': 'map',
                              'icon-allow-overlap': true, 'icon-ignore-placement': true, 'symbol-sort-key': ['get', 'z'] },
                    paint: { 'icon-opacity': ['get', 'op'] } });
      gl.addLayer({ id: 'vsadsb-labels', type: 'symbol', source: 'vsadsb-ac', filter: ['has', 'lbl'],
                    layout: { 'text-field': ['get', 'lbl'], 'text-font': ['JetBrains Mono Regular'], 'text-size': 10, 'text-line-height': 1.15,
                              'text-anchor': 'left', 'text-justify': 'left', 'text-offset': [1.25, 0], 'text-allow-overlap': true,
                              'text-ignore-placement': true },
                    paint: { 'text-color': T.label, 'text-halo-color': T.halo, 'text-halo-width': 1.2, 'text-opacity': ['get', 'lo'] } });
      rxImage();
      glReady = true;
      drawRx(); drawRings();
      kick();
    }
    function hitHex(point) {
      const r = 12;
      const f = gl.queryRenderedFeatures([[point.x - r, point.y - r], [point.x + r, point.y + r]], { layers: ['vsadsb-icons'] });
      if (!f.length) return null;
      // ★ The nearest of whatever is under the finger, not the first MapLibre happened to list.
      let best = null, bd = Infinity;
      for (const x of f) {
        const c = x.geometry.coordinates, p = gl.project(c), d = Math.hypot(p.x - point.x, p.y - point.y);
        if (d < bd) { bd = d; best = x.properties.h; }
      }
      return best;
    }
    function onGlClick(e) { if (!glReady) return; select(hitHex(e.point)); }
    function onGlHover(e) { if (!glReady) return; gl.getCanvas().style.cursor = hitHex(e.point) ? 'pointer' : ''; }
    function drawRx() {
      if (!drawReceiver) return;
      if (gl) {
        const s = glReady && gl.getSource('vsadsb-rx');
        if (s) s.setData({ type: 'FeatureCollection', features: rx ? [{ type: 'Feature', properties: {}, geometry: { type: 'Point', coordinates: [rx.lon, rx.lat] } }] : [] });
      } else {
        if (llRx) { lm.removeLayer(llRx); llRx = null; }
        if (rx) llRx = LL.marker([rx.lat, rx.lon], { zIndexOffset: 1000, interactive: false,
          icon: LL.divIcon({ html: RX_SVG, className: '', iconSize: [22, 22], iconAnchor: [11, 22] }) }).addTo(lm);
      }
    }
    let llRx = null;
    if (gl) {
      if (gl.isStyleLoaded && gl.isStyleLoaded()) glSetup(); else gl.once('load', glSetup);
      gl.on('click', onGlClick);
      gl.on('mousemove', onGlHover);
      // ★ A vm whose style arrives later: setStyle({diff:false}) fires style.load, after which our sources are gone.
      gl.on('style.load', () => { if (!gl.getSource('vsadsb-ac')) { glReady = false; imgs.clear(); glSetup(); } });
      gl.on('styleimagemissing', (e) => {
        const m = /^vsadsb-(dark|light)-([a-z0-9]+)-([a-z]+)(-g)?$/.exec(e.id || '');
        if (m && m[1] === th) imgId(m[2], m[3], !!m[4]);
        if (e.id === 'vsadsb-rx') rxImage();
      });
    } else {
      drawRx(); drawRings();
    }

    /* ── the card ───────────────────────────────────────────────────────────────────────────── */
    let popup = null, popupHex = null, popupHtml = '';
    function cardHtml(s) {
      const a = s.a;
      const name = cleanFlight(a.flight) || s.hex.toUpperCase();
      const rows = [];
      const row = (k, v) => { if (v) rows.push('<tr><td>' + esc(k) + '</td><td>' + esc(v) + '</td></tr>'); };
      const em = emergencyOf(a);
      row('Altitude', altText(a.alt_baro) + (num(a.alt_geom) != null && a.alt_baro !== 'ground' ? '  (GNSS ' + Math.round(a.alt_geom).toLocaleString('en-GB') + ' ft)' : ''));
      row('Climb', vsText(a.baro_rate));
      row('Speed', num(a.gs) != null ? Math.round(a.gs) + ' kt' : '');
      row('Track', num(a.track) != null ? Math.round(a.track) + '° ' + cardinal(a.track) : '');
      row('Squawk', a.squawk ? String(a.squawk).replace(/[^0-7]/g, '').slice(0, 4) : '');
      if (rx && s.disp) {
        const d = distNm(rx.lat, rx.lon, s.fix.lat, s.fix.lon), b = bearingDeg(rx.lat, rx.lon, s.fix.lat, s.fix.lon);
        row('From you', (d < 10 ? d.toFixed(1) : Math.round(d)) + ' NM · ' + Math.round(b) + '° ' + cardinal(b));
      }
      row('Type', CAT_NAME[String(a.category || '').toUpperCase()] || '');
      const seen = num(a.seen);
      row('Heard', seen == null ? '' : seen < 2 ? 'just now' : Math.round(seen) + ' s ago');
      row('Signal', num(a.rssi) != null ? a.rssi.toFixed(1) + ' dBFS' + (num(a.messages) != null ? ' · ' + a.messages + ' msgs' : '') : '');
      return (em ? '<div class="em">' + esc(em) + '</div>' : '')
        + '<div class="t">' + esc(name) + (cleanFlight(a.flight) ? '<i>' + esc(s.hex.toUpperCase()) + '</i>' : '') + '</div>'
        + '<table>' + rows.join('') + '</table>';
    }
    function select(hex) {
      if (hex === selected && popup) return;
      selected = hex && ac.has(hex) ? hex : null;
      closePopup();
      if (!selected) return;
      const s = ac.get(selected);
      popupHtml = cardHtml(s); popupHex = selected;
      if (gl) {
        if (!ML) { console.error('adsblayer: maplibregl is not on the page — no aircraft card'); return; }
        /* ★★ focusAfterOpen OFF: MapLibre focuses the card's x on EVERY setHTML, and the card is refreshed each
         *  report — so the keyboard's and the controller's next Enter closed the card instead of pressing what
         *  was highlighted (web client, ADS-B list, 2026-10-24). The x is still a button anyone can reach. */
        const p = new ML.Popup({ closeButton: true, closeOnClick: false, focusAfterOpen: false, className: 'vsadsb-pop-' + th, maxWidth: '280px', offset: 16 })
          .setLngLat([s.disp.lon, s.disp.lat]).setHTML(popupHtml);
        // ★ Only THIS card's close clears the selection — closePopup() forgets the card before removing it.
        p.on('close', () => { if (popup === p) { popup = null; popupHex = null; selected = null; kick(); } });
        popup = p; p.addTo(gl);
      } else {
        const p = LL.popup({ className: 'vsadsb-pop-' + th, maxWidth: 280, offset: [0, -10], autoPan: false, closeOnClick: false })
          .setLatLng([s.disp.lat, s.disp.lon]).setContent(popupHtml);
        p.on('remove', () => { if (popup === p) { popup = null; popupHex = null; selected = null; kick(); } });
        popup = p; p.openOn(lm);
      }
      kick();
    }
    function closePopup() {
      const p = popup; popup = null; popupHex = null;
      if (p) { try { if (gl) p.remove(); else lm.closePopup(p); } catch (e) { console.error('adsblayer: popup close', e); } }
    }

    /* ── ingest ─────────────────────────────────────────────────────────────────────────────── */
    /* ★★★ THE GLIDE. Each aircraft is drawn at its DEAD-RECKONED position (its last fix flown on along
     *  its track at its ground speed) plus a CORRECTION that eases away. When a new fix lands, the
     *  correction is set to exactly the gap between where it is drawn and where the new fix says it
     *  should be — so the icon does not move at all in that instant — and then that gap is eased to
     *  nothing over GLIDE_MS. A one-second report rate with a 2.2 s ease means corrections overlap and
     *  blend; the aircraft simply flies. Only a gap bigger than SNAP_NM (a bad decode, a return after
     *  silence) fades across instead. */
    function update(list) {
      if (destroyed) return;
      const t = now();
      if (!firstUpdateAt) firstUpdateAt = t;
      const seen = new Set();
      let anyPos = false;
      for (const raw of Array.isArray(list) ? list : []) {
        if (!raw || typeof raw !== 'object') continue;
        const hex = cleanHex(raw.hex);
        if (!hex) continue;
        seen.add(hex);
        const lat = num(raw.lat), lon = num(raw.lon);
        const hasPos = lat != null && lon != null && Math.abs(lat) <= 90 && Math.abs(lon) <= 180;
        let s = ac.get(hex);
        if (!s) { s = { hex, a: raw, fix: null, disp: null, corr: null, hdg: null, bornAt: 0, goneAt: 0, shownAt: 0, kind: 'jet' }; ac.set(hex, s); }
        s.a = raw; s.kind = iconFor(raw.category);
        /* ★ Heard, but its position has aged out (the server keeps one 60 s): off the map, gently — it stays
         *  in the box's list. Back with a position before it has faded = it simply carries on. */
        if (!hasPos) { if (s.disp && !s.goneAt) s.goneAt = t; continue; }
        if (s.goneAt) { s.goneAt = 0; s.fadeIn = true; s.shownAt = t; }
        anyPos = true;
        const fix = { lat, lon, gs: num(raw.gs), track: num(raw.track) != null ? num(raw.track) : num(raw.mag_heading),
                      t: t - Math.max(0, Math.min(60, num(raw.seen_pos) || 0)) * 1000 };
        const same = s.fix && Math.abs(s.fix.lat - fix.lat) < 1e-7 && Math.abs(s.fix.lon - fix.lon) < 1e-7;
        if (same) { s.fix.gs = fix.gs; s.fix.track = fix.track; continue; }   // ★ the same fix again: keep flying it
        if (!s.disp) {
          s.fix = fix; s.disp = deadReckon(fix, t); s.corr = null; s.hdg = fix.track;
          s.shownAt = t;
          // ★★ Born quietly unless the initial load is over (SETTLE_MS and the first positioned batch).
          s.bornAt = (firstPosBatchSeen && t - firstUpdateAt > SETTLE_MS) ? t : 0;
          s.fadeIn = s.bornAt > 0;
          continue;
        }
        const shown = s.disp;
        const pred = deadReckon(fix, t);
        const gap = distNm(shown.lat, shown.lon, pred.lat, pred.lon);
        s.fix = fix;
        if (gap > SNAP_NM) { s.corr = null; s.disp = pred; s.shownAt = t; s.fadeIn = true; }
        else s.corr = { dLat: shown.lat - pred.lat, dLon: wrapLon(shown.lon, pred.lon) - pred.lon, t0: t };
      }
      if (anyPos) firstPosBatchSeen = true;
      for (const s of ac.values()) if (!seen.has(s.hex) && !s.goneAt) s.goneAt = t;
      if (selected && popup && ac.has(selected)) {
        const h = cardHtml(ac.get(selected));
        if (h !== popupHtml) { popupHtml = h; if (gl) popup.setHTML(h); else popup.setContent(h); }
      }
      cameraStep(false);
      kick();
    }

    /* ── the frame loop ─────────────────────────────────────────────────────────────────────── */
    function frameGap(n) {
      if (lm) return n > AC_GLIDE_MAX ? 500 : n > 60 ? 100 : 66;     // ★ DOM markers: a few fps is the honest rate
      return n > AC_GLIDE_MAX ? 250 : n > 150 ? 50 : n > 60 ? 33 : 0;
    }
    function kick() { if (!rafId && !destroyed) rafId = requestAnimationFrame(frame); }
    let lastT = 0;
    function frame() {
      rafId = 0;
      if (destroyed) return;
      const t = now();
      const n = ac.size;
      const gap = frameGap(n);
      if (gap && t - lastFlush < gap) { rafId = requestAnimationFrame(frame); return; }
      const dt = lastT ? Math.min(250, t - lastT) : 16; lastT = t;
      lastFlush = t;
      let animating = false;
      for (const s of ac.values()) {
        if (s.goneAt && t - s.goneAt > FADE_OUT_MS) { ac.delete(s.hex); if (selected === s.hex) { selected = null; closePopup(); } removeLl(s); continue; }
        if (!s.fix) continue;
        const p = deadReckon(s.fix, t);
        if (s.corr) {
          const k = Math.min(1, (t - s.corr.t0) / GLIDE_MS);
          const r = 1 - easeInOut(k);
          p.lat += s.corr.dLat * r; p.lon += s.corr.dLon * r;
          if (k >= 1) s.corr = null;
        }
        s.disp = p;
        // ★ The heading eases too (≈0.4 s), so a turn reads as a turn and a noisy track never twitches.
        const want = s.fix.track;
        if (want != null) {
          if (s.hdg == null) s.hdg = want;
          else s.hdg = (s.hdg + angleTo(s.hdg, want) * (1 - Math.exp(-dt / 400)) + 360) % 360;
        }
        if ((s.fix.gs > 0 && t - s.fix.t < MAX_DR_S * 1000) || s.corr || s.goneAt || (s.bornAt && t - s.bornAt < RING_MS * RING_REPEATS)
            || (s.fadeIn && t - s.shownAt < FADE_IN_MS)) animating = true;
      }
      if (t - labelAt > LABEL_EVERY_MS) { labelAt = t; decideLabels(); }
      if (gl) flushGl(t); else flushLl(t);
      if (popup && selected && ac.has(selected)) {
        const s = ac.get(selected);
        if (s.disp) { if (gl) popup.setLngLat([s.disp.lon, s.disp.lat]); else popup.setLatLng([s.disp.lat, s.disp.lon]); }
      }
      if (animating) kick();
    }
    function labelText(s) {
      const name = cleanFlight(s.a.flight) || s.hex.toUpperCase();
      const alt = altShort(s.a.alt_baro);
      return alt ? name + '\n' + alt : name;
    }
    function decideLabels() {
      const v = viewSize(), items = [];
      for (const s of ac.values()) {
        if (!s.disp || s.goneAt) continue;
        const p = gl ? gl.project([s.disp.lon, s.disp.lat]) : lm.latLngToContainerPoint([s.disp.lat, s.disp.lon]);
        if (p.x < -20 || p.y < -20 || p.x > v.w + 20 || p.y > v.h + 20) continue;
        const txt = labelText(s), lines = txt.split('\n');
        const w = Math.max.apply(null, lines.map((l) => l.length)) * 6.1 + 4, h = lines.length * 11.5 + 2;
        // ★ Priority: the selected aircraft, an emergency, then the nearest to the receiver (or the newest heard).
        const pri = s.hex === selected ? -2 : emergencyOf(s.a) ? -1
          : rx ? distNm(rx.lat, rx.lon, s.disp.lat, s.disp.lon) : (num(s.a.seen) || 0);
        items.push({ id: s.hex, x: p.x, y: p.y, w, h, pri });
      }
      labelled = placeLabels(items, labelled, v.w, v.h, LABEL_MAX);
      for (const s of ac.values()) s.lblWant = labelled.has(s.hex);
      kick();
    }
    function ringProps(s, t) {
      if (!s.bornAt) return null;
      const age = t - s.bornAt;
      if (age >= RING_MS * RING_REPEATS) { s.bornAt = 0; return null; }
      const k = easeOut((age % RING_MS) / RING_MS);
      return { rr: 13 + 26 * k, ro: 0.8 * (1 - k), ho: 0.35 * breath(age) * (1 - age / (RING_MS * RING_REPEATS)) };
    }
    function flushGl(t) {
      if (!glReady) return;
      const src = gl.getSource('vsadsb-ac');
      if (!src) return;
      const feats = [];
      let anyLblAnim = false;
      for (const s of ac.values()) {
        if (!s.disp) continue;
        const tone = toneKey(s.a);
        const glow = s.hex === selected;
        let op = 1;
        if (s.goneAt) op = Math.max(0, 1 - (t - s.goneAt) / FADE_OUT_MS);
        else if (s.fadeIn) { const k = (t - s.shownAt) / FADE_IN_MS; if (k >= 1) s.fadeIn = false; else op = Math.max(0, k); }
        // ★ Labels fade in and out (0.4 s) rather than pop — the HFDL toast's own transition.
        const lw = s.lblWant && !s.goneAt ? 1 : 0;
        if (s.lo == null) s.lo = lw;
        if (s.lo !== lw) {
          const step = Math.max(0.04, Math.min(1, (t - (s.lt || t)) / 400));
          s.lo = lw > s.lo ? Math.min(lw, s.lo + step) : Math.max(lw, s.lo - step);
          anyLblAnim = true;
        }
        s.lt = t;
        const props = { h: s.hex, img: imgId(s.kind, tone, glow), rot: NO_ROTATE[s.kind] ? 0 : (s.hdg || 0),
                        op: +op.toFixed(3), z: glow ? 1000 : tone === 'alert' ? 900 : (s.a.alt_baro === 'ground' ? 0 : 1), col: 'rgb(' + TONE[tone] + ')' };
        if (s.lo > 0.01) { props.lbl = labelText(s); props.lo = +(s.lo * op).toFixed(3); }
        const rp = ringProps(s, t);
        if (rp) { props.rr = +rp.rr.toFixed(2); props.ro = +rp.ro.toFixed(3); props.ho = +rp.ho.toFixed(3); }
        feats.push({ type: 'Feature', properties: props, geometry: { type: 'Point', coordinates: [s.disp.lon, s.disp.lat] } });
      }
      src.setData({ type: 'FeatureCollection', features: feats });
      if (anyLblAnim) kick();
    }

    /* ── Leaflet fallback ───────────────────────────────────────────────────────────────────── */
    function removeLl(s) { if (s.ll) { try { lm.removeLayer(s.ll); } catch (e) { /* already gone */ } s.ll = null; } }
    function flushLl(t) {
      for (const s of ac.values()) {
        if (!s.disp) continue;
        const tone = toneKey(s.a), glow = s.hex === selected;
        const rot = NO_ROTATE[s.kind] ? 0 : Math.round(s.hdg || 0);
        const sig = s.kind + tone + (glow ? 'g' : '');
        if (!s.ll) {
          const born = !!s.bornAt;
          s.ll = LL.marker([s.disp.lat, s.disp.lon], { keyboard: false, zIndexOffset: glow ? 1000 : 0,
            icon: LL.divIcon({ className: '', iconSize: [40, 40], iconAnchor: [20, 20],
              html: '<div class="vsadsb-ll">' + (born ? '<div class="rg" style="border-color:rgba(' + TONE[tone] + ',0.85)"></div>' : '')
                + '<span class="ic"></span><span class="lb"></span></div>' }) }).addTo(lm);
          const hex = s.hex;
          s.ll.on('click', () => select(hex));
          s.llSig = ''; s.llRot = null; s.llLbl = null;
        } else s.ll.setLatLng([s.disp.lat, s.disp.lon]);
        const el = s.ll.getElement && s.ll.getElement();
        if (!el) continue;
        if (s.llSig !== sig || s.llRot !== rot) {
          s.llSig = sig; s.llRot = rot;
          const ic = el.querySelector('.ic');
          if (ic) ic.innerHTML = iconSvg(s.kind, TONE[tone], 0.92, rot).replace('<svg ', glow ? '<svg style="filter:drop-shadow(0 0 5px rgba(' + TONE[tone] + ',0.85))" ' : '<svg ');
        }
        let op = 1;
        if (s.goneAt) op = Math.max(0, 1 - (t - s.goneAt) / FADE_OUT_MS);
        else if (s.fadeIn) { const k = (t - s.shownAt) / FADE_IN_MS; if (k >= 1) s.fadeIn = false; else op = Math.max(0, k); }
        el.style.opacity = String(op);
        const lb = el.querySelector('.lb');
        if (lb) {
          const txt = labelText(s);
          if (s.llLbl !== txt) { s.llLbl = txt; lb.textContent = txt; }
          lb.style.opacity = s.lblWant && !s.goneAt ? '1' : '0';
        }
      }
    }

    /* ── public ─────────────────────────────────────────────────────────────────────────────── */
    const api = {
      update,
      setReceiver(lat, lon) {
        const la = Number(lat), lo = Number(lon);
        rx = isFinite(la) && isFinite(lo) && !(la === 0 && lo === 0) && lat != null && lon != null ? { lat: la, lon: lo } : null;
        paintCtl(); drawRx(); drawRings(); az.reset();
        if (prefs.autoZoom && !paused) cameraStep(true);
      },
      setPrefs(p) {
        if (!p) return;
        let moved = false;
        for (const k of ['autoZoom', 'fit', 'rings']) if (typeof p[k] === 'boolean' && prefs[k] !== p[k]) { prefs[k] = p[k]; moved = true; }
        if (!moved) return;
        paused = false; az.reset(); paintCtl(); drawRings();
        if (prefs.autoZoom) cameraStep(true);
      },
      focus(hex) {
        const h = cleanHex(hex);
        if (!ac.has(h) || !ac.get(h).disp) return false;
        select(h);
        const s = ac.get(h), v = viewSize(), cur = curCam();
        const o = screenOffset(cur, s.disp.lat, s.disp.lon, TILE);
        // ★ Pan to it only if it is off the edge — and that is a person's choice of view, so auto-zoom pauses.
        if (Math.abs(o.x) > v.w / 2 - 40 || Math.abs(o.y) > v.h / 2 - 40) {
          if (prefs.autoZoom) { paused = true; paintCtl(); }
          moveCamera({ center: { lat: s.disp.lat, lon: s.disp.lon }, zoom: cur.zoom }, CAMERA_MS);
        }
        return true;
      },
      prefs() { return Object.assign({}, prefs); },
      destroy() {
        destroyed = true;
        if (rafId) cancelAnimationFrame(rafId);
        closePopup();
        try { ctl.remove(); } catch (e) { /* gone with the map */ }
        for (const ev of ['pointerdown', 'touchstart', 'wheel', 'keydown']) container.removeEventListener(ev, onInput, { capture: true });
        if (gl) {
          gl.off('movestart', onMoveStart); gl.off('moveend', onMoveEnd); gl.off('click', onGlClick); gl.off('mousemove', onGlHover);
          for (const id of ['vsadsb-labels', 'vsadsb-icons', 'vsadsb-ring', 'vsadsb-halo', 'vsadsb-rx', 'vsadsb-ringlbl', 'vsadsb-rings']) if (gl.getLayer && gl.getLayer(id)) gl.removeLayer(id);
          for (const id of ['vsadsb-ac', 'vsadsb-rx', 'vsadsb-ringlbl', 'vsadsb-rings']) if (gl.getSource && gl.getSource(id)) gl.removeSource(id);
        } else {
          lm.off('movestart', onMoveStart); lm.off('moveend', onMoveEnd);
          for (const s of ac.values()) removeLl(s);
          if (llRings) lm.removeLayer(llRings);
          if (llRx) lm.removeLayer(llRx);
        }
        ac.clear();
      },
    };
    return api;
  }

  global.VibeAdsbLayer = { create, maths, ICONS: Object.keys(ICONS), iconSvg };
})(typeof window !== 'undefined' ? window : globalThis);
