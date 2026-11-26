/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * vibemap-water.js — THE DIRECTORY'S WATERWAYS OVERLAY (directory pages only).
 *
 * ★★★ WHY. Stuart, 2026-11-25 (the community map): "make sure the map has the waterways for shipping as we
 *     agreed". The directory draws the BASIC pack only, whose rivers are Natural Earth centre LINES — so the
 *     Nieuwe Waterweg, the Humber, the Saigon river were land and the ships sat on it.
 * ★★ A SEPARATE PACK, vibemap-water.pmtiles (one layer `water`, z5–9, over-zoomed beyond): the wide OSM river
 *    areas — rivers, ship canals, tidal channels, docks of mean width ≥ 150 m — from Overture Maps 2026-11-18.0
 *    (ODbL), built by `node scripts/gen-river-areas.mjs --overlay=150 --maxzoom=9` on V12 (17.3 MB; the Workers
 *    asset cap is 25 MiB a file — the script's header has the size table). The basic pack is SHARED with the app
 *    and the servers and is not touched here (whether they get these rivers too is a beta95 decision); the
 *    shared style and vibemapgl.js stay byte-identical to web/mapkit (check-map-overlay.mjs).
 * ★ Drawn as the lakes are (their own colour, read from the style), above land / cover / lakes and below the
 *   coast, river lines, labels and every marker. Streamed by HTTP Range like the other packs (servePmtiles).
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
(function (global) {
  'use strict';
  const ATTRIBUTION = '© OpenStreetMap contributors (rivers via Overture Maps) · ODbL';
  /** Add the water overlay to a VibeMapGL map (the object createNow returned). Safe to call once per map. */
  function attach(vm, packUrl) {
    if (!vm || !vm.map || !vm.ready) return;
    const map = vm.map;
    vm.ready.then(() => {
      if (map.getSource('vs-water')) return;
      keepZ5Layers(map);
      const style = map.getStyle();
      const lakes = style.layers.find((l) => l.id === 'lakes-b') || style.layers.find((l) => l.id === 'lakes-d');
      const color = (lakes && lakes.paint && lakes.paint['fill-color']) || '#1b4a6b';
      // ★ Under the coast line where it is drawn, else under the first line / symbol layer: above every fill.
      const before = (style.layers.find((l) => l.id === 'coast-b') || style.layers.find((l) => l.type === 'line' || l.type === 'symbol') || {}).id;
      map.addSource('vs-water', { type: 'vector', url: 'pmtiles://' + packUrl, minzoom: 5, maxzoom: 9, attribution: ATTRIBUTION });
      map.addLayer({ id: 'vs-water', type: 'fill', source: 'vs-water', 'source-layer': 'water', minzoom: 5,
                     paint: { 'fill-color': color, 'fill-antialias': true } }, before);
    }).catch((e) => console.error('water overlay: ' + e));
  }
  /* ★★★ THE LAKES WENT AT z6 (Stuart 11-26: "the Netherlands now doesn't have enough water" — the IJsselmeer, the
   *  Markermeer and every other lake green from z6 up). The basic pack's lakes, rivers, islands, reefs and playas are
   *  its tier1 data, cut for z4–5 only (gen-map-tiles TIER_STEP: tier2 — the DETAIL pack — takes over at z6); its land,
   *  cover and urban run to z6. The style's basic source has maxzoom 6, so with no detail pack (the directory never has
   *  one) MapLibre over-zooms the z6 tiles — which have NO lakes — and basicOnlyExtend's "carry on over-zoomed" drew
   *  nothing. So here those five layers read the SAME pack through a second source capped at z5: the z5 tiles, lake
   *  islands and all (Flevoland stays land, map_lake_islands_fix), over-zoomed to every zoom above. Same data, same
   *  order, same paint; only where a layer's tiles come from. Not done when a detail pack is in (its own lakes from z6).
   *  ★ The shared style, vibemapgl.js and the basic pack are untouched — the same gap on a server or the app without
   *    High Detail Maps is the pack's to fix (a rebuild whose tier1 lakes reach z6), not this file's. */
  const Z5_LAYERS = new Set(['lakes', 'rivers', 'islands', 'reefs', 'playas']);
  function keepZ5Layers(map) {
    const style = map.getStyle();
    const basic = style.sources && style.sources.basic;
    if (!basic || !basic.url || (style.sources.detail) || map.getSource('vs-basic-z5')) return;
    map.addSource('vs-basic-z5', { type: 'vector', url: basic.url, maxzoom: 5 });
    const layers = style.layers;
    for (let i = 0; i < layers.length; i++) {
      const l = layers[i];
      if (l.source !== 'basic' || !Z5_LAYERS.has(l['source-layer'])) continue;
      const before = layers.slice(i + 1).map((x) => x.id).find((id) => map.getLayer(id));
      map.removeLayer(l.id);
      map.addLayer(Object.assign({}, l, { source: 'vs-basic-z5' }), before);
    }
  }
  global.VibeWater = { attach, ATTRIBUTION };
})(window);
