/**
 * ★★★ beta96 THE MAP FOLLOWS THE MODE — Mac Ha's report (Discord 11-26, the app on a server in Vietnam): "Say I was tuned
 * and show AIS, then exit and select ADS-B — the radio tuned to 1090 and the list shows planes heard, but nothing shows on
 * the map, while at the top it still says AIS Shipping map." (and the reverse: EXIT ADS-B · ADS-B AIRCRAFT MAP over the
 * RECEIVED ships). src/utils/mapPage.ts says why.
 *
 * Drives a model of MapOverlay's page lifecycle — ONE file on disk for every map (mapPack.ts writeMapPage), a WebView that
 * reads that file when it MOUNTS (a new key = a new WebView) and never again for the same uri — through:
 *   1. open AIS → EXIT → select ADS-B (the path he describes), and back;
 *   2. a mode change while the map is OPEN (the shared dial: another listener, or the server's mode_resume — its `_off`
 *      and the new mode's first report), AIS → ADS-B and ADS-B → AIS, both message orders;
 *   3. a rewrite for the same kind in the same opening (the receiver's position arriving) does NOT tear the map down;
 * each first with the OLD rule (to prove the model reproduces the bug — a negative before the positive), then the new.
 * Then shownMapOf (the screen's map precedence) over the same transitions, and the wiring in MapOverlay / SDRScreen / the
 * web client (main.ts wholeMapYield).
 *
 * ★★★ beta96 STAGE 6: ADS-B's screen is now the AIR & SEA map too, so AIS ↔ ADS-B is the SAME kind ('airsea') — the page
 * STAYS and its chrome (title, EXIT key) is pushed in from the current mode (liveMaps.ts liveChrome → __trkChrome). The
 * page-per-kind rule below is still the rule for two DIFFERENT kinds (AIR & SEA ↔ NDB by the shared dial, the same bug
 * class); section 4 drives AIS ↔ ADS-B on the one page and checks the title and EXIT follow without a stale page.
 *
 * Run: node --no-warnings scripts/test_map_page_kind.ts
 */
import { mapPageShown, mapPageWritten, shownMapOf, type MapPage, type ShownMapState } from '../src/utils/mapPage.ts';
import { liveChrome } from '../src/services/liveMaps.ts';
import { readFileSync } from 'node:fs';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const ok = (what: string, c: boolean) => eq(what, c, true);

type K = 'airsea' | 'ndb';   // ★ stage 6: two DIFFERENT kinds (AIS and ADS-B are both 'airsea' now — section 4)
/** What the user SEES: the page the WebView loaded (title / EXIT key / layers) and the RN list drawn over it. */
interface Seen { page: string | null; list: string | null }
const LIST: Record<K, string> = { airsea: 'AIRCRAFT', ndb: 'BEACONS' };

/** ★ The overlay as it was before beta96: a bare pageUri, kept while hidden; the WebView keyed by nothing. */
class OldOverlay {
  disk = '';                       // the one file's contents
  pageUri: string | null = null;
  web: { loaded: string } | null = null;
  visible = false; kind: K | null = null;
  render(visible: boolean, kind: K | null) {
    this.visible = visible; this.kind = kind;
    const shown = visible && kind ? this.pageUri : null;
    if (!shown) { this.web = null; return; }                     // `if (!visible || !kind) return null` unmounts it
    if (!this.web) this.web = { loaded: this.disk };             // a fresh mount reads the file AS IT IS NOW
  }
  /** the write effect finishing (ensureMapPack + writeMapPage), then the render it causes */
  write() {
    if (!this.visible || !this.kind) return;
    this.disk = `page:${this.kind}`;
    this.pageUri = 'file:///docs/vibemap.html';                  // ★ the same string every time — no reload
    this.render(this.visible, this.kind);
  }
  seen(): Seen { return { page: this.web?.loaded ?? null, list: this.web && this.kind ? LIST[this.kind] : null }; }
}

