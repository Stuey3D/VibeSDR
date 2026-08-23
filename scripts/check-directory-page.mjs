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
const src = html.match(/<script>([\s\S]*?)<\/script>\s*<\/body>/)[1];

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
    : k === 'clientWidth' ? 1900 : k === 'getBoundingClientRect' ? (() => ({ left: 0, top: 0, width: 1900, height: 200 }))
    : (t[k] !== undefined ? t[k] : ''),
  set: (t, k, v) => { if (k === 'innerHTML') globalThis.DIALHTML = String(v); t[k] = v; return true; },
});
globalThis.document = { getElementById: () => el, querySelector: () => el, querySelectorAll: () => [], addEventListener() {} };
globalThis.window = { addEventListener() {}, innerWidth: 1900, scrollY: 0, setTimeout: () => 0, setInterval: () => 0, clearTimeout() {}, clearInterval() {} };
globalThis.localStorage = { getItem: () => null, setItem() {} };
globalThis.performance = globalThis.performance || { now: () => 0 };
const layer = { addTo: () => layer, clearLayers() {}, addLayer() {} };
globalThis.L = {
  map: () => ({ setView: () => ({}), addLayer() {}, removeLayer() {}, hasLayer: () => false, on() {} }),
  tileLayer: () => ({ addTo() {} }), layerGroup: () => layer, polygon: () => ({ addTo() {} }),
  marker: () => ({ addTo() {}, bindPopup: () => ({}) }), divIcon: () => ({}),
};
globalThis.fetch = async (u) => ({ ok: true, json: async () => String(u).includes('bandplan') ? PLAN : {} });
globalThis.setInterval = () => 0; globalThis.setTimeout = (f) => 0; globalThis.clearTimeout = () => {};

const api = new Function(src + `
  return { set ALLv(v){ ALL = v; }, set PLANv(v){ PLAN = v; }, set VIEW(v){ dialView = v; },
           set NEEDLE(v){ needleV = v; vfoLive = true; }, set ANCHOR(v){ needleA = v; },
           set RANGE(v){ rangeSet = v; }, bandsOn, countriesOn,
           learnBands, renderDial, renderCountries, matches, activeRegion, planBandList,
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

console.log(fail.length ? `\n${fail.length} check(s) FAILED` : '\nthe directory page runs');
process.exit(fail.length ? 1 : 0);
