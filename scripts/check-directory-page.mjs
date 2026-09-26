/**
 * check-directory-page.mjs — RUN the directory page, do not merely parse it.
 *
 * ★★★ WHY THIS EXISTS. On 2026-08-23 an edit spliced a block out of index.html and took eleven
 *     functions with it — renderDial, drawRow, planBandList and the rest. The file still PARSED,
 *     so the `new Function(src)` check passed and it was deployed. The page loaded, threw
 *     ReferenceError on its first paint, and the dial did not draw. A parse is not a run: an
 *     identifier used outside the scope it was declared in is exactly what a parser cannot see.
 *     scripts/build-web.mjs already carries this lesson for the web client; the directory had no
 *     equivalent.
 * ★★ So this executes the page against a stub DOM and the LIVE directory feed, then asserts the
 *    things a visitor would notice: the dial draws two rows, the band plan is loaded, the search
 *    narrows, and the region follows the country filter.
 *
 *   node scripts/check-directory-page.mjs            (fetches the live directory)
 *   node scripts/check-directory-page.mjs fixture.json
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const html = fs.readFileSync(path.join(root, 'directory/public/index.html'), 'utf8');
const PLAN = JSON.parse(fs.readFileSync(path.join(root, 'directory/public/bandplan.json'), 'utf8'));
/* ★★★ THE LAST INLINE SCRIPT, NOT EVERYTHING FROM THE FIRST ONE. `[\s\S]*?` cannot stop at a
 *  `</script>` it is allowed to swallow, so once the page grew a SECOND inline script (the portable
 *  store's reset button, up in <main>) this match began at that one and ran through the closing tag,
 *  the <footer> and two <script src> lines -- and new Function() threw "Unexpected token '<'" on a
 *  line of HTML. The whole check had been dead since then, silently, which is worse than absent: a
 *  guard nobody has seen fail is assumed to be passing. ★ The inner group now refuses to cross a
 *  closing tag, so it can only be the last inline block before </body>. */
const src = html.match(/<script>((?:(?!<\/script>)[\s\S])*)<\/script>\s*<\/body>/)[1];
/* ★★ THE PAGE NOW LOADS THE SHARED BASEMAP RENDERER, so the check must too. ✗ Not a VibeMap stub:
 *  a stub would let the page call an attach() that does not exist anywhere real, which is precisely
 *  the "written and never read" shape. The real file runs fine here -- its draws are all behind the
 *  coalesced setTimeout this file stubs out. */
const VIBEMAP_SRC = fs.readFileSync(path.join(root, 'web/mapkit/vibemap.js'), 'utf8');

// ★★ THE FEED IS FETCHED FIRST, BEFORE ANY STUBBING. The stub replaces setTimeout — which the
//    HTTP client itself uses — so fetching afterwards fails in a way that looks like a network
//    fault and is really this file shooting itself.
const fixtureArg = process.argv[2];
const dir = fixtureArg ? JSON.parse(fs.readFileSync(fixtureArg, 'utf8'))
                       : await (await fetch('https://vibeserver.vibesdr.net/api/directory')).json();

// ── a DOM stub that is permissive about everything except existing ──────────────────────────
const el = new Proxy({}, {
  get: (t, k) => k === 'addEventListener' ? (() => {})
    : k === 'querySelector' ? (() => el) : k === 'querySelectorAll' ? (() => [])
    : k === 'closest' ? (() => null) : k === 'dataset' ? {}
    : k === 'classList' ? { add() {}, remove() {}, toggle() {} }
    : k === 'clientWidth' ? (globalThis.CHECK_WIDTH || 1900)
    : k === 'offsetWidth' ? Math.min(340, (globalThis.CHECK_WIDTH || 1900) - 16)
    : k === 'getBoundingClientRect' ? (() => ({ left: 0, top: 40, right: 120, bottom: 62,
                                                width: globalThis.CHECK_WIDTH || 1900, height: 200 }))
    : (t[k] !== undefined ? t[k] : ''),
  set: (t, k, v) => { if (k === 'innerHTML') globalThis.DIALHTML = String(v); t[k] = v; return true; },
});
globalThis.document = { getElementById: () => el, querySelector: () => el, querySelectorAll: () => [], addEventListener() {} };
globalThis.window = { addEventListener() {}, innerWidth: 1900, scrollY: 0, setTimeout: () => 0, setInterval: () => 0, clearTimeout() {}, clearInterval() {} };
globalThis.localStorage = { getItem: () => null, setItem() {} };
// ★ The page reads location.search for its ?demo switch. Default: the real directory.
globalThis.location = { search: process.env.CHECK_DEMO ? '?demo' : '', href: 'https://vibeserver.vibesdr.net/' };
globalThis.performance = globalThis.performance || { now: () => 0 };
const layer = { addTo: () => layer, clearLayers() {}, addLayer() {}, getLayers: () => [] };
const mapStub = {
  setView: () => mapStub, addLayer() {}, removeLayer() {}, hasLayer: () => false, on() {}, off() {},
  // ★ The shared renderer declares its z-order in PANES and asks the map for a container background,
  //   so the stub has to answer those. A stub that throws here would make a map bug out of nothing.
  createPane: () => ({ style: {} }), getPane: () => ({ style: {} }),
  getContainer: () => ({ id: 'map', style: {}, ownerDocument: globalThis.document }),
  getZoom: () => 2, latLngToContainerPoint: () => ({ x: 0, y: 0 }),
  getBounds: () => ({ pad: () => mapStub.getBounds(), getWest: () => -180, getEast: () => 180,
                      getSouth: () => -85, getNorth: () => 85 }),
};
globalThis.L = {
  map: () => mapStub,
  tileLayer: () => ({ addTo() {} }), layerGroup: () => layer, polygon: () => ({ addTo() {} }),
  marker: () => ({ addTo() {}, bindPopup: () => ({}) }), divIcon: () => ({}),
  canvas: () => ({}), polyline: () => ({ addTo: () => ({ bindTooltip() {} }) }),
  circleMarker: () => ({ addTo() {} }), imageOverlay: () => ({ addTo() {} }),
};
// ★ Serve the page's OWN assets from disk. A stub that answers {} to everything checks the page
//   against data it will never see — country-shapes.json in particular, without which the demo
//   estate silently lands every receiver at 0°N 0°E and the check passes on a lie.
const SHAPES = JSON.parse(fs.readFileSync(path.join(root, 'directory/public/country-shapes.json'), 'utf8'));
globalThis.fetch = async (u) => ({ ok: true, json: async () =>
  String(u).includes('bandplan') ? PLAN : String(u).includes('country-shapes') ? SHAPES : {} });