/** ★ The overlay as it is now: the page kept with its kind and opening (mapPage.ts), the WebView keyed by its id. */
class NewOverlay {
  disk = '';
  page: MapPage<K> | null = null;
  openSeq = 0; wasVisible = false; ids = 0;
  web: { key: number; loaded: string } | null = null;
  visible = false; kind: K | null = null;
  mounts = 0;
  render(visible: boolean, kind: K | null) {
    if (visible && !this.wasVisible) this.openSeq++;
    this.wasVisible = visible;
    this.visible = visible; this.kind = kind;
    if (!visible) this.page = null;                              // useEffect(() => { if (!visible) setPage(null) })
    const shown = mapPageShown(this.page, visible, kind, this.openSeq);
    if (!shown) { this.web = null; return; }
    if (!this.web || this.web.key !== shown.id) { this.web = { key: shown.id, loaded: this.disk }; this.mounts++; }
  }
  write(what?: string) {
    if (!this.visible || !this.kind) return;
    const kind = this.kind, open = this.openSeq;
    this.disk = what ?? `page:${kind}`;
    this.page = mapPageWritten(this.page, 'file:///docs/vibemap.html', kind, open, () => ++this.ids);
    this.render(this.visible, this.kind);
  }
  seen(): Seen { return { page: this.web?.loaded ?? null, list: this.web && this.kind ? LIST[this.kind] : null }; }
}

type Overlay = OldOverlay | NewOverlay;
const consistent = (s: Seen) => s.page === null || (s.list === 'AIRCRAFT' ? s.page === 'page:airsea' : s.page === 'page:ndb');

/* ── 1. open AIR & SEA → EXIT → select NDB (Mac Ha's path, between two kinds) ─────────────────────────────────────────────────────────── */
function exitThenPick(o: Overlay): Seen[] {
  const out: Seen[] = [];
  o.render(true, 'airsea'); o.write(); out.push(o.seen());          // AIS: its map
  o.render(false, null);                                         // EXIT AIS (aisUiOff → shownMap null)
  o.render(true, 'ndb'); out.push(o.seen());                    // ADS-B key: the map opens AT ONCE, before its page is written
  o.write(); out.push(o.seen());                                 // …the ADS-B page written
  o.render(false, null);                                         // EXIT ADS-B
  o.render(true, 'airsea'); o.write(); out.push(o.seen());          // and back to AIS
  return out;
}
{
  const old = exitThenPick(new OldOverlay());
  // ★ the negative: the old rule shows the AIS page under the ADS-B list (his first screenshot) and the reverse
  eq('OLD exit→NDB: the AIR & SEA page under the BEACONS list (the bug, reproduced)', old[2], { page: 'page:airsea', list: 'BEACONS' });
  eq('OLD exit→AIR & SEA: the NDB page under the AIRCRAFT list (his second screenshot\'s shape)', old[3], { page: 'page:ndb', list: 'AIRCRAFT' });
  const nu = exitThenPick(new NewOverlay());
  eq('NEW AIR & SEA: its own page', nu[0], { page: 'page:airsea', list: 'AIRCRAFT' });
  eq('NEW exit→NDB, before the page is written: nothing (never the last mode\'s page)', nu[1], { page: null, list: null });
  eq('NEW exit→NDB: the beacon map', nu[2], { page: 'page:ndb', list: 'BEACONS' });
  eq('NEW exit→AIR & SEA again: its map', nu[3], { page: 'page:airsea', list: 'AIRCRAFT' });
}

/* ── 2. the mode changed UNDER the open map (the shared dial) ──────────────────────────────────────────────────── */
function underTheMap(o: Overlay, from: K, to: K): Seen[] {
  const out: Seen[] = [];
  o.render(true, from); o.write(); out.push(o.seen());
  o.render(true, to); out.push(o.seen());                        // `kind` changes while visible (the _off + the first report)
  o.write(); out.push(o.seen());                                 // the new page written
  return out;
}
for (const [from, to] of [['airsea', 'ndb'], ['ndb', 'airsea']] as [K, K][]) {
  const old = underTheMap(new OldOverlay(), from, to);
  ok(`OLD ${from}→${to} under the open map: the page and the list disagree (the bug, reproduced)`, !consistent(old[2]));
  const nu = underTheMap(new NewOverlay(), from, to);
  eq(`NEW ${from}→${to} under the open map: the old page goes at once`, nu[1], { page: null, list: null });
  eq(`NEW ${from}→${to} under the open map: the new mode's map`, nu[2], { page: `page:${to}`, list: LIST[to] });
}

