// test_map_scope_app.ts — beta96 (Stuart 11-26 via the coordinator) the app's half of THE MAPS' SCOPE: OTHER RADIOS ON THIS
// SERVER and THIS SESSION on the AIR & SEA map, the ISM mesh map and the AMATEUR map; the SHOW counts + colour dots on the
// chips (★★★ beta97: the MAP CARDS — TrackLiveList / DigiMapKeys DELETED); AIR & SEA LIVE pressed while lit reopens the map. Source checks — the rules themselves
// are test_map_scope.ts (pure), the pad order test_nav_coverage.
//   npx tsx scripts/test_map_scope_app.ts
import { readFileSync } from 'node:fs';
let pass = 0, fail = 0;
const ok = (c: unknown, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const rd = (f: string) => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8');
const sc = rd('src/screens/SDRScreen.tsx'), mo = rd('src/components/MapOverlay.tsx');   // ★★★ beta97 TrackLiveList / DigiMapKeys DELETED (the map cards)
const ty = rd('src/services/sdrTypes.ts'), lm = rd('src/services/liveMaps.ts');

console.log('── the server\'s answer ──');
ok(/radioId:\s+typeof j\.radioId === 'string' && \/\^\[0-9a-z\]\{1,16\}\$\/\.test\(j\.radioId\)/.test(ty) && /radioCount: typeof j\.radioCount === 'number'/.test(ty),
   'vibeserver.json radioId (a whole id or nothing) and radioCount are parsed');
ok(/setMapRadio\(\{ mine: o\?\.radioId \?\? '', count: o\?\.radioCount \?\? 0 \}\)/.test(sc) && /setMapRadio\(\{ mine: '', count: 0 \}\)/.test(sc), 'SDRScreen takes them per radio (reset on a new connection)');

console.log('── AIR & SEA: the data through the scope first ──');
ok(/const asSrcS = useMemo\(\(\) => airSeaScope\(\{ shared: avShared,/.test(sc), 'the sources go through airSeaScope before anything is composed');
const region = sc.slice(sc.indexOf('const aisTrk = useMemo'), sc.indexOf('/** ★ CLOSE MAP puts every opener'));
ok(region.length > 1000 && !/(?<![A-Za-z0-9_.$])(avShared|shipStore|navReport|navWarnRep|navMetRep|dscGal|aisReport|avReport|sdReport|vmReport|sdStored|vmStored|ndbReport)(?![A-Za-z0-9_$S])/.test(
  region.replace(/setNav[A-Za-z]+|setShipStore|setDscGal|setAvShared|setSdStored|setVmStored/g, '').replace(/\/\/[^\n]*/g, '').replace(/\/\*[\s\S]*?\*\//g, '')),
   'the map\'s compositions read only the scoped sources (*S) — never a raw store');
ok(/useState\(true\);\n  useEffect\(\(\) => \{ AsyncStorage\.getItem\(OTHER_RADIOS_KEY_APP\)\.then\(\(v\) => setMapOthers\(otherRadiosOf\(v\)\)\)/.test(sc), 'OTHER RADIOS: ON by default, remembered in AsyncStorage');
ok(/on \? AsyncStorage\.removeItem\(OTHER_RADIOS_KEY_APP\) : AsyncStorage\.setItem\(OTHER_RADIOS_KEY_APP, '0'\)/.test(sc), '…written per client, never sent to the server');
ok(/otherRadiosOffered\(\{ mine: mapRadio\.mine, radioCount: mapRadio\.count, viewer: asViewer,/.test(sc), 'the switch: only with 2+ sources, never on the viewer');
ok(/const asSessOk = sessionOffered\(\{ viewer: asViewer, onRadio: true \}\)/.test(sc), 'THIS SESSION: never on the viewer');
ok(/n: c\.id === 'airports' \|\| c\.id === 'areas' \? undefined : showN\[c\.id\] \?\? undefined, colour: LIVE_CHIP_COLOUR\[c\.id\]/.test(sc), 'SHOW: every chip carries its count and colour (the web\'s)');
ok(/\.\.\.\(asOthersOffered \? \[\{ id: OTHER_RADIOS_CHIP, label: OTHER_RADIOS_LABEL, words: OTHER_RADIOS_WORDS \}\] : \[\]\)/.test(sc) && /if \(id === OTHER_RADIOS_CHIP\) \{ mapOthersSet\(v\); return; \}/.test(sc),
   'the OTHER RADIOS ON THIS SERVER chip after the layer chips, its own handler');
ok(/withSession\(navWin \? \(ship\.ranges \?\? \[\]\) : \[\], asSessOk\)/.test(sc) && /if \(id === SESSION_ID\) \{ setAsSess\(true\); return; \}/.test(sc), 'THIS SESSION beside the maritime window (ALL | THIS SESSION where none)');
ok(/const rng = asSess && asSessSt \? sessionWindow\(asSessSt\.t0, Math\.floor\(Date\.now\(\) \/ 1000\)\) : navRange;/.test(sc), 'the ship polls ask the window that covers the session');
ok(/setAsSessSt\(\(p\) => sessionStart\(p, airSeaMode, Math\.floor\(Date\.now\(\) \/ 1000\), !p\)\)/.test(sc) && /if \(!airSeaUp\) \{ setAsSess\(false\); setAsSessSt\(null\); asFirst\.current\.clear\(\); return; \}/.test(sc),
   'the session restarts on re-entry (the map up, a new live mode) and is forgotten when the map goes');
ok(/adsbAircraft=\{shownMap === 'airsea' && adsbLiveHere \? adsbReportS\?\.aircraft/.test(sc) && /\? adsbListS : undefined/.test(sc) && /report: ndbReportS,/.test(sc), 'ADS-B\'s layer and list and the NDB report follow the session');
ok(/'Only this radio\\u2019s own decodes' : '',   \/\/ ★★★ beta96 what the scope hides, first/.test(sc), 'the status line says what the scope hides');
ok(/asOthersOffered, mapOthers, mapOthersSet, asSc, asSess, asSessOk, dscCalls, aisOn\]\);/.test(sc), 'the map\'s memo follows the scope (its dependencies)');
ok(/export const LIVE_CHIP_COLOUR/.test(lm), 'one colour table for the web and the app (liveMaps.ts)');

console.log('── AIR & SEA LIVE pressed while lit ──');
const tog = sc.slice(sc.indexOf('const toggleAirSeaLive = useCallback'), sc.indexOf('const rows = airSeaRows.filter((r) => !r.why);'));
ok(/if \(adsbOnRef\.current\) \{ setAsViewer\(false\); setAdsbViewOpen\(true\); return; \}/.test(tog) && /setAvMapOpen\(true\); return; \}/.test(tog), 'lit: the map comes back (every air / sea mode)');
ok(!/exitAdsb\(\)|exitAis\(\)|exitSondes\(\)|exitVormap\(\)|exitNdb\(\)|avUiOff\(\)/.test(tog), 'lit: never leaves the mode (EXIT in the map does)');

console.log('── ★★★ beta97 LAYERS (MapChrome): counts and dots ──');
const mc = readFileSync(new URL('../src/components/MapChrome.tsx', import.meta.url), 'utf8');
ok(/export interface MapLayerRow \{ id: string; label: string; words: string; n\?: number; colour\?: string; live\?: boolean; others\?: string\[\]; group\?: string;/.test(mc) && /nOther\?: number \}/.test(mc), 'a row may carry n, colour, its LIVE state and the other sources\' count');
ok(/\{!!r\.colour && <View style=\{\[styles\.dot, \{ backgroundColor: r\.colour \}\]\} \/>\}/.test(mc) && /\{r\.n != null && <Text style=\{styles\.layerN\}>\{r\.n\}<\/Text>\}/.test(mc),
   'a colour dot, the count');
ok(/words=\{`\$\{r\.words\}\$\{sp\.split \? `, \$\{sp\.mine\} from this radio, \$\{sp\.other\} from/.test(mc) && /\$\{r\.n != null \? `, \$\{r\.n\}` : ''\}/.test(mc), 'a screen reader hears the count (both, where split)');
ok(/toggles\?: Array<\{ id: string; label: string; words: string; n\?: number; colour\?: string; live\?: boolean; others\?: string\[\]; group\?: string; nOther\?: number \}>;/.test(mo), 'MapOverlay passes them through (with LIVE / elsewhere / the others\' count)');

console.log('── the ISM mesh map ──');
const ism = sc.slice(sc.indexOf('const ismTrk = useMemo'), sc.indexOf('const ismMapUp ='));
ok(/scopeRadioOk\(n\.radios, isc\)/.test(ism) && /nowC - d\.age >= isc\.t0 - 2/.test(ism), 'remembered nodes by their card\'s radios; THIS SESSION by first heard');
ok(/ranges: withSession\(ISM_LIVE_RANGES, true\)/.test(ism) && /toggles: othersOk \? \[\{ id: OTHER_RADIOS_CHIP/.test(ism), 'THIS SESSION beside its window; the switch where 2+ sources');
ok(/const ri = radiosOf\(c\.ri\);   \/\/ ★★★ beta96 the card's radios on its nodes/.test(sc), 'the stored nodes carry their card\'s radios');

console.log('── the AMATEUR map ──');
// ★★★ beta97 the AMATEUR map is on the MAP CARDS (DigiMapKeys DELETED): THIS SESSION in TIME, the layers in LAYERS, OTHER RADIOS in SOURCES
ok(/ranges: \[\.\.\.DIGI_WINDOWS, \.\.\.\(amateur \? \[SESSION_RANGE\] : \[\]\)\], range: amSess \? SESSION_ID : digiWinShown/.test(mo), 'MAP CONTROLS: THIS SESSION beside the window keys');
ok(/const amLayers: MapLayerRow\[\] = amateur && amCounts/.test(mo) && /n: amCounts\[id\], colour: AM_LAYER_COLOUR\[id\], live: amateur\.live === id/.test(mo), 'LAYERS: SPOTS / APRS / MESH / BEACONS with counts, colour dots and LIVE (pad keys)');
ok(/sources: amScope\?\.offered \? \[\{ id: 'others', name: OTHER_RADIOS_LABEL, what: OTHER_RADIOS_WORDS, on: amScope\.others \}\] : undefined/.test(mo), 'SOURCES: the OTHER RADIOS key');
ok(/const amData = useMemo\(/.test(mo) && /const d = \{ spots: amData\.spots, aprs: amData\.aprs, mesh: amData\.mesh, beacons: amData\.beacons \};/.test(mo), 'the list, its tabs and the counts come through the scope');
ok(/window\.__amScope=function\(json\)/.test(mo) && /if\(!spotsOn\|\|!amScR\(s\.radios\)\|\|\(amSc\.t0&&!amSesOk\(sf\)\)\)return false;/.test(mo), 'the page draws through the same rule (spots, APRS, MESH, BEACONS)');
ok(!/getElementById\('f(Aprs|Mesh|Bcn)'\)/.test(mo), 'the page\'s own APRS / MESH / BEACONS keys DELETED (LAYERS is React Native\'s)');
ok(/amScope=\{shownMap === 'digi' \? \{ mine: mapRadio\.mine, others: mapOthers, onOthers: mapOthersSet,/.test(sc), 'SDRScreen hands the switch to the AMATEUR map (one remembered state)');
ok(/servers=\{null\} tune=\{null\} chat=\{null\} shared=\{null\}/.test(mo), 'no TUNE on the AMATEUR map: nothing there retunes the radio');

console.log(`test_map_scope_app: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
