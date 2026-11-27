// test_map_autofit.ts — ★★★ beta92 ONE AUTO FIT FOR EVERY MAP (src/utils/mapAutoFit.ts), in node — silent, no browser:
//   · its maths IS the ADS-B map's: the glide's length, the easing, AutoZoom's hysteresis numbers and decisions, the
//     fit-mode camera — compared against web/mapkit/adsblayer.js (VibeAdsbLayer.maths) on the same inputs;
//   · the controller on a fake Leaflet map and a fake MapLibre map: the receiver is inside the framing, the first view is
//     a jump, then glides (2.2 s, ADS-B's easing); a station inside the view or a small drift moves nothing; one that
//     leaves the view pulls it out at once; zooming in waits the 4 s hold; a person's move pauses it (the key striped),
//     the key resumes / turns off / on; the max zoom holds for a single station;
//   · every map host uses it: the app's map page (spots, track kinds, NDB), the web's track views, digi window, DSC
//     window and NDB map, and the NDB layer.
//   npx tsx scripts/test_map_autofit.ts
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { MAP_AUTOFIT_JS, AUTOFIT_CAMERA_MS, AUTOFIT_MAX_ZOOM, AUTOFIT_HYST } from '../src/utils/mapAutoFit';
import { DIGI_FIT_MAX_ZOOM } from '../src/utils/digiMapShared';
import { dscMapHtml } from '../web/client/src/dsc';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };
const near = (a: number, b: number, tol: number) => Math.abs(a - b) <= tol;

// ── load both: ours, and ADS-B's ────────────────────────────────────────────────────────────
let clock = 0;
const timers: Array<{ fn: () => void; every: number; next: number; id: number }> = [];
let tid = 0;
function mkCtx(): any {
  const ctx: any = {
    console, Math, Number, String, Set, Map, Array, Object, JSON, isFinite, Error,
    performance: { now: () => clock },
    setTimeout: (fn: () => void, ms: number) => { const id = ++tid; timers.push({ fn, every: 0, next: clock + ms, id }); return id; },
    clearTimeout: (id: number) => { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); },
    setInterval: (fn: () => void, ms: number) => { const id = ++tid; timers.push({ fn, every: ms, next: clock + ms, id }); return id; },
    clearInterval: (id: number) => { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); },
  };
  ctx.globalThis = ctx; ctx.window = ctx;
  vm.createContext(ctx);
  return ctx;
}
/** Run the clock forward, firing timers in order. */
function advance(ms: number) {
  const end = clock + ms;
  for (;;) {
    timers.sort((a, b) => a.next - b.next);
    const t = timers[0];
    if (!t || t.next > end) break;
    clock = t.next;
    if (t.every) t.next += t.every; else timers.shift();
    t.fn();
  }
  clock = end;
}
const ctx = mkCtx();
vm.runInContext(MAP_AUTOFIT_JS, ctx, { filename: 'mapAutoFit.js' });
const AF = ctx.VibeAutoFit;
const ADSB_SRC = readFileSync('web/mapkit/adsblayer.js', 'utf8');
const actx = mkCtx();
vm.runInContext(ADSB_SRC, actx, { filename: 'adsblayer.js' });
const A = actx.VibeAdsbLayer.maths;

