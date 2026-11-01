/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * tracklayer.js — THE SHIP AND STATION LAYER (V12 AIS + APRS, docs/v12/AIS-APRS.md): one layer for anything that
 * reports a position over the air and moves slowly — ships (AIS) and APRS stations — drawn the ADS-B layer's way:
 * icons that GLIDE to each new fix instead of jumping, a new arrival faded in, a TAPPED one's track drawn behind it,
 * and a card with its details and ▲/▼ cues. What an item looks like (its icon, its label, its card) is the HOST's:
 * this file knows positions, selection, tracks and motion, nothing about ships or callsigns.
 *
 * ★★ THE ADS-B LAYER IS THE REFERENCE FOR FEEL (adsblayer.js, after the HFDL map): the 2.2 s glide, the 600 ms fade
 *    in / 900 ms out, the receiver's little house, the dark monospace card. A ship reports every 2–10 s at 10 knots —
 *    a glide is all the motion it needs; nothing here dead-reckons.
 * ★★ ONE CODE PATH FOR BOTH MAPS: written against the slice of Leaflet's API that vibemapgl-compat.js also answers on
 *    the GPU map (L.marker, L.divIcon, L.polyline/polygon, map.on/flyTo/fitBounds), so the GPU map and the Leaflet
 *    fallback behave alike. DOM markers: a few hundred ships is what a busy coast holds.
 * ✗ NO BACKTICKS IN THIS FILE (the app injects it inside a template literal one day, as it does adsblayer.js).
 *
 *   const layer = VibeTrackLayer.create(map, L, {
 *       receiver: {lat, lon} | null,
 *       icon:  function (item) { return { html: '<svg…>', w: 28, h: 28 }; },   // the item's icon, nose up; rotate inside
 *       label: function (item) { return 'WILSON GOOLE'; },                       // shown at closer zooms
 *       card:  function (item) { return '<div>…</div>'; },                      // the tapped item's card (escaped HTML)
 *       trail: function (key, done) { done([[lat, lon], …]); },                  // its track so far (async; may be null)
 *       onSelect: function (key | null) {},                                       // the selection changed ON THE MAP
 *       labelZoom: 11,                                                            // Leaflet zoom from which labels show
 *       focusZoom: 10,                                                            // focus(key) zooms in at least this far
 *       heard: function (item) { return 12; },
 *       labelRank: function (item) { return 5; },                                // ★ who keeps its label when two collide (higher wins)
 *       arc: function (item) { return true; } });                                  // ★ a faint great-circle line from the receiver                                 // ★ a count that grows when the item is heard
 *                                                                                 //   again: a new item, or a higher count, pulses
 *   ★ V12 2026-11-01 (the NAVTEX warnings and met labels):
 *       shape:   function (item) { return { kind: 'area'|'route'|'points', pts: [[lat, lon], …], colour, faded }; },  // drawn under it
 *       cluster: function (item) { return 'w'; },     // ★ items of one group gather into a numbered marker when zoomed out
 *       clusterZoom: 8,                               //   below this Leaflet zoom (a gathered marker zooms in when chosen)
 *       clusterAlways: ['m'],                         //   groups that gather at every zoom where their icons overlap
 *       aria:    function (item) { return '…'; },     // ★ a marker is a button (keyboard, gamepad, TV focus): its words
 *       onCard:  function (key, el) {}                // ★ the card was drawn (or redrawn): the host fills what it fetches
 *   layer.update(items)   // [{key, lat, lon, …}] — the whole list each time; items without a position are skipped
 *   layer.focus(key) · layer.select(key | null) · layer.fit() · layer.setReceiver(lat, lon) · layer.refreshCard() · layer.destroy()
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
(function (global) {
  'use strict';
  var GLIDE_MS = 2200, FADE_IN_MS = 600, FADE_OUT_MS = 900;
  var STYLE_ID = 'vtl-style';
  var CSS = [
    '.vtl-mk{transition:transform ' + GLIDE_MS + 'ms cubic-bezier(.25,.8,.35,1),opacity ' + FADE_IN_MS + 'ms;cursor:pointer}',
    '.vtl-nog .vtl-mk{transition:opacity ' + FADE_IN_MS + 'ms !important}',
    '@media (prefers-reduced-motion:reduce){.vtl-mk{transition:none !important}}',
    '.vtl-mk.vtl-new{opacity:0}',
    '.vtl-mk.vtl-gone{opacity:0;transition:opacity ' + FADE_OUT_MS + 'ms !important}',
    '.vtl-mk .vtl-ic{position:absolute;left:0;top:0;width:100%;height:100%;display:flex;align-items:center;justify-content:center;',
    'filter:drop-shadow(0 1px 1.5px rgba(0,0,0,.75))}',
    '.vtl-mk .vtl-ic svg{display:block;overflow:visible}',
    '.vtl-mk.vtl-sel .vtl-ic{filter:drop-shadow(0 0 4px #fff) drop-shadow(0 0 2px #ffd34d)}',
    // ★ focus(key) — a row or a link chose this item: three rings pulse out from it, then it stays selected
    '.vtl-mk.vtl-pulse::after{content:"";position:absolute;left:50%;top:50%;width:100%;height:100%;border-radius:50%;',
    'border:2px solid #ffd34d;box-sizing:border-box;pointer-events:none;animation:vtl-pulse 1.1s ease-out 3}',
    '@keyframes vtl-pulse{from{transform:translate(-50%,-50%) scale(1);opacity:1}to{transform:translate(-50%,-50%) scale(2.8);opacity:0}}',
    '@media (prefers-reduced-motion:reduce){.vtl-mk.vtl-pulse::after{animation:none;opacity:0}}',
    '.vtl-mk .vtl-lb{position:absolute;left:100%;top:50%;transform:translate(3px,-50%);white-space:nowrap;display:none;',
    'font:600 10.5px/1.2 ui-monospace,Menlo,Consolas,monospace;color:#e8eef5;text-shadow:0 0 3px #000,0 0 2px #000;pointer-events:none}',
    '.vtl-labels .vtl-mk .vtl-lb,.vtl-mk.vtl-sel .vtl-lb{display:block}',
    // ★ DECLUTTER: a label that would sit on a higher-ranked one (or on another icon) is hidden — its icon stays, and
    //   hovering or selecting it shows the name
    '.vtl-labels .vtl-mk .vtl-lb.vtl-lbhide{display:none}',
    '.vtl-mk .vtl-lb.vtl-lbl{left:auto;right:100%;transform:translate(-3px,-50%)}',
    '.vtl-mk:hover .vtl-lb.vtl-lbhide,.vtl-mk.vtl-sel .vtl-lb.vtl-lbhide{display:block}',
    '.vtl-mk:hover{z-index:10000 !important}',
    '.vtl-rx{pointer-events:none}',
    '.vtl-card{position:absolute;right:10px;top:10px;z-index:900;max-width:min(320px,calc(100% - 20px));max-height:calc(100% - 20px);',
    'overflow:auto;background:rgba(10,14,20,.94);color:#dfe7ef;border:1px solid #2c3a4a;border-radius:8px;padding:10px 12px 10px;',
    'font:12px/1.45 ui-monospace,Menlo,Consolas,monospace;box-shadow:0 6px 24px rgba(0,0,0,.5)}',
    '.vtl-card[hidden]{display:none}',
    '.vtl-card .vtl-x{position:absolute;right:6px;top:4px;background:none;border:0;color:#9fb0c2;font-size:16px;cursor:pointer;padding:2px 6px}',
    '.vtl-card .vtl-x:hover,.vtl-card .vtl-x:focus-visible{color:#fff}',
    // ★ a marker is a button: a ring round it while it has the keyboard / gamepad / TV focus
    '.vtl-mk:focus{outline:none}',
    '.vtl-mk:focus-visible .vtl-ic,.vtl-mk.vtl-kf .vtl-ic{outline:2px solid #ffd34d;outline-offset:2px;border-radius:50%}',
    // ★ CLUSTERS: the gathered markers are hidden, the one standing for them carries the count
    '.vtl-mk.vtl-clhid{display:none !important}',
    '.vtl-mk .vtl-cl{position:absolute;right:-7px;top:-7px;min-width:15px;height:15px;padding:0 3px;box-sizing:border-box;border-radius:8px;',
    'background:#ffd34d;color:#111;font:700 10px/15px ui-monospace,Menlo,Consolas,monospace;text-align:center;pointer-events:none;',
    'box-shadow:0 0 0 1.5px #0b1420}',
    '.vtl-mk .vtl-cl[hidden]{display:none}',
  ].join('');

  function addStyle(doc) {
    if (doc.getElementById(STYLE_ID)) return;
    var st = doc.createElement('style'); st.id = STYLE_ID; st.textContent = CSS;
    (doc.head || doc.documentElement).appendChild(st);
  }
  function esc(v) {
    return String(v == null ? '' : v).replace(/[&<>"']/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]; });
  }
  var RX_HTML = '<svg width="18" height="18" viewBox="0 0 18 18"><path d="M9 2 L16 8 L14 8 L14 16 L4 16 L4 8 L2 8 Z" fill="#ffcf5a" stroke="#2a1d00" stroke-width="1.2"/></svg>';

  function create(map, L, opts) {
    opts = opts || {};
    var doc = (map.getContainer && map.getContainer().ownerDocument) || document;
    addStyle(doc);
    var cont = map.getContainer();
    var items = new Map();        // key → { mk, item, html, label, gone }
    var sel = null, trailLine = null, trailPts = null, trailKey = null;
    var rx = null, rxMk = null, fitted = false, destroyed = false;
    var labelZoom = opts.labelZoom || 11;

    // ── the card ──
    var card = doc.createElement('div');
    card.className = 'vtl-card'; card.hidden = true;
    card.setAttribute('role', 'dialog');
    var cardBody = doc.createElement('div');
    var x = doc.createElement('button');
    x.className = 'vtl-x'; x.type = 'button'; x.textContent = '×'; x.title = 'Close'; x.setAttribute('aria-label', 'Close');
    x.onclick = function () { select(null, true); };
    card.appendChild(x); card.appendChild(cardBody);
    cont.appendChild(card);
    ['mousedown', 'touchstart', 'wheel', 'dblclick', 'click', 'pointerdown'].forEach(function (ev) {
      card.addEventListener(ev, function (e) { e.stopPropagation(); }, { passive: true });
    });

    // ── no glide while the map itself moves (markers would trail the map) ──
    var nogT = null;
    function nogOn() { clearTimeout(nogT); cont.classList.add('vtl-nog'); }
    function nogOff() { clearTimeout(nogT); nogT = setTimeout(function () { cont.classList.remove('vtl-nog'); }, 80); }
    function zoomed() { cont.classList.toggle('vtl-labels', map.getZoom() >= labelZoom); declutterSoon(); }
    /* ★★ LABEL DECLUTTER (2026-11-01, Stuart's screenshot: three names drawn over each other by the receiver). Labels are
     *  placed best first — the selected item, then opts.labelRank (the host's: repeaters, then the most recent) — and one
     *  that would overlap a placed label, or another item's icon, is hidden. Icons are never hidden. Run after every
     *  update, move and zoom (one pass a frame); ~500 items is a few ms. */
    var dcRaf = 0;
    function declutterSoon() {
      if (dcRaf || destroyed) return;
      var raf = (doc.defaultView && doc.defaultView.requestAnimationFrame) || function (f) { return setTimeout(f, 16); };
      dcRaf = raf(function () { dcRaf = 0; declutter(); });
    }
    function hit(a, b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }
    /* ★★ CLUSTERS (2026-11-01, the NAVTEX warnings: hundreds of them along one coast). Below clusterZoom, the items the
     *  host puts in a group gather on the screen: best first (the selected one, then opts.labelRank), each within
     *  CL_PX of one already standing joins it; the one standing shows the count, the others are hidden. Screen space, so
     *  zooming in splits them; one pass with the declutter, a frame after every update, move and zoom. */
    var CL_PX = 30, clusterZoom = opts.clusterZoom || 8, clAlways = opts.clusterAlways || [];
    /* ★ A group in opts.clusterAlways (the met labels: wide pills) gathers at EVERY zoom, wherever two would overlap. */
    function clusterPass() {
      if (!opts.cluster) return;
      var zoomedOut = map.getZoom() < clusterZoom, can = !!map.latLngToContainerPoint;
      var arr = [];
      items.forEach(function (e, k) {
        e.clMembers = null;
        var g = !e.gone && opts.cluster(e.item);
        if (!g) { e.clHid = false; return; }
        arr.push({ k: k, e: e, g: g, rank: k === sel ? Infinity : (opts.labelRank ? Number(opts.labelRank(e.item)) || 0 : 0) });
      });
      arr.sort(function (a, b) { return b.rank - a.rank; });
      var heads = [], nHeads = 0;
      for (var i = 0; i < arr.length; i++) {
        var a = arr[i], joined = null, always = clAlways.indexOf(a.g) >= 0;
        if (can && (zoomedOut || always)) {
          var pt = map.latLngToContainerPoint([a.e.lat, a.e.lon]); a.pt = pt;
          for (var j = 0; j < heads.length; j++) {
            var h = heads[j];
            if (h.g !== a.g || !h.pt) continue;
            var nx = always ? ((h.e.w || 20) + (a.e.w || 20)) / 2 : CL_PX, ny = always ? ((h.e.h || 20) + (a.e.h || 20)) / 2 : CL_PX;
            if (Math.abs(h.pt.x - pt.x) < nx && Math.abs(h.pt.y - pt.y) < ny) { joined = h; break; }
          }
          if (joined && a.rank === Infinity) joined = null;   // ★ the selected one always stands
        }
        if (joined) { a.e.clHid = true; (joined.e.clMembers = joined.e.clMembers || []).push(a.k); }
        else { a.e.clHid = false; heads.push(a); }
      }
      items.forEach(function (e, k) {
        var el = elOf(e.mk);
        if (!el) return;
        el.classList.toggle('vtl-clhid', !!e.clHid);
        el.tabIndex = e.clHid ? -1 : 0;
        var b = el.querySelector('.vtl-cl');
        if (b) { var n = e.clMembers ? e.clMembers.length + 1 : 0; b.hidden = !n; b.textContent = n ? String(n) : ''; if (n) nHeads++; }
        decorate(k);
      });
      cont.setAttribute('data-vtl-clusters', String(nHeads));
    }
    function declutter() {
      if (destroyed) return;
      clusterPass();
      var on = cont.classList.contains('vtl-labels');
      var arr = [], icons = [];
      items.forEach(function (e, k) {
        if (e.gone || e.clHid) return;
        var el = elOf(e.mk);
        if (!el) return;
        var lb = el.querySelector('.vtl-lb'), ic = el.querySelector('.vtl-ic');
        if (lb) { lb.classList.remove('vtl-lbhide'); lb.classList.remove('vtl-lbl'); }
        if (ic) icons.push({ k: k, r: ic.getBoundingClientRect() });
        if (lb && on) arr.push({ k: k, lb: lb, rank: k === sel ? Infinity : (opts.labelRank ? Number(opts.labelRank(e.item)) || 0 : 0) });
      });
      if (!on) return;
      if (rxMk) { var re = elOf(rxMk); if (re) icons.push({ k: '', r: re.getBoundingClientRect() }); }
      arr.sort(function (a, b) { return b.rank - a.rank; });
      var placed = [];
      var clash = function (a, r) {
        for (var j = 0; j < placed.length; j++) if (hit(r, placed[j])) return true;
        for (var m = 0; m < icons.length; m++) if (icons[m].k !== a.k && hit(r, icons[m].r)) return true;
        return false;
      };
      for (var i = 0; i < arr.length; i++) {
        var a = arr[i], r = a.lb.getBoundingClientRect();
        if (!r.width) continue;
        if (clash(a, r)) {   // ★ right of the icon is taken: try its left, then give up the name (never the icon)
          a.lb.classList.add('vtl-lbl');
          var r2 = a.lb.getBoundingClientRect();
          if (!clash(a, r2) || a.rank === Infinity) { placed.push(r2); continue; }
          a.lb.classList.remove('vtl-lbl'); a.lb.classList.add('vtl-lbhide');
          continue;
        }
        placed.push(r);
      }
    }
    map.on('moveend', declutterSoon);
    map.on('movestart', nogOn); map.on('zoomstart', nogOn);
    map.on('moveend', nogOff); map.on('zoomend', nogOff);
    map.on('zoomend', zoomed);
    zoomed();

    function iconFor(it, selected) {
      var ic = opts.icon ? opts.icon(it) : { html: '', w: 20, h: 20 };
      var lb = opts.label ? opts.label(it) : '';
      var html = '<div class="vtl-ic">' + (ic.html || '') + '</div>' + (lb ? '<div class="vtl-lb">' + esc(lb) + '</div>' : '')
        + (opts.cluster && opts.cluster(it) ? '<div class="vtl-cl" hidden></div>' : '');
      return { html: html, w: ic.w || 20, h: ic.h || 20, sig: html + '|' + (selected ? 1 : 0) };
    }
    function divIcon(ic, extra) {
      return L.divIcon({ className: 'vtl-mk' + (extra || ''), html: ic.html, iconSize: [ic.w, ic.h], iconAnchor: [ic.w / 2, ic.h / 2] });
    }
    function elOf(mk) { return mk.getElement ? mk.getElement() : mk._icon; }
    function setCls(mk, cls, on) { var el = elOf(mk); if (el) el.classList.toggle(cls, !!on); }

    /* ★★ ARCS (the NAVTEX stations map, 2026-11-01): a faint great-circle line from the receiver to each item the host
     *  asks for — the DX map's look; a transatlantic catch spans the ocean as it should. 48 segments; under the markers. */
    var arcs = new Map();
    function gc(a, b) {
      var r = Math.PI / 180, p1 = a[0] * r, l1 = a[1] * r, p2 = b[0] * r, l2 = b[1] * r;
      var d = 2 * Math.asin(Math.sqrt(Math.pow(Math.sin((p2 - p1) / 2), 2) + Math.cos(p1) * Math.cos(p2) * Math.pow(Math.sin((l2 - l1) / 2), 2)));
      if (!(d > 1e-6)) return [a, b];
      var pts = [], prevLon = null;
      for (var i = 0; i <= 48; i++) {
        var f = i / 48, A = Math.sin((1 - f) * d) / Math.sin(d), B = Math.sin(f * d) / Math.sin(d);
        var x = A * Math.cos(p1) * Math.cos(l1) + B * Math.cos(p2) * Math.cos(l2);
        var y = A * Math.cos(p1) * Math.sin(l1) + B * Math.cos(p2) * Math.sin(l2);
        var z = A * Math.sin(p1) + B * Math.sin(p2);
        var lon = Math.atan2(y, x) / r;
        if (prevLon != null) { while (lon - prevLon > 180) lon -= 360; while (lon - prevLon < -180) lon += 360; }   // ★ no jump at ±180
        prevLon = lon;
        pts.push([Math.atan2(z, Math.sqrt(x * x + y * y)) / r, lon]);
      }
      return pts;
    }
    function syncArcs() {
      if (!opts.arc) return;
      var want = new Set();
      if (rx) items.forEach(function (e, k) {
        if (e.gone || !opts.arc(e.item)) return;
        want.add(k);
        var sig = e.lat + ',' + e.lon + '|' + rx.lat + ',' + rx.lon;
        var a = arcs.get(k);
        if (a && a.sig === sig) return;
        if (a) try { a.line.remove(); } catch (err) { /* gone */ }
        var line = (L.polyline || L.polygon)(gc([rx.lat, rx.lon], [e.lat, e.lon]), { color: '#9fd8ff', weight: 1.2, opacity: 0.35, fill: false, interactive: false });
        line.addTo(map);
        arcs.set(k, { line: line, sig: sig });
      });
      arcs.forEach(function (a, k) { if (!want.has(k)) { try { a.line.remove(); } catch (err) { /* gone */ } arcs.delete(k); } });
      cont.setAttribute('data-vtl-arcs', String(arcs.size));   // ★ for tests (a canvas renderer draws them out of the DOM)
    }
    /* ★★ A MARKER IS A BUTTON (2026-11-01: keyboard, gamepad and TV focus): in the tab order, its words for a screen
     *  reader, Enter or Space chooses it — a gathered marker zooms in to its members, any other opens its card and moves
     *  the focus into it (the card's × gives it back). Re-applied after every setIcon (Leaflet rewrites the class). */
    function decorate(k) {
      var e = items.get(k); if (!e) return;
      var el = elOf(e.mk); if (!el) return;
      if (el.getAttribute('role') !== 'button') el.setAttribute('role', 'button');
      if (!e.clHid && el.tabIndex !== 0) el.tabIndex = 0;
      var words = opts.aria ? String(opts.aria(e.item) || '') : (opts.label ? opts.label(e.item) : '');
      if (e.clMembers && e.clMembers.length) words = (e.clMembers.length + 1) + ' here, gathered. ' + words.replace(/ Press Enter[^.]*\.$/, '') + ' Press Enter to zoom in.';
      if (el.getAttribute('aria-label') !== words) el.setAttribute('aria-label', words);
      if (!el._vtlKey) {
        el._vtlKey = true;
        el.addEventListener('keydown', function (ev) {
          if (ev.key !== 'Enter' && ev.key !== ' ') return;
          ev.preventDefault(); ev.stopPropagation();
          activate(el._vtlK, true);
        });
      }
      el._vtlK = k;
    }
    function activate(k, byKey) {
      var e = items.get(k); if (!e) return;
      var always = opts.cluster && clAlways.indexOf(opts.cluster(e.item)) >= 0;
      if (e.clMembers && e.clMembers.length && (map.getZoom() < clusterZoom || always) && map.getZoom() < 13) {
        var pts = [[e.lat, e.lon]];
        e.clMembers.forEach(function (m) { var o = items.get(m); if (o) pts.push([o.lat, o.lon]); });
        try { map.fitBounds(L.latLngBounds(pts), { padding: [90, 90], maxZoom: always ? Math.min(13, map.getZoom() + 2.5) : clusterZoom }); } catch (err) { console.error('[tracklayer] cluster zoom', err); }
        return;
      }
      select(k, true);
      if (byKey) { lastFocus = elOf(e.mk); setTimeout(function () { try { x.focus(); } catch (err) { /* gone */ } }, 30); }
    }
    var lastFocus = null;
    /* ★★ SHAPES (the NAVTEX warnings): an AREA as a faint filled outline, a ROUTE as a line, separate POINTS as dots —
     *  under the markers, not clickable (the marker is). Faded ones dashed and grey. */
    var shapes = new Map();
    function syncShapes() {
      if (!opts.shape) return;
      var want = new Set();
      items.forEach(function (e, k) {
        if (e.gone) return;
        var sh = opts.shape(e.item);
        if (!sh || !sh.pts || sh.pts.length < 2) return;
        want.add(k);
        var sig = JSON.stringify(sh) + (k === sel ? '|s' : '');
        var cur = shapes.get(k);
        if (cur && cur.sig === sig) return;
        if (cur) cur.layers.forEach(function (l) { try { l.remove(); } catch (err) { /* gone */ } });
        var st = { color: sh.colour || '#ffd34d', weight: k === sel ? 2.6 : 1.6, opacity: sh.faded ? 0.45 : 0.9, interactive: false };
        if (sh.faded) st.dashArray = '4 4';
        var made = [];
        if (sh.kind === 'area') made.push(L.polygon(sh.pts, Object.assign({ fill: true, fillColor: st.color, fillOpacity: sh.faded ? 0.05 : 0.14 }, st)));
        else if (sh.kind === 'route') made.push((L.polyline || L.polygon)(sh.pts, Object.assign({ fill: false }, st)));
        else if (L.circleMarker) sh.pts.forEach(function (p) { made.push(L.circleMarker(p, { radius: 3, color: st.color, weight: 1, opacity: st.opacity, fillColor: st.color, fillOpacity: sh.faded ? 0.3 : 0.8, interactive: false })); });
        made.forEach(function (l) { l.addTo(map); });
        shapes.set(k, { sig: sig, layers: made });
      });
      shapes.forEach(function (v, k) { if (!want.has(k)) { v.layers.forEach(function (l) { try { l.remove(); } catch (err) { /* gone */ } }); shapes.delete(k); } });
      cont.setAttribute('data-vtl-shapes', String(shapes.size));
    }
    function update(list) {
      if (destroyed) return;
      var seen = new Set();
      for (var i = 0; i < list.length; i++) {
        var it = list[i];
        if (!it || it.key == null || !(isFinite(it.lat) && isFinite(it.lon))) continue;
        var k = String(it.key);
        seen.add(k);
        var e = items.get(k);
        var ic = iconFor(it, k === sel);
        if (!e) {
          var mk = L.marker([it.lat, it.lon], { icon: divIcon(ic, ' vtl-new' + (k === sel ? ' vtl-sel' : '')), keyboard: false, riseOnHover: true });
          (function (key) { mk.on ? mk.on('click', function () { activate(key, false); }) : null; })(k);
          mk.addTo(map);
          // compat markers have no .on — bind on the element
          (function (key, m) {
            var el = elOf(m);
            if (el && !m.on) el.addEventListener('click', function (ev) { ev.stopPropagation(); activate(key, false); });
            // ★ fade in on the next frame (a marker placed at opacity 0, then shown)
            setTimeout(function () { var el2 = elOf(m); if (el2) el2.classList.remove('vtl-new'); }, 30);
          })(k, mk);
          items.set(k, { mk: mk, item: it, sig: ic.sig, lat: it.lat, lon: it.lon, heard: opts.heard ? opts.heard(it) : 0, w: ic.w, h: ic.h });
          decorate(k);
          // ★ LIVE: an item that arrives after the map was first drawn pulses (the first batch does not — it is history)
          if (fitted) pulse(k);
        } else {
          if (e.gone) { e.gone = false; setCls(e.mk, 'vtl-gone', false); }
          if (e.lat !== it.lat || e.lon !== it.lon) { e.mk.setLatLng([it.lat, it.lon]); e.lat = it.lat; e.lon = it.lon; }
          if (e.sig !== ic.sig) {
            e.mk.setIcon(divIcon(ic, k === sel ? ' vtl-sel' : ''));
            e.sig = ic.sig;
            decorate(k);
          }
          e.item = it; e.w = ic.w; e.h = ic.h;
          if (opts.heard) { var h = opts.heard(it); if (h > e.heard) { e.heard = h; pulse(k); } else e.heard = h; }
        }
        if (k === sel) {
          if (trailPts && trailKey === k) {
            var last = trailPts[trailPts.length - 1];
            if (!last || last[0] !== it.lat || last[1] !== it.lon) { trailPts.push([it.lat, it.lon]); drawTrail(); }
          }
        }
      }
      items.forEach(function (e, k) {
        if (seen.has(k) || e.gone) return;
        e.gone = true;
        setCls(e.mk, 'vtl-gone', true);
        setTimeout(function () { var cur = items.get(k); if (cur && cur.gone) { cur.mk.remove(); items.delete(k); if (sel === k) select(null, true); } }, FADE_OUT_MS + 50);
      });
      if (sel) renderCard();
      if (!fitted && items.size) { fitted = true; fit(); }
      syncArcs();
      syncShapes();
      declutterSoon();
    }

    function drawTrail() {
      if (!trailPts || trailPts.length < 2) { if (trailLine) { trailLine.remove(); trailLine = null; } return; }
      if (!trailLine) {
        trailLine = (L.polyline || L.polygon)(trailPts, { color: '#ffd34d', weight: 2.5, opacity: 0.85, fill: false, dashArray: '6 4' });
        trailLine.addTo(map);
      } else trailLine.setLatLngs(trailPts);
    }
    /* ★ The card is replaced only when the host's HTML for it CHANGES (not when what the host filled into it — a fetched
     *  text, a translation — makes the DOM differ from it); then onCard lets the host fill it again. */
    var cardHtml = '', cardKey = null;
    function renderCard() {
      var e = sel ? items.get(sel) : null;
      if (!e || !opts.card) { card.hidden = true; cardHtml = ''; cardKey = null; return; }
      var h = opts.card(e.item);
      if (h !== cardHtml || cardKey !== sel) {
        cardBody.innerHTML = h; cardHtml = h; cardKey = sel;
        card.setAttribute('aria-label', opts.aria ? String(opts.aria(e.item) || '') : (opts.label ? opts.label(e.item) : ''));
        if (opts.onCard) { try { opts.onCard(sel, cardBody); } catch (err) { console.error('[tracklayer] onCard', err); } }
      }
      card.hidden = false;
    }
    function select(key, fromMap) {
      if (destroyed) return;
      if (sel === key) { if (key) renderCard(); return; }
      if (sel && items.get(sel)) { var p = items.get(sel); var ic0 = iconFor(p.item, false); p.mk.setIcon(divIcon(ic0, '')); p.sig = ic0.sig; decorate(sel); }
      var was = sel;
      sel = key;
      if (trailLine) { trailLine.remove(); trailLine = null; }
      trailPts = null; trailKey = null;
      if (key && items.get(key)) {
        var q = items.get(key); var ic1 = iconFor(q.item, true); q.mk.setIcon(divIcon(ic1, ' vtl-sel')); q.sig = ic1.sig; decorate(key);
        renderCard();
        if (opts.trail) {
          var want = key;
          try {
            opts.trail(key, function (pts) {
              if (destroyed || sel !== want) return;
              trailKey = want;
              trailPts = Array.isArray(pts) ? pts.filter(function (p) { return p && isFinite(p[0]) && isFinite(p[1]); }).map(function (p) { return [p[0], p[1]]; }) : [];
              var cur = items.get(want);
              if (cur) { var l2 = trailPts[trailPts.length - 1]; if (!l2 || l2[0] !== cur.lat || l2[1] !== cur.lon) trailPts.push([cur.lat, cur.lon]); }
              drawTrail();
            });
          } catch (err) { console.error('[tracklayer] trail', err); }
        }
      } else {
        card.hidden = true; cardHtml = ''; cardKey = null;
        // ★ the card closed: the focus goes back to the marker it came from (keyboard / gamepad / TV)
        if (was && lastFocus && card.contains(doc.activeElement || null)) { try { lastFocus.focus(); } catch (err) { /* gone */ } }
        else if (was && lastFocus && (doc.activeElement === x || doc.activeElement === doc.body)) { try { lastFocus.focus(); } catch (err) { /* gone */ } }
      }
      syncShapes();
      declutterSoon();
      if (fromMap && opts.onSelect) { try { opts.onSelect(key); } catch (err) { console.error('[tracklayer] onSelect', err); } }
    }
    function pulse(key) {
      var e = items.get(String(key));
      if (!e) return;
      setTimeout(function () { var cur = items.get(String(key)); if (cur) setCls(cur.mk, 'vtl-pulse', true); }, 40);
      clearTimeout(e.pulseT);
      e.pulseT = setTimeout(function () { var cur = items.get(String(key)); if (cur) setCls(cur.mk, 'vtl-pulse', false); }, 3600);
    }
    function focus(key) {
      var e = items.get(String(key));
      if (!e) return false;
      select(String(key), false);
      var z = Math.max(map.getZoom(), opts.focusZoom || 10);
      map.flyTo([e.lat, e.lon], z, { duration: 0.9 });
      pulse(key);
      return true;
    }
    function fit() {
      var pts = [];
      items.forEach(function (e) { if (!e.gone) pts.push([e.lat, e.lon]); });
      if (rx) pts.push([rx.lat, rx.lon]);
      if (!pts.length) return;
      if (pts.length === 1) { map.setView(pts[0], 10); return; }
      try { map.fitBounds(L.latLngBounds(pts), { padding: [40, 40], maxZoom: 12 }); } catch (err) { console.error('[tracklayer] fit', err); }
    }
    function setReceiver(lat, lon) {
      if (!(isFinite(lat) && isFinite(lon))) return;
      rx = { lat: lat, lon: lon };
      if (rxMk) rxMk.setLatLng([lat, lon]);
      else {
        rxMk = L.marker([lat, lon], { icon: L.divIcon({ className: 'vtl-rx', html: RX_HTML, iconSize: [18, 18], iconAnchor: [9, 9] }), interactive: false, keyboard: false });
        rxMk.addTo(map);
      }
      syncArcs();
    }
    if (opts.receiver) setReceiver(opts.receiver.lat, opts.receiver.lon);

    function destroy() {
      destroyed = true;
      items.forEach(function (e) { try { e.mk.remove(); } catch (err) { /* gone with the map */ } });
      arcs.forEach(function (a) { try { a.line.remove(); } catch (err) { /* gone with the map */ } });
      arcs.clear();
      shapes.forEach(function (v) { v.layers.forEach(function (l) { try { l.remove(); } catch (err) { /* gone with the map */ } }); });
      shapes.clear();
      items.clear();
      if (trailLine) try { trailLine.remove(); } catch (err) { /* gone with the map */ }
      if (rxMk) try { rxMk.remove(); } catch (err) { /* gone with the map */ }
      try { map.off('movestart', nogOn); map.off('zoomstart', nogOn); map.off('moveend', nogOff); map.off('zoomend', nogOff); map.off('zoomend', zoomed); map.off('moveend', declutterSoon); } catch (err) { /* gone */ }
      if (card.parentNode) card.parentNode.removeChild(card);
    }
    return { update: update, select: function (k) { select(k == null ? null : String(k), false); }, focus: focus, fit: fit,
             setReceiver: setReceiver, destroy: destroy, selected: function () { return sel; },
             refreshCard: function () { cardHtml = ''; renderCard(); }, cardEl: function () { return cardBody; } };
  }

  global.VibeTrackLayer = { create: create, esc: esc };
})(typeof window !== 'undefined' ? window : globalThis);
