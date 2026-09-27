/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * vibemapgl-compat.js — the SLICE of Leaflet's API our map pages use, on top of the GPU map.
 *
 * ★★★ WHY AN ADAPTER AND NOT A REWRITE. The HFDL and spots pages are hundreds of lines of working
 *     logic — polling, glides, pulses, popups, rings — written against Leaflet. Re-expressing every
 *     line against MapLibre is how working behaviour gets lost. So the page keeps calling L.marker /
 *     divIcon / flyToBounds, and this answers those calls with the GPU map underneath. Only what the
 *     pages ACTUALLY call is here (tallied from MapOverlay's page, 2026-09-27); anything else throws
 *     loudly rather than half-working.
 *
 *   const L = VibeMapGLCompat.install(vm, { leafletZoomOffset: 1 });   // vm from VibeMapGL.createNow
 *
 * ★ ZOOMS: pages speak LEAFLET zooms. MapLibre's are one lower (512 vs 256 px tiles). Converted here.
 * ★★ THE GLIDE: pages glide aircraft with a CSS transition on the marker's transform. Leaflet moves the
 *    whole marker PANE during a pan; MapLibre moves EVERY marker on every frame — a transition would make
 *    aircraft trail behind the map while it moves. So `body.nog` (the pages' own "no glide" switch) is
 *    held for ANY camera movement, not only zooms: they glide when the map is still, snap when it moves.
 * ★★ SPOTS ARE GPU CIRCLES, not DOM: circleMarker feeds one GeoJSON source, so thousands of spots cost
 *    what one layer costs. setStyle / setLatLng / popups keep working.
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
(function (global) {
  'use strict';
  function install(vm, opts) {
    const ml = global.maplibregl, gl = vm.map;
    const DZ = (opts && opts.leafletZoomOffset != null) ? opts.leafletZoomOffset : 1;
    const ll = (a) => (Array.isArray(a) ? { lat: a[0], lng: a[1] } : { lat: a.lat, lng: a.lng != null ? a.lng : a.lon });

    // ── The glide switch: held for any camera movement ──────────────────────────────────────────
    let nogT = null;
    gl.on('movestart', () => { clearTimeout(nogT); document.body.classList.add('nog'); });
    gl.on('moveend', () => { clearTimeout(nogT); nogT = setTimeout(() => document.body.classList.remove('nog'), 60); });

    // ── Bounds / points ──────────────────────────────────────────────────────────────────────────
    function Bounds(a, b) {
      const pts = b ? [ll(a), ll(b)] : a.map(ll);
      let s = 90, w = 180, n = -90, e = -180;
      for (const p of pts) { s = Math.min(s, p.lat); n = Math.max(n, p.lat); w = Math.min(w, p.lng); e = Math.max(e, p.lng); }
      return { getSouthWest: () => ({ lat: s, lng: w }), getNorthEast: () => ({ lat: n, lng: e }),
               getWest: () => w, getEast: () => e, getSouth: () => s, getNorth: () => n, _mb: [[w, s], [e, n]] };
    }

    // ── Markers (DOM, positioned by MapLibre) ────────────────────────────────────────────────────
    function applyIcon(m, icon) {
      const o = icon.options || {};
      const el = m._el;
      /* ★★ ADD AND REMOVE OUR CLASSES; NEVER ASSIGN className. MapLibre puts its own classes on this
       *  element (maplibregl-marker — position:absolute, the transform it drives); assigning className
       *  wiped them, and the marker only landed right by luck (WKWebView test, 2026-09-27). */
      if (m._cls) for (const c of m._cls) el.classList.remove(c);
      /* ★★ AND leaflet-interactive, AS LEAFLET DOES FOR EVERY MARKER (unless interactive:false). The pages
       *  still load leaflet.css, which sets pointer-events:none on .leaflet-marker-icon and gives it back
       *  only with .leaflet-interactive — without it every tap fell THROUGH the aircraft to the map and
       *  no popup ever opened (Stuart, build 344: "cannot click on an aircraft"). */
      m._cls = ['leaflet-marker-icon'].concat(m._interactive ? ['leaflet-interactive'] : [],
        String(o.className || '').split(/\s+/).filter(Boolean));
      for (const c of m._cls) el.classList.add(c);                   // ★ the pages' CSS targets these
      el.innerHTML = o.html || '';
      const sz = o.iconSize || [0, 0], an = o.iconAnchor || [sz[0] / 2, sz[1] / 2];
      el.style.width = sz[0] + 'px'; el.style.height = sz[1] + 'px';
      m._mk.setOffset([sz[0] / 2 - an[0], sz[1] / 2 - an[1]]);       // ★ Leaflet anchors a point; MapLibre centres
      m._popupAnchor = o.popupAnchor || [0, -an[1]];
    }
    function Marker(latlng, o) {
      o = o || {};
      const m = { _ll: ll(latlng), _el: document.createElement('div'), _popup: null, _interactive: o.interactive !== false };
      m._mk = new ml.Marker({ element: m._el, anchor: 'center' }).setLngLat([m._ll.lng, m._ll.lat]);
      if (o.zIndexOffset) m._el.style.zIndex = String(o.zIndexOffset);
      if (o.title) m._el.title = o.title;
      if (o.icon) applyIcon(m, o.icon);
      m.addTo = (t) => { if (t && t._isGroup) t.addLayer(m); else m._mk.addTo(gl); return m; };
      m._show = () => m._mk.addTo(gl);
      m.remove = () => { m._mk.remove(); return m; };
      m.setLatLng = (p) => { m._ll = ll(p); m._mk.setLngLat([m._ll.lng, m._ll.lat]); return m; };
      m.getLatLng = () => m._ll;
      m.setIcon = (icon) => { applyIcon(m, icon); return m; };
      m.getElement = () => m._el;
      m.bindPopup = (html, po) => {
        m._popup = new ml.Popup({ maxWidth: ((po && po.maxWidth) || 240) + 'px', offset: m._popupAnchor || [0, 0], closeButton: true })
          .setHTML(html);
        m._mk.setPopup(m._popup); return m;
      };
      m.getPopup = () => m._popup;
      m.setPopupContent = (html) => { if (m._popup) m._popup.setHTML(html); return m; };
      m.openPopup = () => { if (m._popup && !m._popup.isOpen()) m._mk.togglePopup(); return m; };
      m.setZIndexOffset = (z) => { m._el.style.zIndex = String(z); return m; };
      return m;
    }

    // ── Circle markers → one GPU circle layer ───────────────────────────────────────────────────
    const circles = new Map(); let circleSeq = 0, circleDirty = false;
    function flushCircles() {
      circleDirty = false;
      const src = gl.getSource('lc-circles');
      if (!src) return;
      src.setData({ type: 'FeatureCollection', features: [...circles.values()].map((c) => ({
        type: 'Feature', id: c.id, properties: Object.assign({ _id: c.id }, c.st),
        geometry: { type: 'Point', coordinates: [c.ll.lng, c.ll.lat] } })) });
    }
    function queueCircles() { if (!circleDirty) { circleDirty = true; requestAnimationFrame(flushCircles); } }
    vm.ready.then(() => {
      if (gl.getSource('lc-circles')) return;
      gl.addSource('lc-circles', { type: 'geojson', data: { type: 'FeatureCollection', features: [] } });
      gl.addLayer({ id: 'lc-circles', type: 'circle', source: 'lc-circles', paint: {
        'circle-radius': ['coalesce', ['get', 'radius'], 5],
        'circle-color': ['coalesce', ['get', 'fillColor'], ['get', 'color'], '#3388ff'],
        'circle-opacity': ['coalesce', ['get', 'fillOpacity'], 0.2],
        'circle-stroke-color': ['coalesce', ['get', 'color'], '#3388ff'],
        'circle-stroke-width': ['case', ['==', ['get', 'stroke'], false], 0, ['coalesce', ['get', 'weight'], 3]],
        'circle-stroke-opacity': ['coalesce', ['get', 'opacity'], 1] } });
      gl.on('click', 'lc-circles', (e) => {
        const f = e.features && e.features[0]; const c = f && circles.get(f.properties._id);
        if (c && c.html) new ml.Popup({ maxWidth: (c.maxWidth || 200) + 'px' }).setLngLat([c.ll.lng, c.ll.lat]).setHTML(c.html).addTo(gl);
      });
      flushCircles();
    });
    function CircleMarker(latlng, st) {
      const c = { id: ++circleSeq, ll: ll(latlng), st: Object.assign({}, st), html: null };
      const h = {
        addTo: (t) => { if (t && t._isGroup) t.addLayer(h); else h._show(); return h; },
        _show: () => { circles.set(c.id, c); queueCircles(); },
        remove: () => { circles.delete(c.id); queueCircles(); return h; },
        setStyle: (s) => { Object.assign(c.st, s); queueCircles(); return h; },
        setLatLng: (p) => { c.ll = ll(p); queueCircles(); return h; },
        getLatLng: () => c.ll,
        bindPopup: (html, po) => { c.html = html; c.maxWidth = po && po.maxWidth; return h; },
        setPopupContent: (html) => { c.html = html; return h; },
        getPopup: () => (c.html ? {} : null),
      };
      return h;
    }

    // ── Polygons → one GPU fill + outline layer (the directory's country highlight, the greyline) ──
    const polys = new Map(); let polySeq = 0, polyDirty = false;
    function flushPolys() {
      polyDirty = false;
      const src = gl.getSource('lc-polys');
      if (!src) return;
      for (const q of polys.values()) if (q.st.dashArray) dashLayer(q.st);
      src.setData({ type: 'FeatureCollection', features: [...polys.values()].map((q) => ({
        type: 'Feature', id: q.id, properties: Object.assign({}, q.st, q.st.dashArray ? { _dash: String(q.st.dashArray) } : {}),
        geometry: { type: 'MultiPolygon', coordinates: q.rings.map((r) => [r]) } })) });
    }
    /* ★ DASHED OUTLINES (Leaflet's dashArray — the spots map's range rings). MapLibre cannot drive
     *  line-dasharray from a feature, so each distinct pattern gets its own line layer, made on first
     *  use and filtered to its features; the plain outline layer skips anything dashed. Leaflet's
     *  dashes are PIXELS, MapLibre's are multiples of the line width — hence the division. */
    const dashes = new Map();
    function dashLayer(st) {
      const key = String(st.dashArray);
      if (dashes.has(key) || !gl.getSource('lc-polys')) return;
      const w = st.weight != null ? st.weight : 3;
      const arr = key.split(/[\s,]+/).map(Number).filter((n) => isFinite(n) && n >= 0).map((n) => n / (w || 1));
      const id = 'lc-polys-dash-' + dashes.size;
      dashes.set(key, id);
      const under = gl.getStyle().layers.find((l) => l.type === 'symbol');
      gl.addLayer({ id, type: 'line', source: 'lc-polys', filter: ['==', ['get', '_dash'], key], paint: {
        'line-color': ['coalesce', ['get', 'color'], '#3388ff'],
        'line-width': ['coalesce', ['get', 'weight'], 3],
        'line-opacity': ['case', ['==', ['get', 'stroke'], false], 0, ['coalesce', ['get', 'opacity'], 1]],
        ...(arr.length >= 2 ? { 'line-dasharray': arr } : {}) } }, under && under.id);
    }
    function queuePolys() { if (!polyDirty) { polyDirty = true; requestAnimationFrame(flushPolys); } }
    vm.ready.then(() => {
      if (gl.getSource('lc-polys')) return;
      gl.addSource('lc-polys', { type: 'geojson', data: { type: 'FeatureCollection', features: [] } });
      // ★ Below the circles and every label: a highlight or a night shadow is ground, not a marker.
      const under = gl.getStyle().layers.find((l) => l.type === 'symbol');
      gl.addLayer({ id: 'lc-polys-fill', type: 'fill', source: 'lc-polys', paint: {
        'fill-color': ['coalesce', ['get', 'fillColor'], ['get', 'color'], '#3388ff'],
        // ★ Leaflet's fill:false (a ring, not a disc) is an outline with nothing inside it.
        'fill-opacity': ['case', ['==', ['get', 'fill'], false], 0, ['coalesce', ['get', 'fillOpacity'], 0.2]] } }, under && under.id);
      gl.addLayer({ id: 'lc-polys-line', type: 'line', source: 'lc-polys', filter: ['!', ['has', '_dash']], paint: {
        'line-color': ['coalesce', ['get', 'color'], '#3388ff'],
        'line-width': ['coalesce', ['get', 'weight'], 3],
        'line-opacity': ['case', ['==', ['get', 'stroke'], false], 0, ['coalesce', ['get', 'opacity'], 1]] } }, under && under.id);
      flushPolys();
    });
    /** Leaflet polygon: [lat, lng] rings (one ring, or a list of them). Each ring is drawn as its own
     *  polygon — what the directory's country outlines are — never as holes. */
    function Polygon(latlngs, st) {
      const isRing = (a) => Array.isArray(a) && a.length && (Array.isArray(a[0]) ? typeof a[0][0] === 'number' : a[0].lat != null);
      const toRing = (r) => { const c = r.map((p) => { const q = ll(p); return [q.lng, q.lat]; });
        if (c.length && (c[0][0] !== c[c.length - 1][0] || c[0][1] !== c[c.length - 1][1])) c.push(c[0]); return c; };
      const q = { id: ++polySeq, rings: (isRing(latlngs) ? [latlngs] : latlngs).map(toRing), st: Object.assign({}, st) };
      const h = {
        addTo: (t) => { if (t && t._isGroup) t.addLayer(h); else h._show(); return h; },
        _show: () => { polys.set(q.id, q); queuePolys(); },
        remove: () => { polys.delete(q.id); queuePolys(); return h; },
        setStyle: (s2) => { Object.assign(q.st, s2); queuePolys(); return h; },
        setLatLngs: (a) => { q.rings = (isRing(a) ? [a] : a).map(toRing); queuePolys(); return h; },
      };
      return h;
    }
    /** Leaflet circle: a radius in METRES round a point — drawn as a geodesic ring (true great-circle
     *  distance, which is what a range ring means), through the polygon layers above. Leaflet drew a
     *  projected ellipse instead; on a regional map the two agree, far out the geodesic is the honest one.
     *  ★ A ring that encloses a pole (5000 km from the UK does) cannot close across the map: its
     *    longitudes run a full 360°. It is closed OVER the pole instead, off the top of the map. */
    function circleRing(c, m) {
      const rad = Math.PI / 180, d = m / 6371008.8, la = c.lat * rad, lo = c.lng * rad, N = 128;
      const out = []; let prev = null;
      for (let i = 0; i < N; i++) {
        const b = (2 * Math.PI * i) / N;
        const lat = Math.asin(Math.sin(la) * Math.cos(d) + Math.cos(la) * Math.sin(d) * Math.cos(b));
        let lng = (lo + Math.atan2(Math.sin(b) * Math.sin(d) * Math.cos(la), Math.cos(d) - Math.sin(la) * Math.sin(lat))) / rad;
        if (prev != null) { while (lng - prev > 180) lng -= 360; while (lng - prev < -180) lng += 360; }
        prev = lng; out.push([lat / rad, lng]);
      }
      const span = out[out.length - 1][1] - out[0][1];
      if (Math.abs(span) > 180) {
        const pole = (c.lat >= 0 ? 89.9 : -89.9);
        out.push([pole, out[out.length - 1][1]], [pole, out[0][1]]);
      }
      return out;
    }
    function Circle(latlng, st) {
      st = Object.assign({ fill: true }, st);
      let c = ll(latlng), r = st.radius != null ? st.radius : 10;
      const h = Polygon(circleRing(c, r), st);
      h.getLatLng = () => c;
      h.getRadius = () => r;
      h.setLatLng = (p) => { c = ll(p); return h.setLatLngs([circleRing(c, r)]); };
      h.setRadius = (m) => { r = m; return h.setLatLngs([circleRing(c, r)]); };
      return h;
    }
    /** Leaflet layerGroup: its members show only while the group is on the map. */
    function LayerGroup() {
      const members = new Set(); let on = false;
      const g = {
        _isGroup: true,
        addLayer: (l) => { members.add(l); if (on) (l._show ? l._show() : l.addTo(map)); return g; },
        removeLayer: (l) => { members.delete(l); l.remove(); return g; },
        clearLayers: () => { for (const l of members) l.remove(); members.clear(); return g; },
        getLayers: () => [...members],
        addTo: () => { on = true; for (const l of members) (l._show ? l._show() : l.addTo(map)); return g; },
        remove: () => { on = false; for (const l of members) l.remove(); return g; },
        _on: () => on,
      };
      return g;
    }

    // ── The map ─────────────────────────────────────────────────────────────────────────────────
    const map = {
      _gl: gl,
      setView(c, z) { const p = ll(c); gl.jumpTo({ center: [p.lng, p.lat], zoom: (z != null ? z : gl.getZoom() + DZ) - DZ }); return map; },
      flyTo(c, z, o) { const p = ll(c);
        gl.flyTo({ center: [p.lng, p.lat], zoom: (z != null ? z : gl.getZoom() + DZ) - DZ,
                   duration: ((o && o.duration) != null ? o.duration : 0.8) * 1000, essential: true }); return map; },
      flyToBounds(b, o) {
        o = o || {};
        const tl = o.paddingTopLeft || o.padding || { x: 0, y: 0 }, br = o.paddingBottomRight || o.padding || { x: 0, y: 0 };
        gl.fitBounds(b._mb, { padding: { top: tl.y, left: tl.x, bottom: br.y, right: br.x },
                              maxZoom: o.maxZoom != null ? o.maxZoom - DZ : 22,
                              duration: (o.duration != null ? o.duration : 0.8) * 1000, essential: true });
        return map;
      },
      getZoom: () => gl.getZoom() + DZ,
      getCenter: () => { const c = gl.getCenter(); return { lat: c.lat, lng: c.lng }; },
      getBounds: () => { const b = gl.getBounds(); return Bounds([[b.getSouth(), b.getWest()], [b.getNorth(), b.getEast()]]); },
      on(ev, fn) { for (const e of String(ev).split(/\s+/)) gl.on(e, fn); return map; },
      once(ev, fn) { gl.once(ev, fn); return map; },
      off(ev, fn) { for (const e of String(ev).split(/\s+/)) gl.off(e, fn); return map; },
      removeLayer(layer) { if (layer && layer.remove) layer.remove(); return map; },
      hasLayer(layer) { return !!(layer && layer._on ? layer._on() : false); },
      zoomIn(d) { gl.zoomIn({ duration: 250 }); return map; },
      zoomOut(d) { gl.zoomOut({ duration: 250 }); return map; },
      /** fitBounds(Bounds | [[lat, lng], …], { padding: [x, y] | point, maxZoom }) — immediate, as Leaflet's is. */
      fitBounds(b, o) {
        o = o || {};
        const bb = b && b._mb ? b : Bounds(b);
        const pad = Array.isArray(o.padding) ? { x: o.padding[0], y: o.padding[1] } : (o.padding || { x: 0, y: 0 });
        gl.fitBounds(bb._mb, { padding: { top: pad.y, bottom: pad.y, left: pad.x, right: pad.x },
                               maxZoom: o.maxZoom != null ? o.maxZoom - DZ : 22, duration: 0 });
        return map;
      },
      latLngToContainerPoint(p) { const q = ll(p); const pt = gl.project([q.lng, q.lat]); return { x: pt.x, y: pt.y }; },
      invalidateSize() { gl.resize(); return map; },
      getContainer: () => gl.getContainer(),
      latLngToLayerPoint(p) { const q = ll(p); const pt = gl.project([q.lng, q.lat]); return { x: pt.x, y: pt.y }; },
      getPane() { return null; },   // ★ no panes: a page must use L.marker (see MapOverlay placeRing)
    };

    const L = {
      __gpu: true,
      map(id, o) { if (o && o.scrollWheelZoom === false) gl.scrollZoom.disable(); return map; },
      layerGroup: LayerGroup,
      polygon: Polygon,
      circle: Circle,
      marker: Marker,
      circleMarker: CircleMarker,
      divIcon: (options) => ({ options }),
      latLngBounds: Bounds,
      latLng: (a, b) => (b != null ? { lat: a, lng: b } : ll(a)),
      point: (x, y) => ({ x, y }),
      control: { zoom: (o) => ({ addTo: () => { gl.addControl(new ml.NavigationControl({ showCompass: false }),
        ((o && o.position) || 'bottomright').replace('bottom', 'bottom-').replace('top', 'top-')); } }) },
    };
    return L;
  }
  global.VibeMapGLCompat = { install };
})(typeof window !== 'undefined' ? window : globalThis);