/* ── 3. a rewrite for the same kind in the same opening keeps the map (no reload under the user's finger) ─────── */
{
  const o = new NewOverlay();
  o.render(true, 'ndb'); o.write();
  const m = o.mounts, key = o.web?.key;
  o.write('page:ndb');   // the receiver's position arrived → html rebuilt → the file rewritten
  eq('same kind, same opening: the WebView is not remounted', [o.mounts, o.web?.key], [m, key]);
  // …but a close and reopen of the SAME kind is a new opening: a fresh WebView on the fresh file
  o.render(false, null); o.render(true, 'ndb'); eq('reopen, before the write: nothing shown', o.seen(), { page: null, list: null });
  o.write(); ok('reopen of the same kind: a new WebView', o.web?.key !== key && o.mounts === m + 1);
}

/* ── the pure rules on their own ─────────────────────────────────────────────────────────────────────────────── */
{
  let n = 0; const next = () => ++n;
  const a = mapPageWritten<K>(null, 'u', 'ais', 1, next);
  eq('first page: id 1', a, { uri: 'u', kind: 'ais', open: 1, id: 1 });
  ok('same kind + opening: the same object back', mapPageWritten(a, 'u', 'ais', 1, next) === a);
  eq('another kind: a new id', mapPageWritten(a, 'u', 'adsb', 1, next).id, 2);
  eq('another opening: a new id', mapPageWritten(a, 'u', 'ais', 2, next).id, 3);
  eq('shown: its own kind and opening', mapPageShown(a, true, 'ais', 1), a);
  eq('not shown: another kind', mapPageShown(a, true, 'adsb', 1), null);
  eq('not shown: another opening', mapPageShown(a, true, 'ais', 2), null);
  eq('not shown: hidden', mapPageShown(a, false, 'ais', 1), null);
  eq('not shown: no kind', mapPageShown(a, true, null, 1), null);
}

/* ── shownMapOf: the screen's map over the same transitions ──────────────────────────────────────────────────── */
{
  const base: ShownMapState = {
    appleTv: false, adsbView: false, ndbView: false, aisView: false, aprsMap: false, dscMap: false, navMap: false,
    ismGallery: false, ismMap: false, ismOn: false, vmMap: false, vmOn: false, sdMap: false, sdOn: false,
    avMap: false, avOn: false, avFocusDatalink: true, picked: null,
  };
  const S = (o: Partial<ShownMapState>) => shownMapOf({ ...base, ...o });
  eq('nothing open', S({}), null);
  eq('AIS: the AIR & SEA map (beta96)', S({ aisView: true }), 'airsea');
  eq('★ stage 6 ADS-B: the AIR & SEA map too', S({ adsbView: true }), 'airsea');
  eq('AIS → ADS-B, report first (both up a beat): the SAME page', S({ aisView: true, adsbView: true }), 'airsea');
  eq('AIS → ADS-B, ais_off first: nothing, then the AIR & SEA map', [S({}), S({ adsbView: true })], [null, 'airsea']);
  eq('ADS-B → AIS: adsb_off then the report', [S({}), S({ aisView: true })], [null, 'airsea']);
  eq('NDB under AIS: NDB', S({ ndbView: true, aisView: true }), 'ndb');
  eq('ISM map only while ISM is on', [S({ ismMap: true }), S({ ismMap: true, ismOn: true }), S({ ismGallery: true })], [null, 'ism', 'ism']);
  eq('VOR map only while on', [S({ vmMap: true }), S({ vmMap: true, vmOn: true })], [null, 'vormap']);
  eq('SONDES map only while on', [S({ sdMap: true }), S({ sdMap: true, sdOn: true })], [null, 'sondes']);
  eq('AVIATION: datalink focus needs the mode; another focus does not', [S({ avMap: true }), S({ avMap: true, avOn: true }), S({ avMap: true, avFocusDatalink: false })], [null, 'airsea', 'airsea']);
  eq('a picked server map under the modes', [S({ picked: 'hfdl' }), S({ picked: 'hfdl', aisView: true })], ['hfdl', 'airsea']);
  eq('NAVTEX / shipping MAP: the AIR & SEA map; DSC keeps its own', [S({ navMap: true }), S({ dscMap: true })], ['airsea', 'dsc']);
  eq('★ stage 6 AIS ↔ ADS-B: one kind, so the page stays (its chrome is pushed — section 4)', [S({ aisView: true }), S({ aisView: true, adsbView: true }), S({ adsbView: true })], ['airsea', 'airsea', 'airsea']);
  eq('NDB and ADS-B both up a beat: ADS-B\'s map wins (the precedence as before), then NDB\'s own kind', [S({ ndbView: true, adsbView: true }), S({ ndbView: true })], ['airsea', 'ndb']);
  eq('Apple TV: only the picked map (no WebView for the modes)', [S({ appleTv: true, adsbView: true }), S({ appleTv: true, picked: 'digi', aisView: true })], [null, 'digi']);
}

