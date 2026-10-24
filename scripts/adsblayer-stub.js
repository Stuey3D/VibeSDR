/* ══════════════════════════════════════════════════════════════════════════════════════════════
 * ✗✗✗ STUB — NOT THE ADS-B LAYER. A STAND-IN UNTIL web/mapkit/adsblayer.js IS MERGED. ✗✗✗
 *
 * The real layer (icons by category, glide, breathe, auto-zoom / fit / range rings and their
 * in-map toggles) is being written on another branch. scripts/gen-adsblayer-source.mjs uses
 * web/mapkit/adsblayer.js when it exists and falls back to THIS file only when it does not, and
 * says so (ADSB_LAYER_IS_STUB in the generated module, and a console warning in the page).
 * At merge: bring web/mapkit/adsblayer.js in, run  node scripts/gen-adsblayer-source.mjs  and
 * delete this file.
 *
 * It implements the agreed API with plain markers so the app's map can be exercised end to end:
 *   VibeAdsbLayer.create(map, { receiver, prefs, onPrefs, theme }) -> { update, setReceiver, setPrefs, destroy }
 * ★ No backticks anywhere: the app injects this inside a template literal (see the generator).
 * ══════════════════════════════════════════════════════════════════════════════════════════════ */
(function () {
  if (window.VibeAdsbLayer) return;
  window.VibeAdsbLayer = {
    STUB: true,
    create: function (map, o) {
      var marks = {};
      var prefs = (o && o.prefs) || {};
      var rx = (o && o.receiver) || null;
      console.warn('[ADS-B] adsblayer.js is the STUB (scripts/adsblayer-stub.js) - plain markers only');
      function icon() {
        return L.divIcon({ html: '<div style="font-size:16px;line-height:18px;color:#ffd060">&#9992;</div>',
                           className: '', iconSize: [18, 18], iconAnchor: [9, 9] });
      }
      return {
        update: function (list) {
          var seen = {};
          (list || []).forEach(function (a) {
            if (!a || a.lat == null || a.lon == null) return;
            seen[a.hex] = 1;
            if (marks[a.hex]) marks[a.hex].setLatLng([a.lat, a.lon]);
            else marks[a.hex] = L.marker([a.lat, a.lon], { icon: icon() }).addTo(map);
          });
          Object.keys(marks).forEach(function (k) {
            if (!seen[k]) { try { map.removeLayer(marks[k]); } catch (e) {} delete marks[k]; }
          });
        },
        setReceiver: function (lat, lon) { rx = { lat: lat, lon: lon }; },
        setPrefs: function (p) { if (p) prefs = p; },
        destroy: function () {
          Object.keys(marks).forEach(function (k) { try { map.removeLayer(marks[k]); } catch (e) {} });
          marks = {};
        }
      };
    }
  };
})();
