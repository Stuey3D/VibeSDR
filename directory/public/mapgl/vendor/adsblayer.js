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
 *     layer.setPrefs({autoZoom, fit, rings, labels, range, rangeAll})
 *     layer.rangeShown()             // ★ beta94 the RANGE OUTLINE drawn: {window, points, allPoints, far, record} | null
 *     layer.setObstacle({corner:'bottom-left', w, h} | null)   — the host's live list, kept clear of aircraft
 *     layer.toggleLegend(on?)   — the colour key, opened above that list (the host's (i) button)
 *     layer.focus(hex)               // select an aircraft and open its card (a list row was tapped)
 *     opts.onSelect(hex | null)      // the selection changed ON THE MAP (an icon tapped, the card closed) — the host's
 *                                    // list follows it (Stuart, 2026-10-24: clicking an icon left the list's row stale)
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
  /** ★ V12 beta90 a datalink aircraft's glide (a.sparse): longer, so a 40 NM hop between fixes reads as flight, and the
   *  hop past which it fades instead (a position that far from the last is a different answer). */
  const SPARSE_GLIDE_MS = 3500, SPARSE_SNAP_NM = 400;
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

  /** ★ beta96 the message-SNR scale for ADS-B (src/utils/snrColour.ts SNR_SCALES.adsb + SNR_COLOURS) — see the card's SNR row. */
  const SNR_ADSB = { thr: 10, red: '#e5484d', amber: '#f0a020', green: '#3dbb5f' };
  const CAT_NAME = { A1: 'Light aircraft', A2: 'Small aircraft', A3: 'Large aircraft', A4: 'High-vortex large (B757)',
                     A5: 'Heavy', A6: 'High performance', A7: 'Rotorcraft', B1: 'Glider', B2: 'Lighter than air',
                     B3: 'Parachutist', B4: 'Ultralight', B6: 'Unmanned aerial vehicle', B7: 'Space vehicle',
                     C1: 'Emergency vehicle', C2: 'Service vehicle', C3: 'Obstacle' };

  function onGround(a) { return a && a.alt_baro === 'ground'; }
  /** ★★ FEET, ALWAYS (Stuart, 2026-10-24: "we have the space to fit the altitude in as ft"): "12,200 ft", to the nearest
   *  hundred. Flight levels went — the transition altitude they hang on differs by country (UK 3–6 000 ft, US 18 000),
   *  and the colour of the text (altColour) already says how high at a glance. */
  function altText(alt) {
    if (alt === 'ground') return 'GND';
    const ft = Number(alt);
    if (alt == null || alt === '' || !isFinite(ft)) return '—';
    return (Math.round(ft / 100) * 100).toLocaleString('en-GB') + ' ft';
  }
  /** The same for a map label, '' when unknown. */
  function altShort(alt) {
    if (alt == null || alt === '') return '';
    if (alt !== 'ground' && !isFinite(Number(alt))) return '';
    return altText(alt);
  }
  function vsText(rate) {
    const r = Number(rate);
    if (rate == null || !isFinite(r)) return '';
    if (Math.abs(r) < 100) return 'level';
    return (r > 0 ? '↑ ' : '↓ ') + Math.abs(Math.round(r / 64) * 64).toLocaleString('en-GB') + ' ft/min';
  }
  /** ★ The card's row name says which way (Stuart, 2026-10-24: a descending aircraft read "Climb ↓ −1,920 ft/min").
   *  Level is "Level" — "cruise" is only the level part at the top of a flight (Stuart asked; an aircraft level at
   *  3 000 ft in a hold is not cruising), so the VALUE says cruising when it is level at FL200 or above. */
  function vsLabel(rate) {
    const r = Number(rate);
    if (rate == null || !isFinite(r)) return 'Vertical';
    if (Math.abs(r) < 100) return 'Level';
    return r > 0 ? 'Climb' : 'Descent';
  }
  function vsCardText(rate, altFt) {
    const r = Number(rate);
    if (rate != null && isFinite(r) && Math.abs(r) < 100 && num(altFt) != null) return num(altFt) >= 20000 ? 'cruising' : 'level';
    return vsText(rate);
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

  /* ★★ THE RANGE RINGS, SHARED (V12 2026-10-27: the NDB map uses them too — Stuart, "same rings, for nm range"). One
   *  ladder, one geometry, one look, the labels on all four sides, in NAUTICAL MILES. ADS-B's layer and ndblayer.js
   *  both draw them through these four, so the two maps can never drift apart. */
  /** The rings for a view: rx {lat,lon}, the zoom, the view's size v {w,h}, the receiver's screen offset o {x,y} from
   *  the view's centre, the tile size. Enough to reach the furthest corner of the view, at most twelve. */
  function ringsFor(rx, zoom, v, o, tile) {
    const npp = nmPerPx(rx.lat, zoom, tile);
    const step = ringStep(npp, Math.max(70, Math.min(v.w, v.h) / 4.5));
    const far = Math.max(Math.hypot(v.w / 2 + Math.abs(o.x), v.h / 2 + Math.abs(o.y)), 1) * npp;
    const n = Math.max(1, Math.min(12, Math.ceil(far / step)));
    const rings = [];
    for (let i = 1; i <= n; i++) {
      const nm = i * step, pts = [];
      for (let k = 0; k <= 128; k++) { const d = destination(rx.lat, rx.lon, k * 360 / 128, nm); pts.push([d.lon, d.lat]); }
      /* ★ FOUR LABELS PER RING — top, right, bottom, left (Stuart, 2026-10-25: "so that no matter where the map is panned
       *  you can see the label"). One at the top went off screen with the receiver panned or zoomed past it. Each sits just
       *  outside its ring on its own side (a: the text's anchor), so the number never sits on the line. */
      rings.push({ nm, pts, lbls: [[0, 'bottom'], [90, 'left'], [180, 'top'], [270, 'right']].map(([b, a]) => Object.assign(destination(rx.lat, rx.lon, b, nm), { a })) });
    }
    return rings;
  }
  /** The GPU map's two sources and two layers for the rings, ids prefixed (e.g. 'vsadsb-', 'vsndb-'). */
  function addRingLayers(gl, idp, T) {
    const empty = { type: 'FeatureCollection', features: [] };
    for (const id of [idp + 'rings', idp + 'ringlbl']) if (!gl.getSource(id)) gl.addSource(id, { type: 'geojson', data: empty });
    gl.addLayer({ id: idp + 'rings', type: 'line', source: idp + 'rings',
                  paint: { 'line-color': T.ring, 'line-width': 1, 'line-dasharray': [4, 6] } });
    gl.addLayer({ id: idp + 'ringlbl', type: 'symbol', source: idp + 'ringlbl',
                  layout: { 'text-field': ['get', 't'], 'text-font': ['JetBrains Mono Regular'], 'text-size': 10, 'text-anchor': ['get', 'a'],
                            'text-offset': ['match', ['get', 'a'], 'bottom', ['literal', [0, -0.3]], 'top', ['literal', [0, 0.3]],
                                            'left', ['literal', [0.4, 0]], ['literal', [-0.4, 0]]],
                            'text-allow-overlap': true, 'text-ignore-placement': true },
                  paint: { 'text-color': T.ringLbl, 'text-halo-color': T.halo, 'text-halo-width': 1.2 } });
  }
  /* ★★ V12 beta94 THE RANGE OUTLINE (Stuart, 2026-11-25: "a jagged ring around the receiver showing the maximum range
   *  detected" — Virtual Radar Server's range plot). The server keeps the furthest believed position per 1° bearing
   *  (vibe_adsb_range.h); this turns its 360 distances (km, 0 = nothing heard that way) into a closed ring of [lon, lat],
   *  each vertex on a BOUNDARY between two bins, at the further of the two (b96-rangefeed, Stuart 11-27: aircraft sat just
   *  outside the line). A vertex on each bin's middle bearing cut every bin's edges short by its lower neighbour — so the
   *  very aircraft that set a bearing's range (the furthest, the ones anyone looks at) were drawn 0.5–4 km OUTSIDE it, a
   *  whole icon at a regional zoom. On the boundaries, both ends of a bin's edge are at least its range: every position
   *  that counted is on or inside the line, still one vertex per bearing (no staircase). A boundary with nothing heard on
   *  either side is skipped — the line joins its neighbours rather than diving to the receiver. Fewer than three: null. */
  function rangeRing(lat, lon, km) {
    if (!isFinite(lat) || !isFinite(lon) || !Array.isArray(km)) return null;
    const pts = [], n = km.length;
    for (let i = 0; i < n; i++) {
      const k = Math.max(Number(km[i]) || 0, Number(km[(i - 1 + n) % n]) || 0);   // bins i-1 and i meet at bearing i
      if (!(k > 0)) continue;
      const d = destination(lat, lon, i * 360 / n, k * 1000 / NM_M);
      pts.push([d.lon, d.lat]);
    }
    if (pts.length < 3) return null;
    pts.push(pts[0].slice());
    return pts;
  }
  /* ★★ beta96 THE OUTLINE IS LABELLED "ADS-B RANGE" (Stuart, 11-26: so nobody takes it for the distance rings). The GPU map
   *  writes it ALONG the outline, repeated (symbol-placement line), so some of it is on screen at every zoom; the Leaflet
   *  fallback puts one label at the outline's FURTHEST vertex — this — just outside it. [lon, lat] or null. */
  const RANGE_LABEL = 'ADS-B RANGE';
  function rangeLabelPoint(lat, lon, ring) {
    if (!Array.isArray(ring) || ring.length < 4 || !isFinite(lat) || !isFinite(lon)) return null;
    let best = null, bestNm = -1;
    for (let i = 0; i < ring.length - 1; i++) {
      const d = distNm(lat, lon, ring[i][1], ring[i][0]);
      if (d > bestNm) { bestNm = d; best = ring[i]; }
    }
    return best ? [best[0], best[1]] : null;
  }
  /* ★★ V12 beta95 THE OUTLINE GROWS LIVE (Stuart, 2026-11-25: "virtual radar does it straight away"). The map extends the
   *  outline itself from every position it already receives — the server's rule (vibe_adsb_range.h + vibe_adsb.h setPos):
   *  a position counts only from an aircraft's SECOND consistent one in a row (the step flown at ≤ 1 200 kt, at least 1 s
   *  assumed), at most RANGE_MAX_KM out, the furthest per 1° bearing. Merged (max per bearing) with the server's stored
   *  outline, which is fetched on open and re-synced slowly. Zero extra requests. */
  const RANGE_BINS = 360, RANGE_MAX_KM = 650, RANGE_MAX_KT = 1200;
  /** Is fix ({lat, lon, t ms}) a believable step on from prev? The server's posOk rule. */
  function rangeConsistent(prev, fix) {
    if (!prev || !fix) return false;
    const dt = Math.max(1, (fix.t - prev.t) / 1000);
    return distNm(prev.lat, prev.lon, fix.lat, fix.lon) / dt * 3600 <= RANGE_MAX_KT;
  }
  /** Offer one believed position to km (RANGE_BINS distances, km) around (cLat, cLon). True = a bearing grew. */
  function rangeOffer(km, cLat, cLon, lat, lon) {
    if (!Array.isArray(km) || ![cLat, cLon, lat, lon].every(isFinite)) return false;
    const d = distNm(cLat, cLon, lat, lon) * NM_M / 1000;
    if (!(d > 0.05) || d > RANGE_MAX_KM) return false;
    const bin = Math.min(km.length - 1, Math.max(0, Math.floor(bearingDeg(cLat, cLon, lat, lon) * km.length / 360)));
    const q = Math.round(d * 10) / 10;
    if (q <= (Number(km[bin]) || 0)) return false;
    km[bin] = q;
    return true;
  }
  /** Max per bearing of two outlines (either may be missing or short). */
  function rangeMerge(a, b) {
    const n = Math.max(Array.isArray(a) ? a.length : 0, Array.isArray(b) ? b.length : 0);
    const out = new Array(n);
    for (let i = 0; i < n; i++) out[i] = Math.max(Number(a && a[i]) || 0, Number(b && b[i]) || 0);
    return out;
  }
  /* ★★ NO NEEDLES (Stuart, 11-25: "once it filled in, the outline is a STAR"): empty bearings are skipped (rangeRing), and
   *  this LIGHT ±2° smoothing lets a bearing lend its neighbours 90 % / 75 % of its range, so one far aircraft is a bump,
   *  not a needle. A bearing's own heard range is never lowered; a smooth stretch of outline is unchanged (a neighbour
   *  is raised only where it sits more than 10 % below the bearing beside it). */
  const RANGE_SMOOTH = [0.9, 0.75];
  function rangeSmooth(km) {
    if (!Array.isArray(km)) return km;
    const n = km.length, out = km.map((v) => Number(v) || 0);
    for (let i = 0; i < n; i++) {
      const v = Number(km[i]) || 0;
      if (!(v > 0)) continue;
      for (let d = 1; d <= RANGE_SMOOTH.length; d++) {
        const w = v * RANGE_SMOOTH[d - 1];
        const a = (i + d) % n, b = (i - d + n) % n;
        if (out[a] < w) out[a] = w;
        if (out[b] < w) out[b] = w;
      }
    }
    return out;
  }
  /* ★★★ beta96 THE OUTLINE'S SHAPE (Stuart 11-26: "still a STAR" on the Sony and Almere — per-1° max plus beta95's ±2°
   *  lend gives long thin needles to 150+ NM and deep dips between them where the data is sparse). Three steps, on the
   *  merged per-bearing maxima (the live-grown outline and the server's stored one go through the SAME rule):
   *  1. EMPTY SECTORS stay empty: a run of RANGE_GAP_DEG or more bearings with nothing heard (the sea, a hill) is a GAP —
   *     no range is invented there; the outline closes across it from the last heard bearing on each side (a chord,
   *     never a spoke to the receiver).
   *  2. FILL DIPS: a morphological CLOSE over the bearings (dilate, then erode, window ±RANGE_CLOSE_DEG, wrapping at 360°,
   *     gaps ignored), so a bearing that just saw fewer aircraft between two well-heard ones is lifted to their level.
   *  3. SOFTEN NEEDLES: each bearing lends its neighbours its range less RANGE_TAPER of it per degree, out to
   *     ±RANGE_SPREAD_DEG, and the outline is the max of those — one far aircraft makes a broad bump, not a needle.
   *  Every step only raises a bearing, so each bearing's true furthest point (the FURTHEST chip's aircraft) stays ON the
   *  outline. Gap bearings stay 0. */
  const RANGE_GAP_DEG = 10, RANGE_CLOSE_DEG = 4, RANGE_TAPER = 0.03, RANGE_SPREAD_DEG = 8;
  function rangeShape(km) {
    if (!Array.isArray(km)) return km;
    const n = km.length, v = km.map((x) => Math.max(0, Number(x) || 0));
    if (!v.some((x) => x > 0)) return v;
    const per = n / 360, gapN = Math.max(1, Math.round(RANGE_GAP_DEG * per));
    const cw = Math.max(1, Math.round(RANGE_CLOSE_DEG * per)), sw = Math.max(1, Math.round(RANGE_SPREAD_DEG * per));
    // 1. the gaps: runs of ≥ gapN empty bearings (found around the circle, starting just after a heard one)
    const gap = new Array(n).fill(false);
    let start = v.findIndex((x) => x > 0);
    for (let k = 1, run = []; k <= n; k++) {
      const i = (start + k) % n;
      if (v[i] > 0) { if (run.length >= gapN) for (const j of run) gap[j] = true; run = []; }
      else run.push(i);
    }
    // 2. CLOSE: dilate over the heard (and short-dip) bearings, then erode — both ignoring the gaps
    const dil = new Array(n).fill(0);
    for (let i = 0; i < n; i++) {
      if (gap[i]) continue;
      let m = 0;
      for (let d = -cw; d <= cw; d++) { const j = (i + d + n) % n; if (!gap[j] && v[j] > m) m = v[j]; }
      dil[i] = m;
    }
    const clo = new Array(n).fill(0);
    for (let i = 0; i < n; i++) {
      if (gap[i]) continue;
      let m = Infinity;
      for (let d = -cw; d <= cw; d++) { const j = (i + d + n) % n; if (!gap[j] && dil[j] < m) m = dil[j]; }
      clo[i] = Math.max(v[i], m === Infinity ? 0 : m);
    }
    // 3. SOFTEN: the max of every bearing's tapered lend (never into a gap)
    const out = clo.slice();
    for (let i = 0; i < n; i++) {
      const r = clo[i];
      if (!(r > 0)) continue;
      for (let d = 1; d <= sw; d++) {
        const w = r * (1 - RANGE_TAPER * d / per);
        if (!(w > 0)) break;
        const a = (i + d) % n, b = (i - d + n) % n;
        if (!gap[a] && out[a] < w) out[a] = w;
        if (!gap[b] && out[b] < w) out[b] = w;
      }
    }
    return out;
  }
  /** Put rings (or none: null) on the GPU map's ring sources. */
  function setRingData(gl, idp, rings) {
    const s1 = gl.getSource(idp + 'rings'), s2 = gl.getSource(idp + 'ringlbl');
    if (!s1 || !s2) return;
    s1.setData({ type: 'FeatureCollection', features: (rings || []).map((r) => ({ type: 'Feature', properties: {}, geometry: { type: 'LineString', coordinates: r.pts } })) });
    s2.setData({ type: 'FeatureCollection', features: (rings || []).flatMap((r) => r.lbls.map((l) => ({ type: 'Feature', properties: { t: r.nm + ' NM', a: l.a }, geometry: { type: 'Point', coordinates: [l.lon, l.lat] } }))) });
  }
  /** The Leaflet fallback's rings: a layer group (the caller adds and removes it). cls: the label class prefix. */
  function leafletRings(LL, rx, rings, T, cls) {
    const g = LL.layerGroup();
    for (const r of rings) {
      LL.circle([rx.lat, rx.lon], { radius: r.nm * NM_M, color: T.ring, weight: 1, fill: false, dashArray: '4 6', interactive: false }).addTo(g);
      for (const l of r.lbls) LL.marker([l.lat, l.lon], { interactive: false, icon: LL.divIcon({ className: '', html: '<div class="' + cls + ' ' + cls + '-' + l.a + '">' + r.nm + ' NM</div>', iconSize: [0, 0] }) }).addTo(g);
    }
    return g;
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

  /** ★★ AN OBSTACLE IN A CORNER (the live list) — the cheaper of two insets: a left (or right) margin as wide as it,
   *  or a bottom (or top) margin as tall as it, whichever leaves more of the view. obs = { corner:'bottom-left'|…,
   *  w, h } in CSS px, or null. Returns { l, r, t, b }. */
  function insetFor(obs, w, h) {
    const z = { l: 0, r: 0, t: 0, b: 0 };
    if (!obs || !(obs.w > 0) || !(obs.h > 0) || !(w > 0) || !(h > 0)) return z;
    const c = String(obs.corner || 'bottom-left');
    const side = c.indexOf('right') >= 0 ? 'r' : 'l', end = c.indexOf('top') >= 0 ? 't' : 'b';
    if (obs.w / w <= obs.h / h) z[side] = Math.min(obs.w, w * 0.6); else z[end] = Math.min(obs.h, h * 0.6);
    return z;
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
    dark:  { fresh: '80,255,120', aging: '180,255,80', stale: '255,190,40', ground: '150,160,170', alert: '255,80,60', military: '90,170,255' },
    light: { fresh: '20,140,60', aging: '110,150,20', stale: '190,120,0', ground: '90,100,110', alert: '210,40,30', military: '20,90,200' },
  };
  /** ★★ MILITARY (Stuart, 2026-10-24: "a colour for military if possible") — blue, clear of the age tones, the ground
   *  grey and the emergency red. ADS-B carries no military flag, so it is recognised as the trackers do, from two
   *  clues, and only where we are SURE: an ICAO address in a block a state keeps for its military — the UK's 43C000–
   *  43CFFF and the US's AE0000–AFFFFF (more can be added as they are checked) — or a callsign prefix of a military
   *  operator. Many military aircraft fly with these hidden; it never marks one that is not. */
  const MIL_BLOCKS = [[0x43C000, 0x43CFFF], [0xAE0000, 0xAFFFFF]];
  const MIL_CALLS = /^(RRR|ASCOT|TARTN|RCH|NATO|GAF|CTM|IAM|BAF)\d/;
  function isMilitary(a) {
    // ★★ THE SERVER SAYS FIRST (V12 aircraft data: its own Annex 10 + VRS block table and the Mictronics flag — mil:1),
    //    else the old rule below, which a server without the data (or an older one) still relies on
    if (a && (a.mil === 1 || a.mil === true)) return true;
    const h = parseInt(String(a.hex || '').replace(/^~/, ''), 16);
    if (h === h && String(a.hex || '')[0] !== '~') for (const [lo, hi] of MIL_BLOCKS) if (h >= lo && h <= hi) return true;
    return MIL_CALLS.test(String(a.flight || '').trim().toUpperCase());
  }
  function toneKey(a) {
    if (emergencyOf(a)) return 'alert';
    if (onGround(a)) return 'ground';
    if (isMilitary(a)) return 'military';
    const age = Number(a.seen_pos) || 0;
    /* ★★ V12 beta90 THE AVIATION MAP's datalink aircraft (a.sparse — ACARS, VDL2, HFDL, ADS-C: a fix minutes apart, not
     *  twice a second): the same three tones on their own clock — under 2 min, under 10 min, older. */
    if (a.sparse) return age < 120 ? 'fresh' : age < 600 ? 'aging' : 'stale';
    return age < 10 ? 'fresh' : age < 30 ? 'aging' : 'stale';
  }

  /** ★★ THE ALTITUDE ARROW (Stuart, 2026-10-24): beside each label's altitude, ▲ climbing / ▼ descending / ● level
   *  (within ±300 ft/min), coloured by ALTITUDE the way other ADS-B maps colour their aircraft — fading through the
   *  spectrum, orange-red near the ground to violet at FL400. The aircraft keep their own age / emergency / military
   *  colours; the arrow carries the altitude. Nothing for an aircraft on the ground or with no altitude.
   *  ★ The glyphs are in the map font's 9472–9727 range (checked in the pbf), or MapLibre would draw nothing. */
  const ALT_HUES = [[0, 15], [4000, 55], [10000, 120], [20000, 180], [30000, 235], [40000, 285]];
  function altHue(ft) {
    if (!(ft >= 0)) ft = 0;
    for (let i = 1; i < ALT_HUES.length; i++) {
      const [a0, h0] = ALT_HUES[i - 1], [a1, h1] = ALT_HUES[i];
      if (ft <= a1) return h0 + (h1 - h0) * (ft - a0) / (a1 - a0);
    }
    return ALT_HUES[ALT_HUES.length - 1][1];
  }
  function altColour(ft, theme) { return 'hsl(' + altHue(ft).toFixed(0) + ',90%,' + (theme === 'light' ? '38%' : '62%') + ')'; }
  function trendOf(a) {
    if (!a || a.alt_baro === 'ground') return '';
    const alt = num(a.alt_baro) != null ? num(a.alt_baro) : num(a.alt_geom);
    if (alt == null) return '';
    const vr = num(a.baro_rate) != null ? num(a.baro_rate) : num(a.geom_rate);
    return vr != null && vr > 300 ? '\u25B2' : vr != null && vr < -300 ? '\u25BC' : '\u25CF';
  }

  /* ══ WHAT THE AIRCRAFT IS (V12 aircraft data — the server's adsb_info fields, merged into each aircraft by the host) ══
   * r, t, desc, wtc, eng, species, country, countryName, mil, op, opName, logo, sil, route {from, to, fromIata, toIata,
   * fromName, toName, fromPlace, toPlace, stops, plausible}. All optional: an older server sends none, and the card
   * is exactly what it was. */
  const WAKE = { L: 'Light', M: 'Medium', H: 'Heavy', J: 'Super' };
  const ENGINE = { J: 'jet', T: 'turboprop', P: 'piston', E: 'electric', R: 'rocket' };
  const SPECIES = { H: 'helicopter', S: 'seaplane', A: 'amphibian', G: 'gyrocopter', T: 'tiltrotor' };
  function engText(e) {
    const m = /^([1-8C])([JTPER])?$/.exec(String(e || ''));
    if (!m) return '';
    const n = m[1] === 'C' ? 'coupled' : m[1];
    return n + (m[2] ? ' \u00d7 ' + ENGINE[m[2]] : (m[1] === '1' ? ' engine' : ' engines'));
  }
  /** ★ A flag from the ISO code as regional-indicator emoji — no assets (the page is embedded in the server, and
   *  ~250 SVG flags would be most of a web page per listener). Windows shows the two letters instead of a flag, so
   *  there the caller shows the code as text (flagOrCode). */
  function flagEmoji(iso) {
    const c = String(iso || '').toUpperCase();
    if (!/^[A-Z]{2}$/.test(c)) return '';
    return String.fromCodePoint(0x1F1E6 + c.charCodeAt(0) - 65, 0x1F1E6 + c.charCodeAt(1) - 65);
  }
  function noEmojiFlags() { try { return /Windows/.test(global.navigator && global.navigator.userAgent || ''); } catch (e) { return false; } }
  function flagOrCode(iso) { const c = String(iso || '').toUpperCase(); return /^[A-Z]{2}$/.test(c) ? (noEmojiFlags() ? c : flagEmoji(c)) : ''; }
  /** "AMS → STN" — IATA when both ends have one, else ICAO; '' when there is no route or it is NOT plausible. */
  function routeShort(r) {
    if (!r || typeof r !== 'object' || r.plausible === false) return '';
    const a = r.fromIata && r.toIata ? r.fromIata : r.from, b = r.fromIata && r.toIata ? r.toIata : r.to;
    return a && b ? String(a) + ' \u2192 ' + String(b) : '';
  }
  /** "London Luton Airport" → "London Luton": the words every airport's name ends with carry nothing on a card. */
  function shortAirport(n) { return String(n || '').replace(/\s+(International\s+)?(Airport|Airfield|Aerodrome|Air Base)$/i, '').trim(); }
  /** The operator's text badge: the ICAO designator, else nothing (a private operator has none). */
  function opBadge(a) { return a && /^[A-Z0-9]{2,4}$/.test(String(a.op || '')) ? String(a.op) : ''; }
  /** Only keys the server can have named: letters, digits, '-', '_' (vibe_adsb_data.h Images::validKey). */
  function imgKey(k) { const v = String(k || ''); return /^[A-Za-z0-9_-]{1,40}$/.test(v) ? v : ''; }

  /* ══ THE TRAIL — the selected aircraft's track, coloured by its altitude ═══════════════════════════
   * ★★ Stuart, 2026-10-24: "clicking an aircraft should draw its track in as a rainbow coloured line to show its
   *    altitude … approaching Heathrow I should see a line changing from purple down to orange … a nice blended
   *    colour … It should flow … only show the trail for the selected aircraft or the map will get too messy".
   *    So: ONE line, the selected aircraft's, in the altitude arrows' own colours (altHue — one key for both), BLENDED
   *    along its length (MapLibre line-gradient over line-progress), its head on the gliding icon, and drawn in from
   *    its oldest end when it is picked. The server keeps the whole trail (vibe_adsb.h kTrailStepNm, GET
   *    /adsb/trail/<hex>); the layer keeps its own from the reports too, so a server without one (OWRX) still has
   *    the part since the map opened. ★ Same step rule as the server's: 0.3 NM or 250 ft. */
  const TRAIL_STEP_NM = 0.3, TRAIL_STEP_FT = 250, TRAIL_MAX = 1000, TRAIL_STOPS = 96, TRAIL_DRAW_MS = 1600;
  /** Altitude of a report for the trail: feet, 0 on the ground, null unknown. */
  function trailAlt(a) {
    if (!a) return null;
    if (a.alt_baro === 'ground') return 0;
    const v = num(a.alt_baro) != null ? num(a.alt_baro) : num(a.alt_geom);
    return v != null ? v : null;
  }
  /** Add [lat, lon, alt] to a trail by the step rule; true if it was added. Halves itself at TRAIL_MAX. */
  function trailPush(tr, lat, lon, alt) {
    const l = tr.length ? tr[tr.length - 1] : null;
    if (l && distNm(l[0], l[1], lat, lon) < TRAIL_STEP_NM && !(alt != null && l[2] != null && Math.abs(alt - l[2]) >= TRAIL_STEP_FT)) return false;
    if (tr.length >= TRAIL_MAX) { let w = 0; for (let k = 0; k < tr.length; k += 2) tr[w++] = tr[k]; tr.length = w; }
    tr.push([lat, lon, alt]);
    return true;
  }
  /** The line-gradient for a trail (each point [lat, lon, alt|null]): the colour at each point's share of the line's
   *  length, sampled to at most TRAIL_STOPS stops; 'reveal' 0..1 = how much of it is drawn yet (the draw-in), the rest
   *  transparent. An unknown altitude takes the last known one. Returns null when there is no line to draw. */
  function trailGradient(pts, theme, reveal) {
    if (!pts || pts.length < 2) return null;
    const cum = [0];
    for (let i = 1; i < pts.length; i++) cum.push(cum[i - 1] + distNm(pts[i - 1][0], pts[i - 1][1], pts[i][0], pts[i][1]));
    const len = cum[cum.length - 1];
    if (!(len > 0)) return null;
    let known = null;
    for (const p of pts) if (p[2] != null) { known = p[2]; break; }
    if (known == null) known = 0;
    const cols = pts.map((p) => { if (p[2] != null) known = p[2]; return altColour(known, theme); });
    const pick = [];
    const every = Math.max(1, Math.ceil(pts.length / TRAIL_STOPS));
    for (let i = 0; i < pts.length; i += every) pick.push(i);
    if (pick[pick.length - 1] !== pts.length - 1) pick.push(pts.length - 1);
    const rv = reveal == null ? 1 : Math.max(0, Math.min(1, reveal));
    const e = ['interpolate', ['linear'], ['line-progress']];
    let last = -1;
    for (const i of pick) {
      const f = cum[i] / len;
      if (f <= last) continue;                       // ★ stops must rise strictly
      if (rv < 1 && f > rv) break;
      e.push(f, cols[i]); last = f;
    }
    if (rv < 1) {
      const head = Math.max(last + 1e-6, rv);
      if (head < 1) { e.push(head, 'rgba(0,0,0,0)', 1, 'rgba(0,0,0,0)'); }
    }
    if (e.length < 7) e.push(1, e[e.length - 1]);    // ★ interpolate needs two stops
    return e;
  }

  const maths = { GLIDE_MS, RING_MS, RING_REPEATS, BREATHE_MS, MAX_DR_S, SNAP_NM, SETTLE_MS, TRANSITION_FT,
                  wrapLon, distNm, bearingDeg, destination, rangeRing, rangeLabelPoint, RANGE_LABEL, rangeConsistent, rangeOffer, rangeMerge, rangeSmooth, rangeShape, RANGE_GAP_DEG, RANGE_CLOSE_DEG, RANGE_TAPER, RANGE_SPREAD_DEG, RANGE_BINS, cardinal, deadReckon, iconFor, iconClassOf, altText, altShort,
                  vsText, vsLabel, vsCardText, emergencyOf, nmPerPx, ringStep, RING_LADDER, merc, unmerc, cameraFor, screenOffset,
                  anyOutside, AutoZoom, placeLabels, easeInOut, easeOut, breath, angleTo, toneKey, isMilitary, insetFor, altHue, trendOf, altColour, ALT_HUES,
                  engText, flagEmoji, routeShort, opBadge, imgKey, WAKE, shortAirport,
                  trailPush, trailGradient, trailAlt, TRAIL_STEP_NM, TRAIL_STEP_FT, TRAIL_MAX, commbRows };

  /* ══ ACICONS — BEGIN ═══════════════════════════════════════════════════════════════════════════
   * ★★ beta96 THE AIRCRAFT GLYPHS AND THE ONE RULE THAT PICKS THEM — self-contained on purpose: scripts/build-community-kit.mjs
   *    lifts everything between BEGIN and END into the community map's kit (directory/public/community-kit.js), so the
   *    directory draws the very shapes this layer draws, chosen by the very same rule. Nothing in here may use a name defined
   *    outside it (test_adsb_layer.mjs evaluates the block on its own). */
  /** ★ DO-260B emitter category → icon (docs/v12/ADSB.md). A0 / none / anything unknown = the generic
   *  jet: most of the sky that does not say what it is, is an airliner. */
  const CAT_ICON = { A1: 'light', A2: 'regional', A3: 'jet', A4: 'b757', A5: 'heavy', A6: 'fighter',
                     A7: 'heli', B1: 'glider', B2: 'balloon', B3: 'parachute', B4: 'ultralight',
                     B6: 'uav', B7: 'space', C1: 'ground', C2: 'ground', C3: 'obstacle' };
  function iconFor(category) {
    const c = typeof category === 'string' ? category.trim().toUpperCase() : '';
    return CAT_ICON[c] || 'jet';
  }

  /** ★★ beta96 WHICH GLYPH AN AIRCRAFT DRAWS — ONE RULE, EVERY SOURCE, EVERY MAP (Stuart 11-26: "the correct icon for the
   *  aircraft type like the ADS-B map … I hope it's not just a generic plane icon"). Before this only an aircraft whose
   *  ADS-B said its emitter category got a class glyph; a VDL2 / ACARS / HFDL / ADS-C aircraft, a remembered one and every
   *  aircraft on the community map drew the generic jet.
   *  A is an ADS-B aircraft object (category, t, desc, wtc, eng, species, mil) or an aviation-map / community row (type in
   *  place of t). In order:
   *   1. the DO-260B emitter category, when it says something (A0 / none / unknown says nothing);
   *   2. the aircraft data (ICAO Doc 8643, as the server's tables carry it): GLID → glider, BALL / SHIP → balloon,
   *      description letter H / G → helicopter, a known fast-jet designator → fighter, B752 / B753 → the 757, wake J or H →
   *      heavy, engines "…T" → regional (the turboprop class), "…P" / "…E" → light, wake L → light (a light JET → regional);
   *   3. with no wake / engines / description (an older server sends the type alone): the designator's own family —
   *      wide-bodies heavy, the common helicopters, turboprops and light singles;
   *   4. else the generic jet: most of the sky that does not say what it is, is an airliner. */
  const FIGHTER_TYPES = /^(F4|F5|F14|F15|F16|F18[A-Z]?|F22|F35|F1|A10|AV8B|HARR|EUFI|TOR|RFAL|MIR2|M2KF?|JAGR|GRIF|JS39|HAWK|T38|T50|L39|L159|M346|AJET|K8|MG2[39]|MG31|SU2[457]|SU3[0-5]|SU57|J10|JF17)$/;
  const HEAVY_TYPES = /^(A3[3-5]\d|A38\d|B74\w|B76\w|B77\w|B78\w|MD11|DC10|L101|IL62|IL76|IL86|IL96|A124|A225|C5M?|C17|K35R|KC10|A400|E3TF|E3CF|B52)$/;
  const HELI_TYPES = /^(EC\d\d|H1[2-7]\d|H160|AS\d\d|A1[0-9]9|A119|S76|S92|S61|R22|R44|R66|B06|B47G|B4[0-2]\d|B505|B212|B412|NH90|EH10|LYNX|WILD|H60|UH1|CH47|H47|MI8|MI17|MI24|MI26|KA\d\d|GAZL|PUMA|SUPU|EXPL|AW09|GYRO)$/;
  const PROP_TYPES = /^(AT4\d|AT7\d|DH8\w|SF34|SB20|JS31|JS32|JS41|D328|F50|E120|SW4|BE20|BE30|B350|BE9L|PC12|DHC6|C208|C130|C30J|AN2\d|AN12|AN26|AN30|AN32|L410|DO28|P180|TBM\d|PC21|CN35|C295)$/;
  const LIGHT_TYPES = /^(C1[5-9]\d|C2[0-1]\d|P28\w|PA\d\d\w?|SR2[02]|DA[24]\d\w?|DR40|BE3[356]|BE55|BE58|M20P|M20T|RV\d+\w?|TB\d\d|AA5|C42|CTSW|PIVI|ULAC)$/;
  function iconClassOf(a) {
    if (!a || typeof a !== 'object') return 'jet';
    const cat = typeof a.category === 'string' ? a.category.trim().toUpperCase() : '';
    if (CAT_ICON[cat]) return CAT_ICON[cat];
    const t = String(a.t || a.type || '').trim().toUpperCase();
    const desc = String(a.desc || '').toLowerCase();
    const wtc = String(a.wtc || '').trim().toUpperCase();
    const sp = String(a.species || '').trim().toUpperCase();
    const em = /^[1-8C]?([JTPER])$/.exec(String(a.eng || '').trim().toUpperCase());
    const eng = em ? em[1] : '';
    if (t === 'GLID' || /\b(glider|sailplane)\b/.test(desc)) return 'glider';
    if (t === 'BALL' || t === 'SHIP' || /\b(balloon|airship)\b/.test(desc)) return 'balloon';
    if (sp === 'H' || sp === 'G') return 'heli';
    if (FIGHTER_TYPES.test(t)) return 'fighter';
    if (t === 'B752' || t === 'B753') return 'b757';
    if (t === 'ULAC') return 'ultralight';
    if (wtc === 'J' || wtc === 'H') return 'heavy';
    if (eng === 'T') return 'regional';
    if (eng === 'P' || eng === 'E') return 'light';
    if (wtc === 'L') return eng === 'J' ? 'regional' : 'light';
    if (!wtc && !eng && !sp && t) {
      if (HEAVY_TYPES.test(t)) return 'heavy';
      if (HELI_TYPES.test(t)) return 'heli';
      if (PROP_TYPES.test(t)) return 'regional';
      if (LIGHT_TYPES.test(t)) return 'light';
    }
    return 'jet';
  }

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
    /* ★ A Cessna's proportions from above: wing 11 m, tailplane 3.4 m, propeller 1.9 m. The prop was 7 of the
     *  wing's 24 against a 9 tail, and read as a second tailplane on the nose (Stuart, 2026-10-24: "the propellor is
     *  almost as big as the tail, this looked really odd"). Now ~a sixth of the wing, half the tail, at the nose. */
    light: [
      { f: ell(20, 21, 1.8, 7) },
      { f: 'M8,17.5H32V20.5H8Z' },
      { f: 'M16,26.5H24V28.5H16Z' },
      { s: 'M18,13.6H22', w: 1.1 },
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

  /** One icon as SVG: FILL any CSS colour, SIZE CSS px square (the 40-unit box scaled), rotated ROT° about its centre
   *  (never for the NO_ROTATE ones). The parts as iconImage paints them: filled paths with a faint dark edge, strokes plain. */
  function acIconSvg(kind, fill, size, rot) {
    const parts = ICONS[kind] || ICONS.jet;
    let s = '<svg xmlns="http://www.w3.org/2000/svg" width="' + size + '" height="' + size + '" viewBox="0 0 40 40">'
          + '<g transform="rotate(' + (NO_ROTATE[kind] ? 0 : (rot || 0).toFixed(1)) + ',20,20)">';
    for (const p of parts) {
      const op = p.o != null ? ' opacity="' + p.o + '"' : '';
      if (p.f) s += '<path d="' + p.f + '" fill="' + (p.dark ? 'rgba(0,0,0,0.55)' : fill) + '" stroke="rgba(0,0,0,0.3)" stroke-width="0.6"' + op + '/>';
      else s += '<path d="' + p.s + '" fill="none" stroke="' + fill + '" stroke-width="' + p.w + '" stroke-linecap="round"' + op + '/>';
    }
    return s + '</g></svg>';
  }
  /** What each class is, in words — the KEY's rows (the community map's legend; ADSB.md's table). */
  const AC_CLASS_NAME = { jet: 'Airliner', regional: 'Regional jet or turboprop', b757: 'Boeing 757', heavy: 'Heavy (wide-body)',
                          light: 'Light aircraft', fighter: 'Fast jet', heli: 'Helicopter or gyroplane', glider: 'Glider',
                          balloon: 'Balloon or airship', parachute: 'Parachutist', ultralight: 'Ultralight', uav: 'Drone (UAV)',
                          space: 'Space vehicle', ground: 'Ground vehicle', obstacle: 'Obstacle' };
  /* ══ ACICONS — END ══ */

  function iconSvg(kind, rgb, alpha, rot) {
    return acIconSvg(kind, 'rgba(' + rgb + ',' + alpha + ')', ICON_BOX, rot);
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
             label: 'rgba(225,238,228,0.88)', halo: 'rgba(0,0,0,0.9)', range: '255,56,210' },
    light: { bg: '250,247,240', ink: '120,78,0', hi: '60,40,0', border: '160,100,0', ring: 'rgba(150,90,0,0.40)', ringLbl: 'rgba(120,78,0,0.75)',
             label: 'rgba(30,30,30,0.9)', halo: 'rgba(255,255,255,0.95)', range: '205,0,150' },
  };
  function css(th) {
    const T = THEME[th];
    const p = '.vsadsb-' + th;
    return p + '.vsadsb-leg{position:absolute;left:10px;bottom:10px;z-index:6;max-width:270px;padding:9px 11px 8px;'
      + 'background:rgba(' + T.bg + ',0.95);border:1px solid rgba(' + T.border + ',0.30);border-radius:8px;'
      + 'font:10.5px ui-monospace,"SF Mono",Menlo,Consolas,monospace;color:rgba(' + T.ink + ',0.85);box-shadow:0 6px 24px rgba(0,0,0,0.45)}'
      + p + '.vsadsb-leg .t{letter-spacing:1.5px;opacity:0.75;margin:6px 0 3px}' + p + '.vsadsb-leg .t:first-child{margin-top:0}'
      + p + '.vsadsb-leg .r{display:flex;align-items:center;gap:7px;line-height:17px;white-space:nowrap}'
      + p + '.vsadsb-leg .g{display:inline-block;width:12px;text-align:center;font-size:12px}'
      + p + '.vsadsb-leg .bar{height:7px;border-radius:4px;margin:5px 0 2px}'
      + p + '.vsadsb-leg .ax{display:flex;justify-content:space-between;opacity:0.7;font-size:9.5px}'
      + p + '.vsadsb-ctl{position:absolute;top:10px;right:10px;z-index:5;display:flex;gap:0;'
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
      /* ★★ THE CARD SCROLLS INSIDE ITS OWN BOX, NOT THE POPUP'S (Stuart, 2026-11-25: "sometimes the x button to close it
       *  greys out. work around is to minimise it and then press x"). The scroller used to be MapLibre's popup content —
       *  the very element MapLibre puts its x in, at top:0 right:0. A card taller than the box (a phone, a short window,
       *  Comm-B rows, the aviation map's source lines) is a scroller, and an OVERLAY scrollbar (macOS, Edge and Safari
       *  with a trackpad) is drawn over its right edge: the grey thumb sat on the x and took the press. Folded, the card
       *  does not scroll, so its x worked. Scrolled, the x scrolled away with the text. Now the text scrolls in
       *  .vsadsb-sc, the x and the fold key sit above it (z-index) and never move. */
      + '.vsadsb-pop-' + th + ' .vsadsb-sc{max-height:min(46vh,360px);overflow-y:auto;overscroll-behavior:contain}'
      // ★ MORE ABOVE / BELOW (moreCue): zero-height sticky rows, a small pill in the readout colour centred on the edge
      + '.vsadsb-pop-' + th + ' .vsadsb-more{position:sticky;height:0;z-index:3;pointer-events:none;display:none}'
      + '.vsadsb-pop-' + th + ' .vsadsb-more.up{top:0}.vsadsb-pop-' + th + ' .vsadsb-more.dn{bottom:0}'
      + '.vsadsb-pop-' + th + ' .vsadsb-more-up .vsadsb-more.up,.vsadsb-pop-' + th + ' .vsadsb-more-dn .vsadsb-more.dn{display:block}'
      + '.vsadsb-pop-' + th + ' .vsadsb-more span{position:absolute;left:50%;transform:translateX(-50%);font-size:10px;line-height:14px;'
      + 'padding:0 10px;border-radius:7px;color:rgba(' + T.hi + ',0.95);background:rgba(' + T.bg + ',0.96);border:1px solid rgba(' + T.border + ',0.35)}'
      + '.vsadsb-pop-' + th + ' .vsadsb-more.up span{top:0}.vsadsb-pop-' + th + ' .vsadsb-more.dn span{bottom:0}'
      + '.vsadsb-pop-' + th + ' .leaflet-popup-tip{background:rgba(' + T.bg + ',0.96)}'
      + '.vsadsb-pop-' + th + '.maplibregl-popup-anchor-bottom .maplibregl-popup-tip{border-top-color:rgba(' + T.bg + ',0.96)}'
      + '.vsadsb-pop-' + th + '.maplibregl-popup-anchor-top .maplibregl-popup-tip{border-bottom-color:rgba(' + T.bg + ',0.96)}'
      + '.vsadsb-pop-' + th + '.maplibregl-popup-anchor-left .maplibregl-popup-tip{border-right-color:rgba(' + T.bg + ',0.96)}'
      + '.vsadsb-pop-' + th + '.maplibregl-popup-anchor-right .maplibregl-popup-tip{border-left-color:rgba(' + T.bg + ',0.96)}'
      + '.vsadsb-pop-' + th + ' .maplibregl-popup-close-button,.vsadsb-pop-' + th + ' .leaflet-popup-close-button{color:rgba(' + T.ink + ',0.5);font-size:16px;z-index:4}'
      + '.vsadsb-pop-' + th + ' .maplibregl-popup-close-button:hover,.vsadsb-pop-' + th + ' .leaflet-popup-close-button:hover{color:rgba(' + T.hi + ',0.95)}'
      + '.vsadsb-pop-' + th + ' .t{font-size:15px;font-weight:600;color:rgba(' + T.hi + ',1);margin-bottom:6px;letter-spacing:0.8px;padding-right:38px}'
      + '.vsadsb-pop-' + th + ' .t.mn{margin:0}'
      + '.vsadsb-pop-' + th + ' .vsadsb-min{position:absolute;z-index:4;top:3px;right:24px;background:none;border:0;padding:2px 6px;cursor:pointer;'
      + 'font:16px ui-monospace,"SF Mono",Menlo,Consolas,monospace;line-height:1;color:rgba(' + T.ink + ',0.5)}'
      + '.vsadsb-pop-' + th + ' .vsadsb-min:hover{color:rgba(' + T.hi + ',0.95)}'
      + '.vsadsb-pop-' + th + ' .t i{font-style:normal;font-size:12px;font-weight:400;color:rgba(' + T.ink + ',0.65);margin-left:6px}'
      + '.vsadsb-pop-' + th + ' table{border-collapse:collapse}'
      + '.vsadsb-pop-' + th + ' td{padding:1px 0;vertical-align:top}'
      + '.vsadsb-pop-' + th + ' td:first-child{color:rgba(' + T.ink + ',0.65);padding-right:10px;white-space:nowrap}'
      + '.vsadsb-pop-' + th + ' td:last-child{color:rgba(' + T.ink + ',0.95)}'
      + '.vsadsb-pop-' + th + ' .em{color:#ff6a50;font-weight:600;letter-spacing:1px;margin-bottom:4px}'
      // ★ V12 Comm-B rows: their heading line, the TCAS advisory, a dim age
      + '.vsadsb-pop-' + th + ' td.sec{padding-top:5px;font-size:11px;color:rgba(' + T.ink + ',0.65)}'
      + '.vsadsb-pop-' + th + ' td .ra{color:#ff6a50;font-weight:600}'
      + '.vsadsb-pop-' + th + ' td .d{color:rgba(' + T.ink + ',0.65);font-size:11px}'
      // ★ V12 aircraft data: the operator line (logo or text badge), the route, the photo and the credits
      + '.vsadsb-pop-' + th + ' .op{display:flex;align-items:center;gap:8px;margin:-2px 0 6px;min-height:20px}'
      + '.vsadsb-pop-' + th + ' .op img{max-width:85px;max-height:20px;object-fit:contain}'
      + '.vsadsb-pop-' + th + ' .badge{border:1px solid rgba(' + T.border + ',0.55);border-radius:3px;padding:0 4px;font-size:11px;letter-spacing:1px;color:rgba(' + T.hi + ',0.95)}'
      + '.vsadsb-pop-' + th + ' .sil{display:block;max-width:85px;max-height:20px;object-fit:contain;margin-left:auto}'
      + '.vsadsb-pop-' + th + ' .idb{margin:0 0 7px;padding-bottom:6px;border-bottom:1px solid rgba(' + T.border + ',0.18);line-height:16px}'
      + '.vsadsb-pop-' + th + ' .idb .l{color:rgba(' + T.ink + ',0.95)}'
      + '.vsadsb-pop-' + th + ' .idb .d,.vsadsb-pop-' + th + ' .idb .l.d{color:rgba(' + T.ink + ',0.65);font-size:12px}'
      + '.vsadsb-pop-' + th + ' .idb .rt{color:rgba(' + T.hi + ',0.95)}'
      + '.vsadsb-pop-' + th + ' .off{text-decoration:line-through;opacity:0.6}'
      + '.vsadsb-pop-' + th + ' .ph{display:block;margin:8px 0 0}.vsadsb-pop-' + th + ' .ph img{display:block;width:100%;max-width:256px;border-radius:4px}'
      + '.vsadsb-pop-' + th + ' .ph span{display:block;font-size:10px;opacity:0.75;margin-top:2px}'
      + '.vsadsb-pop-' + th + ' a{color:rgba(' + T.hi + ',0.95)}'
      + '.vsadsb-pop-' + th + ' .cr{margin-top:7px;font-size:9.5px;line-height:12px;opacity:0.6;max-width:256px}'
      // ★ the look-it-up links: one small line above the credits, readable, tappable (line-height for a finger)
      + '.vsadsb-pop-' + th + ' .lk{margin-top:8px;font-size:11px;line-height:18px;max-width:256px}'
      // ★ The Leaflet fallback's icons, labels and the HFDL ring (keyframes verbatim).
      + '.vsadsb-ll{position:relative;width:40px;height:40px}'
      + '.vsadsb-ll svg{position:absolute;left:0;top:0}'
      + '.vsadsb-ll .lb{position:absolute;left:31px;top:12px;font:10px ui-monospace,Menlo,monospace;line-height:11px;white-space:pre;'
      + 'color:' + T.label + ';text-shadow:0 0 3px ' + T.halo + ',0 0 2px ' + T.halo + ';pointer-events:none;transition:opacity .4s}'
      + '.vsadsb-ll .rg{position:absolute;left:20px;top:20px;width:26px;height:26px;border-radius:50%;border:2px solid;pointer-events:none;'
      + 'transform:translate(-50%,-50%);animation:vsadsbRing ' + RING_MS + 'ms ease-out ' + RING_REPEATS + ';opacity:0}'
      + '@keyframes vsadsbRing{0%{transform:translate(-50%,-50%) scale(1);opacity:0.8}100%{transform:translate(-50%,-50%) scale(3);opacity:0}}'
      + '.vsadsb-rl{font:10px ui-monospace,Menlo,monospace;color:' + T.ringLbl + ';white-space:nowrap;transform:translate(-50%,-120%);'
      + 'text-shadow:0 0 3px ' + T.halo + ';pointer-events:none}'
      // ★ the four sides (see the ring labels): above the top, below the bottom, outside right and left
      + '.vsadsb-rl-top{transform:translate(-50%,20%)}.vsadsb-rl-left{transform:translate(6px,-50%)}.vsadsb-rl-right{transform:translate(calc(-100% - 6px),-50%)}';
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
  /* ★★ V12 MODE S COMM-B (vibe_adsb_commb.h): what the aircraft told a ground radar — its selected altitude, heading,
   *  airspeeds, roll, the wind and temperature outside, a collision-avoidance advisory. The server sends a register
   *  only once it is CONFIRMED (never guessed), inside its own "commb" object, so the card can say where it came from. */
  function commbRows(c) {
    const out = [];
    if (!c || typeof c !== 'object') return out;
    const row = (k, v) => { if (v) out.push('<tr><td>' + esc(k) + '</td><td>' + esc(v) + '</td></tr>'); };
    const n = (k) => num(c[k]);
    const tcas = c.acas_ra && typeof c.acas_ra === 'object' ? c.acas_ra : null;
    if (tcas && (tcas.active === true || tcas.terminated === true)) {
      const adv = typeof tcas.advisory === 'string' ? tcas.advisory.slice(0, 40) : '';
      const age = num(tcas.age);
      out.push('<tr><td>TCAS</td><td><span class="ra">' + esc(tcas.active === true ? 'Advisory: ' + (adv || 'active') : 'Advisory ended') + '</span>'
        + (age != null ? ' <span class="d">' + Math.round(age) + ' s ago</span>' : '') + '</td></tr>');
    }
    const sel = n('nav_altitude_mcp') != null ? n('nav_altitude_mcp') : n('nav_altitude_fms');
    const qnh = n('nav_qnh');
    row('Selected', [sel != null ? Math.round(sel).toLocaleString('en-GB') + ' ft' : '', qnh != null ? 'set ' + qnh.toFixed(qnh % 1 ? 1 : 0) + ' hPa' : ''].filter(Boolean).join(' \u00b7 '));
    if (n('mag_heading') != null) row('Heading', Math.round(n('mag_heading')) + '\u00b0 magnetic');
    row('Airspeed', [n('ias') != null ? 'IAS ' + Math.round(n('ias')) + ' kt' : '', n('mach') != null ? 'Mach ' + n('mach').toFixed(2) : '',
                     n('tas') != null ? 'TAS ' + Math.round(n('tas')) + ' kt' : ''].filter(Boolean).join(' \u00b7 '));
    const roll = n('roll');
    if (roll != null) row('Bank', Math.abs(roll) < 1 ? 'wings level' : Math.round(Math.abs(roll)) + '\u00b0 ' + (roll > 0 ? 'right' : 'left'));
    if (n('ws') != null && n('wd') != null) row('Wind', Math.round(n('ws')) + ' kt from ' + Math.round(n('wd')) + '\u00b0' + (n('oat') != null ? ' \u00b7 ' + n('oat').toFixed(0) + ' \u00b0C outside' : ''));
    // ★ where these came from, said once above them
    if (out.length) out.unshift('<tr><td colspan="2" class="sec">From its replies to radar (Mode S)</td></tr>');
    return out;
  }
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
    /* ★★ b96-rangefeed (Stuart 11-27: "i cant activate the range rings either" — RINGS / FIT greyed on a server set up with
     *  only a locator, opened from the front door): the receiver is the host's (its lat/lon, else its locator square's
     *  centre — rxPosition.ts — with its approx words), ELSE the range outline's centre once GET range answers (the server's
     *  own location: the locator's centre there too). rxNote: the words the controls' titles add for a position not exact. */
    let rxNote = rx && opts.receiver && typeof opts.receiver.approx === 'string' ? opts.receiver.approx : '', rxFromRange = false;
    const prefs = Object.assign({ autoZoom: true, fit: true, rings: true, labels: true, range: true, rangeAll: false }, opts.prefs || {});
    /* ★ opts.imageBase: where this server serves the OWNER's logos and silhouettes ("…/adsb/"), or absent = none —
     *   and (beta94) this radio's range outline and the trails.
     *   The server names a key (a.logo / a.sil) only when the file exists, so nothing here hunts through 404s. */
    const imageBase = typeof opts.imageBase === 'string' ? opts.imageBase : null;
    const onPrefs = typeof opts.onPrefs === 'function' ? opts.onPrefs : null;
    /* ★ drawReceiver:false — the host draws its own receiver marker (the app's page already has the HFDL
     *  house); the layer still uses the position for rings, distances and the camera. Default true. */
    const drawReceiver = opts.drawReceiver !== false;
    const onSelect = typeof opts.onSelect === 'function' ? opts.onSelect : null;
    let toldSel = null;
    /** Tell the host when the selection changes — once per change, whoever changed it. */
    function tellSel() {
      if (toldSel === selected) return;
      toldSel = selected;
      if (onSelect) { try { onSelect(selected); } catch (e) { console.error('adsblayer: onSelect', e); } }
    }

    const ac = new Map();          // hex → state
    let firstUpdateAt = 0, firstPosBatchSeen = false;
    let selected = null;
    /* ★ the selected aircraft's trail on the map — see trailGradient. 'at' = when it was picked (the draw-in). */
    const TRAIL_NONE = ['interpolate', ['linear'], ['line-progress'], 0, 'rgba(0,0,0,0)', 1, 'rgba(0,0,0,0)'];
    let trail = { hex: null, at: 0, dataAt: 0, dirty: false, shown: false, ll: null };
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
    // ★ beta96 data-k names each key for a host that hosts the control (ctlHost) and hides some of them (the map cards)
    const mk = (txt, title) => { const b = doc.createElement('button'); b.type = 'button'; b.textContent = txt; b.title = title; b.dataset.k = txt.length === 1 ? 'centre' : txt.toLowerCase(); ctl.appendChild(b); return b; };
    const bAuto = mk('AUTO', 'Auto-zoom: keep every aircraft in view');
    const bFit = mk('FIT', 'Fit to aircraft: the receiver need not be in the middle');
    const bRings = mk('RINGS', 'Range rings in nautical miles around the receiver');
    /* ★★ V12 beta94 RANGE — the range outline (rangeRing). Drawn only once this server has answered with one (an older
     *  server has none: no button — AGENTS.md, never a control that cannot act). */
    const bRange = mk('RANGE', 'ADS-B RANGE outline: the furthest aircraft heard on each bearing (not the distance rings)');
    bRange.hidden = true;
    /* ★ LABELS (Stuart, 2026-10-24: "on a busy screen they may be a bit of a mess"): off = no labels, except the aircraft
     *  you have selected, which keeps its own so you can still read it. */
    const bLabels = mk('LABELS', 'Callsign and altitude beside each aircraft');
    const bCentre = mk('⌖', 'Recentre on the aircraft');
    bCentre.setAttribute('aria-label', 'Recentre');
    // ★ The map must not take the control's taps as map gestures (a drag that starts on a button).
    for (const ev of ['pointerdown', 'mousedown', 'touchstart', 'dblclick', 'wheel']) ctl.addEventListener(ev, (e) => e.stopPropagation(), { passive: true });
    /* ★★ beta96 opts.ctlHost — an element of the HOST's to draw these keys in (the web map's MAP card, Stuart 11-27:
     *  "the controls need work"); absent = inside the map, as always. The keys behave exactly the same either way. */
    (opts.ctlHost && opts.ctlHost.appendChild ? opts.ctlHost : container).appendChild(ctl);

    /* ── ★★ THE LEGEND (Stuart, 2026-10-24: "a little (i) button above the aircraft list which pops out a legend") —
     *  what the aircraft colours and the altitude ARROW colours mean, drawn by the layer itself so the key can never
     *  disagree with the map. The host's (i) button calls toggleLegend(); it opens above the host's list (setObstacle). */
    const leg = doc.createElement('div');
    leg.className = 'vsadsb-leg vsadsb-' + th;
    leg.hidden = true;
    const legRow = (rgb, glyph, what) => '<div class="r"><span class="g" style="color:rgb(' + rgb + ')">' + glyph + '</span>' + what + '</div>';
    const grad = ALT_HUES.map((h) => 'hsl(' + h[1] + ',90%,' + (th === 'light' ? '38%' : '62%') + ') ' + (h[0] / 400).toFixed(1) + '%').join(',');
    leg.innerHTML = '<div class="t">AIRCRAFT</div>'
      + legRow(TONE.fresh, '\u2708', 'Position under 10 s old')
      + legRow(TONE.aging, '\u2708', '10\u201330 s old')
      + legRow(TONE.stale, '\u2708', '30\u201360 s old (dropped at 60 s)')
      + (opts.sparseLegend ? '<div class="r" style="opacity:.85">Datalink (ACARS \u00b7 VDL2 \u00b7 HFDL \u00b7 ADS-C): the same colours at under 2 min \u00b7 2\u201310 min \u00b7 older; facing the way it was going between its last two fixes</div>' : '')
      + legRow(TONE.ground, '\u2708', 'On the ground')
      + legRow(TONE.alert, '\u2708', 'Emergency')
      + legRow(TONE.military, '\u2708', 'Military')
      + '<div class="t">ALTITUDE (the text and its arrow)</div>'
      + '<div class="r"><span class="g">\u25B2</span>Climbing \u00b7 <span class="g">\u25BC</span>descending \u00b7 <span class="g">\u25CF</span>level</div>'
      + '<div class="bar" style="background:linear-gradient(90deg,' + grad + ')"></div>'
      + '<div class="ax"><span>0</span><span>10k</span><span>20k</span><span>30k</span><span>40k ft</span></div>'
      // ★ beta94 the range outline's key: what it is, the furthest in its window and ever, and the all-time line's switch
      // ★ beta96 named as it is on the map: ADS-B RANGE (the dashed NM rings are the distance rings, not this)
      + '<div class="vsadsb-rg" hidden><div class="t">' + RANGE_LABEL + ' (the solid outline)</div>'
      + '<div class="r"><span class="g" style="color:rgb(' + T.range + ')">\u25AC</span><span class="vsadsb-rg-w"></span></div>'
      + '<div class="r vsadsb-rg-far" style="white-space:normal"></div>'
      + '<label class="r" style="cursor:pointer"><input type="checkbox" class="vsadsb-rg-all" style="margin:0"> All-time outline (dotted)</label>'
      + '<div class="r vsadsb-rg-rec" style="white-space:normal;opacity:.8"></div></div>';
    for (const ev of ['pointerdown', 'mousedown', 'touchstart', 'dblclick', 'wheel']) leg.addEventListener(ev, (e) => e.stopPropagation(), { passive: true });
    container.appendChild(leg);
    function placeLegend() { leg.style.bottom = ((obstacle ? obstacle.h : 0) + 8) + 'px'; }
    function paintCtl() {
      bAuto.classList.toggle('on', !!prefs.autoZoom && !paused);
      bAuto.classList.toggle('paused', !!prefs.autoZoom && paused);
      bAuto.title = !prefs.autoZoom ? 'Auto-zoom is off — tap to keep every aircraft in view'
        : paused ? 'Auto-zoom paused because you moved the map — tap to resume' : 'Auto-zoom on — tap to turn it off';
      bFit.classList.toggle('on', !!prefs.fit || !rx);
      bRings.classList.toggle('on', !!prefs.rings && !!rx);
      bLabels.classList.toggle('on', prefs.labels !== false);
      bRange.hidden = !rangeData && !liveHas();
      bRange.classList.toggle('on', !!prefs.range);
      bRange.title = rangeTitle();
      // ★ A control that cannot do anything is shown disabled with the reason, never left inert (AGENTS.md).
      bFit.disabled = !rx; bRings.disabled = !rx;
      if (!rx) { bFit.title = bRings.title = 'Not available: this server has not given its receiver position (no latitude/longitude or locator, and no ADS-B range outline yet)'; }
      else { const n = rxNote ? ' (the receiver position is ' + rxNote + ')' : '';
             bFit.title = (prefs.fit ? 'Fit to aircraft — tap to keep the receiver in the middle' : 'Receiver centred — tap to fit the view to the aircraft instead') + n;
             bRings.title = 'Range rings in nautical miles around the receiver' + n; }
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
    bLabels.addEventListener('click', () => { setPref('labels', prefs.labels === false); paintCtl(); labelAt = 0; kick(); });
    bRange.addEventListener('click', () => { setPref('range', !prefs.range); paintCtl(); drawRange(); if (prefs.range) fetchRange(false); });
    bCentre.addEventListener('click', () => { paused = false; az.reset(); paintCtl(); cameraStep(true); });
    /* ── ★★ V12 beta94 THE RANGE OUTLINE (vibe_adsb_range.h; ADSB.md "The range outline") ─────────────────────────────
     *  GET <imageBase>range — this radio's outline over the owner's window (7 / 30 / 90 days or all time), the all-time
     *  one and the furthest catch ever. Fetched when the map opens, then every 15 minutes (RANGE_POLL_MS) while it is shown and the page
     *  is visible (it grows slowly; a poll is floored, never per report — memory: cloudflare_workers_limit_incident).
     *  ★★ BOLD, NOT CALM (Stuart, 11-25: "make the line thicker and a bolder colour too so its easier to see on the
     *  map"): a 3 px magenta edge — clear of the aircraft's green/amber/grey/red/blue tones and the amber rings, on both
     *  themes — over a faint fill so the aircraft beneath stay readable. The all-time outline is a thin dotted line of
     *  the same colour, behind it, off unless asked for (the key's tick box). */
    // ★ beta95 the outline grows LIVE (rangeOffer, from update()), so the server's copy is only a slow re-sync now
    const RANGE_POLL_MS = 15 * 60 * 1000;
    let rangeData = null, rangeAt = 0, rangeBusy = false, rangeTimer = 0, llRange = null;
    /* ★★ beta95 THE LIVE OUTLINE: what this map has seen since it opened, binned around the receiver (the server's centre
     *  where the page has none). It is drawn around the server's own centre once that has answered — the two are within
     *  5 km (vibe_adsb_range.h kMovedKm), a hair at outline scale. Collected whether or not RANGE is on, so turning it on
     *  shows it at once. liveFar = its furthest catch. */
    let liveKm = new Array(RANGE_BINS).fill(0), liveN = 0, liveFar = null, liveC = null, rangeDrawTimer = 0;
    function serverCentre() { return rangeData && rangeData.lat != null && rangeData.lon != null ? { lat: rangeData.lat, lon: rangeData.lon } : null; }
    function liveCentre() { return rx || serverCentre(); }
    function drawCentre() { return serverCentre() || rx; }
    /** One aircraft's new fix: counted only on its second consistent position in a row (the server's posOk).
     *  ★★ b96-rangefeed: posT is WHEN the position was made (now − seen_pos, NOT clamped: the glide's fix.t stops at 60 s,
     *  so a position two minutes old followed by a fresh one looked like 25 NM in a minute — a "bad fix" — and the far
     *  aircraft at the edge of range, whose positions come and go, waited one more fix to count). A datalink (sparse)
     *  position is never the first of two ADS-B fixes: it clears the pair. */
    function liveRangeFix(s, fix, sparse, posT) {
      if (sparse) { s.rg = null; return false; }
      const prev = s.rg;
      s.rg = { lat: fix.lat, lon: fix.lon, t: posT };
      if (!rangeConsistent(prev, s.rg)) return false;
      const c = liveCentre();
      if (!c) return false;
      // ★ the receiver moved (a different site, the server's 5 km rule): the live bins start again around it
      if (!liveC || distNm(liveC.lat, liveC.lon, c.lat, c.lon) * NM_M > 5000) { liveKm = new Array(RANGE_BINS).fill(0); liveN = 0; liveFar = null; }
      liveC = c;
      if (!rangeOffer(liveKm, c.lat, c.lon, fix.lat, fix.lon)) return false;
      liveN++;
      const brg = bearingDeg(c.lat, c.lon, fix.lat, fix.lon), km = distNm(c.lat, c.lon, fix.lat, fix.lon) * NM_M / 1000;
      if (!liveFar || km > liveFar.km) liveFar = { km, brg, at: Math.floor(Date.now() / 1000) };
      return true;
    }
    function liveHas() { return liveKm.some((v) => v > 0); }
    // ★ for the drivers (scripts/test-web-map-cards.mjs, the range checks): the outline's state and the aircraft's fixes
    global.__vsRange = () => ({ live: liveKm.slice(), stored: rangeData && rangeData.km ? rangeData.km.slice() : null, shaped: rangeShape(rangeMerge(rangeData && rangeData.km, liveKm)),
      c: drawCentre(), liveC, ac: [...ac.values()].filter((s) => s.fix).map((s) => ({ hex: s.hex, name: labelName(s), lat: s.fix.lat, lon: s.fix.lon, rg: !!s.rg, sparse: !!(s.a && s.a.sparse) })) });
    function farOf(a, b) { return a && b ? (b.km > a.km ? b : a) : a || b || null; }
    /** Redraw soon (at most every 1.5 s) — the outline grows from 1 Hz reports. */
    function rangeSoon() {
      if (rangeDrawTimer || destroyed) return;
      rangeDrawTimer = setTimeout(() => { rangeDrawTimer = 0; if (destroyed) return; paintCtl(); paintRangeKey(); drawRange(); }, 1500);
    }
    const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
    function dayText(utc) { const d = new Date(utc * 1000); return d.getUTCDate() + ' ' + MONTHS[d.getUTCMonth()]; }
    // ★★ beta95 NM (Stuart 11-25: NM everywhere on the aviation and shipping maps) — the server still sends km
    function catchText(c) { return c && c.km > 0 ? Math.round(c.km * 1000 / NM_M) + ' NM at ' + Math.round(c.brg) + '\u00b0, ' + dayText(c.at) : ''; }
    function windowText(w) { return w === 0 ? 'all time' : w === 1 ? 'the last day' : 'the last ' + w + ' days'; }
    function rangeWindowText() { return rangeData ? windowText(rangeData.window) : 'since this map opened'; }
    function rangeTitle() {
      if (!rangeData && !liveHas()) return '';
      const f = catchText(farOf(rangeData && rangeData.far, liveFar));
      return (prefs.range ? 'ADS-B RANGE outline on' : 'ADS-B RANGE outline off') + ' \u2014 the furthest aircraft heard on each bearing (not the distance rings) '
        + (rangeData ? 'over ' : '') + rangeWindowText() + (f ? ' \u00b7 furthest ' + f : ' \u00b7 nothing heard yet');
    }
    function paintRangeKey() {
      const box = leg.querySelector('.vsadsb-rg');
      if (!box) return;
      box.hidden = !rangeData && !liveHas();
      if (box.hidden) return;
      const f = catchText(farOf(rangeData && rangeData.far, liveFar)), r = catchText(farOf(rangeData && rangeData.record, liveFar));
      box.querySelector('.vsadsb-rg-w').textContent = 'Furthest per bearing, ' + rangeWindowText();
      box.querySelector('.vsadsb-rg-far').textContent = f ? 'Furthest: ' + f : 'Nothing heard yet in this window';
      box.querySelector('.vsadsb-rg-rec').textContent = r ? 'Furthest ever: ' + r : '';
      box.querySelector('.vsadsb-rg-all').checked = !!prefs.rangeAll;
      box.querySelector('.vsadsb-rg-all').parentNode.hidden = !rangeData || rangeData.window === 0;   // ★ no all-time line to show
    }
    leg.addEventListener('change', (e) => {
      if (!e.target || !e.target.classList || !e.target.classList.contains('vsadsb-rg-all')) return;
      setPref('rangeAll', !!e.target.checked); drawRange();
    });
    /** The outline drawn: the server's (if any) merged with the live one, shaped (rangeShape), and its all-time line. */
    function rangeGeo() {
      const c = drawCentre();
      if (!c || (!rangeData && !liveHas())) return { w: null, a: null };
      const live = liveC && distNm(liveC.lat, liveC.lon, c.lat, c.lon) * NM_M <= 5000 ? liveKm : null;
      // ★★★ beta96 rangeShape (gaps kept, dips closed, needles softened) — the live and the stored outline alike
      return { w: prefs.range ? rangeRing(c.lat, c.lon, rangeShape(rangeMerge(rangeData && rangeData.km, live))) : null,
               a: prefs.range && prefs.rangeAll && rangeData && rangeData.window !== 0
                 ? rangeRing(c.lat, c.lon, rangeShape(rangeMerge(rangeData.allKm, live))) : null };
    }
    function drawRange() {
      if (destroyed) return;
      const g = rangeGeo();
      if (gl) {
        const src = glReady && gl.getSource('vsadsb-range');
        if (!src) return;
        const f = [];
        if (g.a) f.push({ type: 'Feature', properties: { k: 'a' }, geometry: { type: 'LineString', coordinates: g.a } });
        if (g.w) f.push({ type: 'Feature', properties: { k: 'w' }, geometry: { type: 'Polygon', coordinates: [g.w] } });
        // ★ beta96 the label rides the outline itself (a LineString copy: the 'vsadsb-range-lbl' layer, placement line)
        if (g.w) f.push({ type: 'Feature', properties: { k: 'l', t: RANGE_LABEL }, geometry: { type: 'LineString', coordinates: g.w } });
        src.setData({ type: 'FeatureCollection', features: f });
      } else {
        if (llRange) { lm.removeLayer(llRange); llRange = null; }
        if (!g.w && !g.a) return;
        const col = 'rgb(' + T.range + ')', grp = LL.layerGroup();
        if (g.a) LL.polyline(g.a.map((p) => [p[1], p[0]]), { color: col, weight: 1.5, opacity: 0.85, dashArray: '1 5', lineCap: 'round', interactive: false }).addTo(grp);
        if (g.w) LL.polygon(g.w.map((p) => [p[1], p[0]]), { color: col, weight: 3, opacity: 0.95, fillColor: col, fillOpacity: 0.08, interactive: false }).addTo(grp);
        // ★ beta96 one "ADS-B RANGE" at the furthest vertex, in the map's lettering and the outline's colour
        const c = g.w && drawCentre(), lp = c && rangeLabelPoint(c.lat, c.lon, g.w);
        if (lp) LL.marker([lp[1], lp[0]], { interactive: false, keyboard: false, icon: LL.divIcon({ className: '', iconSize: [0, 0],
          html: '<div class="vsadsb-rglbl" style="position:absolute;transform:translate(-50%,-130%);white-space:nowrap;font:700 10px \'JetBrains Mono\',ui-monospace,monospace;'
            + 'letter-spacing:.06em;color:' + col + ';text-shadow:0 0 2px ' + T.halo + ',0 0 2px ' + T.halo + ',0 0 3px ' + T.halo + '">' + RANGE_LABEL + '</div>' }) }).addTo(grp);
        llRange = grp.addTo(lm);
      }
    }
    function fetchRange(force) {
      if (destroyed || rangeBusy || imageBase == null || typeof fetch !== 'function') return;
      if (!force && now() - rangeAt < RANGE_POLL_MS - 1000) return;
      rangeBusy = true; rangeAt = now();
      fetch(imageBase + 'range', { cache: 'no-store' })
        .then((r) => (r.ok ? r.json() : null))
        .then((j) => {
          rangeBusy = false;
          if (destroyed || !j || !j.ok || !Array.isArray(j.km)) return;
          rangeData = { window: Number(j.window) || 0, lat: j.lat == null ? null : Number(j.lat), lon: j.lon == null ? null : Number(j.lon),
                        km: j.km, allKm: Array.isArray(j.allKm) ? j.allKm : [], far: j.far || null, record: j.record || null, days: j.days };
          // ★★ b96-rangefeed the LAST fallback for the receiver: the outline's centre (RINGS, FIT and the house then work)
          const sc = serverCentre();
          if (!rx && sc && isFinite(sc.lat) && isFinite(sc.lon) && !(sc.lat === 0 && sc.lon === 0)) {
            rx = { lat: sc.lat, lon: sc.lon }; rxFromRange = true; rxNote = 'approximate — the centre of the server\u2019s ADS-B range outline';
            drawRx(); drawRings(); az.reset(); if (prefs.autoZoom && !paused) cameraStep(true);
          }
          paintCtl(); paintRangeKey(); drawRange();
        })
        .catch(() => { rangeBusy = false; });   // ★ an older server (404) or no network: no outline, no button
    }
    function rangeTick() {
      if (prefs.range && !(doc.visibilityState === 'hidden')) fetchRange(false);
    }
    const onVis = () => { if (doc.visibilityState !== 'hidden') rangeTick(); };
    if (imageBase != null) {
      rangeTimer = setInterval(rangeTick, 30000);   // ★ looks every 30 s; asks only when RANGE_POLL_MS (15 min) has passed — the answer is MERGED with the live outline (rangeGeo), never replaces it
      doc.addEventListener('visibilitychange', onVis);
    }
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
    /* ★★ THE CLEAR PART OF THE MAP. The host's live list sits over a corner (setObstacle); fit, centring and the
     *  "an aircraft has left the view" test all work inside what is left, or aircraft hide under the list (seen on
     *  the Pi 2, 2026-10-24). The camera is computed for that clear rectangle, then the map's centre is offset so the
     *  rectangle's centre lands where the camera wants it. */
    let obstacle = null;
    function clearRect() {
      const v = viewSize(), i = insetFor(obstacle, v.w, v.h);
      return { w: v.w - i.l - i.r, h: v.h - i.t - i.b, dx: (i.l - i.r) / 2, dy: (i.t - i.b) / 2 };
    }
    function shiftCam(cam, dx, dy) {          // the camera whose CLEAR centre is cam.center
      if (!cam || (!dx && !dy)) return cam;
      const ws = TILE * Math.pow(2, cam.zoom), c = merc(cam.center.lat, cam.center.lon);
      const m = unmerc(c.x - dx / ws, c.y - dy / ws);
      return { center: { lat: m.lat, lon: wrapLon(m.lon, cam.center.lon) }, zoom: cam.zoom };
    }
    function target() {
      const r = clearRect();
      const cam = cameraFor({ points: positioned(), rx, fit: prefs.fit || !rx, w: r.w, h: r.h, pad: padPx(), tileSize: TILE,
                              minZoom: ZMIN, maxZoom: ZMAX, emptyZoom: gl ? 7 : 8 });
      return shiftCam(cam, r.dx, r.dy);
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
      const r = clearRect();
      const out = anyOutside(positioned(), shiftCam(cur, -r.dx, -r.dy), r.w, r.h, padPx() * 0.5, TILE);
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
    else { lm.on('movestart', onMoveStart); lm.on('moveend', onMoveEnd); lm.on('moveend resize', () => llClamp()); }   // ★ beta96 the card kept on the map
    /* ★★ beta96 A PIXEL-RATIO CHANGE (a window dragged to another screen, a browser zoom, device emulation): Leaflet's canvas
     *  renderer only redraws on a move, so the range outline was drawn stale (at the old ratio) until the map was moved. The
     *  ratio is watched and the canvas redrawn at once. The GPU map redraws itself. */
    let dprMq = null;
    function dprWatch() {
      if (gl || typeof matchMedia !== 'function') return;
      try {
        if (dprMq) dprMq.removeEventListener('change', dprChanged);
        dprMq = matchMedia('(resolution: ' + (window.devicePixelRatio || 1) + 'dppx)');
        dprMq.addEventListener('change', dprChanged);
      } catch (e) { dprMq = null; }
    }
    function dprChanged() {
      if (destroyed) return;
      dprWatch();
      try { lm.invalidateSize(); lm.fire('moveend'); drawRange(); llClamp(); } catch (e) { console.error('adsblayer: pixel ratio redraw', e); }
    }
    dprWatch();

    /* ── range rings ────────────────────────────────────────────────────────────────────────── */
    let llRings = null;
    function ringGeo() {
      if (!rx || !prefs.rings) return null;
      const z = gl ? gl.getZoom() : lm.getZoom();
      return ringsFor(rx, z, viewSize(), screenOffset(curCam(), rx.lat, rx.lon, TILE), TILE);
    }
    function drawRings() {
      if (destroyed) return;
      const rings = ringGeo();
      if (gl) setRingData(gl, 'vsadsb-', rings);
      else {
        if (llRings) { lm.removeLayer(llRings); llRings = null; }
        if (!rings) return;
        llRings = leafletRings(LL, rx, rings, T, 'vsadsb-rl');
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
      for (const id of ['vsadsb-ac', 'vsadsb-rx']) gl.addSource(id, { type: 'geojson', data: empty });
      // ★ beta94 the range outline, under the rings and every aircraft: a faint fill, the all-time dotted line, the bold edge
      gl.addSource('vsadsb-range', { type: 'geojson', data: empty });
      gl.addLayer({ id: 'vsadsb-range-fill', type: 'fill', source: 'vsadsb-range', filter: ['==', ['get', 'k'], 'w'],
                    paint: { 'fill-color': 'rgb(' + T.range + ')', 'fill-opacity': th === 'light' ? 0.07 : 0.09 } });
      gl.addLayer({ id: 'vsadsb-range-all', type: 'line', source: 'vsadsb-range', filter: ['==', ['get', 'k'], 'a'],
                    layout: { 'line-cap': 'round', 'line-join': 'round' },
                    paint: { 'line-color': 'rgb(' + T.range + ')', 'line-width': 1.5, 'line-opacity': 0.85, 'line-dasharray': [0.1, 3] } });
      gl.addLayer({ id: 'vsadsb-range-line', type: 'line', source: 'vsadsb-range', filter: ['==', ['get', 'k'], 'w'],
                    layout: { 'line-join': 'round' },
                    paint: { 'line-color': 'rgb(' + T.range + ')', 'line-width': 3, 'line-opacity': 0.95 } });
      /* ★★ beta96 "ADS-B RANGE" along the outline (Stuart 11-26: not to be confused with the distance rings) — the map's own
       *  lettering, in the outline's colour, repeated every ~280 px of line so it is on screen whatever the zoom, kept upright,
       *  just outside the line (offset), and never hidden by the aircraft labels' collision boxes. */
      gl.addLayer({ id: 'vsadsb-range-lbl', type: 'symbol', source: 'vsadsb-range', filter: ['==', ['get', 'k'], 'l'],
                    layout: { 'symbol-placement': 'line', 'symbol-spacing': 280, 'text-field': ['get', 't'],
                              'text-font': ['JetBrains Mono Regular'], 'text-size': 11, 'text-letter-spacing': 0.08,
                              'text-keep-upright': true, 'text-offset': [0, -0.9], 'text-allow-overlap': true, 'text-ignore-placement': true },
                    paint: { 'text-color': 'rgb(' + T.range + ')', 'text-halo-color': T.halo, 'text-halo-width': 1.4 } });
      addRingLayers(gl, 'vsadsb-', T);
      gl.addLayer({ id: 'vsadsb-rx', type: 'symbol', source: 'vsadsb-rx',
                    layout: { 'icon-image': 'vsadsb-rx', 'icon-anchor': 'bottom', 'icon-allow-overlap': true, 'icon-ignore-placement': true } });
      // ★ THE TRAIL (trailGradient) — under the aircraft, a soft glow beneath a 3 px line, one gradient for both
      gl.addSource('vsadsb-trail', { type: 'geojson', lineMetrics: true, data: empty });
      gl.addLayer({ id: 'vsadsb-trail-glow', type: 'line', source: 'vsadsb-trail', layout: { 'line-cap': 'round', 'line-join': 'round' },
                    paint: { 'line-width': 9, 'line-blur': 6, 'line-opacity': 0.35, 'line-gradient': TRAIL_NONE } });
      gl.addLayer({ id: 'vsadsb-trail', type: 'line', source: 'vsadsb-trail', layout: { 'line-cap': 'round', 'line-join': 'round' },
                    paint: { 'line-width': 3, 'line-gradient': TRAIL_NONE } });
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
                    layout: { 'text-field': ['format', ['get', 'lbl'], {}, ['coalesce', ['get', 'al'], ''], { 'text-color': ['coalesce', ['get', 'alc'], T.label] }],
                              'text-font': ['JetBrains Mono Regular'], 'text-size': 10, 'text-line-height': 1.15,
                              'text-anchor': 'left', 'text-justify': 'left', 'text-offset': [1.25, 0], 'text-allow-overlap': true,
                              'text-ignore-placement': true },
                    paint: { 'text-color': T.label, 'text-halo-color': T.halo, 'text-halo-width': 1.2, 'text-opacity': ['get', 'lo'] } });
      rxImage();
      glReady = true;
      gpuStart();
      drawRx(); drawRings(); drawRange();
      kick();
    }
    /* ★★★ the GPU marker layer (GpuMarkers above): the aircraft icons drawn and dead-reckoned on the GPU; the symbol
     *  layer keeps the labels, the halo and the ring (moved at the half-pixel rule, a few times a second at most). */
    let gpu = null, gpuOn = false, gpuSig = '';
    function gpuStart() {
      if (gpu || !GpuMarkers || /[?&]gpu=0\b/.test((global.location && global.location.search) || '')) return;
      gpu = GpuMarkers.create(gl, { id: 'vsadsb-gpu', iconBox: ICON_BOX, dpr: DPR, beforeId: 'vsadsb-labels',
        iconFor: (key) => { const k = key.split('|'); return iconImage(k[0], TONE[k[1]], k[1] === 'ground' ? 0.8 : 0.92, k[2] === 'g', DPR); },
        onReady: () => { if (!gpu || !gpu.ok || destroyed) return; gpuOn = true; gpuSig = ''; symVis('none'); forceFlush = true; kick(); },
        onFail: () => { gpuOn = false; symVis('visible'); forceFlush = true; if (!destroyed) kick(); } });
    }
    function symVis(v) {
      for (const id of ['vsadsb-icons', 'vsadsb-labels', 'vsadsb-halo', 'vsadsb-ring']) { try { if (gl.getLayer(id)) gl.setLayoutProperty(id, 'visibility', v); } catch (e) { /* none */ } }
      if (v === 'none') { try { const src = gl.getSource('vsadsb-ac'); if (src) src.setData({ type: 'FeatureCollection', features: [] }); } catch (e) { /* none */ } }
    }
    /** The aircraft as the GPU layer's markers — written only when something the shader cannot work out has changed. */
    function gpuSync(t) {
      if (!gpuOn) return;
      let sig = (selected || '') + th;
      for (const s of ac.values()) if (s.fix) {
        const lw = !!(s.lblWant && !s.goneAt);
        if (lw && !s.gLbl) s.gLblAt = t;
        s.gLbl = lw;
        sig += s.hex + s.fix.t + toneKey(s.a) + s.kind + (s.goneAt || 0) + (s.corr ? s.corr.t0 : 0) + (s.fadeIn ? 'f' : '') + (s.bornAt ? 'b' : '') + (lw ? labelText(s) + labelAltColour(s) : '');
      }
      if (sig === gpuSig) return;
      gpuSig = sig;
      const list = [];
      for (const s of ac.values()) {
        if (!s.fix) continue;
        const tone = toneKey(s.a), glow = s.hex === selected;
        const to = s.fix.track != null && isFinite(s.fix.track) ? s.fix.track : (s.hdg || 0);
        if (s.gpuTo !== to) { const fr = s.gpuTo != null ? s.gpuTo : (s.hdg != null ? s.hdg : to); s.gpuFrom = Math.abs(angleTo(fr, to)) < 3 ? to : fr; s.gpuTo = to; s.gpuT0 = t; }
        list.push({ id: s.hex, lat: s.fix.lat, lon: s.fix.lon, t: s.fix.t, gs: s.fix.gs, track: s.fix.track, maxDrMs: MAX_DR_S * 1000,
                    key: s.kind + '|' + tone + '|' + (glow ? 'g' : '') + '|' + th, hdgFrom: s.gpuFrom, hdgTo: s.gpuTo, hdgT0: s.gpuT0,
                    noRotate: !!NO_ROTATE[s.kind], corr: s.corr ? { dLat: s.corr.dLat, dLon: s.corr.dLon, t0: s.corr.t0, ms: s.corr.ms || GLIDE_MS } : null,
                    shownAt: s.fadeIn ? s.shownAt : null, goneAt: s.goneAt || null, fadeInMs: FADE_IN_MS, fadeOutMs: FADE_OUT_MS,
                    z: glow ? 1000 : tone === 'alert' ? 900 : (s.a.alt_baro === 'ground' ? 0 : 1),
                    bornAt: s.bornAt && t - s.bornAt < RING_MS * RING_REPEATS ? s.bornAt : null, rgb: String(TONE[tone] || '255,255,255').split(',').map(Number),
                    label: s.gLbl ? { l1: labelName(s), l2: labelAlt(s), c2: labelAltColour(s), col: T.label, halo: T.halo, at: s.gLblAt } : null });
      }
      gpu.set(list);
    }
    function hitHex(point) { return hitHexR(point, 12); }
    function hitHexR(point, r) {
      if (gpuOn) return gpu.pick(point, r);
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
      gl.on('moveend', () => { if (gpuOn && !destroyed) { forceFlush = true; kick(); } });   // ★ the labels' on-screen set follows a pan
      // ★ A vm whose style arrives later: setStyle({diff:false}) fires style.load, after which our sources are gone.
      gl.on('style.load', () => { if (!gl.getSource('vsadsb-ac')) { glReady = false; imgs.clear(); if (gpu) { gpu.destroy(); gpu = null; gpuOn = false; } glSetup(); } });
      gl.on('styleimagemissing', (e) => {
        const m = /^vsadsb-(dark|light)-([a-z0-9]+)-([a-z]+)(-g)?$/.exec(e.id || '');
        if (m && m[1] === th) imgId(m[2], m[3], !!m[4]);
        if (e.id === 'vsadsb-rx') rxImage();
      });
    } else {
      drawRx(); drawRings();
    }
    fetchRange(true);

    /* ── the card ───────────────────────────────────────────────────────────────────────────── */
    let popup = null, popupHex = null, popupHtml = '';
    /* ★★★ PHOTOS ARE OFF — TODO(Stuart): read and save the Planespotters.net photo API terms
     *     (https://www.planespotters.net/photo/api — behind a Cloudflare check that no script of ours can pass, so not
     *     confirmed at first hand; research §2.5). The code below follows the terms as reported: fetched BY THIS SCREEN
     *     only when a card opens, one request per aircraft, answers kept 24 h at most, the image from the address the
     *     API returns, unchanged, never proxied; the photographer credited in text beside it and the thumbnail a plain
     *     link to the photo's page. A host turns it on with opts.photos === true once the terms are confirmed. */
    const photosOn = opts.photos === true;
    const photoCache = new Map();          // hex → { at, p: {src, link, by} | null } | 'pending'
    function photoFor(s) {
      if (!photosOn || !global.fetch) return null;
      const c = photoCache.get(s.hex);
      if (c && c !== 'pending' && now() - c.at < 24 * 3600e3) return c.p;
      if (c === 'pending') return null;
      photoCache.set(s.hex, 'pending');
      const reg = String(s.a.r || '');
      const url = 'https://api.planespotters.net/pub/photos/' + (s.hex[0] === '~' ? 'reg/' + encodeURIComponent(reg) : 'hex/' + s.hex);
      global.fetch(url).then((r) => r.ok ? r.json() : null).then((j) => {
        const ph = j && Array.isArray(j.photos) ? j.photos[0] : null;
        const src = ph && ph.thumbnail_large && ph.thumbnail_large.src || ph && ph.thumbnail && ph.thumbnail.src;
        const p = src && /^https:\/\//.test(src) && ph.link ? { src: String(src), link: String(ph.link), by: String(ph.photographer || '') } : null;
        photoCache.set(s.hex, { at: now(), p });
        if (selected === s.hex && popup && ac.has(s.hex)) { const h = cardHtml(ac.get(s.hex)); if (h !== popupHtml) setCard(h); }
      }).catch((e) => { console.error('adsblayer: photo lookup', e); photoCache.set(s.hex, { at: now(), p: null }); });
      return null;
    }
    /* ★ MINIMISE (Stuart, 2026-10-24: "can the card have a minimise button so you can view the track only"): the card folds
     *  to its name, the aircraft stays selected and its trail drawn; the same key opens it again. It stays folded for the
     *  next aircraft picked (stepping through trails), until opened. */
    let cardMin = false;
    const MIN_BTN = (folded) => '<button type="button" class="vsadsb-min" title="' + (folded ? 'Show the card' : 'Fold the card — the trail stays')
      + '" aria-label="' + (folded ? 'Show the card' : 'Fold the card') + '">' + (folded ? '+' : '\u2013') + '</button>';
    function cardHtml(s) {
      const a = s.a;
      const name = cleanFlight(a.flight) || String(a.r || '') || s.hex.toUpperCase();
      if (cardMin) return '<div class="t mn">' + esc(name) + MIN_BTN(true) + '</div>';
      const rows = [];
      const row = (k, v) => { if (v) rows.push('<tr><td>' + esc(k) + '</td><td>' + esc(v) + '</td></tr>'); };
      const rowHtml = (k, html) => { if (html) rows.push('<tr><td>' + esc(k) + '</td><td>' + html + '</td></tr>'); };
      const em = emergencyOf(a);
      // ── what it is (the server's aircraft data) ──
      const known = !!(a.r || a.t || a.opName || a.country || a.route);
      /* ★★ COMPACT LINES, NOT TABLE ROWS: a table row per field wrapped to a dozen lines on a phone and pushed the card's
       *  top under the map's own top bar (390 pt, bench 2026-10-24). One line each: what it is, who flies it, where. */
      const idl = [];
      const line = (html, cls) => { if (html) idl.push('<div class="' + (cls || 'l') + '">' + html + '</div>'); };
      line([a.t, a.desc, a.r].filter(Boolean).map(esc).join(' \u00b7 ') + (SPECIES[a.species] ? ' (' + esc(SPECIES[a.species]) + ')' : ''));
      const fl = flagOrCode(a.country);
      line([a.opName ? esc(a.opName) : '', a.countryName || a.country ? (fl ? esc(fl) + ' ' : '') + esc(a.countryName || a.country) : ''].filter(Boolean).join(' \u00b7 '));
      line([isMilitary(a) ? 'MILITARY' : '', WAKE[a.wtc] ? WAKE[a.wtc] + ' wake' : '', engText(a.eng)].filter(Boolean).map(esc).join(' \u00b7 '), 'l d');
      const rt = a.route && typeof a.route === 'object' ? a.route : null;
      if (rt && rt.from && rt.to) {
        // ★ short: the code, then the airport's name without its "International Airport" tail
        const end = (code, iata, nm, place) => esc((iata || code) + ((nm = shortAirport(nm) || place || '') ? ' ' + nm : ''));
        let txt = end(rt.from, rt.fromIata, rt.fromName, rt.fromPlace) + ' \u2192 ' + end(rt.to, rt.toIata, rt.toName, rt.toPlace);
        if (Array.isArray(rt.stops) && rt.stops.length > 2) txt += ' <span class="d">(' + rt.stops.map(esc).join(' \u2013 ') + ')</span>';
        // ★ a filed route the aircraft is nowhere near (a reused callsign, a diversion): struck through, and said why
        line(rt.plausible === false ? '<span class="off" title="The aircraft is far from this filed route">' + txt + '</span>' : '\u2708 ' + txt, 'l rt');
      }
      row('Altitude', altText(a.alt_baro) + (num(a.alt_geom) != null && a.alt_baro !== 'ground' ? '  (GNSS ' + Math.round(a.alt_geom).toLocaleString('en-GB') + ' ft)' : ''));
      row(vsLabel(a.baro_rate), vsCardText(a.baro_rate, a.alt_baro === 'ground' ? null : a.alt_baro));
      row('Speed', num(a.gs) != null ? Math.round(a.gs) + ' kt' : '');
      row('Track', num(a.track) != null ? Math.round(a.track) + '° ' + cardinal(a.track) : '');
      row('Squawk', a.squawk ? String(a.squawk).replace(/[^0-7]/g, '').slice(0, 4) : '');
      rows.push(...commbRows(a.commb));
      if (rx && s.disp) {
        const d = distNm(rx.lat, rx.lon, s.fix.lat, s.fix.lon), b = bearingDeg(rx.lat, rx.lon, s.fix.lat, s.fix.lon);
        row('From you', (d < 10 ? d.toFixed(1) : Math.round(d)) + ' NM · ' + Math.round(b) + '° ' + cardinal(b));
      }
      // ★ how far the server has followed it (vibe_adsb.h setPos) — the trail on the map is that path
      row('Tracked', num(a.tracked_nm) != null && a.tracked_nm >= 1 ? Math.round(a.tracked_nm) + ' NM' : '');
      if (!a.t) row('Type', CAT_NAME[String(a.category || '').toUpperCase()] || '');   // ★ the real type says more
      /* ★ V12 beta90 the host's own lines (the AVIATION MAP: which sources heard it, the last message, the route) — text
       *  only, escaped like every other row; at most 10. */
      if (Array.isArray(a.rowsExtra)) for (const x of a.rowsExtra.slice(0, 10)) if (Array.isArray(x)) row(String(x[0] || '').slice(0, 24), String(x[1] == null ? '' : x[1]).slice(0, 200));
      const seen = num(a.seen);
      row('Heard', seen == null ? '' : seen < 2 ? 'just now' : Math.round(seen) + ' s ago');
      row('Signal', num(a.rssi) != null ? a.rssi.toFixed(1) + ' dBFS' + (num(a.messages) != null ? ' · ' + a.messages + ' msgs' : '') : '');
      /* ★★ beta96 (Stuart 11-26: the 3-bar meter gave way to a colour-coded SNR): the server's reply SNR over the 1090 MHz
       *  floor, smoothed ×0.25 a reply (vibe_adsb.h — about the last four replies). The colour is the margin over ADS-B's
       *  decode threshold — src/utils/snrColour.ts 'adsb' (thr 10 dB; red < 3 dB over, amber 3–6, green ≥ 6); ONE RULE,
       *  TWO READERS: scripts/test_snr_colour.ts fails if SNR_ADSB here parts from it. Nothing from an older server. */
      if (num(a.snr) != null) {
        const m = a.snr - SNR_ADSB.thr, col = m < 3 ? SNR_ADSB.red : m < 6 ? SNR_ADSB.amber : SNR_ADSB.green;
        const tip = 'SNR ' + Math.round(a.snr) + ' dB over the noise floor, smoothed over the last few replies \u2014 ADS-B decodes from about '
          + SNR_ADSB.thr + ' dB: red under ' + (SNR_ADSB.thr + 3) + ', green from ' + (SNR_ADSB.thr + 6);
        rowHtml('SNR', '<span title="' + esc(tip) + '" style="display:inline-block;padding:0 4px;border-radius:3px;font-weight:700;color:#111111;background:'
          + col + '">' + Math.round(a.snr) + ' dB</span> <span style="opacity:.7">last few replies</span>');
      }
      const logo = imageBase != null && imgKey(a.logo) ? '<img src="' + esc(imageBase + 'logo/' + imgKey(a.logo)) + '" alt="' + esc(a.op || '') + '">' : '';
      const badge = !logo && opBadge(a) ? '<span class="badge">' + esc(opBadge(a)) + '</span>' : '';
      const sil = imageBase != null && imgKey(a.sil) ? '<img class="sil" src="' + esc(imageBase + 'silhouette/' + imgKey(a.sil)) + '" alt="">' : '';
      const ph = photoFor(s);
      const photo = ph ? '<a class="ph" href="' + esc(ph.link) + '" target="_blank"><img src="' + esc(ph.src) + '" alt="Photo of ' + esc(a.r || name) + '">'
        + '<span>\u00a9 ' + esc(ph.by) + ' \u00b7 Planespotters.net</span></a>' : '';
      /* ★★ LOOK IT UP ELSEWHERE (Stuart, 2026-10-25: "these are commercial sites and if they are happy for the traffic then
       *  it is ok"). Plain links, keyed by the ICAO address (or the registration for Flightradar24) — nothing of theirs is
       *  copied in, so no data licence applies. target=_blank on the web; in the app's map WebView every http(s) tap is
       *  handed to the phone's browser (MapOverlay onShouldStartLoadWithRequest), never a web view on top of the map.
       *  A non-ICAO address (TIS-B '~…') has no page by address on these sites, so only the registration link is offered. */
      const icao = /^[0-9a-f]{6}$/.test(s.hex) ? s.hex : '';
      const reg = String(a.r || '').trim();
      const ext = [];
      const link = (label, url) => { ext.push('<a href="' + esc(url) + '" target="_blank" rel="noopener">' + esc(label) + '</a>'); };
      if (icao) link('ADS-B Exchange', 'https://globe.adsbexchange.com/?icao=' + icao);
      if (icao) link('FlightAware', 'https://www.flightaware.com/live/modes/' + icao + '/redirect');
      if (reg) link('Flightradar24', 'https://www.flightradar24.com/data/aircraft/' + encodeURIComponent(reg.toLowerCase()));
      if (icao) link('Planespotters', 'https://www.planespotters.net/hex/' + icao.toUpperCase());
      const lookup = ext.length ? '<div class="lk">' + ext.join(' \u00b7 ') + '</div>' : '';
      // ★★ ODC-By 1.0 §4.3: the notice goes WITH the produced work — wherever the Mictronics fields show
      const credit = known ? '<div class="cr">Aircraft data: <a href="https://github.com/Mictronics/aircraft-database" target="_blank">Mictronics</a>'
        + ' (contains information from Mictronics aircraft database, which is made available under the '
        + '<a href="https://opendatacommons.org/licenses/by/1-0/" target="_blank">ODC Attribution License</a>) \u00b7 '
        + '<a href="https://github.com/vradarserver/standing-data" target="_blank">VRS standing data</a> (CC0)</div>' : '';
      const sub = cleanFlight(a.flight) || a.r ? s.hex.toUpperCase() : '';
      return MORE_UP + (em ? '<div class="em">' + esc(em) + '</div>' : '')
        + '<div class="t">' + esc(name) + (sub ? '<i>' + esc(sub) + '</i>' : '') + MIN_BTN(false) + '</div>'
        + (logo || badge || sil ? '<div class="op">' + (logo || badge) + sil + '</div>' : '')
        + (idl.length ? '<div class="idb">' + idl.join('') + '</div>' : '')
        + '<div class="vsadsb-dyn"><table>' + rows.join('') + '</table></div>' + photo + lookup + credit + MORE_DN;
    }
    /* ★★ MORE ABOVE / MORE BELOW (Stuart, 2026-10-25: "on the little popup cards for the aircraft can you put a little down
     *  arrow/up arrow or some way of showing that there is more info to scroll"). Two sticky markers, the first and last
     *  thing in the card's scroller, shown only when there IS more that way; re-judged on every scroll, every refresh of
     *  the card and every resize. Zero height, so they never move the card's contents. */
    const MORE_UP = '<div class="vsadsb-more up" aria-hidden="true"><span>▲</span></div>';
    const MORE_DN = '<div class="vsadsb-more dn" aria-hidden="true"><span>▼</span></div>';
    function moreCue() {
      const el = popup && popup.getElement ? popup.getElement() : null;
      const sc = el ? el.querySelector('.vsadsb-sc') : null;
      if (!sc) return;
      const up = sc.scrollTop > 2, dn = sc.scrollTop + sc.clientHeight < sc.scrollHeight - 2;
      sc.classList.toggle('vsadsb-more-up', up);
      sc.classList.toggle('vsadsb-more-dn', dn);
    }
    // ★ scroll does not bubble: caught on the way down, for whichever card is open
    container.addEventListener('scroll', (e) => {
      const t = e.target;
      if (t && t.classList && t.classList.contains('vsadsb-sc')) moreCue();
    }, true);
    window.addEventListener('resize', () => requestAnimationFrame(moreCue));
    function select(hex) {
      if (hex === selected && popup) return;
      selected = hex && ac.has(hex) ? hex : null;
      closePopup();
      tellSel();
      if (!selected) return;
      const s = ac.get(selected);
      trailPick(selected);
      popupHtml = cardHtml(s); popupHex = selected;
      if (gl) {
        if (!ML) { console.error('adsblayer: maplibregl is not on the page — no aircraft card'); return; }
        /* ★★ focusAfterOpen OFF: MapLibre focuses the card's x on EVERY setHTML, and the card is refreshed each
         *  report — so the keyboard's and the controller's next Enter closed the card instead of pressing what
         *  was highlighted (web client, ADS-B list, 2026-10-24). The x is still a button anyone can reach. */
        const p = new ML.Popup({ closeButton: true, closeOnClick: false, focusAfterOpen: false, className: 'vsadsb-pop-' + th, maxWidth: '280px', offset: 16 })
          .setLngLat([s.disp.lon, s.disp.lat]).setHTML(SC_OPEN + popupHtml + SC_CLOSE);
        // ★ Only THIS card's close clears the selection — closePopup() forgets the card before removing it.
        p.on('close', () => { if (popup === p) { popup = null; popupHex = null; selected = null; tellSel(); kick(); } });
        popup = p; p.addTo(gl);
        setTimeout(() => cardClear(p), 60);
      } else {
        const p = LL.popup({ className: 'vsadsb-pop-' + th, maxWidth: 280, offset: [0, -10], autoPan: false, closeOnClick: false })
          .setLatLng([s.disp.lat, s.disp.lon]).setContent(SC_OPEN + popupHtml + SC_CLOSE);
        p.on('remove', () => { if (popup === p) { popup = null; popupHex = null; selected = null; tellSel(); kick(); } });
        popup = p; p.openOn(lm);
        llClamp();
      }
      requestAnimationFrame(moreCue);   // ★ once it is laid out — and again if the photo arrives and grows it
      setTimeout(moreCue, 600);
      kick();
    }
    /* ★★ The trail of a newly picked aircraft: what this map has seen at once, then the server's whole trail when it
     *  answers (it has kept it since it first heard the aircraft — the line reaches back past when the map opened). */
    function trailPick(hex) {
      trail.hex = hex; trail.at = now(); trail.dirty = true;
      if (imageBase == null || typeof fetch !== 'function') return;
      fetch(imageBase + 'trail/' + encodeURIComponent(hex), { cache: 'no-store' })
        .then((r) => (r.ok ? r.json() : null))
        .then((j) => {
          if (!j || !Array.isArray(j.pts) || trail.hex !== hex || !ac.has(hex)) return;
          const pts = [];
          for (const p of j.pts) {
            if (!Array.isArray(p)) continue;
            const la = num(p[0]), lo = num(p[1]);
            if (la == null || lo == null || Math.abs(la) > 90 || Math.abs(lo) > 180) continue;
            pts.push([la, lo, num(p[2])]);
          }
          const s = ac.get(hex);
          if (pts.length >= 2 && pts.length >= (s.tr ? s.tr.length : 0)) { s.tr = pts; trail.dirty = true; kick(); }
        })
        .catch(() => { /* ★ a server without trails (OWRX, an older VibeServer): this map's own is drawn */ });
    }
    /** Draw (or clear) the selected aircraft's trail. True while the draw-in is still running. */
    function drawTrail(t) {
      const s = selected && trail.hex === selected ? ac.get(selected) : null;
      if (!s || !s.disp || !s.tr || !s.tr.length) {
        if (trail.shown) {
          trail.shown = false;
          if (gl && glReady) {
            const src = gl.getSource('vsadsb-trail');
            if (src) src.setData({ type: 'FeatureCollection', features: [] });
          } else if (trail.ll) { try { lm.removeLayer(trail.ll); } catch (e) { /* gone */ } trail.ll = null; }
        }
        if (!selected) trail.hex = null;
        return false;
      }
      const k = Math.min(1, (t - trail.at) / TRAIL_DRAW_MS), rv = easeInOut(k);
      const drawing = k < 1;
      // ★ its head is the icon, gliding — four times a second is smooth at map scale, and cheap on a 1 GB phone
      if (!trail.dirty && !drawing && t - trail.dataAt < 250) return false;
      trail.dataAt = t; trail.dirty = false; trail.shown = true;
      const pts = s.tr.slice();
      pts.push([s.disp.lat, s.disp.lon, trailAlt(s.a)]);
      if (gl) {
        if (!glReady) return drawing;
        const src = gl.getSource('vsadsb-trail');
        const grad = trailGradient(pts, th, rv);
        if (!src || !grad) return drawing;
        src.setData({ type: 'Feature', properties: {}, geometry: { type: 'LineString', coordinates: pts.map((p) => [p[1], p[0]]) } });
        gl.setPaintProperty('vsadsb-trail', 'line-gradient', grad);
        gl.setPaintProperty('vsadsb-trail-glow', 'line-gradient', grad);
      } else if (!drawing || !trail.ll) {
        // ★ Leaflet: no gradients — short segments, each in its own altitude colour, read as a blend
        if (trail.ll) { try { lm.removeLayer(trail.ll); } catch (e) { /* gone */ } }
        // ★ its own pane, above the basemap (vibemap draws it on a canvas over Leaflet's overlay pane — the first
        //   trail was drawn and hidden under it) and below the aircraft (markerPane, 600)
        if (!lm.getPane('vsadsb-trail')) { const pn = lm.createPane('vsadsb-trail'); pn.style.zIndex = '590'; pn.style.pointerEvents = 'none'; }
        const g = LL.layerGroup();
        const every = Math.max(1, Math.ceil(pts.length / 150));
        let alt = null;
        for (let i = 0; i + 1 < pts.length; i += every) {
          const j = Math.min(pts.length - 1, i + every);
          if (pts[i][2] != null) alt = pts[i][2];
          LL.polyline([[pts[i][0], pts[i][1]], [pts[j][0], pts[j][1]]],
                      { color: altColour(alt != null ? alt : 0, th), weight: 3, opacity: 0.95, interactive: false, lineCap: 'round', pane: 'vsadsb-trail' }).addTo(g);
        }
        g.addTo(lm);
        trail.ll = g;
      }
      return drawing;
    }
    /* ★★ THE CARD'S FIGURES CHANGE EVERY SECOND ("Heard", the message count), ITS PICTURES DO NOT. Replacing the whole card
     *  made new <img>s each report — and an image whose first load had failed showed Safari's "?" flickering every
     *  second (Stuart, 2026-10-24: the list showed the logo, the card did not). Only the table is replaced when nothing
     *  else changed; the logo, silhouette and photo stay the nodes that loaded. */
    const DYN_OPEN = '<div class="vsadsb-dyn">', DYN_CLOSE = '</table></div>';
    function splitDyn(h) {
      const a = h.indexOf(DYN_OPEN);
      if (a < 0) return null;
      const b = h.indexOf(DYN_CLOSE, a);
      if (b < 0) return null;
      return { out: h.slice(0, a) + h.slice(b + DYN_CLOSE.length), dyn: h.slice(a + DYN_OPEN.length, b + '</table>'.length) };
    }
    function setCard(h) {
      const el = popup && popup.getElement ? popup.getElement() : null;
      const o = popupHtml ? splitDyn(popupHtml) : null, n = splitDyn(h);
      if (el && o && n && o.out === n.out) {
        const d = el.querySelector('.vsadsb-dyn');
        if (d) { d.innerHTML = n.dyn; popupHtml = h; moreCue(); return; }
      }
      popupHtml = h;
      renderCard(h);
    }
    /* ★★ THE x IS MADE ONCE PER CARD (Stuart, 2026-11-25, the greyed x). MapLibre's setHTML empties the popup and makes a
     *  NEW close button every time — so a card refreshed between a press and its release (a route or a logo arriving,
     *  the fold) lost the click, and threw away the reader's scroll position. On the GPU map only the card's own scroller
     *  is refilled: MapLibre's x stays the node that was pressed. Leaflet keeps its x outside the content anyway; there
     *  setContent re-lays the popup's width (a folded card is narrow), and the scroll position is put back. */
    const SC_OPEN = '<div class="vsadsb-sc">', SC_CLOSE = '</div>';
    /* ★★ beta96 THE LEAFLET CARD STAYS ON THE MAP (a 390 px map: a card over an aircraft near the left edge sat partly off
     *  it, under the zoom keys). autoPan stays off — the card is refreshed every report and would drag the map each time —
     *  so the card's box (never its tip, which keeps pointing at the aircraft) is slid sideways to fit inside the map. */
    let llClampAt = 0;
    /** ★★ the card clear of the host's chrome (opts.obstacles — the map cards' layout registry): the map moves under it,
     *  by the least that clears every rectangle it would sit on, at most twice. */
    function cardClear(p) {
      if (!gl || !popup || popup !== p || typeof opts.obstacles !== 'function') return;
      const el = p.getElement && p.getElement(), w = el && el.querySelector('.maplibregl-popup-content');
      if (!w) return;
      const a = w.getBoundingClientRect(), m = gl.getContainer().getBoundingClientRect();
      const obs = (opts.obstacles() || []).filter(function (o) { return o && o.r > o.l && o.b > o.t; });
      const hits = function (dx, dy) { return obs.some(function (o) { return a.left + dx < o.r && a.right + dx > o.l && a.top + dy < o.b && a.bottom + dy > o.t; }); };
      if (!hits(0, 0)) return;
      /* ★ every obstacle at once (the avmap e2e at 390 px: one escape at a time slid the card from the zoom keys onto the
       *  left chips and back): the candidate moves are each obstacle's four edges, alone and combined; the shortest move
       *  that clears them ALL and keeps the card inside the map wins; none at all = the card stays where it is */
      const xs = [0], ys = [0];
      for (const o of obs) { xs.push(o.r - a.left + 6, o.l - a.right - 6); ys.push(o.b - a.top + 6, o.t - a.bottom - 6); }
      // ★ none clears everything (a tall card on a phone with the LIST open): the move that leaves the LEAST covered — the
      //   zoom keys and the chips first (they are what a finger needs), the drawer's edge after
      //   ★ the controls a finger needs (weight 1: the zoom keys, the chips) are cleared FIRST — the drawer's edge (a low
      //   weight) only breaks ties: a few px of zoom key under the card cost the 390 px avmap e2e its tap
      const area = function (dx, dy, hard) { let t = 0; for (const o of obs) { const ow = o.w || 1; if (hard ? ow < 1 : ow >= 1) continue; const w = Math.min(a.right + dx, o.r) - Math.max(a.left + dx, o.l), h = Math.min(a.bottom + dy, o.b) - Math.max(a.top + dy, o.t); if (w > 0 && h > 0) t += w * h * ow; } return t; };
      let best = null, bestH = Infinity, bestA = Infinity;
      for (const dx of xs) for (const dy of ys) {
        if (a.left + dx < m.left || a.right + dx > m.right || a.top + dy < m.top || a.bottom + dy > m.bottom) continue;
        const hd = area(dx, dy, true), ar = area(dx, dy, false);
        const better = hd < bestH - 1 || (Math.abs(hd - bestH) <= 1 && (ar < bestA - 1 || (Math.abs(ar - bestA) <= 1 && best && Math.hypot(dx, dy) < Math.hypot(best[0], best[1]))));
        if (better) { best = [dx, dy]; bestH = hd; bestA = ar; }
      }
      if (best && (best[0] || best[1])) gl.panBy([-best[0], -best[1]], { duration: 0 });
    }
    function llClamp() {
      if (gl || !popup || !lm) return;
      try {
        const el = popup.getElement && popup.getElement();
        const w = el && el.querySelector('.leaflet-popup-content-wrapper');
        if (!w) return;
        w.style.transform = '';
        const sc = w.querySelector('.vsadsb-sc');
        if (sc) sc.style.maxHeight = '';
        const mr = lm.getContainer().getBoundingClientRect(), pad = 8;
        let r = w.getBoundingClientRect();
        if (!r.width || !mr.width) return;
        // ★ never taller than the room above the aircraft: the card scrolls inside (its own scroller) instead of running
        //   up under the map's top bar
        const room = r.bottom - (mr.top + pad);
        if (sc && r.height > room) { sc.style.maxHeight = Math.max(60, sc.getBoundingClientRect().height - (r.height - room)) + 'px'; r = w.getBoundingClientRect(); }
        // ★ and clear of the zoom keys (top-left) where the card reaches down beside them
        const zk = lm.getContainer().querySelector('.leaflet-top.leaflet-left .leaflet-control');
        const zr = zk && zk.getBoundingClientRect();
        let left = mr.left + pad;
        if (zr && zr.width && r.top < zr.bottom && zr.right + 6 + r.width <= mr.right - pad) left = zr.right + 6;
        let dx = 0;
        if (r.width > mr.right - pad - left) dx = left - r.left;
        else if (r.left < left) dx = left - r.left;
        else if (r.right > mr.right - pad) dx = mr.right - pad - r.right;
        if (dx) w.style.transform = 'translateX(' + Math.round(dx) + 'px)';
      } catch (e) { console.error('adsblayer: card clamp', e); }
    }
    function renderCard(h) {
      if (!popup) return;
      const el = popup.getElement ? popup.getElement() : null;
      const sc = el ? el.querySelector('.vsadsb-sc') : null;
      if (gl && sc) sc.innerHTML = h;
      else {
        const top = sc ? sc.scrollTop : 0;
        if (gl) popup.setHTML(SC_OPEN + h + SC_CLOSE); else popup.setContent(SC_OPEN + h + SC_CLOSE);
        const sc2 = top && popup.getElement ? popup.getElement().querySelector('.vsadsb-sc') : null;
        if (sc2) sc2.scrollTop = top;
      }
      llClamp();
      moreCue();
    }
    function onMinClick(e) {
      const b = e.target && e.target.closest ? e.target.closest('.vsadsb-min') : null;
      if (!b) return;
      e.preventDefault(); e.stopPropagation();
      cardMin = !cardMin;
      if (selected && popup && ac.has(selected)) {
        popupHtml = cardHtml(ac.get(selected));
        renderCard(popupHtml);
      }
    }
    container.addEventListener('click', onMinClick, true);
    /* ★★ A CARD PICTURE THAT FAILED TO LOAD IS NEVER LEFT BROKEN (Mac Ha, 2026-11-20, Vietnam Airlines: the list showed the
     *  logo, the card a broken image). The card keeps its picture nodes so they do not flicker (setCard), so a logo whose
     *  first load failed — a slow SD card still reading the owner's logos when the server named it — stayed broken until
     *  a restart. Now: hidden at once and tried again once 10 s later; failing again, the logo becomes the operator's text
     *  badge and a silhouette or photo is removed. ('error' does not bubble — caught on the way down.) */
    function onCardImgError(e) {
      const im = e.target;
      if (!im || im.tagName !== 'IMG' || !im.closest || !im.closest('.vsadsb-pop-' + th)) return;
      if (!im.dataset.vsRetry) {
        im.dataset.vsRetry = '1';
        im.style.visibility = 'hidden';
        const src = im.getAttribute('src');
        setTimeout(() => { if (im.isConnected) { im.addEventListener('load', () => { im.style.visibility = ''; }, { once: true }); im.src = src + (src.includes('?') ? '&' : '?') + 'r=1'; } }, 10000);
        return;
      }
      const ph = im.closest('a.ph');
      if (ph) { ph.remove(); return; }
      if (im.classList.contains('sil')) { im.remove(); return; }
      const op = /^[A-Z0-9]{2,4}$/.test(im.alt || '') ? im.alt : '';
      if (op) { const b = document.createElement('span'); b.className = 'badge'; b.textContent = op; im.replaceWith(b); } else im.remove();
    }
    container.addEventListener('error', onCardImgError, true);
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
      let anyPos = false, rangeGrew = false;
      for (const raw of Array.isArray(list) ? list : []) {
        if (!raw || typeof raw !== 'object') continue;
        const hex = cleanHex(raw.hex);
        if (!hex) continue;
        seen.add(hex);
        const lat = num(raw.lat), lon = num(raw.lon);
        const hasPos = lat != null && lon != null && Math.abs(lat) <= 90 && Math.abs(lon) <= 180;
        let s = ac.get(hex);
        if (!s) { s = { hex, a: raw, fix: null, disp: null, corr: null, hdg: null, bornAt: 0, goneAt: 0, shownAt: 0, kind: 'jet' }; ac.set(hex, s); }
        s.a = raw; s.kind = iconClassOf(raw);   // ★ beta96 the one rule: category, else the aircraft data
        /* ★ Heard, but its position has aged out (the server keeps one 60 s): off the map, gently — it stays
         *  in the box's list. Back with a position before it has faded = it simply carries on. */
        if (!hasPos) { if (s.disp && !s.goneAt) s.goneAt = t; continue; }
        if (s.goneAt) { s.goneAt = 0; s.fadeIn = true; s.shownAt = t; }
        anyPos = true;
        const fix = { lat, lon, gs: num(raw.gs), track: num(raw.track) != null ? num(raw.track) : num(raw.mag_heading),
                      t: t - Math.max(0, Math.min(60, num(raw.seen_pos) || 0)) * 1000 };
        const same = s.fix && Math.abs(s.fix.lat - fix.lat) < 1e-7 && Math.abs(s.fix.lon - fix.lon) < 1e-7;
        /* ★★ V12 beta90 FACING ITS WAY (Stuart, 2026-11-21): an aircraft that sends no track (ACARS, VDL2, HFDL, ADS-C) and has
         *  two fixes points along the line between them — the bearing from the one before to this one (a hop under
         *  0.05 NM says nothing: the last bearing stays). It never moves on that bearing: with no speed there is no dead
         *  reckoning, only the glide to each new fix. */
        if (fix.track == null && s.fix) {
          if (!same && distNm(s.fix.lat, s.fix.lon, lat, lon) >= 0.05) fix.track = bearingDeg(s.fix.lat, s.fix.lon, lat, lon);
          else fix.track = s.fix.track;
        }
        if (same) { s.fix.gs = fix.gs; s.fix.track = fix.track; continue; }   // ★ the same fix again: keep flying it
        // ★ beta95 the outline grows live (b96-rangefeed: the position's own time, not the glide's clamped one)
        if (liveRangeFix(s, fix, !!raw.sparse, t - Math.max(0, Math.min(3600, num(raw.seen_pos) || 0)) * 1000)) rangeGrew = true;
        if (!s.tr) s.tr = [];
        if (trailPush(s.tr, lat, lon, trailAlt(raw)) && hex === trail.hex) trail.dirty = true;
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
        /* ★★ V12 beta90 A DATALINK AIRCRAFT GLIDES TO EACH NEW FIX (Stuart, 2026-11-21): minutes apart, its fixes are many
         *  miles apart, so the SNAP rule (a big gap = a bad decode) would fade every move. It slides there instead, over
         *  SPARSE_GLIDE_MS — up to SPARSE_SNAP_NM; past that it is a different answer and fades as ADS-B's does. */
        const sparse = !!raw.sparse;
        if (gap > (sparse ? SPARSE_SNAP_NM : SNAP_NM)) { s.corr = null; s.disp = pred; s.shownAt = t; s.fadeIn = true; }
        else s.corr = { dLat: shown.lat - pred.lat, dLon: wrapLon(shown.lon, pred.lon) - pred.lon, t0: t, ms: sparse ? SPARSE_GLIDE_MS : GLIDE_MS };
      }
      if (anyPos) firstPosBatchSeen = true;
      if (rangeGrew) rangeSoon();
      for (const s of ac.values()) if (!seen.has(s.hex) && !s.goneAt) s.goneAt = t;
      if (selected && popup && ac.has(selected)) {
        const h = cardHtml(ac.get(selected));
        if (h !== popupHtml) setCard(h);
      }
      cameraStep(false);
      kick();
    }

    /* ── the frame loop ─────────────────────────────────────────────────────────────────────── */
    /* ★★★ beta96 IDLE COST, SMOOTH (Stuart 11-27 13:18 "my iphone started burning my leg" — Safari 14.6 %, the GPU at a
     *  steady 64 fps with the map left alone; then 13:30 "the older map is smoother" after a 2 Hz throttle). Every moving
     *  aircraft was dead-reckoned and the whole layer setData'd 18–30 times a second — each setData a worker parse and two
     *  or three repaints — though at country zoom a jet moves a tenth of a pixel a second. Now a flush happens only when
     *  something has VISIBLY changed: an aircraft half a pixel from where it was last drawn (at the map's own zoom), a
     *  turn of 2°, a fade, a ring, a label easing. Zoomed in, that is as smooth as before (up to 30 fps); zoomed out it is
     *  a frame every few seconds. Between flushes nothing runs — the wait is a timer sized to the fastest aircraft's next
     *  half pixel, never a spinning rAF — so the map draws no frame and the page sleeps. DOM markers (Leaflet) 2 Hz. */
    function frameGap(n) {
      void n;
      return lm ? 500 : 33;   // ★ with the GPU drawing the icons this loop only moves the labels — only what is on screen, at the half-pixel rule
    }
    const MOVE_PX = 0.5, TURN_DEG = 2;
    let waitT = 0, forceFlush = true;
    function kick() { if (!rafId && !waitT && !destroyed) rafId = requestAnimationFrame(frame); }
    let lastT = 0;
    function frame() {
      rafId = 0;
      if (destroyed) return;
      const t = now();
      const n = ac.size;
      // ★ the GPU draws the motion: this loop runs at most twice a second (ten with a card open), whoever kicks it
      const gap = gpuOn ? (popup ? 100 : 500) : frameGap(n);
      if (gap && t - lastFlush < gap && !forceFlush) { if (!waitT) waitT = setTimeout(() => { waitT = 0; kick(); }, gap - (t - lastFlush)); return; }
      const dt = lastT ? Math.min(250, t - lastT) : 16; lastT = t;
      lastFlush = t;
      let animating = false;
      for (const s of ac.values()) {
        if (s.goneAt && t - s.goneAt > FADE_OUT_MS) { ac.delete(s.hex); if (selected === s.hex) { selected = null; closePopup(); tellSel(); } removeLl(s); continue; }
        if (!s.fix) continue;
        const p = deadReckon(s.fix, t);
        if (s.corr) {
          const k = Math.min(1, (t - s.corr.t0) / (s.corr.ms || GLIDE_MS));
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
      if (gl && gpuOn) {
        /* ★★★ Stuart 14:00 ("anything that moves, GPU"): icons, labels and the new-aircraft rings are the GPU layer's —
         *  no setData for the aircraft at all. This loop now only decides the labels, keeps the trail and an open card's
         *  place, and hands the GPU new attributes when the data changes: 2 a second (10 with a card open). */
        gpuSync(t);
        if (drawTrail(t)) animating = true;
        if (popup && selected && ac.has(selected)) { const s = ac.get(selected); if (s.disp) popup.setLngLat([s.disp.lon, s.disp.lat]); }
        forceFlush = false;
        if (animating && !waitT && !destroyed) waitT = setTimeout(() => { waitT = 0; kick(); }, popup ? 100 : 500);
        return;
      }
      if (gl) {
        // ★ only when something has visibly changed (above): the half-pixel test at the map's zoom, else wait for it
        const z = gl.getZoom ? gl.getZoom() : 8, ppd = 512 * Math.pow(2, z) / 360;   // CSS px per degree of longitude (512-px tiles)
        const MOVE = gpuOn ? 1.5 : MOVE_PX;
        let moved = forceFlush, fastest = 0;
        let vb2 = null;
        if (gpuOn) { try { const b = gl.getBounds(); vb2 = b; } catch (e) { vb2 = null; } }
        for (const s of ac.values()) {
          if (!s.disp) continue;
          if (vb2 && !vb2.contains([s.disp.lon, s.disp.lat])) continue;
          const ky = ppd / Math.max(0.2, Math.cos(s.disp.lat * Math.PI / 180));
          if (s.goneAt || (!gpuOn && (s.fadeIn || s.corr)) || (s.lo != null && s.lo !== (s.lblWant && !s.goneAt ? 1 : 0)) || (s.bornAt && t - s.bornAt < RING_MS * RING_REPEATS)) moved = true;
          if (!s.fl) { moved = true; continue; }
          const px = Math.max(Math.abs(s.disp.lon - s.fl.lon) * ppd, Math.abs(s.disp.lat - s.fl.lat) * ky);
          if (px >= MOVE || (!gpuOn && Math.abs(angleTo(s.fl.h || 0, s.hdg || 0)) >= TURN_DEG)) moved = true;
          if (s.fix && s.fix.gs > 0) fastest = Math.max(fastest, (s.fix.gs * 1.852 / 3600 / 111.32) * ky);   // px a second
        }
        if (moved) {
          forceFlush = false;
          flushGl(t);
          for (const s of ac.values()) if (s.disp) s.fl = { lat: s.disp.lat, lon: s.disp.lon, h: s.hdg || 0 };
        } else if (animating && fastest > 0) {
          // nothing visible yet: sleep until the fastest aircraft is due its next half pixel (≤ 2 s)
          const wait = Math.min(2000, Math.max(33, (MOVE / fastest) * 1000));
          if (drawTrail(t)) { /* the trail keeps its own pace */ }
          if (!waitT && !destroyed) waitT = setTimeout(() => { waitT = 0; kick(); }, wait);
          return;
        }
      } else flushLl(t);
      if (drawTrail(t)) animating = true;
      if (popup && selected && ac.has(selected)) {
        const s = ac.get(selected);
        if (s.disp) { if (gl) popup.setLngLat([s.disp.lon, s.disp.lat]); else { popup.setLatLng([s.disp.lat, s.disp.lon]); if (t - llClampAt > 200) { llClampAt = t; llClamp(); } } }
      }
      if (animating) kick();
    }
    function labelName(s) { return cleanFlight(s.a.flight) || s.hex.toUpperCase(); }
    /** The altitude line, "12,200 ft ▲", or '' — drawn in the altitude colour (labelAltColour). */
    function labelAlt(s) {
      const alt = altShort(s.a.alt_baro);
      if (!alt) return '';
      const tr = trendOf(s.a);
      return tr ? alt + ' ' + tr : alt;
    }
    function labelAltColour(s) {
      if (s.a.alt_baro === 'ground') return 'rgb(' + TONE.ground + ')';
      return altColour(num(s.a.alt_baro) != null ? num(s.a.alt_baro) : num(s.a.alt_geom), th);
    }
    function labelText(s) { const al = labelAlt(s); return al ? labelName(s) + '\n' + al : labelName(s); }
    function decideLabels() {
      const v = viewSize(), items = [];
      for (const s of ac.values()) {
        if (!s.disp || s.goneAt) continue;
        const p = gl ? gl.project([s.disp.lon, s.disp.lat]) : lm.latLngToContainerPoint([s.disp.lat, s.disp.lon]);
        if (p.x < -20 || p.y < -20 || p.x > v.w + 20 || p.y > v.h + 20) continue;
        const txt = labelText(s), lines = txt.split('\n');
        const w = (Math.max.apply(null, lines.map((l) => l.length)) + 2) * 6.1 + 4, h = lines.length * 11.5 + 2;   // + the arrow
        // ★ Priority: the selected aircraft, an emergency, then the nearest to the receiver (or the newest heard).
        if (prefs.labels === false && s.hex !== selected) continue;   // ★ LABELS off: only the selected one
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
      // ★ with the GPU drawing the icons, the labels' source carries only what is on screen (and a margin): close up a
      //   handful of features at the frame rate, far out all of them a few times a minute
      let vb = null;
      if (gpuOn) { try { const b = gl.getBounds(), w = b.getEast() - b.getWest(), h = b.getNorth() - b.getSouth(); vb = { w: b.getWest() - w * 0.2, e: b.getEast() + w * 0.2, s: b.getSouth() - h * 0.2, n: b.getNorth() + h * 0.2 }; } catch (e) { vb = null; } }
      for (const s of ac.values()) {
        if (!s.disp) continue;
        if (vb && (s.disp.lon < vb.w || s.disp.lon > vb.e || s.disp.lat < vb.s || s.disp.lat > vb.n) && s.hex !== selected) continue;
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
        if (s.lo > 0.01) {
          props.lbl = labelName(s); props.lo = +(s.lo * op).toFixed(3);
          const al = labelAlt(s);   // ★ the altitude line and its arrow, in the altitude colour
          if (al) { props.al = '\n' + al; props.alc = labelAltColour(s); }
        }
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
          const al = labelAlt(s), col = al ? labelAltColour(s) : '';
          const key = labelName(s) + '|' + al + '|' + col;
          if (s.llLbl !== key) {
            s.llLbl = key; lb.textContent = labelName(s);
            if (al) { lb.appendChild(doc.createElement('br')); const sp = doc.createElement('span'); sp.textContent = al; sp.style.color = col; lb.appendChild(sp); }
          }
          lb.style.opacity = s.lblWant && !s.goneAt ? '1' : '0';
        }
      }
    }

    /* ── public ─────────────────────────────────────────────────────────────────────────────── */
    const api = {
      update,
      /** ★ V12 beta90 what is drawn (a test's view, read-only): each aircraft on the map — hex, where it is drawn, the way
       *  it faces, whether a glide is under way. */
      shown() {
        const out = [];
        for (const s of ac.values()) if (s.disp && !s.goneAt) out.push({ hex: s.hex, lat: s.disp.lat, lon: s.disp.lon, hdg: s.hdg, track: s.fix ? s.fix.track : null, gliding: !!s.corr, sparse: !!(s.a && s.a.sparse) });
        return out;
      },
      setReceiver(lat, lon, approx) {
        const la = Number(lat), lo = Number(lon);
        rx = isFinite(la) && isFinite(lo) && !(la === 0 && lo === 0) && lat != null && lon != null ? { lat: la, lon: lo } : null;
        rxNote = rx && typeof approx === 'string' ? approx : ''; rxFromRange = false;   // ★ b96-rangefeed the host's own position wins
        paintCtl(); drawRx(); drawRings(); az.reset();
        if (prefs.autoZoom && !paused) cameraStep(true);
      },
      /** The host's overlay in a corner (its live list), { corner, w, h } in CSS px, or null — see clearRect. */
      setObstacle(o) {
        const n = o && o.w > 0 && o.h > 0 ? { corner: o.corner || 'bottom-left', w: +o.w, h: +o.h } : null;
        const same = (n && obstacle) ? (Math.abs(n.w - obstacle.w) < 4 && Math.abs(n.h - obstacle.h) < 4 && n.corner === obstacle.corner) : n === obstacle;
        obstacle = n;
        placeLegend();
        if (!same && prefs.autoZoom && !paused) cameraStep(false);
      },
      /** Show or hide the colour key (the host's (i) button). on omitted = toggle. Returns whether it is shown. */
      toggleLegend(on) {
        leg.hidden = on == null ? !leg.hidden : !on;
        placeLegend();
        return !leg.hidden;
      },
      setPrefs(p) {
        if (!p) return;
        let moved = false;
        if (typeof p.labels === 'boolean' && prefs.labels !== p.labels) { prefs.labels = p.labels; paintCtl(); labelAt = 0; kick(); }
        let rg = false;
        for (const k of ['range', 'rangeAll']) if (typeof p[k] === 'boolean' && prefs[k] !== p[k]) { prefs[k] = p[k]; rg = true; }
        if (rg) { paintCtl(); paintRangeKey(); drawRange(); if (prefs.range) fetchRange(false); }
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
      /** ★★ beta96 THE CLICK ORDER (receiver → points → lines → areas): a host whose AREA marker took a press asks whether an
       *  aircraft is under it (client px; more slack on touch) — true = this layer selected it and the area must not. */
      pickAt(clientX, clientY, touch) {
        if (clientX == null || clientY == null) return false;
        const rad = touch ? 20 : 12;
        let h = null;
        if (gl) {
          if (!glReady) return false;
          const r = gl.getCanvas().getBoundingClientRect();
          h = hitHexR({ x: clientX - r.left, y: clientY - r.top }, rad);
        } else if (lm) {
          // ★ the Leaflet fallback (a server without a map pack): the nearest drawn aircraft within the slack
          const r = lm.getContainer().getBoundingClientRect(), x = clientX - r.left, y = clientY - r.top;
          let bd = rad;
          for (const s of ac.values()) {
            if (!s.disp || s.goneAt || !s.ll) continue;
            const p = lm.latLngToContainerPoint([s.disp.lat, s.disp.lon]), d = Math.hypot(p.x - x, p.y - y);
            if (d <= bd) { bd = d; h = s.hex; }
          }
        }
        if (!h) return false;
        select(h);
        return true;
      },
      /** ★ beta94 what the range outline draws (a test's view): null until the server has answered or (beta95) the map
       *  has grown one live. live = the bearings grown from this map's own positions; liveFar its furthest. */
      /** ★ b96-rangefeed the receiver the map uses: {lat, lon, from: 'host' | 'range', note} | null (tests, the host) */
      receiverNow() { return rx ? { lat: rx.lat, lon: rx.lon, from: rxFromRange ? 'range' : 'host', note: rxNote } : null; },
      rangeShown() {
        if (!rangeData && !liveHas()) return null;
        const g = rangeGeo();
        return { window: rangeData ? rangeData.window : null, points: g.w ? g.w.length - 1 : 0, allPoints: g.a ? g.a.length - 1 : 0,
                 far: rangeData ? rangeData.far : null, record: rangeData ? rangeData.record : null, title: rangeTitle(),
                 live: liveKm.filter((v) => v > 0).length, liveFar, ring: g.w };
      },
      destroy() {
        destroyed = true;
        try { if (dprMq) dprMq.removeEventListener('change', dprChanged); } catch (e) { /* gone */ }
        if (rafId) cancelAnimationFrame(rafId);
        if (waitT) { clearTimeout(waitT); waitT = 0; }
        if (gpu) { gpu.destroy(); gpu = null; gpuOn = false; }
        if (rangeTimer) clearInterval(rangeTimer);
        if (rangeDrawTimer) clearTimeout(rangeDrawTimer);
        doc.removeEventListener('visibilitychange', onVis);
        closePopup();
        try { ctl.remove(); leg.remove(); } catch (e) { /* gone with the map */ }
        for (const ev of ['pointerdown', 'touchstart', 'wheel', 'keydown']) container.removeEventListener(ev, onInput, { capture: true });
        container.removeEventListener('click', onMinClick, true);
        container.removeEventListener('error', onCardImgError, true);
        if (gl) {
          gl.off('movestart', onMoveStart); gl.off('moveend', onMoveEnd); gl.off('click', onGlClick); gl.off('mousemove', onGlHover);
          for (const id of ['vsadsb-labels', 'vsadsb-icons', 'vsadsb-ring', 'vsadsb-halo', 'vsadsb-trail', 'vsadsb-trail-glow', 'vsadsb-rx', 'vsadsb-ringlbl', 'vsadsb-rings',
                            'vsadsb-range-lbl', 'vsadsb-range-line', 'vsadsb-range-all', 'vsadsb-range-fill']) if (gl.getLayer && gl.getLayer(id)) gl.removeLayer(id);
          for (const id of ['vsadsb-ac', 'vsadsb-trail', 'vsadsb-rx', 'vsadsb-ringlbl', 'vsadsb-rings', 'vsadsb-range']) if (gl.getSource && gl.getSource(id)) gl.removeSource(id);
        } else {
          lm.off('movestart', onMoveStart); lm.off('moveend', onMoveEnd);
          for (const s of ac.values()) removeLl(s);
          if (llRings) lm.removeLayer(llRings);
          if (llRange) lm.removeLayer(llRange);
          if (llRx) lm.removeLayer(llRx);
          if (trail.ll) lm.removeLayer(trail.ll);
        }
        ac.clear();
      },
    };
    return api;
  }

  /* ══════════════════════════════════════════════════════════════════════════════════════════════
   * ★★★ beta96 THE GPU MARKER LAYER (Stuart 11-27: "use the gpu where possible"; docs/v12/MAP-TEMPLATE.md) — moving
   * markers drawn by ONE MapLibre custom WebGL layer: each marker's last fix, its velocity, its correction glide, its
   * heading ease and its fade go up as vertex attributes ONLY when the data changes; every frame sets one uniform (the
   * time) and the vertex shader dead-reckons, rotates and fades. One draw call a frame, next to no CPU.
   *   • 30 fps ambient, and only while something on screen would visibly move (half a pixel at this zoom) or a glide /
   *     fade / turn is under way — a timer drives map.triggerRepaint(); nothing moving → no frames at all. A finger on
   *     the map is MapLibre's own render loop (the display's full rate), which draws this layer as it goes.
   *   • Icons: one texture atlas of canvas-drawn glyphs (iconFor(key) → ImageData, cached by key).
   *   • Picking on demand (pick(point, r)) against the positions extrapolated at that moment.
   *   • WebGL1 and 2; any failure to initialise → ok false, the host keeps its symbol-layer path.
   * Generic: ADS-B uses it first; any map with moving markers (ships, sondes, APRS…) can host it.
   * create(map, { iconBox, dpr, beforeId, iconFor }) → { ok, set(markers), pick(point, r), poke(), destroy() }
   *   marker = { id, lat, lon, t (performance.now ms of the fix), gs (knots), track (deg), maxDrMs, key (icon),
   *              hdgFrom, hdgTo, hdgT0, noRotate, corr: { dLat, dLon, t0, ms } | null, shownAt, goneAt, fadeInMs,
   *              fadeOutMs, z }
   * ══════════════════════════════════════════════════════════════════════════════════════════════ */
  const GpuMarkers = (function () {
    const VS = [
      'precision highp float;',
      'attribute vec2 a_pos; attribute vec2 a_vel; attribute vec4 a_time; attribute vec2 a_corr; attribute vec2 a_corner;',
      'attribute vec4 a_rot; attribute vec2 a_uv; attribute vec4 a_fade; attribute vec4 a_quad; attribute vec2 a_uvs; attribute vec4 a_kind;',
      'uniform mat4 u_mvp; uniform float u_now; uniform vec2 u_px; uniform float u_bearing;',
      'varying vec2 v_uv; varying float v_op; varying float v_kind; varying vec3 v_col; varying float v_r;',
      'float ease(float k) { return k < 0.5 ? 4.0 * k * k * k : 1.0 - pow(-2.0 * k + 2.0, 3.0) / 2.0; }',
      'void main() {',
      '  float dt = clamp(u_now - a_time.x, 0.0, a_time.w);',
      '  vec2 p = a_pos + a_vel * dt;',
      '  if (a_time.z > 0.0) { float k = clamp((u_now - a_time.y) / a_time.z, 0.0, 1.0); p += a_corr * (1.0 - ease(k)); }',
      '  vec4 c = u_mvp * vec4(p, 0.0, 1.0);',
      '  float hk = clamp((u_now - a_rot.z) / 400.0, 0.0, 1.0);',
      '  float d = mod(a_rot.y - a_rot.x + 540.0, 360.0) - 180.0;',
      '  float h = a_rot.w > 0.5 ? 0.0 : radians(a_rot.x + d * hk) - u_bearing;',
      '  float cs = cos(h), sn = sin(h);',
      '  vec2 q = a_corner * a_quad.zw;',
      '  vec2 o = vec2(q.x * cs + q.y * sn, -q.x * sn + q.y * cs) + a_quad.xy;',
      '  c.xy += o * u_px * c.w;',
      '  gl_Position = c;',
      '  v_kind = a_kind.x; v_col = a_kind.yzw / 255.0;',
      '  v_uv = a_kind.x > 1.5 ? a_corner : a_uv + vec2(a_corner.x * 0.5 + 0.5, 0.5 - a_corner.y * 0.5) * a_uvs;',
      '  float op = 1.0;',
      '  if (a_fade.y >= 0.0) op = clamp(1.0 - (u_now - a_fade.y) / a_fade.w, 0.0, 1.0);',
      '  else if (a_fade.x >= 0.0) op = clamp((u_now - a_fade.x) / a_fade.z, 0.0, 1.0);',
      '  v_r = 0.0;',
      '  if (a_kind.x > 1.5) {',   // the new aircraft's ring: three 1.4 s swells from its birth (a_fade.x)
      '    float k = (u_now - a_fade.x) / 1400.0;',
      '    float f = fract(k); float e = 1.0 - (1.0 - f) * (1.0 - f) * (1.0 - f);',
      '    v_r = (13.0 + 26.0 * e) / a_quad.z; op = (k < 0.0 || k > 3.0) ? 0.0 : 0.8 * (1.0 - e);',
      '  }',
      '  v_op = op;',
      '}'].join('\n');
    const FS = ['precision mediump float;', 'uniform sampler2D u_tex; varying vec2 v_uv; varying float v_op; varying float v_kind; varying vec3 v_col; varying float v_r;',
      'void main() {',
      '  if (v_kind > 1.5) { float dd = length(v_uv); float a = 1.0 - smoothstep(0.0, 0.06, abs(dd - v_r)); gl_FragColor = vec4(v_col, 1.0) * a * v_op; }',
      '  else gl_FragColor = texture2D(u_tex, v_uv) * v_op;',
      '}'].join('\n');
    const ATTRS = [['a_pos', 2], ['a_vel', 2], ['a_time', 4], ['a_corr', 2], ['a_corner', 2], ['a_rot', 4], ['a_uv', 2], ['a_fade', 4], ['a_quad', 4], ['a_uvs', 2], ['a_kind', 4]];
    const FLOATS = ATTRS.reduce((t, a) => t + a[1], 0);
    const CORNERS = [[-1, -1], [1, -1], [1, 1], [-1, -1], [1, 1], [-1, 1]];
    const R = 6371.0088 / 1.852;   // the earth in nautical miles
    const nowMs = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

    /** A growable 2-D atlas of equal slots, uploaded whole when it grows, slot by slot after. */
    function Atlas(sw, sh, cols) {
      const a = { sw, sh, cols, rows: 0, cv: null, cx: null, full: true, dirty: [], free: [], n: 0 };
      a.alloc = () => {
        const s = a.free.length ? a.free.pop() : a.n++;
        const need = Math.floor(s / cols) + 1;
        if (need > a.rows) {
          const old = a.cv; a.rows = Math.max(need, a.rows * 2 || 2);
          a.cv = document.createElement('canvas'); a.cv.width = cols * sw; a.cv.height = a.rows * sh;
          a.cx = a.cv.getContext('2d'); if (old) a.cx.drawImage(old, 0, 0);
          a.full = true;
        }
        return s;
      };
      a.xy = (s) => [(s % cols) * sw, Math.floor(s / cols) * sh];
      a.uv = (s) => [(s % cols) / cols, Math.floor(s / cols) / a.rows, 1 / cols, 1 / a.rows];
      a.upload = (g, tex) => {
        if (!a.cv) return;
        g.bindTexture(g.TEXTURE_2D, tex);
        g.pixelStorei(g.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
        if (a.full) {
          g.texImage2D(g.TEXTURE_2D, 0, g.RGBA, g.RGBA, g.UNSIGNED_BYTE, a.cv);
          g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MIN_FILTER, g.LINEAR); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MAG_FILTER, g.LINEAR);
          g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_S, g.CLAMP_TO_EDGE); g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_T, g.CLAMP_TO_EDGE);
          a.full = false; a.dirty = [];
        } else for (const s of a.dirty) { const [x, y] = a.xy(s); g.texSubImage2D(g.TEXTURE_2D, 0, x, y, g.RGBA, g.UNSIGNED_BYTE, a.cx.getImageData(x, y, sw, sh)); }
        a.dirty = [];
        g.pixelStorei(g.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
      };
      return a;
    }

    function create(map, o) {
      const ML = (typeof window !== 'undefined' && window.maplibregl) || null;
      const box = o.iconBox || 40, dpr = o.dpr || 1, S = Math.ceil(box * dpr);
      const LW = o.labelW || 120, LH = o.labelH || 28;
      let drawN = 0, lastErr = '', checked = false;
      const api = { ok: false, fps: 0, pxs: 0, set, pick, poke, destroy,
                    stats: () => ({ n, labels: nl, ok: api.ok, draws: drawN, err: lastErr, fps: api.fps || 0, pxs: api.pxs || 0, why: api.why }) };
      function fail(why) { lastErr = why; console.error('gpumarkers: ' + why + ' — the symbol path draws the markers'); if (api.ok) { api.ok = false; o.onFail && o.onFail(new Error(why)); } }
      if (!ML || !ML.MercatorCoordinate) return api;
      const merc = (lon, lat) => { const m = ML.MercatorCoordinate.fromLngLat({ lng: lon, lat }); return [m.x, m.y]; };
      const T0 = nowMs();
      let glc = null, prog = null, bufI = null, bufL = null, texI = null, texL = null, loc = {}, n = 0, nl = 0, dead = false;
      let markers = [];
      const icons = Atlas(S, S, 8), iconSlot = new Map();
      const labels = Atlas(Math.ceil(LW * dpr), Math.ceil(LH * dpr), 16), labelSlot = new Map();   // id → { s, sig }
      let tickT = 0, rafT = 0, held = 0, downAt = 0;
      const layer = {
        id: o.id || 'vs-gpu-markers', type: 'custom', renderingMode: '2d',
        onAdd(m, g) {
          try {
            glc = g;
            const sh = (type, src) => { const s = g.createShader(type); g.shaderSource(s, src); g.compileShader(s); if (!g.getShaderParameter(s, g.COMPILE_STATUS)) throw new Error(g.getShaderInfoLog(s) || 'shader'); return s; };
            prog = g.createProgram(); g.attachShader(prog, sh(g.VERTEX_SHADER, VS)); g.attachShader(prog, sh(g.FRAGMENT_SHADER, FS)); g.linkProgram(prog);
            if (!g.getProgramParameter(prog, g.LINK_STATUS)) throw new Error(g.getProgramInfoLog(prog) || 'link');
            for (const [a] of ATTRS) loc[a] = g.getAttribLocation(prog, a);
            for (const u of ['u_mvp', 'u_now', 'u_px', 'u_bearing', 'u_tex']) loc[u] = g.getUniformLocation(prog, u);
            bufI = g.createBuffer(); bufL = g.createBuffer(); texI = g.createTexture(); texL = g.createTexture();
            api.ok = true;
            o.onReady && setTimeout(o.onReady, 0);
          } catch (e) { lastErr = String(e && e.message || e); console.error('gpumarkers: no GPU layer — the symbol path stays', e); api.ok = false; o.onFail && o.onFail(e); }
        },
        render(g, args) {
          if (!api.ok || (!n && !nl)) return;
          /* ★ the matrix that takes MERCATOR [0,1] to clip space: v5 hands it as defaultProjectionData.mainMatrix (its
           *  modelViewProjectionMatrix is in world pixels — the planes drew 17 screens off, Stuart 13:51); v3/v4 the matrix */
          const mvp = args && args.defaultProjectionData && args.defaultProjectionData.mainMatrix ? args.defaultProjectionData.mainMatrix
            : args && args.length === 16 ? args : null;
          if (!mvp) { fail('no projection matrix from this MapLibre'); return; }
          if (!checked && markers.length) {   // ★ once per data set: an on-screen marker must land on screen, or fail over
            checked = true;
            const b = map.getBounds(), mk = markers.find((x) => b && b.contains([x.lon, x.lat]));
            if (mk) { const p = merc(mk.lon, mk.lat), M = mvp, c = [0, 1, 3].map((r) => M[r] * p[0] + M[4 + r] * p[1] + M[12 + r]);
              if (!(Math.abs(c[0] / c[2]) <= 1.2 && Math.abs(c[1] / c[2]) <= 1.2)) { fail('the projection put an on-screen marker off-screen'); return; } }
          }
          g.useProgram(prog);
          const cv = map.getCanvas(), pr = g.drawingBufferWidth / Math.max(1, cv.clientWidth || cv.width);
          g.uniformMatrix4fv(loc.u_mvp, false, mvp);
          g.uniform1f(loc.u_now, nowMs() - T0);
          g.uniform2f(loc.u_px, 2 * pr / g.drawingBufferWidth, 2 * pr / g.drawingBufferHeight);
          g.uniform1f(loc.u_bearing, (map.getBearing() || 0) * Math.PI / 180);
          g.uniform1i(loc.u_tex, 0); g.activeTexture(g.TEXTURE0);
          g.enable(g.BLEND); g.blendFunc(g.ONE, g.ONE_MINUS_SRC_ALPHA); g.disable(g.DEPTH_TEST); g.disable(g.STENCIL_TEST); g.disable(g.CULL_FACE); g.colorMask(true, true, true, true);
          const pass = (buf, tex, atlas, count) => {
            if (!count) return;
            if (atlas.full || atlas.dirty.length) atlas.upload(g, tex);
            g.bindTexture(g.TEXTURE_2D, tex);
            g.bindBuffer(g.ARRAY_BUFFER, buf);
            let off = 0;
            for (const [a, k] of ATTRS) { if (loc[a] >= 0) { g.enableVertexAttribArray(loc[a]); g.vertexAttribPointer(loc[a], k, g.FLOAT, false, FLOATS * 4, off * 4); } off += k; }
            g.drawArrays(g.TRIANGLES, 0, count * 6);
            for (const [a] of ATTRS) if (loc[a] >= 0) g.disableVertexAttribArray(loc[a]);
          };
          pass(bufI, texI, icons, n);
          pass(bufL, texL, labels, nl);
          drawN++;
          { const e = g.getError(); if (e) fail('WebGL error ' + e); }
        },
        onRemove() { dead = true; if (tickT) clearTimeout(tickT); if (rafT) cancelAnimationFrame(rafT); }
      };
      try { map.addLayer(layer, o.beforeId && map.getLayer(o.beforeId) ? o.beforeId : undefined); }
      catch (e) { console.error('gpumarkers: addLayer', e); return api; }
      const onMove = () => poke();
      map.on('moveend', onMove); map.on('zoomend', onMove);

      function iconUv(key) {
        let s = iconSlot.get(key);
        if (s == null) {
          s = icons.alloc(); iconSlot.set(key, s);
          const img = o.iconFor(key); const [x, y] = icons.xy(s);
          if (img) icons.cx.putImageData(img, x, y);
          if (!icons.full) icons.dirty.push(s);
        }
        return s;
      }
      /** A marker's label, drawn into its own slot only when its words or colours change. */
      function labelUv(mk) {
        const L = mk.label, sig = L.l1 + '|' + (L.l2 || '') + '|' + (L.c2 || '') + '|' + L.col + L.halo;
        let e = labelSlot.get(mk.id);
        if (!e) { e = { s: labels.alloc(), sig: '' }; labelSlot.set(mk.id, e); }
        if (e.sig !== sig) {
          e.sig = sig;
          const [x, y] = labels.xy(e.s), c = labels.cx, w = labels.sw, h = labels.sh;
          c.save(); c.clearRect(x, y, w, h); c.beginPath(); c.rect(x, y, w, h); c.clip();
          c.scale(1, 1); c.font = (10 * dpr) + 'px ui-monospace, "JetBrains Mono", Menlo, monospace'; c.textBaseline = 'middle'; c.lineJoin = 'round';
          const lh = 11.5 * dpr, two = !!L.l2, y1 = y + h / 2 - (two ? lh / 2 : 0), x0 = x + 2 * dpr;
          c.lineWidth = 2.4 * dpr; c.strokeStyle = L.halo; c.fillStyle = L.col;
          c.strokeText(L.l1, x0, y1); c.fillText(L.l1, x0, y1);
          if (two) { c.strokeText(L.l2, x0, y1 + lh); c.fillStyle = L.c2 || L.col; c.fillText(L.l2, x0, y1 + lh); }
          c.restore();
          if (!labels.full) labels.dirty.push(e.s);
        }
        return e.s;
      }
      function velOf(mk, p) {
        if (!(mk.gs > 0) || mk.track == null || !isFinite(mk.track)) return [0, 0];
        const d = mk.gs / 3600 / R, b = mk.track * Math.PI / 180, la = mk.lat * Math.PI / 180, lo = mk.lon * Math.PI / 180;   // one second on
        const la2 = Math.asin(Math.sin(la) * Math.cos(d) + Math.cos(la) * Math.sin(d) * Math.cos(b));
        const lo2 = lo + Math.atan2(Math.sin(b) * Math.sin(d) * Math.cos(la), Math.cos(d) - Math.sin(la) * Math.sin(la2));
        const q = merc(lo2 * 180 / Math.PI, la2 * 180 / Math.PI); return [(q[0] - p[0]) / 1000, (q[1] - p[1]) / 1000];
      }
      function put(f, i, mk, p, v, corr, rot, uv, fade, quad, kind) {
        for (const [kx, ky] of CORNERS) {
          f[i++] = p[0]; f[i++] = p[1]; f[i++] = v[0]; f[i++] = v[1];
          f[i++] = mk.t - T0; f[i++] = corr[2]; f[i++] = corr[3]; f[i++] = mk.maxDrMs || 0;
          f[i++] = corr[0]; f[i++] = corr[1]; f[i++] = kx; f[i++] = ky;
          f[i++] = rot[0]; f[i++] = rot[1]; f[i++] = rot[2]; f[i++] = rot[3];
          f[i++] = uv[0]; f[i++] = uv[1];
          f[i++] = fade[0]; f[i++] = fade[1]; f[i++] = fade[2]; f[i++] = fade[3];
          f[i++] = quad[0]; f[i++] = quad[1]; f[i++] = quad[2]; f[i++] = quad[3];
          f[i++] = uv[2]; f[i++] = uv[3];
          f[i++] = kind[0]; f[i++] = kind[1]; f[i++] = kind[2]; f[i++] = kind[3];
        }
        return i;
      }
      /** The markers, written as attributes (only on a data change — the shader moves them between). */
      function set(list) {
        if (!api.ok || !glc) return;
        markers = list.slice().sort((a, b) => (a.z || 0) - (b.z || 0)); checked = false;
        // labels whose aircraft went: their slots back to the free list
        const keep = new Set(markers.filter((m) => m.label).map((m) => m.id));
        for (const [id, e] of labelSlot) if (!keep.has(id)) { labelSlot.delete(id); labels.free.push(e.s); }
        const rings = markers.filter((m) => m.bornAt != null);
        const withLbl = markers.filter((m) => m.label);
        // slots first (an atlas may grow and change every uv)
        for (const mk of markers) iconUv(mk.key);
        for (const mk of withLbl) labelUv(mk);
        const fI = new Float32Array((rings.length + markers.length) * 6 * FLOATS), fL = new Float32Array(withLbl.length * 6 * FLOATS);
        let i = 0, j = 0;
        const cache = new Map();
        const geo = (mk) => {
          let g = cache.get(mk.id);
          if (!g) {
            const p = merc(mk.lon, mk.lat), v = velOf(mk, p);
            let corr = [0, 0, 0, 0];
            if (mk.corr) { const a = merc(mk.lon + mk.corr.dLon, mk.lat + mk.corr.dLat); corr = [a[0] - p[0], a[1] - p[1], mk.corr.t0 - T0, mk.corr.ms]; }
            g = { p, v, corr }; cache.set(mk.id, g);
          }
          return g;
        };
        const fadeOf = (mk) => [mk.shownAt != null ? mk.shownAt - T0 : -1, mk.goneAt != null ? mk.goneAt - T0 : -1, mk.fadeInMs || 600, mk.fadeOutMs || 900];
        for (const mk of rings) {   // the new aircraft's three rings, under the icons
          const g = geo(mk), c = mk.rgb || [255, 255, 255];
          i = put(fI, i, mk, g.p, g.v, g.corr, [0, 0, 0, 1], [0, 0, 0, 0], [mk.bornAt - T0, -1, 1, 1], [0, 0, 40, 40], [2, c[0], c[1], c[2]]);
        }
        for (const mk of markers) {
          const g = geo(mk), u = icons.uv(iconSlot.get(mk.key));
          const rot = [mk.hdgFrom != null ? mk.hdgFrom : (mk.hdgTo || 0), mk.hdgTo || 0, (mk.hdgT0 || 0) - T0, mk.noRotate ? 1 : 0];
          i = put(fI, i, mk, g.p, g.v, g.corr, rot, u, fadeOf(mk), [0, 0, box / 2, box / 2], [0, 0, 0, 0]);
        }
        for (const mk of withLbl) {   // the label: upright, its left edge 12.5 px right of the icon's centre (MapLibre's 1.25 em)
          const g = geo(mk), u = labels.uv(labelSlot.get(mk.id).s);
          const lf = [mk.label.at != null ? mk.label.at - T0 : -1, mk.goneAt != null ? mk.goneAt - T0 : -1, 400, mk.fadeOutMs || 900];
          j = put(fL, j, mk, g.p, g.v, g.corr, [0, 0, 0, 1], u, lf, [12.5 + LW / 2 - 2, 0, LW / 2, LH / 2], [1, 0, 0, 0]);
        }
        glc.bindBuffer(glc.ARRAY_BUFFER, bufI); glc.bufferData(glc.ARRAY_BUFFER, fI, glc.DYNAMIC_DRAW);
        glc.bindBuffer(glc.ARRAY_BUFFER, bufL); glc.bufferData(glc.ARRAY_BUFFER, fL, glc.DYNAMIC_DRAW);
        n = rings.length + markers.length; nl = withLbl.length;
        poke();
      }
      /* ★ the frame policy (Stuart 13:58): ADAPTIVE — fast enough that no on-screen marker steps more than ~1.5 px a frame
       *  (5 fps far out, the display's own rate up close), a glide / turn / fade / ring at least 30 fps; nothing moving →
       *  no frames. High rates ride the display's frame clock, low ones a timer, so a slow scene lets the panel drop. */
      function poke() { if (tickT) { clearTimeout(tickT); tickT = 0; } if (rafT) { cancelAnimationFrame(rafT); rafT = 0; } tick(); }
      function tick() {
        tickT = 0; rafT = 0;
        if (dead || !api.ok || !n) return;
        const t = nowMs();
        const z = map.getZoom(), ppd = 512 * Math.pow(2, z) / 360, bnd = map.getBounds();
        let fastest = 0, easing = false, need = 0, fixed = false; const why = {};
        const ez = (k) => { easing = true; why[k] = (why[k] || 0) + 1; };
        for (const mk of markers) {
          const onScreen = !bnd || bnd.contains([mk.lon, mk.lat]);
          const kyy = ppd / Math.max(0.2, Math.cos(mk.lat * Math.PI / 180));
          // ★ a glide or a turn counts only on screen and only if it is visible (half a pixel / two degrees)
          // …and at the rate its own speed needs, not a flat 30: a glide of g px over its ms, a turn of a° (the nose, 20 px out)
          if (onScreen && mk.corr && t - mk.corr.t0 < mk.corr.ms) { const g = Math.max(Math.abs(mk.corr.dLon) * ppd, Math.abs(mk.corr.dLat) * kyy); if (g >= 0.5) { ez('glide'); need = Math.max(need, 1.9 * g / (mk.corr.ms / 1000) / 1.5); } }
          if (onScreen && mk.hdgT0 && t - mk.hdgT0 < 400) { const a = Math.abs((((mk.hdgTo || 0) - (mk.hdgFrom || 0) + 540) % 360) - 180); if (a >= 3) { ez('turn'); need = Math.max(need, 1.9 * (a * Math.PI / 180 * 20) / 0.4 / 1.5); } }
          if (onScreen && ((mk.goneAt != null && t - mk.goneAt < (mk.fadeOutMs || 900)) || (mk.shownAt != null && t - mk.shownAt < (mk.fadeInMs || 600)))) { ez('fade'); fixed = true; }
          if (onScreen && mk.bornAt != null && t - mk.bornAt < 4200) { ez('ring'); fixed = true; }
          if (onScreen && mk.label && mk.label.at != null && t - mk.label.at < 400) { ez('label'); fixed = true; }
          if (!(mk.gs > 0) || t - mk.t > (mk.maxDrMs || 0)) continue;
          if (bnd && !bnd.contains([mk.lon, mk.lat])) continue;
          const ky = ppd / Math.max(0.2, Math.cos(mk.lat * Math.PI / 180));
          fastest = Math.max(fastest, (mk.gs * 1.852 / 3600 / 111.32) * ky);   // CSS px a second
        }
        const want = Math.max(fastest / 1.5, need, fixed ? 12 : 0);   // ★ a ring / fade / label easing in: 12 fps is smooth enough for opacity and a 19 px/s swell
        let fps = want > 0 ? Math.min(120, Math.max(5, Math.ceil(want))) : 0;
        void easing;
        // ★ hysteresis (Stuart 14:07 "it drops to 5 fps every so often"): up at once, down only after a second below
        if (fps >= held) { held = fps; downAt = 0; }
        else { if (!downAt) downAt = t; if (t - downAt > 1000) { held = fps; downAt = 0; } }
        fps = held;
        api.fps = fps; api.pxs = Math.round(fastest); api.why = why;
        if (!fps) return;   // ★ nothing on screen moves: no frames
        map.triggerRepaint();
        if (fps >= 50) rafT = requestAnimationFrame(tick);
        else tickT = setTimeout(tick, 1000 / fps);
      }
      /** The marker under a point (CSS px), positions extrapolated now — on a tap or a hover only. */
      function pick(point, r) {
        const t = nowMs();
        let best = null, bd = r;
        for (const mk of markers) {
          if (mk.goneAt != null) continue;
          let lat = mk.lat, lon = mk.lon;
          if (mk.gs > 0 && mk.track != null) {
            const dt = Math.max(0, Math.min(mk.maxDrMs || 0, t - mk.t)) / 1000, d = mk.gs * dt / 3600 / R, b = mk.track * Math.PI / 180, la = lat * Math.PI / 180;
            const la2 = Math.asin(Math.sin(la) * Math.cos(d) + Math.cos(la) * Math.sin(d) * Math.cos(b));
            lon = lon + Math.atan2(Math.sin(b) * Math.sin(d) * Math.cos(la), Math.cos(d) - Math.sin(la) * Math.sin(la2)) * 180 / Math.PI; lat = la2 * 180 / Math.PI;
          }
          if (mk.corr) { const k = Math.min(1, (t - mk.corr.t0) / mk.corr.ms), e = 1 - easeInOut(k); lat += mk.corr.dLat * e; lon += mk.corr.dLon * e; }
          const p = map.project([lon, lat]), dd = Math.hypot(p.x - point.x, p.y - point.y);
          if (dd < bd) { bd = dd; best = mk.id; }
        }
        return best;
      }
      function destroy() { dead = true; if (tickT) clearTimeout(tickT); if (rafT) cancelAnimationFrame(rafT); try { map.off('moveend', onMove); map.off('zoomend', onMove); if (map.getLayer(layer.id)) map.removeLayer(layer.id); } catch (e) { /* gone with the map */ } }
      if (typeof window !== 'undefined') (window.__vsGpu = window.__vsGpu || []).push(api);
      return api;
    }
    return { create };
  })();

  /* ★ What the NDB layer (ndblayer.js) borrows so its map looks and measures exactly like this one: the rings, the
   *  theme, the receiver's house, the camera and the CSS (its ring labels' classes). */
  const shared = { ringsFor, addRingLayers, setRingData, leafletRings, THEME, RX_SVG, injectCss };
  global.VibeAdsbLayer = { create, maths, shared, ICONS: Object.keys(ICONS), iconSvg, iconClassOf, acIconSvg, AC_CLASS_NAME };
  global.VibeGpuMarkers = GpuMarkers;   // ★ the template's moving-marker layer (docs/v12/MAP-TEMPLATE.md)
})(typeof window !== 'undefined' ? window : globalThis);
