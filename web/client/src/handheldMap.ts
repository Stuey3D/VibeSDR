/**
 * handheldMap.ts — "SET FROM MAP" on the R36S handheld (docs/v12/R36S-HANDHELD-IMAGE.md §0.1, R36S HARDWARE → LOCATION).
 *
 * ★★★ IN THE PAGE, NOT A WINDOW. Every other map in the client opens in a popup of its own; the handheld runs Cog as a
 *     one-view kiosk, which cannot show one. This is a full-screen layer over whatever is up (the receiver, or "Please
 *     connect an SDR" — no radio needed), drawn by the SAME basemap renderer as every other map (web/mapkit/vibemap.js
 *     on Leaflet, both served by the engine itself) from the map files on the card: /mapdata/v1/, the basic pack and the
 *     high-detail one. No tile server, no internet — there is none on the handheld.
 * ★★ The crosshair is FIXED at the centre and the map moves under it: a pad has no pointer, so "where you are" is
 *    simply the middle of the screen. D-pad / left stick pan, the shoulders (or the right stick up / down) zoom, the BOTTOM face button sets
 *    the location there, B leaves it as it was. A keyboard does the same (arrows, + / − or Page Up / Down, Enter,
 *    Escape) — a TV remote and desktop testing. The pad and keys reach it through gamepad.ts's capture (setPadCapture),
 *    so nothing underneath tunes, zooms or moves its highlight while it is up.
 * ★ The 6-character Maidenhead locator of the crosshair is shown live in a corner; A posts the LATITUDE and LONGITUDE
 *   (the more exact of the two) through the same route as the typed locator — the caller's onPick.
 * ★ Leaflet without its animations (zoom, fade, inertia) and the renderer's lowest memory band: the R36S has 1 GB and a
 *   Panfrost GPU shared with the waterfall underneath.
 */
import { loadLeafletInto } from './leafletAsset';
import { loadVibemapSource } from './chunkCache';
import { setPadCapture, type PadCaptureAction } from './gamepad';
import { padFace } from './padFace';

// ── Maidenhead ──────────────────────────────────────────────────────────────────────────────────────────────────────
/** The 6-character locator of a point ("IO92nh": field, square, subsquare — the subsquare in lower case, as written). */
export function latLonToLocator(lat: number, lon: number): string {
  const la = Math.min(89.999999, Math.max(-90, lat)) + 90;
  const lo = (((lon + 180) % 360) + 360) % 360;
  const A = 'ABCDEFGHIJKLMNOPQRSTUVWX';
  const f1 = Math.floor(lo / 20), f2 = Math.floor(la / 10);
  const s1 = Math.floor((lo % 20) / 2), s2 = Math.floor(la % 10);
  const u1 = Math.floor(((lo % 2) * 60) / 5), u2 = Math.floor(((la % 1) * 60) / 2.5);
  return A[f1] + A[f2] + s1 + s2 + A[u1].toLowerCase() + A[u2].toLowerCase();
}
/** The CENTRE of a 4- or 6-character locator, or null if it is not one. */
export function locatorToLatLon(loc: string): { lat: number; lon: number } | null {
  const m = /^([A-R]{2})(\d\d)([A-X]{2})?$/i.exec(loc.trim());
  if (!m) return null;
  const f = m[1].toUpperCase(), c = (s: string, i: number) => s.charCodeAt(i) - 65;
  let lon = c(f, 0) * 20 - 180 + Number(m[2][0]) * 2;
  let lat = c(f, 1) * 10 - 90 + Number(m[2][1]);
  if (m[3]) { const u = m[3].toUpperCase(); lon += c(u, 0) * (5 / 60) + 2.5 / 60; lat += c(u, 1) * (2.5 / 60) + 1.25 / 60; }
  else { lon += 1; lat += 0.5; }
  return { lat, lon };
}

// ── The layer ───────────────────────────────────────────────────────────────────────────────────────────────────────
export interface MapPickOptions {
  /** Where to start: the device's location if it has one, else null = the whole world. */
  start: { lat: number; lon: number } | null;
  /** ★ Bottom face button (B on the R36S — by POSITION, Stuart 2026-11-06; the right one, A, cancels): the crosshair's point. The layer closes whatever this returns. */
  onPick(lat: number, lon: number): void;
}

const PAN_STEP = 64;          // px a D-pad press (or a held one's repeat, ~9 a second) moves the map
const STICK_DEAD = 0.18;      // the stick's rest wobble
const STICK_SPEED = 700;      // px a second at full deflection (squared below: fine near the middle, fast at the edge)
const ZOOM_EVERY = 380;       // ms between zoom steps while the right stick is held up or down
const MIN_Z = 1, MAX_Z = 12, START_Z = 8;

let openNow = false;
export function mapPickOpen(): boolean { return openNow; }