globalThis.setInterval = () => 0; globalThis.setTimeout = (f) => 0; globalThis.clearTimeout = () => {};
// ★ Loaded the way the browser loads it: a script of its own, before the page's, defining window.VibeMap.
globalThis.document.head = { appendChild() {} };
globalThis.document.createElement = () => ({ style: {} });
// ★ vibemap.js reads Leaflet off `window`, as a browser script does; the stub above only put it on
//   globalThis. Two names for one object is exactly the kind of gap that reads as a renderer bug.
globalThis.window.L = globalThis.L;
globalThis.window.document = globalThis.document;
new Function(VIBEMAP_SRC)();
globalThis.VibeMap = globalThis.window.VibeMap;

const api = new Function(src + `
  return { set ALLv(v){ ALL = v; }, set PLANv(v){ PLAN = v; }, set VIEW(v){ dialView = v; },
           set NEEDLE(v){ needleV = v; vfoLive = true; }, set ANCHOR(v){ needleA = v; },
           set RANGE(v){ rangeSet = v; }, bandsOn, countriesOn,
           learnBands, renderDial, renderCountries, matches, activeRegion, planBandList,
           demoServers, DEMO_PLACES,
           get ROWS(){ return DIAL_ROWS; } };`)();

api.PLANv = PLAN;
api.ALLv = dir.servers;
api.learnBands(dir.servers);
api.renderCountries();
api.renderDial();

const fail = [];
const ok = (cond, what) => { console.log(`${cond ? '  ok  ' : ' FAIL '} ${what}`); if (!cond) fail.push(what); };

const html2 = globalThis.DIALHTML || '';
ok(api.ROWS.length === 2, 'two dials drawn (detail + whole network)');
ok(api.ROWS.some((r) => r.kind === 'over'), 'the whole-network dial is present');
ok(/class="dNum/.test(html2), 'the ticker has numerals');
ok(/class="dNeedle b"/.test(html2), 'the VFO needle is drawn');
ok(api.planBandList().length > 20, `band plan loaded (${api.planBandList().length} bands)`);
ok(/dDecade/.test(html2), 'the wavelength strip is drawn');

const all = dir.servers.filter(api.matches).length;
ok(all === dir.servers.length, 'no filter → every server listed');
api.bandsOn.add('40 m amateur'); api.bandsOn.add('20 m amateur');
const both = dir.servers.filter(api.matches);
ok(both.length < dir.servers.length || dir.servers.length === 1,
   `two bands narrow the list (${both.length} of ${dir.servers.length})`);
ok(both.every((s) => (s.radios || []).length > 0), 'matches only servers with radios');
api.bandsOn.clear();

const r1 = api.activeRegion();
ok(r1 >= 1 && r1 <= 3, `region resolves (${r1})`);

// ★ The simulated estate is a test aid, so it gets tested too — an aid that lies is worse than
//   none (the same rule as the blunt AGC detector that cleared a stuttering file twice).
const demo = await api.demoServers();
ok(demo.length === api.DEMO_PLACES.length, `demo estate builds (${demo.length} servers)`);
ok(demo.every((s) => Number.isFinite(s.lat) && Number.isFinite(s.lon) && (s.lat || s.lon)),
   'every simulated server has a position');
ok(new Set(demo.map((s) => s.country)).size > 15,
   `spread across countries (${new Set(demo.map((s) => s.country)).size})`);
ok(demo.every((s) => s.demo === true), 'every simulated server is marked as such');
api.ALLv = dir.servers.concat(demo);
api.learnBands(api.ALLv ?? []);
api.renderDial();
ok(api.ROWS.length === 2, 'the dial still draws with the demo estate');

// ★★ A PHONE-WIDTH PASS. The dial is drawn in pixels, so a narrow window is a different drawing,
//    not the same one scaled — and the numerals, the band labels and the tick ladder all have to
//    survive it. 360 is the width of the narrowest phone anybody still carries.
globalThis.CHECK_WIDTH = 360;
globalThis.window.innerWidth = 360; globalThis.window.innerHeight = 640;
api.renderDial();
const narrow = globalThis.DIALHTML || '';
ok(/class="dNum/.test(narrow), 'narrow (360px): the ticker still has numerals');
ok(api.ROWS.length === 2, 'narrow (360px): both dials still drawn');
ok(/class="dNeedle b"/.test(narrow), 'narrow (360px): the needle is still drawn');
const nums = (narrow.match(/class="dNum[^"]*"/g) || []).length;
ok(nums >= 4 && nums <= 40, `narrow (360px): a sensible number of figures (${nums})`);
globalThis.CHECK_WIDTH = 1900;

console.log(fail.length ? `\n${fail.length} check(s) FAILED` : '\nthe directory page runs');
process.exit(fail.length ? 1 : 0);