console.log('── the maths is the ADS-B map\'s ──');
{
  ok(!!AF && !!AF.attach && !!AF.maths, 'VibeAutoFit loads from the string (plain script, no module)');
  ok(!/[`\\]/.test(MAP_AUTOFIT_JS) && !/<\//.test(MAP_AUTOFIT_JS), 'no backtick, backslash or "</" (it is embedded in template literals and inline scripts)');
  ok(/const CAMERA_MS = GLIDE_MS;/.test(ADSB_SRC) && AUTOFIT_CAMERA_MS === A.GLIDE_MS && AF.maths.CAMERA_MS === A.GLIDE_MS,
     `the glide is ADS-B's: ${AF.maths.CAMERA_MS} ms = adsblayer CAMERA_MS (GLIDE_MS ${A.GLIDE_MS})`);
  const za = new A.AutoZoom(), zf = new AF.maths.AutoZoom();
  for (const k of Object.keys(AUTOFIT_HYST) as Array<keyof typeof AUTOFIT_HYST>)
    ok(za[k] === zf[k] && zf[k] === AUTOFIT_HYST[k], `hysteresis ${k} = ADS-B's (${zf[k]})`);
  let same = true;
  for (let i = 0; i <= 20; i++) if (!near(A.easeInOut(i / 20), AF.maths.easeInOut(i / 20), 1e-12)) same = false;
  ok(same, 'the easing is ADS-B\'s cubic ease-in-out');
  ok(/easeTo\(\{ center: \[cam\.center\.lon, cam\.center\.lat\], zoom: cam\.zoom, duration: ms, easing: easeInOut \}/.test(ADSB_SRC)
     && /gl\.easeTo\(\{center:\[cam\.center\.lon,cam\.center\.lat\],zoom:cam\.zoom,duration:ms,easing:easeInOut\}/.test(MAP_AUTOFIT_JS),
     'the GPU map glides with easeTo(duration, easeInOut), as ADS-B');
  ok(/lm\.flyTo\(\[cam\.center\.lat, cam\.center\.lon\], cam\.zoom, \{ duration: ms \/ 1000, easeLinearity: 0\.25 \}\)/.test(ADSB_SRC)
     && /lm\.flyTo\(\[cam\.center\.lat,cam\.center\.lon\],cam\.zoom,\{duration:ms\/1000,easeLinearity:0\.25\}\)/.test(MAP_AUTOFIT_JS),
     'the Leaflet map glides with flyTo(duration, easeLinearity 0.25), as ADS-B');
  ok(AUTOFIT_MAX_ZOOM === 11 && /ZMAX = gl \? 10 : 11/.test(ADSB_SRC), 'max zoom 11 (Leaflet) = ADS-B\'s ZMAX');
  // the fit-mode camera on the same inputs
  const VIEW = { w: 800, h: 600, pad: 48, tileSize: 512, minZoom: 1.5, maxZoom: 10 };
  const cases = [
    { points: [{ lat: 52, lon: -1 }, { lat: 53, lon: 1 }, { lat: 50.5, lon: -3 }], rx: { lat: 51.5, lon: -0.1 } },
    { points: [{ lat: 0, lon: 179 }, { lat: 0, lon: -179 }], rx: null },
    { points: [{ lat: 35.7, lon: 139.7 }], rx: { lat: 51.5, lon: -0.1 } },
    { points: [{ lat: 51.51, lon: -0.11 }], rx: null },
  ];
  for (const c of cases) {
    const a = A.cameraFor({ ...VIEW, ...c, fit: true }), f = AF.maths.cameraFor({ ...VIEW, ...c });
    ok(near(a.zoom, f.zoom, 1e-9) && near(a.center.lat, f.center.lat, 1e-9) && near(a.center.lon, f.center.lon, 1e-9),
       `cameraFor = ADS-B's fit camera (${c.points.length} pts${c.rx ? ' + rx' : ''}: z ${f.zoom.toFixed(2)})`);
  }
  // the decisions, step by step, on one scripted run
  const za2 = new A.AutoZoom(), zf2 = new AF.maths.AutoZoom();
  const cur = { center: { lat: 51, lon: 0 }, zoom: 6 };
  const runs: Array<[number, any, boolean]> = [[0, { center: { lat: 51, lon: 0 }, zoom: 7 }, false], [2000, { center: { lat: 51, lon: 0 }, zoom: 7 }, false],
    [4500, { center: { lat: 51, lon: 0 }, zoom: 7 }, false], [7000, { center: { lat: 51.4, lon: 0.6 }, zoom: 6.1 }, true], [9000, { center: { lat: 53, lon: 2 }, zoom: 6.2 }, false]];
  let agree = true;
  for (const [t, tgt, out] of runs) if (JSON.stringify(za2.decide(t, cur, tgt, out, 800, 600, 512, false)) !== JSON.stringify(zf2.decide(t, cur, tgt, out, 800, 600, 512, false))) agree = false;
  ok(agree, 'AutoZoom.decide gives ADS-B\'s answers on a scripted run (hold, out, re-centre)');
}

// ── a fake Leaflet map ───────────────────────────────────────────────────────────────────────
function fakeLeaflet(w = 800, h = 600) {
  const handlers: Record<string, Array<(e?: any) => void>> = {};
  const listeners: Record<string, Array<(e: any) => void>> = {};
  const m: any = {
    c: { lat: 30, lng: 0 }, z: 3, calls: [] as any[], options: { zoomSnap: 1 },
    getCenter: () => ({ lat: m.c.lat, lng: m.c.lng }), getZoom: () => m.z,
    getContainer: () => ({ clientWidth: w, clientHeight: h,
      addEventListener: (ev: string, fn: any) => { (listeners[ev] ||= []).push(fn); }, removeEventListener: () => {} }),
    setView: (c: [number, number], z: number, o: any) => { m.calls.push(['setView', c, z, o]); m.fire('movestart'); m.c = { lat: c[0], lng: c[1] }; m.z = z; m.fire('moveend'); },
    flyTo: (c: [number, number], z: number, o: any) => { m.calls.push(['flyTo', c, z, o]); m.fire('movestart'); m.c = { lat: c[0], lng: c[1] }; m.z = z; setTimeoutCtx(() => m.fire('moveend'), o.duration * 1000); },
    on: (ev: string, fn: any) => { (handlers[ev] ||= []).push(fn); }, off: () => {},
    once: (ev: string, fn: any) => { const f = (e: any) => { handlers[ev] = handlers[ev].filter((x) => x !== f); fn(e); }; (handlers[ev] ||= []).push(f); },
    fire: (ev: string, e?: any) => { for (const f of (handlers[ev] || []).slice()) f(e || {}); },
    input: (ev: string, target: any = {}) => { for (const f of listeners[ev] || []) f({ target }); },
  };
  return m;
}
const setTimeoutCtx = (fn: () => void, ms: number) => ctx.setTimeout(fn, ms);
function fakeButton() {
  const cls = new Set<string>(); const on: Record<string, Array<() => void>> = {}; const attrs: Record<string, string> = {};
  return { classList: { add: (c: string) => cls.add(c), toggle: (c: string, v: boolean) => { if (v) cls.add(c); else cls.delete(c); }, has: (c: string) => cls.has(c) },
           addEventListener: (ev: string, fn: () => void) => { (on[ev] ||= []).push(fn); }, removeEventListener: () => {},
           setAttribute: (k: string, v: string) => { attrs[k] = v; }, attrs, contains: (t: any) => t === 'BTN', title: '',
           click: () => { for (const f of on.click || []) f(); } };
}
/** Is (lat, lon) inside the fake map's view (Web Mercator, 256-px tiles), less a margin? */
function inView(m: any, lat: number, lon: number, w = 800, h = 600, margin = 20) {
  const o = AF.maths.screenOffset({ center: { lat: m.c.lat, lon: m.c.lng }, zoom: m.z }, lat, lon, 256);
  return Math.abs(o.x) < w / 2 - margin && Math.abs(o.y) < h / 2 - margin;
}

console.log('── the controller on Leaflet: framing, glide, hysteresis, the person, the key ──');
{
  clock = 1000; timers.length = 0;
  const m = fakeLeaflet(), btn = fakeButton();
  const rx = { lat: 51.5, lon: -0.1 };
  let pts: Array<[number, number]> = [[53.5, -2.2], [52.2, 0.1]];
  const af = AF.attach(m, { points: () => pts, receiver: () => rx, button: btn, what: 'station' });
  ok(btn.classList.has('on') && btn.classList.has('vaf-key') && btn.attrs['aria-pressed'] === 'true', 'the key: lit (on) when the map opens');
  af.poke();
  ok(m.calls.length === 1 && m.calls[0][0] === 'setView' && m.calls[0][3]?.animate === false, 'the first framing, as the map opens, is a jump (ADS-B\'s first camera)');
  ok(inView(m, rx.lat, rx.lon) && pts.every(([la, lo]) => inView(m, la, lo)), 'the receiver AND every station are in view');
  ok(Number.isInteger(m.z) && m.z <= AUTOFIT_MAX_ZOOM, `Leaflet zoom whole and capped (${m.z})`);
  // a station appears INSIDE the view, and one drifts a little — nothing moves, however long
  const n0 = m.calls.length;
  pts = [...pts, [52.6, -1.1]]; af.poke(); advance(10000);
  pts = pts.map(([a, b]) => [a + 0.01, b + 0.01] as [number, number]); af.poke(); advance(10000);
  ok(m.calls.length === n0, `a station inside the view, a small drift: no re-fit (${m.calls.length - n0} moves in 20 s)`);
  // one in the middle ages out (the box is the same) — still nothing
  pts = pts.slice(0, 2); af.poke(); advance(10000);
  ok(m.calls.length === n0, 'a station ageing out inside the box: no re-fit');
  // a far one appears — out at once, as a glide
  pts = [...pts, [40.4, -3.7]]; af.poke();
  const g = m.calls[m.calls.length - 1];
  ok(m.calls.length === n0 + 1 && g[0] === 'flyTo' && g[3].duration === AUTOFIT_CAMERA_MS / 1000 && g[3].easeLinearity === 0.25,
     `a station outside the view: OUT at once, as a ${g?.[3]?.duration} s glide`);
  ok(inView(m, 40.4, -3.7) && inView(m, rx.lat, rx.lon), 'the new station and the receiver are in the glided-to view');
  // it goes again: IN only after the 4 s hold (and never inside the 1.5 s gap)
  advance(3000);
  const n1 = m.calls.length;
  pts = pts.slice(0, -1); af.poke(); advance(2500);
  ok(m.calls.length === n1, 'the far station gone: no zoom in before the 4 s hold');
  const zFar = m.z;
  advance(8000);
  const back = m.calls.slice(n1);
  ok(back.length >= 1 && back.length <= 2 && back.every((c: any) => c[0] === 'flyTo') && m.z > zFar && inView(m, rx.lat, rx.lon) && pts.every(([la, lo]) => inView(m, la, lo)),
     `after the holds: glided back in (${back.length} calm move${back.length === 1 ? '' : 's'} — ADS-B's re-centre at 3 s, zoom in at 4 s; z ${zFar} → ${m.z})`);
  // a person drags the map: paused, striped; nothing moves however the stations change
  advance(5000);
  m.input('pointerdown'); m.fire('movestart', {});
  ok(af.state().paused && btn.classList.has('paused') && !btn.classList.has('on'), 'a person\'s move PAUSES it (the key striped, not lit)');
  const n2 = m.calls.length;
  pts = [...pts, [-33.9, 18.4]]; af.poke(); advance(10000);
  ok(m.calls.length === n2, 'paused: a far station does not move the map');
  // a code move (not a person) does not pause
  // the key: paused → resume (a glide now)
  btn.click();
  ok(!af.state().paused && af.state().on && btn.classList.has('on') && m.calls.length === n2 + 1 && m.calls[m.calls.length - 1][0] === 'flyTo',
     'the key when paused: resumes, and glides to frame everything');
  ok(inView(m, -33.9, 18.4) && inView(m, rx.lat, rx.lon), '…the far station and the receiver');
  advance(5000);
  btn.click();
  ok(!af.state().on && !btn.classList.has('on') && !btn.classList.has('paused') && btn.attrs['aria-pressed'] === 'false', 'the key when on: off');
  const n3 = m.calls.length;
  pts = [...pts, [64.1, -21.9]]; af.poke(); advance(10000);
  ok(m.calls.length === n3, 'off: nothing moves');
  btn.click();
  ok(af.state().on && m.calls.length === n3 + 1, 'the key when off: on, and a glide at once');
  // our own glide never pauses it, even right after the key (the click is not "a person's move" of the map)
  ok(!af.state().paused, 'its own glide (right after a tap) does not pause it');
  af.destroy();
}

console.log('── a code move, a focus, a lone station, nothing heard ──');
{
  clock = 100000; timers.length = 0;
  const m = fakeLeaflet();
  const af = AF.attach(m, { points: () => [[48.85, 2.35]], receiver: () => null });
  m.fire('movestart', {});   // the host's own setView, no input near it
  ok(!af.state().paused, 'a move made in code (no touch, wheel, key) does not pause it');
  af.poke();
  ok(m.z <= AUTOFIT_MAX_ZOOM && m.z >= AUTOFIT_MAX_ZOOM - 1, `one station, no receiver: zoom ${m.z} — at the cap (less ADS-B's slack), never street level`);
  af.pause();
  ok(af.state().paused, 'pause() — a row chosen (focus) — pauses it');
  af.destroy();
  const m2 = fakeLeaflet();
  const af2 = AF.attach(m2, { points: () => [[48.85, 2.35]], receiver: () => null, maxZoom: DIGI_FIT_MAX_ZOOM });
  af2.poke();
  ok(m2.z <= DIGI_FIT_MAX_ZOOM && m2.z >= DIGI_FIT_MAX_ZOOM - 1, `the digi map's own cap: ${m2.z} ≤ ${DIGI_FIT_MAX_ZOOM}`);
  af2.destroy();
  const m3 = fakeLeaflet();
  const af3 = AF.attach(m3, { points: () => [], receiver: () => ({ lat: 51.5, lon: -0.1 }) });
  af3.poke(); advance(5000);
  ok(m3.calls.length === 0, 'nothing heard yet: the map stays where it opened (the receiver alone is not a view)');
  af3.destroy();
  // a first framing that comes late (data seconds after opening) glides — no snap
  const m4 = fakeLeaflet(); let p4: Array<[number, number]> = [];
  const af4 = AF.attach(m4, { points: () => p4, receiver: () => ({ lat: 51.5, lon: -0.1 }) });
  advance(4000); p4 = [[55.9, -3.2]]; af4.poke();
  ok(m4.calls.length === 1 && m4.calls[0][0] === 'flyTo', 'the first stations, seconds after the map opened: a glide, not a jump');
  af4.destroy();
  const m5 = fakeLeaflet(); let n5 = 0;
  const af5 = AF.attach(m5, { points: () => [[1, 1]], receiver: () => null });
  af5.destroy(); advance(5000); n5 = m5.calls.length;
  ok(n5 === 0 && !timers.length, 'destroy(): the tick stops');
}

console.log('── the GPU map (the compat adapter): easeTo with ADS-B\'s easing, its own moves tagged ──');
{
  clock = 200000; timers.length = 0;
  const handlers: Record<string, Array<(e?: any) => void>> = {};
  const calls: any[] = [];
  const gl: any = {
    c: { lat: 30, lng: 0 }, z: 2,
    getCenter: () => gl.c, getZoom: () => gl.z, isMoving: () => false, project: () => ({}),
    getContainer: () => ({ clientWidth: 800, clientHeight: 600, addEventListener: () => {}, removeEventListener: () => {} }),
    easeTo: (o: any, ev: any) => { calls.push(['easeTo', o, ev]); for (const f of handlers.movestart || []) f(ev); gl.c = { lat: o.center[1], lng: o.center[0] }; gl.z = o.zoom; },
    jumpTo: (o: any, ev: any) => { calls.push(['jumpTo', o, ev]); for (const f of handlers.movestart || []) f(ev); gl.c = { lat: o.center[1], lng: o.center[0] }; gl.z = o.zoom; },
    on: (e: string, f: any) => { (handlers[e] ||= []).push(f); }, off: () => {}, once: () => {},
  };
  const compat = { _gl: gl, getZoom: () => gl.z + 1 };
  let pts: Array<[number, number]> = [[53.5, -2.2]];
  const af = AF.attach(compat, { points: () => pts, receiver: () => ({ lat: 51.5, lon: -0.1 }) });
  af.poke();
  ok(calls[0][0] === 'jumpTo' && calls[0][2]?.vsfit === true, 'first view: jumpTo, tagged as our own');
  advance(3000);
  pts = [...pts, [40.4, -3.7]]; af.poke();
  const e = calls[calls.length - 1];
  ok(e[0] === 'easeTo' && e[1].duration === AUTOFIT_CAMERA_MS && e[1].easing === AF.maths.easeInOut && e[2]?.vsfit === true,
     `then easeTo over ${e?.[1]?.duration} ms with the cubic ease-in-out, tagged`);
  ok(!af.state().paused, 'its own moves never pause it');
  ok(e[1].zoom <= AUTOFIT_MAX_ZOOM - 1, `GPU zoom capped one below Leaflet's (${e[1].zoom.toFixed(2)} ≤ ${AUTOFIT_MAX_ZOOM - 1})`);
  for (const f of handlers.movestart || []) f({ originalEvent: {} });
  ok(af.state().paused, 'a gesture (originalEvent) pauses it');
  af.destroy();
}

console.log('── every map uses it ──');
{
  const app = readFileSync('src/components/MapOverlay.tsx', 'utf8');
  const tracksWeb = readFileSync('web/client/src/tracks.ts', 'utf8') + readFileSync('web/client/src/mapChrome.ts', 'utf8');   // ★ the card system moved to mapChrome.ts
  const main = readFileSync('web/client/src/main.ts', 'utf8');
  const dsc = readFileSync('web/client/src/dsc.ts', 'utf8');
  const ndbWeb = readFileSync('web/client/src/ndb.ts', 'utf8');
  const ndbLayer = readFileSync('web/mapkit/ndblayer.js', 'utf8');
  const trackLayer = readFileSync('web/mapkit/tracklayer.js', 'utf8');
  const ndbList = readFileSync('src/components/NdbLiveList.tsx', 'utf8');
  // ★ beta96 stage 6: the ADS-B page is retired (ADS-B's screen is the AIR & SEA map, whose ADS-B layer keeps its own AUTO / FIT)
  ok(/\$\{!isHfdl \? `<script>\$\{MAP_AUTOFIT_JS\}<\/script>` : ''\}/.test(app), 'the app\'s map page carries it for every kind but HFDL (SNAP follows a flight)');
  // ★ beta96 stage 8 on the AIR & SEA map the NDB beacons heard are among its points (one camera, one FIT key)
  ok(/trkFit=VibeAutoFit\.attach\(map,\{points:function\(\)\{var p=l\.points\(\);if\(KIND==='airsea'&&ndbLayer&&ndbLayer\.points\)\{var q=ndbLayer\.points\(\);if\(q\.length\)p=p\.concat\(q\);\}return p;\},receiver:function\(\)\{return l\.receiver\(\);\},\s*button:document\.getElementById\('snap'\)/.test(app),
     'app: AIS · APRS · DSC · ISM · NAVTEX · sondes — the FIT key is the auto fit\'s, framing the items AND the receiver');
  ok(/firstFit:\(KIND==='avdata'\|\|!window\.VibeAutoFit\)\?undefined:false/.test(app) && /isTrk && kind !== 'avdata' \?/.test(app),
     'app: the aviation map keeps the ADS-B layer\'s own AUTO / FIT (no second key)');
  ok(/digiFit=window\.VibeAutoFit\?VibeAutoFit\.attach\(map,\{/.test(app), 'app: digi / CW spots maps');
  // ★★ beta96 stage 8 NDB is a live state of the AIR & SEA map: no FIT chip of its own — the map's FIT frames the beacons
  ok(/autoFit:false,drawReceiver:false/.test(app) && !/__ndbFitKey|label="FIT"/.test(app + ndbList), 'app: NDB — on the AIR & SEA map, framed by its FIT (no second chip)');
  // ★★ beta96 any selection (a row, a tap, a mesh route) holds the view until FIT or the selection is cleared (trkSelFit)
  ok(/var af=trkFitMake\(\);trkSelFit\(String\(k\)\);/.test(app) && /if\(k!=null\)\{trkSelNow=String\(k\);if\(st\.on\)\{if\(!st\.paused\)af\.pause\(\);trkSelHeld=true;\}\}/.test(app)
     && /else\{trkSelNow=null;if\(trkSelHeld\)\{trkSelHeld=false;if\(st\.on&&st\.paused\)af\.resume\(\);\}\}/.test(app)
     && /onSelect:function\(k\)\{trkPost\(\{t:'trkSel',key:k==null\?null:String\(k\)\}\);trkSelFit\(k\);/.test(app),
     'app: a selection (row or tap) pauses it until FIT or the selection is cleared');
  ok(/if \(!w\.VibeAutoFit\) runSource\(MAP_AUTOFIT_JS\);/.test(tracksWeb) && /this\.af = w\.VibeAutoFit\.attach\(map, \{\s*points: \(\) => \{ const p = layer\.points\(\); const m = this\.morePoints\?\.\(\); return m\?\.length \? \[\.\.\.p, \.\.\.m\] : p; \}, receiver: \(\) => layer\.receiver\(\), button: this\.fitBtn/.test(tracksWeb),
     'web: every track view (AIS, APRS, DSC, ISM + gallery, NAVTEX, sondes, datalink) — the FIT key is the auto fit\'s, with the receiver');
  ok(!/this\.autoFit\b|fitKeys/.test(tracksWeb), 'web: the old per-view fit-on-new-item logic is gone (one implementation)');
  ok(/this\.fitBtn\.hidden = !!this\.acLayer;/.test(tracksWeb), 'web: the aviation map keeps the ADS-B layer\'s AUTO / FIT');
  ok(/\$\{MAP_AUTOFIT_JS\}\nlet digiFit = null/.test(main) && /receiver: \(\) => me, button: document\.getElementById\('fitBtn'\)/.test(main), 'web: the digi modes / spots window (and the satellite pass map, which is it)');
  ok(/\$\{MAP_AUTOFIT_JS\}\nfunction autoFit\(\)/.test(dsc) && /receiver: \(\) => me,\s*button: document\.getElementById\('fitBtn'\)/.test(dsc), 'web: the DSC positions window');
  ok(/autoFit: false, drawReceiver: false/.test(ndbWeb) && /host\.morePoints = \(\) =>/.test(ndbWeb), 'web: NDB on the AIR & SEA map — the map\'s FIT frames the beacons (morePoints), no second auto fit');
  ok(/const AF = global\.VibeAutoFit \|\| null;/.test(ndbLayer) && /receiver: \(\) => rx/.test(ndbLayer), 'ndblayer.js frames with it (beacons heard + the receiver)');
  ok(/function points\(\)/.test(trackLayer) && /opts\.firstFit !== false/.test(trackLayer), 'tracklayer.js: points() for it, and no immediate first fit under it');
  ok(!/l\.fit\(\);\}catch\(e\)\{console\.error\('tracks fit'/.test(app.replace(/window\.__trkFit=[^\n]*/, '')), 'app: no one-shot FIT click left');
}

console.log('── a page that embeds it still parses (the web\'s DSC window) ──');
{
  const html = dscMapHtml(null, '/* vibemap */', [{ lat: 50.1, lon: -4.2, label: 'MAYDAY', level: 3 } as any], { lat: 51.5, lon: -0.1 }, null, '/mapdata/v1/');
  const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
  let parsed = scripts.length > 0;
  for (const src of scripts) { try { new vm.Script(src); } catch (e) { parsed = false; console.log(e); } }
  ok(parsed && scripts.some((x) => x.includes('var VibeAutoFit=')), 'the DSC window\'s script parses with the auto fit inside it');
}

console.log(`\n${fail ? 'FAILED' : 'all passed'} — ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