function injectStyle() {
  if (document.getElementById('hhMapStyle')) return;
  const st = document.createElement('style');
  st.id = 'hhMapStyle';
  st.textContent = `
#hhMapPick { position: fixed; inset: 0; z-index: 9450; background: #0c1a2a; color: #ffb833; font: 600 12px ui-monospace, Menlo, monospace; }
#hhMapPickMap { position: absolute; inset: 0; }
#hhMapPick .xh { position: absolute; left: 50%; top: 50%; width: 0; height: 0; pointer-events: none; z-index: 1000; }
#hhMapPick .xh i { position: absolute; background: #ffb833; box-shadow: 0 0 0 1px rgba(0,0,0,.7); }
#hhMapPick .xh i.h { left: -18px; top: -1px; width: 12px; height: 2px; } #hhMapPick .xh i.h2 { left: 6px; top: -1px; width: 12px; height: 2px; }
#hhMapPick .xh i.v { left: -1px; top: -18px; width: 2px; height: 12px; } #hhMapPick .xh i.v2 { left: -1px; top: 6px; width: 2px; height: 12px; }
#hhMapPick .xh b { position: absolute; left: -3px; top: -3px; width: 6px; height: 6px; border-radius: 50%; border: 1px solid #ffb833; box-sizing: border-box; }
#hhMapPickLoc { position: absolute; top: 6px; left: 6px; z-index: 1000; background: rgba(8,6,1,.85); border: 1px solid #ffb833;
  border-radius: 6px; padding: 4px 8px; pointer-events: none; font-variant-numeric: tabular-nums; white-space: pre; line-height: 1.35; }
#hhMapPickLoc .g { font-size: 18px; letter-spacing: 2px; }
#hhMapPickBar { position: absolute; left: 0; right: 0; bottom: 0; z-index: 1000; display: flex; gap: 8px; align-items: center;
  background: rgba(8,6,1,.85); padding: 4px 8px; font-weight: 400; font-size: 11px; }
#hhMapPickBar span { flex: 1; }
#hhMapPickBar button { font: 600 11px ui-monospace, monospace; color: #ffb833; background: transparent; border: 1px solid #ffb833;
  border-radius: 5px; padding: 3px 8px; }
#hhMapPickNote { position: absolute; left: 50%; top: 40%; transform: translateX(-50%); z-index: 1000; background: rgba(8,6,1,.9);
  border: 1px solid #ffb833; border-radius: 8px; padding: 8px 14px; text-align: center; max-width: 80vw; pointer-events: none; }
#hhMapPickNote:empty { display: none; }
#hhMapPick .leaflet-container .leaflet-control-attribution { font-size: 8px; line-height: 1.25; margin-bottom: 26px; }
`;
  document.head.appendChild(st);
}

function runSource(src: string) {
  const s = document.createElement('script');
  s.textContent = src;          // ★ runs synchronously on append — the global exists on the next line
  document.head.appendChild(s);
}