/* ── 4. ★★★ stage 6: AIS ↔ ADS-B on the ONE page — the title and EXIT key follow the mode, never a stale page ──────── */
{
  type M = 'ais' | 'adsb';
  /** The AIR & SEA overlay as built: ONE kind ('airsea'), the page keyed by mapPage.ts, and the chrome pushed from the
   *  CURRENT mode (SDRScreen airSeaTrk → liveChrome → MapOverlay's __trkChrome effect, deps [title, exitLabel, pageUri]). */
  class AirSea {
    page: MapPage<'airsea'> | null = null; openSeq = 0; wasVisible = false; ids = 0; mounts = 0;
    web: { key: number; title: string; exit: string } | null = null;
    visible = false; mode: M | null = null;
    pushed = { title: '', exit: '' };
    render(visible: boolean, mode: M | null) {
      if (visible && !this.wasVisible) this.openSeq++;
      this.wasVisible = visible; this.visible = visible; this.mode = mode;
      if (!visible) this.page = null;
      const kind = visible && mode ? 'airsea' as const : null;
      const shown = mapPageShown(this.page, visible, kind, this.openSeq);
      if (!shown) { this.web = null; return; }
      if (!this.web || this.web.key !== shown.id) { this.web = { key: shown.id, title: 'AVIATION & MARITIME', exit: 'CLOSE MAP' }; this.mounts++; this.pushed = { title: '', exit: '' }; }
      // ★ the chrome effect: pushed when it differs from what the page last got (and on every new page)
      const c = liveChrome('airsea', mode);
      if (this.pushed.title !== c.title || this.pushed.exit !== c.exitLabel) { this.web.title = c.title; this.web.exit = c.exitLabel; this.pushed = { title: c.title, exit: c.exitLabel }; }
    }
    write() {
      if (!this.visible || !this.mode) return;
      this.page = mapPageWritten(this.page, 'file:///docs/vibemap.html', 'airsea', this.openSeq, () => ++this.ids);
      this.render(this.visible, this.mode);
    }
    seen() { return this.web ? { title: this.web.title, exit: this.web.exit } : null; }
  }
  const AIS = { title: 'AVIATION & MARITIME · AIS 161.975 + 162.025 MHz', exit: 'EXIT AIS' };
  const ADSB = { title: 'AVIATION & MARITIME · ADS-B 1090 MHz', exit: 'EXIT ADS-B' };
  // ── under the open map (the shared dial): AIS → ADS-B → AIS ──
  {
    const o = new AirSea();
    o.render(true, 'ais'); o.write(); eq('stage 6 AIS: its title and EXIT AIS', o.seen(), AIS);
    const m = o.mounts, key = o.web?.key;
    o.render(true, 'adsb'); eq('stage 6 AIS → ADS-B under the open map: re-titled at once, EXIT ADS-B', o.seen(), ADSB);
    ok('stage 6 AIS → ADS-B: the SAME page (no reload, no blank)', o.mounts === m && o.web?.key === key);
    o.render(true, 'ais'); eq('stage 6 ADS-B → AIS under the open map: back to AIS\'s chrome', o.seen(), AIS);
    ok('stage 6 ADS-B → AIS: still the same page', o.mounts === m && o.web?.key === key);
  }
  // ── Mac Ha's path: AIS → EXIT → ADS-B, and back ──
  {
    const o = new AirSea();
    o.render(true, 'ais'); o.write();
    const key = o.web?.key;
    o.render(false, null);
    o.render(true, 'adsb'); eq('stage 6 exit → ADS-B, before the page is written: nothing (never AIS\'s page)', o.seen(), null);
    o.write(); eq('stage 6 exit → ADS-B: ADS-B\'s chrome on a fresh page', o.seen(), ADSB);
    ok('stage 6 exit → ADS-B: a new opening = a new WebView', o.web?.key !== key);
    o.render(false, null); o.render(true, 'ais'); o.write(); eq('stage 6 exit → AIS again: AIS\'s chrome', o.seen(), AIS);
  }
  // ── the only mode: CLOSE MAP, never EXIT ──
  eq('stage 6 ADS-B the only mode: CLOSE MAP', liveChrome('airsea', 'adsb', true).exitLabel, 'CLOSE MAP');
}

