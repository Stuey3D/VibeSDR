/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * vibemapgl.js — THE GPU BASEMAP. MapLibre GL JS (WebGL 2) drawing our own vector tiles, relief,
 * fonts and icons, with the same style in every host: the app's map WebView, the server's web client
 * (listening map and admin screens) and the directory.
 *
 * ★★★ WHY. Stuart, 2026-09-27: "that is what I've been after ever since we had the animated maps, I
 *     asked if we could GPU accelerate to improve animations and the tiles rendering in". Leaflet draws
 *     on the CPU and redraws the view after every pan; this draws tiles on the GPU. MEASURED through the
 *     HFDL flyover (the test Stuart uses — "if they survive that they survive anything"), same engine,
 *     full style: Leaflet 49 fps / 6.5 % frames over 33 ms / worst 233 ms; this 60 fps locked / 0 % / 17 ms.
 * ★★ ROUTE B (Stuart chose it over native MapLibre): the same WebView, so every host shares one renderer
 *    and all the HFDL / spots logic in the pages stays where it is.
 *
 *   const vm = await VibeMapGL.create(container, {
 *     style,          // web/mapkit/vibemap-style.json, already parsed
 *     load,           // (path, 'arraybuffer' | 'text') => Promise<ArrayBuffer | string | null>
 *     base,           // where the packs live, e.g. 'mapgl/' (app) or '/mapgl/' (a server)
 *     rangeBase,      // OPTIONAL http(s) URL of the packs: stream them by HTTP range instead of loading
 *                     //   them whole (a browser should not download 18 MB per visit)
 *     profile,        // 'directory' | 'aero' | 'marine' | 'spots' | 'admin'
 *     center, zoom,   // [lon, lat] and a MapLibre zoom (= Leaflet zoom - 1)
 *   });
 *   // vm === null  ->  no WebGL 2 here: the host keeps its Leaflet map.
 *
 * ★ Loaded AFTER vendor/maplibre-gl.js and vendor/pmtiles.js (both BSD, unmodified).
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
(function (global) {
  'use strict';
  const ICONS = ['vs-aircraft', 'vs-airport', 'vs-heli', 'vs-port'];

  /** ★ WebGL 2 is what MapLibre GL JS 5 needs. Every iPhone on the app's iOS 16.4 floor has it; a few
   *  old Android GPUs / never-updated System WebViews do not — they keep the Leaflet map, never a blank. */
  function webgl2() {
    try { return !!document.createElement('canvas').getContext('webgl2'); } catch (e) { return false; }
  }

  /** A pmtiles Source over a buffer already in memory (the app reads each pack whole, off disk). */
  function bufferSource(key, buf) {
    return {
      getKey: () => key,
      getBytes: async (offset, length) => ({ data: buf.slice(offset, offset + length) }),
    };
  }

  /** The style with this host's URLs, the detail pack's presence and the profile applied — a pure
   *  function of its inputs, so every host gets the same answer from the same facts. */
  function prepareStyle(src, { hasDetail, basicUrl, reliefUrl, detailUrl, runwaysUrl, profile }) {
    const st = JSON.parse(JSON.stringify(src));
    st.glyphs = 'vs://fonts/{fontstack}/{range}.pbf';
    st.sources.basic = Object.assign({}, st.sources.basic, { url: basicUrl });
    st.sources.relief = Object.assign({}, st.sources.relief, { url: reliefUrl });
    // ★ Runways: their own small bundled pack, so a correction never needs the detail download again.
    if (st.sources.runways) {
      if (runwaysUrl) st.sources.runways = Object.assign({}, st.sources.runways, { url: runwaysUrl });
      else { delete st.sources.runways; st.layers = st.layers.filter((l) => l.source !== 'runways'); }
    }
    if (hasDetail) {
      st.sources.detail = Object.assign({}, st.sources.detail, { url: detailUrl });
      // ★ Layers that only stand in for detail ones (runways-coarse) go, so nothing is drawn twice.
      const only = new Set((st.metadata && st.metadata['vibesdr:basicOnlyLayers']) || []);
      st.layers = st.layers.filter((l) => !only.has(l.id));
    } else {
      // ★ No detail pack: drop its layers and let the basic ones carry on over-zoomed (see the style's
      //   vibesdr:basicOnlyExtend) — a coarser map, never a hole.
      delete st.sources.detail;
      st.layers = st.layers.filter((l) => l.source !== 'detail');
      const extend = new Set((st.metadata && st.metadata['vibesdr:basicOnlyExtend']) || []);
      for (const l of st.layers) if (extend.has(l.id)) delete l.maxzoom;
    }
    applyProfileToStyle(st, profile);
    applyTextScale(st);
    return st;
  }
  /* ★ ONE KNOB FOR LABEL SIZE (vibesdr:textScale), not forty hand edits. Stuart, 2026-09-27: "all fonts
   *  could be a little bigger". A zoom curve must stay the TOP-LEVEL expression (MapLibre's rule), so its
   *  stop outputs are scaled; anything else is wrapped whole. */
  function applyTextScale(st) {
    const k = st.metadata && st.metadata['vibesdr:textScale'];
    if (!k || k === 1) return;
    const scale = (v) => (typeof v === 'number' ? Math.round(v * k * 10) / 10 : ['*', k, v]);
    for (const l of st.layers) {
      const t = l.layout && l.layout['text-size'];
      if (t === undefined) continue;
      if (Array.isArray(t) && (t[0] === 'interpolate' || t[0] === 'step') && JSON.stringify(t[t[0] === 'step' ? 1 : 2]) === '["zoom"]') {
        const out = t.slice();
        // interpolate: [op, curve, input, z0, v0, z1, v1…]; step: [op, input, v0, z1, v1…]
        for (let i = t[0] === 'step' ? 2 : 4; i < out.length; i += 2) out[i] = scale(out[i]);
        l.layout['text-size'] = out;
      } else {
        l.layout['text-size'] = scale(t);
      }
    }
  }
  function applyProfileToStyle(st, name) {
    const P = (st.metadata && st.metadata['vibesdr:profiles']) || {};
    const p = P[name] || P.directory || { hide: [] };
    const hide = new Set(p.hide || []);
    for (const l of st.layers) {
      l.layout = Object.assign({}, l.layout, { visibility: hide.has(l.id) ? 'none' : 'visible' });
    }
    if (p.places === 'major' && P._majorFilter) {
      for (const id of P._placeLayers || []) {
        const l = st.layers.find((x) => x.id === id);
        if (l) l.filter = P._majorFilter;
      }
    }
  }

  /* ══ THE GREYLINE — where it is night right now ══════════════════════════════════════════════════
   * ★★ Stuart, 2026-09-27: on the HFDL map too — "be interesting to see how many planes are flying at
   *    night vs the day". For this audience it is information, not decoration: HF propagates differently
   *    either side of it. Computed here (no Leaflet plugin), so every host draws the same line.
   * ★ Same astronomy as L.Terminator, which the directory used: the sun's right ascension and
   *   declination from the date, then for each longitude the latitude where the sun is on the horizon.
   *   The night side is closed over whichever pole is dark. Clamped to ±85° (Web Mercator's edge). */
  function nightRing(date) {
    const rad = Math.PI / 180, jd = date.getTime() / 86400000 + 2440587.5, n = jd - 2451545;
    const gmst = (18.697374558 + 24.06570982441908 * n) % 24;
    const Lm = (280.46 + 0.9856474 * n) % 360, g = (357.528 + 0.9856003 * n) % 360;
    const lambda = (Lm + 1.915 * Math.sin(g * rad) + 0.02 * Math.sin(2 * g * rad)) * rad;
    const eps = (23.4393 - 0.0000004 * n) * rad;
    const alpha = Math.atan2(Math.cos(eps) * Math.sin(lambda), Math.cos(lambda)) / rad;
    const delta = Math.asin(Math.sin(eps) * Math.sin(lambda));
    const ring = [];
    for (let lng = -180; lng <= 180; lng += 1) {
      const ha = (gmst * 15 + lng - alpha) * rad;
      const lat = Math.atan(-Math.cos(ha) / Math.tan(delta)) / rad;
      ring.push([lng, Math.max(-85, Math.min(85, lat))]);
    }
    const pole = delta < 0 ? 85 : -85;          // ★ the dark pole: the north in the southern summer
    ring.unshift([-180, pole]); ring.push([180, pole]); ring.push([-180, pole]);
    return ring;
  }

  /**
   * ★★ SYNCHRONOUS, BECAUSE THE PAGES ARE. A host page calls L.map(...) and uses the map on the next
   *  line; MapLibre's constructor is synchronous, so the map exists at once — sea-coloured — and the
   *  packs, style and icons arrive a moment later. Camera moves and DOM markers work immediately;
   *  anything that needs the style (a GeoJSON layer) waits for `ready`.
   *  Returns null where there is no WebGL 2: the host keeps Leaflet.
   */
  function createNow(container, o) {
    if (!webgl2()) return null;
    const ml = global.maplibregl, pm = global.pmtiles;
    if (!ml || !pm) throw new Error('vibemapgl: load vendor/maplibre-gl.js and vendor/pmtiles.js first');
    const base = o.base || 'mapgl/';
    const seaOnly = { version: 8, sources: {}, layers: [{ id: 'sea', type: 'background',
      paint: { 'background-color': ((o.style.layers || []).find((l) => l.id === 'sea') || { paint: {} }).paint['background-color'] || '#123049' } }] };
    const map = new ml.Map({
      container, style: seaOnly,
      center: o.center || [0, 30], zoom: o.zoom != null ? o.zoom : 1.5,
      /* ★ CREDITS, from each tile source's own `attribution` — so what is credited is exactly what is
       *  drawn: RESOLVE (CC BY) with the relief; GeoNames, HydroLAKES (CC BY) and OpenStreetMap (ODbL)
       *  only when the detail pack is in. The same obligation Leaflet's attribution control met. */
      attributionControl: { compact: true }, fadeDuration: 0,
      renderWorldCopies: true,          // ★ the world repeats sideways, as worldOffsets() drew by hand
      maxPitch: 0, dragRotate: false, pitchWithRotate: false, touchPitch: false,
    });
    map.touchZoomRotate.disableRotation();
    /* ★★ SAFARI'S TRACKPAD PINCH ZOOMS THE MAP, NOT THE PAGE. On a Mac, Safari reports a pinch as its own
     *  gesturestart/gesturechange/gestureend events (Chrome sends ctrl+wheel, which MapLibre handles);
     *  MapLibre does not listen for them, so Safari zoomed the WHOLE PAGE (Stuart, 2026-09-28, the spots
     *  map). Taken here: the page zoom is refused over the map and the scale drives the map's zoom,
     *  anchored where the pinch is. Harmless elsewhere — no other engine fires these events. */
    {
      let z0 = null;
      const pt = (e) => { const r = container.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
      container.addEventListener('gesturestart', (e) => { e.preventDefault(); z0 = map.getZoom(); }, { passive: false });
      container.addEventListener('gesturechange', (e) => {
        e.preventDefault();
        if (z0 == null || !(e.scale > 0)) return;
        const around = (e.clientX != null) ? map.unproject(pt(e)) : map.getCenter();
        map.easeTo({ zoom: z0 + Math.log2(e.scale), around, duration: 0 });
      }, { passive: false });
      container.addEventListener('gestureend', (e) => { e.preventDefault(); z0 = null; }, { passive: false });
    }
    /* ★ A LOST GPU CONTEXT (backgrounded, memory pressure) is not a dead map: MapLibre restores its own
     *  resources on 'webglcontextrestored'. The host is told either way. */
    map.on('webglcontextlost', () => { if (o.onContextLost) o.onContextLost(); });

    let profile = o.profile || 'directory';
    const ready = (async () => {
      /* ★★ FONTS THROUGH OUR OWN PROTOCOL: a file:// response has status 0 and MapLibre's own fetch
       *  would call that a failure — the trap that made the app's map skip its disk for weeks. The
       *  host's loader judges success by the BODY. */
      ml.addProtocol('vs', async (params) => {
        const path = params.url.replace(/^vs:\/\//, '');
        const data = await o.load(base + decodeURIComponent(path), 'arraybuffer');
        if (!data) throw new Error('vibemapgl: missing ' + path);
        return { data };
      });
      const protocol = new pm.Protocol();
      ml.addProtocol('pmtiles', protocol.tile);
      let basicUrl, reliefUrl = null, detailUrl = null, runwaysUrl = null, hasDetail = false;
      if (o.rangeBase) {
        // ★ A browser: stream by HTTP range, only the tiles in view.
        basicUrl = 'pmtiles://' + o.rangeBase + 'vibemap-basic.pmtiles';
        reliefUrl = 'pmtiles://' + o.rangeBase + 'vibemap-relief.pmtiles';
        runwaysUrl = 'pmtiles://' + o.rangeBase + 'vibemap-runways.pmtiles';
        if (o.detail) { hasDetail = true; detailUrl = 'pmtiles://' + o.rangeBase + 'vibemap-detail.pmtiles'; }
      } else {
        // ★ The app: each bundled pack read ONCE off disk (milliseconds) and served from memory.
        const [b, r] = await Promise.all([o.load(base + 'vibemap-basic.pmtiles', 'arraybuffer'),
                                          o.load(base + 'vibemap-relief.pmtiles', 'arraybuffer')]);
        if (!b) throw new Error('vibemapgl: the basic pack could not be read');
        protocol.add(new pm.PMTiles(bufferSource('basic', b))); basicUrl = 'pmtiles://basic';
        if (r) { protocol.add(new pm.PMTiles(bufferSource('relief', r))); reliefUrl = 'pmtiles://relief'; }
        const rw = await o.load(base + 'vibemap-runways.pmtiles', 'arraybuffer');
        if (rw) { protocol.add(new pm.PMTiles(bufferSource('runways', rw))); runwaysUrl = 'pmtiles://runways'; }
        else console.error('vibemapgl: vibemap-runways.pmtiles missing — no runways');
        /* ★★ THE OPTIONAL DETAIL PACK (~169 MB) IS NOT READ WHOLE: the host supplies ranged reads
         *  (the app answers them over the bridge from the one file on disk — mapglDetail.ts). Only the
         *  bytes a close-zoom view needs ever cross. */
        if (o.detailRange) {
          protocol.add(new pm.PMTiles({
            getKey: () => 'detail',
            getBytes: async (offset, length) => {
              const data = await o.detailRange(offset, length);
              if (!data) throw new Error('vibemapgl: detail read failed');
              return { data };
            },
          }));
          hasDetail = true; detailUrl = 'pmtiles://detail';
        }
      }
      const style = prepareStyle(o.style, { hasDetail, basicUrl, reliefUrl, detailUrl, runwaysUrl, profile });
      if (!reliefUrl) { delete style.sources.relief; style.layers = style.layers.filter((l) => l.source !== 'relief'); }
      /* ★★★ diff:false, OR `ready` CAN HANG FOREVER. setStyle defaults to applying the new style as a DIFF
       *  against the sea-only one — and a diff never fires 'style.load'. Whether the wait below then
       *  finished depended on whether isStyleLoaded() happened to be true at that instant: the HFDL page
       *  and the directory got through, the DIGITAL SPOTS page did not — so the spot circles (added on
       *  `ready`), the icons and the greyline never arrived: "72 spots", none drawn (Stuart, build 345).
       *  A full load always fires 'style.load'. The timeout is so this can never again wait silently. */
      map.setStyle(style, { diff: false });
      await new Promise((res) => {
        if (map.isStyleLoaded()) return res();
        const t = setTimeout(() => { console.error('vibemapgl: style.load not seen in 10 s — carrying on'); res(); }, 10000);
        map.once('style.load', () => { clearTimeout(t); res(); });
      });
      // ★ Credits start COLLAPSED (MapLibre opens its compact panel at first) — a tap on (i) shows them,
      //   as Leaflet's small strip did, without a white panel over the bottom of the map.
      const attrib = container.querySelector && container.querySelector('.maplibregl-ctrl-attrib');
      if (attrib) attrib.classList.remove('maplibregl-compact-show');
      // ★ The icons are white SDFs: the style tints them (airports amber, heliports grey, aircraft by age).
      for (const n of ICONS) {
        if (map.hasImage(n)) continue;
        const buf = await o.load(base + 'icons/' + n + '.png', 'arraybuffer');
        if (!buf) continue;
        const bmp = await createImageBitmap(new Blob([buf], { type: 'image/png' }));
        map.addImage(n, bmp, { sdf: true, pixelRatio: 2 });
      }
      installAirportNames();
      return true;
    })();

    /* ★★ AN AIRPORT'S FULL NAME ON HOVER (or tap). The Leaflet map put it in a DOM title; here the codes are
     *  drawn on the GPU, so the map shows it itself (Stuart, 2026-09-28, the directory: "the hover over
     *  airport codes to show the full name is not working"). One small popup, reused. */
    function installAirportNames() {
      const ids = map.getStyle().layers.filter((l) => l['source-layer'] === 'airports').map((l) => l.id);
      if (!ids.length) return;
      const tip = new ml.Popup({ closeButton: false, closeOnClick: false, className: 'vs-airport-tip', offset: 10, maxWidth: '260px' });
      const text = (p) => {
        const code = [p.icao, p.iata].filter(Boolean).join(' / ');
        const el = document.createElement('div');
        // ★ Wraps: "Charles de Gaulle International Airport" overflowed a nowrap box on a phone (2026-09-28).
        el.style.cssText = 'font-size:12px;line-height:1.35;white-space:normal;overflow-wrap:anywhere';
        el.textContent = p.name || code;          // ★ textContent: a name is data, never markup
        if (p.name && code) { const c = document.createElement('div'); c.style.opacity = '0.6'; c.textContent = code; el.appendChild(c); }
        return el;
      };
      const show = (e) => {
        const f = e.features && e.features[0];
        if (!f || !(f.properties.name || f.properties.icao)) return;
        map.getCanvas().style.cursor = 'help';
        tip.setLngLat(f.geometry.coordinates).setDOMContent(text(f.properties)).addTo(map);
      };
      const hide = () => { map.getCanvas().style.cursor = ''; tip.remove(); };
      for (const id of ids) {
        map.on('mousemove', id, show);
        map.on('mouseleave', id, hide);
        map.on('click', id, show);                 // ★ touch has no hover
      }
      map.on('movestart', hide);
      // ★ Touch has no mouseleave: a tap anywhere that is not an airport puts the name away.
      map.on('click', (e) => { if (!map.queryRenderedFeatures(e.point, { layers: ids }).length) hide(); });
    }

    return {
      map, ready,
      profile: () => profile,
      /** Switch what the map is FOR, without rebuilding it. */
      setProfile(name) {
        const P = (o.style.metadata && o.style.metadata['vibesdr:profiles']) || {};
        if (!P[name]) return false;
        profile = name;
        const hide = new Set(P[name].hide || []);
        for (const l of o.style.layers) if (map.getLayer(l.id)) map.setLayoutProperty(l.id, 'visibility', hide.has(l.id) ? 'none' : 'visible');
        for (const id of P._placeLayers || []) {
          if (!map.getLayer(id)) continue;
          const orig = o.style.layers.find((x) => x.id === id);
          map.setFilter(id, P[name].places === 'major' ? P._majorFilter : (orig && orig.filter) || null);
        }
        return true;
      },
      /** The night shadow on (refreshed every minute) or off. Drawn under the labels and markers. */
      setNight(on, opacity) {
        clearInterval(this._nightT); this._nightT = null;
        ready.then(() => {
          const data = () => ({ type: 'Feature', properties: {}, geometry: { type: 'Polygon', coordinates: [nightRing(new Date())] } });
          if (!on) { if (map.getLayer('vs-night')) map.removeLayer('vs-night'); if (map.getSource('vs-night')) map.removeSource('vs-night'); return; }
          if (!map.getSource('vs-night')) {
            map.addSource('vs-night', { type: 'geojson', data: data() });
            const under = map.getStyle().layers.find((l) => l.type === 'symbol');
            map.addLayer({ id: 'vs-night', type: 'fill', source: 'vs-night',
              paint: { 'fill-color': '#000', 'fill-opacity': opacity != null ? opacity : 0.28, 'fill-antialias': false } }, under && under.id);
          }
          this._nightT = setInterval(() => { const s = map.getSource('vs-night'); if (s) s.setData(data()); }, 60000);
        });
      },
      destroy() { clearInterval(this._nightT); map.remove(); },
    };
  }
  /** The same, for a host that can wait: resolves once the style is in. */
  async function create(container, o) {
    const vm = createNow(container, o);
    if (!vm) return null;
    await vm.ready;
    return vm;
  }

  global.VibeMapGL = { create, createNow, prepareStyle, webgl2, nightRing };
})(typeof window !== 'undefined' ? window : globalThis);
