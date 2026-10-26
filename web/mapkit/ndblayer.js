/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * ndblayer.js — the NDB beacons on our map (V12, docs/v12/NDB-SKIMMER.md): every beacon at its PUBLISHED position (the
 * OurAirports navaid — the airport or beacon site), each marker carrying a small 3-BAR SIGNAL METER from its live SNR,
 * dimmed when not heard recently; the published beacons not heard, optionally, faint; the ADS-B map's own range rings
 * (in nautical miles, labels on all four sides) and receiver house; a tapped beacon's line to the receiver.
 *
 * ★★★ THE RINGS ARE ADS-B'S — the same functions (VibeAdsbLayer.shared: ringsFor / addRingLayers / setRingData /
 *     leafletRings), the same theme and the same CSS (Stuart, 2026-10-27: "same rings, for nm range"). So
 *     adsblayer.js must be loaded first; without it this layer refuses rather than drawing different rings.
 * ★★ web/mapkit/ndblayer.js IS THE ONLY COPY ANYONE EDITS: scripts/gen-ndblayer-source.mjs writes the web client's
 *    string copy (web/client/src/generated/ndbLayerSource.ts). ✗ NO BACKTICKS (the copy is a JSON string).
 *
 *   const layer = VibeNdbLayer.create(mapOrVm, { receiver: {lat,lon}|null, theme: 'dark'|'light',
 *                                               prefs: {rings, unheard, autofit}, onSelect: (key|null) => {} })
 *   layer.update(beacons, unheard)   beacons: [{key, ident, label, lat, lon, bars 0-3, stale, kind}], unheard: [{lat, lon, ident}]
 *   layer.select(key) · layer.focus(key) → bool · layer.setReceiver(lat, lon) · layer.setPrefs({...}) · layer.fit()
 *   layer.destroy()
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
(function (global) {
  'use strict';
  const NM_M = 1852;

  /** ★ The meter's colours: ADS-B's list bars (index.html .sig.l1/l2/l3) — one red, two amber, three green. */
  const BAR_ON = { 1: '#e04040', 2: '#e0b020', 3: '#33cc44' };
  /** Bars from an SNR in the skimmer's ≈ 23 Hz bin: < 10 dB one, < 20 dB two, else three; 0 = not heard now. */
  function barsFor(snrDb, live) { if (!live) return 0; return snrDb < 10 ? 1 : snrDb < 20 ? 2 : 3; }

  /** The marker: a beacon dot (its kind's colour) and three ascending bars beside it — 34 × 18 CSS px. */
  function markerImage(bars, kind, stale, dpr) {
    const w = 34, h = 18, cv = document.createElement('canvas');
    cv.width = Math.ceil(w * dpr); cv.height = Math.ceil(h * dpr);
    const x = cv.getContext('2d'); x.scale(dpr, dpr);
    x.globalAlpha = stale ? 0.45 : 1;
    const dot = kind === 'cand' ? 'rgba(255,184,51,0.55)' : kind === 'near' ? '#ffe066' : '#ffb000';
    x.beginPath(); x.arc(9, 9, 5.5, 0, 2 * Math.PI); x.fillStyle = 'rgba(0,0,0,0.6)'; x.fill();
    x.beginPath(); x.arc(9, 9, 4.2, 0, 2 * Math.PI); x.fillStyle = dot; x.fill();
    x.beginPath(); x.arc(9, 9, 1.5, 0, 2 * Math.PI); x.fillStyle = 'rgba(0,0,0,0.7)'; x.fill();
    const hs = [6, 10, 14];
    for (let i = 0; i < 3; i++) {
      const bx = 18 + i * 5, bh = hs[i];
      x.fillStyle = 'rgba(0,0,0,0.65)'; x.fillRect(bx - 0.75, 15.75 - bh - 0.75, 4.5, bh + 1.5);
      x.fillStyle = i < bars ? BAR_ON[bars] : 'rgba(255,184,51,0.22)';
      x.fillRect(bx, 15 - bh, 3, bh);
    }
    return x.getImageData(0, 0, cv.width, cv.height);
  }
  function markerHtml(bars, kind, stale) {
    const dot = kind === 'cand' ? 'rgba(255,184,51,0.55)' : kind === 'near' ? '#ffe066' : '#ffb000';
    let b = '';
    for (let i = 0; i < 3; i++) b += '<i style="display:inline-block;width:3px;margin-left:2px;height:' + [6, 10, 14][i] + 'px;background:'
      + (i < bars ? BAR_ON[bars] : 'rgba(255,184,51,0.22)') + '"></i>';
    return '<div style="display:flex;align-items:flex-end;opacity:' + (stale ? 0.45 : 1) + ';transform:translate(-9px,-9px)">'
      + '<span style="width:9px;height:9px;border-radius:50%;background:' + dot + ';border:1px solid rgba(0,0,0,0.6);margin:0 3px 3px 0"></span>' + b + '</div>';
  }
  function esc(v) { return String(v == null ? '' : v).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }

  function create(mapIn, opts) {
    opts = opts || {};
    const A = global.VibeAdsbLayer;
    if (!A || !A.shared || !A.maths) throw new Error('ndblayer: needs adsblayer.js first (its rings)');
    const S = A.shared, M = A.maths;
    const th = opts.theme === 'light' ? 'light' : 'dark';
    const T = S.THEME[th];
    const doc = document;
    S.injectCss(th, doc);   // ★ the ring labels' classes (.vsadsb-rl) for the Leaflet fallback
    let gl = null, lm = null;
    if (mapIn && mapIn.map && typeof mapIn.map.addLayer === 'function' && mapIn.map.getStyle) gl = mapIn.map;
    else if (mapIn && typeof mapIn.getStyle === 'function' && typeof mapIn.project === 'function') gl = mapIn;
    else if (mapIn && typeof mapIn.latLngToContainerPoint === 'function') lm = mapIn;
    else throw new Error('ndblayer: not a MapLibre or Leaflet map');
    const LL = lm ? global.L : null;
    const TILE = gl ? 512 : 256;
    const container = gl ? gl.getContainer() : lm.getContainer();
    const DPR = Math.min(4, Math.max(1, global.devicePixelRatio || 1));
    let rx = opts.receiver && isFinite(opts.receiver.lat) && isFinite(opts.receiver.lon) && !(opts.receiver.lat === 0 && opts.receiver.lon === 0)
      ? { lat: +opts.receiver.lat, lon: +opts.receiver.lon } : null;
    const prefs = Object.assign({ rings: true, unheard: true, autofit: true }, opts.prefs || {});
    const onSelect = typeof opts.onSelect === 'function' ? opts.onSelect : null;
    let beacons = [], unheard = [], sel = '', destroyed = false, ready = false, fittedKey = '', userMoved = false;
    let llRings = null, llMarks = null, llRx = null;

    function viewSize() { return { w: container.clientWidth || 600, h: container.clientHeight || 400 }; }
    function curCam() {
      if (gl) { const c = gl.getCenter(); return { center: { lat: c.lat, lon: c.lng }, zoom: gl.getZoom() }; }
      const c = lm.getCenter(); return { center: { lat: c.lat, lon: c.lng }, zoom: lm.getZoom() };
    }
    function drawRings() {
      if (destroyed) return;
      const rings = rx && prefs.rings ? S.ringsFor(rx, curCam().zoom, viewSize(), M.screenOffset(curCam(), rx.lat, rx.lon, TILE), TILE) : null;
      if (gl) { if (ready) S.setRingData(gl, 'vsndb-', rings); return; }
      if (llRings) { lm.removeLayer(llRings); llRings = null; }
      if (rings) { llRings = S.leafletRings(LL, rx, rings, T, 'vsadsb-rl'); llRings.addTo(lm); }
    }
    const imgs = new Set();
    function imgId(bars, kind, stale) {
      const id = 'vsndb-m-' + bars + '-' + kind + (stale ? '-s' : '');
      if (!imgs.has(id) || !gl.hasImage(id)) {
        try { if (!gl.hasImage(id)) gl.addImage(id, markerImage(bars, kind, stale, DPR), { pixelRatio: DPR }); imgs.add(id); }
        catch (e) { console.error('ndblayer: marker image', id, e); }
      }
      return id;
    }
    function rxImage() {
      if (gl.hasImage('vsndb-rx')) return;
      const im = new Image();
      im.onload = () => {
        const cv = doc.createElement('canvas'); cv.width = 22 * DPR; cv.height = 22 * DPR;
        const x = cv.getContext('2d'); x.scale(DPR, DPR); x.drawImage(im, 0, 0, 22, 22);
        try { if (!gl.hasImage('vsndb-rx')) gl.addImage('vsndb-rx', x.getImageData(0, 0, cv.width, cv.height), { pixelRatio: DPR }); } catch (e) { console.error('ndblayer: receiver icon', e); }
      };
      im.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(S.RX_SVG);
    }
    function glSetup() {
      if (destroyed || gl.getSource('vsndb-b')) return;
      const empty = { type: 'FeatureCollection', features: [] };
      S.addRingLayers(gl, 'vsndb-', T);
      for (const id of ['vsndb-link', 'vsndb-u', 'vsndb-b', 'vsndb-rx']) gl.addSource(id, { type: 'geojson', data: empty });
      gl.addLayer({ id: 'vsndb-link', type: 'line', source: 'vsndb-link', paint: { 'line-color': 'rgba(255,229,102,0.75)', 'line-width': 1.5, 'line-dasharray': [2, 2] } });
      gl.addLayer({ id: 'vsndb-u', type: 'circle', source: 'vsndb-u', paint: { 'circle-radius': 2.5, 'circle-color': 'rgba(170,170,170,0.55)' } });
      gl.addLayer({ id: 'vsndb-ulbl', type: 'symbol', source: 'vsndb-u', minzoom: 7,
                    layout: { 'text-field': ['get', 'i'], 'text-font': ['JetBrains Mono Regular'], 'text-size': 9, 'text-offset': [0, 0.9], 'text-anchor': 'top' },
                    paint: { 'text-color': 'rgba(190,190,190,0.7)', 'text-halo-color': T.halo, 'text-halo-width': 1 } });
      gl.addLayer({ id: 'vsndb-rx', type: 'symbol', source: 'vsndb-rx',
                    layout: { 'icon-image': 'vsndb-rx', 'icon-anchor': 'bottom', 'icon-allow-overlap': true, 'icon-ignore-placement': true } });
      gl.addLayer({ id: 'vsndb-sel', type: 'circle', source: 'vsndb-b', filter: ['==', ['get', 'k'], ''],
                    paint: { 'circle-radius': 13, 'circle-color': 'rgba(255,229,102,0.10)', 'circle-stroke-color': 'rgba(255,229,102,0.85)', 'circle-stroke-width': 1.5,
                             'circle-translate': [8, 0] } });
      gl.addLayer({ id: 'vsndb-icons', type: 'symbol', source: 'vsndb-b',
                    layout: { 'icon-image': ['get', 'img'], 'icon-anchor': 'left', 'icon-offset': [-9, 0], 'icon-allow-overlap': true, 'icon-ignore-placement': true,
                              'symbol-sort-key': ['get', 'z'] } });
      gl.addLayer({ id: 'vsndb-lbl', type: 'symbol', source: 'vsndb-b',
                    layout: { 'text-field': ['get', 'l'], 'text-font': ['JetBrains Mono Regular'], 'text-size': 11, 'text-anchor': 'top', 'text-offset': [0.6, 0.9],
                              'text-allow-overlap': false, 'symbol-sort-key': ['get', 'z'] },
                    paint: { 'text-color': ['case', ['get', 'st'], 'rgba(255,214,120,0.55)', '#ffd27a'], 'text-halo-color': T.halo, 'text-halo-width': 1.2 } });
      gl.on('click', onClick);
      gl.on('mouseenter', 'vsndb-icons', () => { gl.getCanvas().style.cursor = 'pointer'; });
      gl.on('mouseleave', 'vsndb-icons', () => { gl.getCanvas().style.cursor = ''; });
      rxImage();
      ready = true;
      draw(); drawRings();
    }
    function onClick(e) {
      const r = 14, p = e.point;
      const f = gl.queryRenderedFeatures([[p.x - r, p.y - r], [p.x + r, p.y + r]], { layers: ['vsndb-icons'] });
      const k = f.length ? f[0].properties.k : '';
      select(k, true);
    }
    function link() {
      const b = beacons.find((x) => x.key === sel);
      return b && rx ? [[rx.lon, rx.lat], [b.lon, b.lat]] : null;
    }
    function draw() {
      if (destroyed) return;
      if (gl) {
        if (!ready) return;
        gl.getSource('vsndb-b').setData({ type: 'FeatureCollection', features: beacons.map((b) => ({ type: 'Feature',
          properties: { k: b.key, l: b.label || b.ident, img: imgId(b.bars, b.kind, b.stale), st: !!b.stale, z: b.stale ? 0 : 1 + b.bars },
          geometry: { type: 'Point', coordinates: [b.lon, b.lat] } })) });
        gl.getSource('vsndb-u').setData({ type: 'FeatureCollection', features: (prefs.unheard ? unheard : []).map((u) => ({ type: 'Feature', properties: { i: u.ident },
          geometry: { type: 'Point', coordinates: [u.lon, u.lat] } })) });
        gl.getSource('vsndb-rx').setData({ type: 'FeatureCollection', features: rx ? [{ type: 'Feature', properties: {}, geometry: { type: 'Point', coordinates: [rx.lon, rx.lat] } }] : [] });
        const l = link();
        gl.getSource('vsndb-link').setData({ type: 'FeatureCollection', features: l ? [{ type: 'Feature', properties: {}, geometry: { type: 'LineString', coordinates: l } }] : [] });
        gl.setFilter('vsndb-sel', ['==', ['get', 'k'], sel || '\u0000']);
        return;
      }
      if (llMarks) lm.removeLayer(llMarks);
      llMarks = LL.layerGroup();
      if (prefs.unheard) for (const u of unheard) LL.circleMarker([u.lat, u.lon], { radius: 2.5, color: 'rgba(170,170,170,0.6)', weight: 1, fillOpacity: 0.6, interactive: false }).addTo(llMarks);
      const l = link();
      if (l) LL.polyline([[l[0][1], l[0][0]], [l[1][1], l[1][0]]], { color: 'rgba(255,229,102,0.75)', weight: 1.5, dashArray: '4 4', interactive: false }).addTo(llMarks);
      for (const b of beacons) {
        const m = LL.marker([b.lat, b.lon], { icon: LL.divIcon({ className: '', iconSize: [0, 0],
          html: markerHtml(b.bars, b.kind, b.stale) + '<div style="font:11px ui-monospace,Menlo,monospace;color:#ffd27a;text-shadow:0 0 2px #000;white-space:nowrap;transform:translate(-4px,-2px)">' + esc(b.label || b.ident) + '</div>' }) });
        m.on('click', () => select(b.key, true));
        m.addTo(llMarks);
      }
      if (rx) { if (llRx) lm.removeLayer(llRx); llRx = LL.marker([rx.lat, rx.lon], { interactive: false, icon: LL.divIcon({ className: '', iconSize: [22, 22], iconAnchor: [11, 22], html: S.RX_SVG }) }).addTo(lm); }
      llMarks.addTo(lm);
    }
    function fit(force) {
      const pts = beacons.filter((b) => !b.stale).map((b) => ({ lat: b.lat, lon: b.lon }));
      if (!pts.length && !rx) return;
      const v = viewSize();
      const cam = M.cameraFor({ points: pts, rx, fit: true, w: v.w, h: v.h, pad: 56, tileSize: TILE, minZoom: 2, maxZoom: 10, emptyZoom: 6 });
      if (!cam) return;
      if (gl) gl.jumpTo({ center: [cam.center.lon, cam.center.lat], zoom: cam.zoom });
      else lm.setView([cam.center.lat, cam.center.lon], Math.round(cam.zoom));
      if (force) userMoved = false;
      drawRings();
    }
    function select(k, fromMap) {
      sel = k || '';
      draw();
      if (fromMap && onSelect) { try { onSelect(sel || null); } catch (e) { console.error('ndblayer: onSelect', e); } }
    }
    // ★ a user's drag / zoom stops the autofit until FIT (the host's key) — the map must not jump under a finger
    const onUserMove = (e) => { if (e && (e.originalEvent || e.type === 'dragstart')) userMoved = true; };
    const onMoveEnd = () => drawRings();
    if (gl) {
      gl.on('dragstart', onUserMove); gl.on('zoomstart', onUserMove); gl.on('moveend', onMoveEnd);
      if (gl.isStyleLoaded && gl.isStyleLoaded()) glSetup(); else gl.once('load', glSetup);
      gl.once('idle', glSetup);
    } else {
      lm.on('dragstart', () => { userMoved = true; }); lm.on('moveend', onMoveEnd);
      draw(); drawRings();
    }
    return {
      update(bs, uh) {
        beacons = Array.isArray(bs) ? bs.filter((b) => isFinite(b.lat) && isFinite(b.lon)) : [];
        unheard = Array.isArray(uh) ? uh.filter((u) => isFinite(u.lat) && isFinite(u.lon)) : [];
        draw();
        const key = beacons.filter((b) => !b.stale).map((b) => b.key).sort().join(',');
        if (prefs.autofit && !userMoved && key !== fittedKey) { fittedKey = key; fit(false); }
      },
      select(k) { select(k, false); },
      focus(k) {
        const b = beacons.find((x) => x.key === k);
        if (!b) return false;
        select(k, false);
        if (gl) gl.easeTo({ center: [b.lon, b.lat], duration: 400 }); else lm.panTo([b.lat, b.lon]);
        userMoved = true;
        return true;
      },
      setReceiver(lat, lon) { if (isFinite(lat) && isFinite(lon)) { rx = { lat: +lat, lon: +lon }; draw(); drawRings(); } },
      setPrefs(p) { Object.assign(prefs, p || {}); if (p && p.autofit) { userMoved = false; fittedKey = ''; } draw(); drawRings(); },
      fit() { fit(true); },
      destroy() {
        destroyed = true;
        try {
          if (gl) {
            gl.off('click', onClick); gl.off('moveend', onMoveEnd); gl.off('dragstart', onUserMove); gl.off('zoomstart', onUserMove);
            for (const l of ['vsndb-lbl', 'vsndb-icons', 'vsndb-sel', 'vsndb-rx', 'vsndb-ulbl', 'vsndb-u', 'vsndb-link', 'vsndb-ringlbl', 'vsndb-rings']) if (gl.getLayer(l)) gl.removeLayer(l);
            for (const s of ['vsndb-b', 'vsndb-u', 'vsndb-rx', 'vsndb-link', 'vsndb-rings', 'vsndb-ringlbl']) if (gl.getSource(s)) gl.removeSource(s);
          } else { for (const g of [llRings, llMarks, llRx]) if (g) lm.removeLayer(g); }
        } catch (e) { console.error('ndblayer: destroy', e); }
      },
    };
  }

  global.VibeNdbLayer = { create, barsFor, BAR_ON };
})(typeof window !== 'undefined' ? window : globalThis);