/* ── the wiring ──────────────────────────────────────────────────────────────────────────────────────────────── */
{
  const mo = readFileSync(new URL('../src/components/MapOverlay.tsx', import.meta.url), 'utf8');
  const sdr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  const web = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
  const pack = readFileSync(new URL('../src/services/mapPack.ts', import.meta.url), 'utf8');
  ok('mapPack: still ONE file for every map (why the uri alone cannot tell pages apart)', /new File\(Paths\.document, 'vibemap\.html'\)/.test(pack));
  ok('MapOverlay: no bare pageUri state any more', !/useState<string \| null>\(null\);\s*\n\s*const \[packError/.test(mo) && !/setPageUri\(/.test(mo));
  ok('MapOverlay: the page shown comes from mapPageShown', /const shownPage = mapPageShown\(page, visible, kind, openSeq\.current\)/.test(mo));
  ok('MapOverlay: a page written goes through mapPageWritten', /setPage\(\(p\) => mapPageWritten\(p, uri, kind, open,/.test(mo));
  ok('MapOverlay: the page is dropped when the overlay closes', /useEffect\(\(\) => \{ if \(!visible\) setPage\(null\); \}, \[visible\]\)/.test(mo));
  ok('MapOverlay: the WebView is keyed by the page id', /<WebView[\s\S]{0,200}key=\{shownPage\?\.id \?\? 0\}/.test(mo));
  ok('★ stage 6 MapOverlay: kind \'adsb\' is retired (no page, no list of its own)', !/'adsb' \| 'ais'/.test(mo) && !/kind === 'adsb'/.test(mo) && !/KIND==='adsb'/.test(mo) && !/<AdsbLiveList/.test(mo));
  ok('★ stage 6 MapOverlay: the AIR & SEA page is re-titled from the current mode on every change and every new page',
     /pageCall\('__trkChrome', trk\.title, trk\.exitLabel \?\? 'CLOSE MAP'\); \}, \[visible, kind, trk\?\.title, trk\?\.exitLabel, pageUri\]\)/.test(mo));
  ok('★ stage 6 MapOverlay: the ADS-B list body heads the AIRCRAFT tab while ADS-B is live', /adsb=\{adsbLive && trk\.tab === 'aircraft' \?/.test(mo));
  ok('★ stage 6 SDRScreen: ADS-B live is the AIR & SEA map\'s mode, EXIT ADS-B leaves it', /const airSeaMode: LiveMode = adsbLiveHere \? 'adsb'/.test(sdr) && /exitLabel === 'EXIT ADS-B'\) \{ exitAdsb\(\); return; \}/.test(sdr));
  ok('SDRScreen: the map shown comes from shownMapOf', /const shownMap: MapKind \| null = shownMapOf\(\{/.test(sdr));
  for (const [k, fn] of [['adsb', 'adsbOnRef'], ['ais', 'aisOnRef'], ['ndb', 'ndbOnRef'], ['sondes', 'sdOnRef'], ['vormap', 'vmOnRef']] as const) {
    ok(`SDRScreen: a ${k} report arriving puts the other map modes away`, new RegExp(`if \\(!${fn}\\.current\\) \\{\\s*wholeMapYield\\('${k}'\\)`).test(sdr));
  }
  for (const [k, on] of [['adsb', 'adsbUiOn'], ['ais', 'aisUiOn'], ['ndb', 'ndbUiOn'], ['sondes', 'sdUiOn'], ['vormap', 'vmUiOn']] as const) {
    ok(`web: ${on} puts the other map modes away first`, new RegExp(`function ${on}\\(\\) \\{\\n  wholeMapYield\\('${k}'\\)`).test(web));
  }
  ok('web: wholeMapYield covers the five', /function wholeMapYield[\s\S]{0,400}adsbUiOff\(\)[\s\S]{0,100}aisUiOff\(\)[\s\S]{0,100}ndbUiOff\(\)[\s\S]{0,100}sdUiOff\(\)[\s\S]{0,100}vmUiOff\(\)/.test(web));
}

console.log(`test_map_page_kind: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
