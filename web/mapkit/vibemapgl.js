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
  function prepareStyle(src, { hasDetail, basicUrl, reliefUrl, detailUrl, profile }) {
    const st = JSON.parse(JSON.stringify(src));
    st.glyphs = 'vs://fonts/{fontstack}/{range}.pbf';
    st.sources.basic = Object.assign({}, st.sources.basic, { url: basicUrl });
    st.sources.relief = Object.assign({}, st.sources.relief, { url: reliefUrl });
    if (hasDetail) {
      st.sources.detail = Object.assign({}, st.sources.detail, { url: detailUrl });
    } else {
      // ★ No detail pack: drop its layers and let the basic ones carry on over-zoomed (see the style's
      //   vibesdr:basicOnlyExtend) — a coarser map, never a hole.
      delete st.sources.detail;
      st.layers = st.layers.filter((l) => l.source !== 'detail');
      const extend = new Set((st.metadata && st.metadata['vibesdr:basicOnlyExtend']) || []);
      for (const l of st.layers) if (extend.has(l.id)) delete l.maxzoom;
    }
    applyProfileToStyle(st, profile);
    return st;
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
      let basicUrl, reliefUrl = null, detailUrl = null, hasDetail = false;
      if (o.rangeBase) {
        // ★ A browser: stream by HTTP range, only the tiles in view.
        basicUrl = 'pmtiles://' + o.rangeBase + 'vibemap-basic.pmtiles';
        reliefUrl = 'pmtiles://' + o.rangeBase + 'vibemap-relief.pmtiles';
        if (o.detail) { hasDetail = true; detailUrl = 'pmtiles://' + o.rangeBase + 'vibemap-detail.pmtiles'; }
      } else {
        // ★ The app: each bundled pack read ONCE off disk (milliseconds) and served from memory.
        const [b, r] = await Promise.all([o.load(base + 'vibemap-basic.pmtiles', 'arraybuffer'),
                                          o.load(base + 'vibemap-relief.pmtiles', 'arraybuffer')]);
        if (!b) throw new Error('vibemapgl: the basic pack could not be read');
        protocol.add(new pm.PMTiles(bufferSource('basic', b))); basicUrl = 'pmtiles://basic';
        if (r) { protocol.add(new pm.PMTiles(bufferSource('relief', r))); reliefUrl = 'pmtiles://relief'; }
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
      const style = prepareStyle(o.style, { hasDetail, basicUrl, reliefUrl, detailUrl, profile });
      if (!reliefUrl) { delete style.sources.relief; style.layers = style.layers.filter((l) => l.source !== 'relief'); }
      map.setStyle(style);
      await new Promise((res) => (map.isStyleLoaded() ? res() : map.once('style.load', res)));
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
      return true;
    })();

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
      destroy() { map.remove(); },
    };
  }
  /** The same, for a host that can wait: resolves once the style is in. */
  async function create(container, o) {
    const vm = createNow(container, o);
    if (!vm) return null;
    await vm.ready;
    return vm;
  }

  global.VibeMapGL = { create, createNow, prepareStyle, webgl2 };
})(typeof window !== 'undefined' ? window : globalThis);