/** Open the map layer. A second call while it is up does nothing. */
export function openMapPick(o: MapPickOptions): void {
  if (openNow) return;
  openNow = true;
  injectStyle();
  const root = document.createElement('div');
  root.id = 'hhMapPick';
  root.setAttribute('role', 'dialog');
  root.setAttribute('aria-label', 'Set location from the map');
  root.innerHTML = '<div id="hhMapPickMap"></div>'
    + '<div class="xh"><i class="h"></i><i class="h2"></i><i class="v"></i><i class="v2"></i><b></b></div>'
    + '<div id="hhMapPickLoc"></div><div id="hhMapPickNote">Loading the map…</div>'
    // ★ Switch-style face glyphs (padFace.ts): bottom = set, right = cancel, whatever the letters on this pad
    + `<div id="hhMapPickBar"><span>D-pad / stick move · L / R zoom · ${padFace('bottom', 'bottom button')} set · ${padFace('right', 'right button')} cancel</span>`
    + '<button type="button" id="hhMapPickSet">SET HERE</button><button type="button" id="hhMapPickCancel">CANCEL</button></div>';
  document.body.appendChild(root);
  const host = root.querySelector('#hhMapPickMap') as HTMLDivElement;
  const locEl = root.querySelector('#hhMapPickLoc') as HTMLDivElement;
  const note = root.querySelector('#hhMapPickNote') as HTMLDivElement;

  let map: any = null, vm: any = null, closed = false;
  // ★ The point under the crosshair. Kept here as well as in the map: before the map has loaded (or if it never
  //   does) A still means "the start point", and the readout still says what it is.
  let lat = o.start ? o.start.lat : 20, lon = o.start ? o.start.lon : 0, zoom = o.start ? START_Z : MIN_Z;
  const paint = () => {
    const wl = ((((lon + 180) % 360) + 360) % 360) - 180;
    locEl.innerHTML = '';
    const g = document.createElement('div'); g.className = 'g'; g.textContent = latLonToLocator(lat, wl);
    locEl.append(g, document.createTextNode(`${lat.toFixed(5)}, ${wl.toFixed(5)}`));
    root.dataset.lat = lat.toFixed(5); root.dataset.lon = wl.toFixed(5); root.dataset.zoom = String(zoom);
    root.dataset.locator = g.textContent;
  };
  const fromMap = () => {
    if (!map) return;
    const c = map.getCenter();
    lat = c.lat; lon = c.lng; zoom = map.getZoom();
    paint();
  };
  paint();

  const pan = (dx: number, dy: number) => {
    if (!map) return;
    map.panBy([dx, dy], { animate: false });
    // ★ Never past the top or bottom of the world (Leaflet would happily show grey there)
    const c = map.getCenter();
    if (Math.abs(c.lat) > 85) map.setView([Math.sign(c.lat) * 85, c.lng], map.getZoom(), { animate: false });
    fromMap();
  };
  const zoomBy = (d: 1 | -1) => {
    if (!map) return;
    const z = Math.max(MIN_Z, Math.min(MAX_Z, Math.round(map.getZoom()) + d));
    if (z !== map.getZoom()) map.setView(map.getCenter(), z, { animate: false });   // ★ about the CENTRE: the crosshair stays put
    fromMap();
  };
  const close = () => {
    if (closed) return;
    closed = true; openNow = false;
    release();
    try { vm?.detach?.(); } catch (e) { console.error('[map pick] detach', e); }
    try { map?.remove(); } catch (e) { console.error('[map pick] remove', e); }
    root.remove();
  };
  const pick = () => {
    const wl = ((((lon + 180) % 360) + 360) % 360) - 180;
    const la = Number(lat.toFixed(5)), lo = Number(wl.toFixed(5));
    close();
    o.onPick(la, lo);
  };

  let zoomAt = 0, lastFrame = 0;
  const release = setPadCapture({
    act: (a: PadCaptureAction) => {
      switch (a) {
        case 'up': pan(0, -PAN_STEP); break;
        case 'down': pan(0, PAN_STEP); break;
        case 'left': pan(-PAN_STEP, 0); break;
        case 'right': pan(PAN_STEP, 0); break;
        case 'zoomIn': zoomBy(1); break;
        case 'zoomOut': zoomBy(-1); break;
        case 'enter': pick(); break;
        case 'back': close(); break;
      }
    },
    sticks: (lx, ly, _rx, ry) => {
      const now = performance.now();
      const dt = lastFrame ? Math.min(0.1, (now - lastFrame) / 1000) : 0;
      lastFrame = now;
      const mag = Math.hypot(lx, ly);
      if (mag > STICK_DEAD && dt > 0) {
        const k = ((mag - STICK_DEAD) / (1 - STICK_DEAD)) ** 2 * STICK_SPEED * dt / mag;
        pan(lx * k, ly * k);
      }
      // ★ The right stick pushed up zooms in, down zooms out — a step, then one every ZOOM_EVERY while held
      if (Math.abs(ry) > 0.6) { if (now >= zoomAt) { zoomBy(ry < 0 ? 1 : -1); zoomAt = now + ZOOM_EVERY; } }
      else zoomAt = 0;
    },
  });
  (root.querySelector('#hhMapPickSet') as HTMLButtonElement).onclick = pick;
  (root.querySelector('#hhMapPickCancel') as HTMLButtonElement).onclick = close;

  void (async () => {
    const w = window as any;
    if (!await loadLeafletInto(document)) { if (!closed) note.innerHTML = `The map could not be loaded — ${padFace('right', 'right button')} to go back.`; return; }
    if (closed) return;
    try {
      if (!w.VibeMap) { const { VIBEMAP_JS } = await loadVibemapSource(); if (closed) return; if (!w.VibeMap) runSource(VIBEMAP_JS); }
    } catch (e) { console.error('[map pick] the basemap renderer did not load', e); }
    if (closed) return;
    const L = w.L;
    map = L.map(host, { zoomControl: false, keyboard: false, worldCopyJump: true, preferCanvas: true, inertia: false,
                        zoomAnimation: false, fadeAnimation: false, markerZoomAnimation: false, minZoom: MIN_Z, maxZoom: MAX_Z,
                        zoomSnap: 1, boxZoom: false });
    map.setView([lat, lon], zoom, { animate: false });
    if (w.VibeMap) vm = w.VibeMap.attach(map, { dataBase: '/mapdata/v1/', profile: 'directory', memory: { lowRam: true } });
    else note.textContent = 'The map\'s drawing code did not load — the crosshair still works.';
    if (w.VibeMap) note.textContent = '';
    // ★ A finger or a mouse may move it too (desktop testing): the readout follows any move, not only ours
    map.on('move zoomend', fromMap);
    fromMap();
  })().catch((e) => { console.error('[map pick] the map did not load', e); if (!closed) note.innerHTML = `The map did not load — ${padFace('right', 'right button')} to go back.`; });
}
