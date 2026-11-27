// test_map_shared_app.ts — beta96 (Stuart 11-26) the app's half: the SHARED TUNER line, CHAT, the TUNE chip and the dial
// lock on the full-screen maps (MapTopStrip in MapOverlay, fed by SDRScreen), AIR & SEA LIVE in the Mode sheet and VIEWS'
// AIR & SEA beside GALLERY. Source checks (the words and the rows are test_air_sea_tune.ts; the pad order test_nav_coverage).
//   npx tsx scripts/test_map_shared_app.ts
import { readFileSync } from 'node:fs';
let pass = 0, fail = 0;
const ok = (c: unknown, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const rd = (f: string) => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8');
const mo = rd('src/components/MapOverlay.tsx'), st = rd('src/components/MapTopStrip.tsx'), sc = rd('src/screens/SDRScreen.tsx'), ms = rd('src/components/ModeSelector.tsx');
const bands = rd('src/utils/decoderBands.ts');

console.log('── MapOverlay ──');
// ★ beta96: Android back is onBackPress (mapExit.mapBackAction — test_map_exit), whose ordinary case is closeGuarded
ok(/if \(d === 'close'\) return closeGuarded\(\);/.test(mo) && /onRequestClose=\{onBackPress\}/.test(mo) && /default: closeGuarded\(\);/.test(mo), 'EXIT (the page key) and Android back both go through closeGuarded');
ok(/const exitLocked = !!dialLock\?\.why && dialLock\.exitMoves;/.test(mo) && /dialLock\.onDialLocked\(dialLock\.why\); return;/.test(mo), 'a locked EXIT that would leave the mode is refused with the reason');
ok(/window\.__backLock=function\(on,why\)/.test(mo) && /__backLock&&window\.__backLock\(\$\{exitLocked\}/.test(mo), 'the page greys its EXIT key while locked (and on every load)');
ok(/<MapTopStrip top=\{mapTop\}/.test(mo) && /t === 'mapTop'/.test(mo), 'the strip sits under the page\'s own top bar (mapTop)');
ok(/nav=\{stripOn \? \{ navCtx: trkNav\.navCtx, scrollProps: trkNav\.scrollProps \} : undefined\}/.test(mo) && /navCtx=\{trk \? trkNav\.navCtx : undefined\}/.test(mo),
   'ONE pad order for the strip and the live list (never two owners)');

console.log('── MapTopStrip ──');
ok(/<ScrollView horizontal/.test(st), 'one sideways-scrolling row (a phone fits the line, CHAT and TUNE)');
ok((st.match(/useNavButton\(onPress\)/g) || []).length >= 2 && /<NavRing/.test(st), 'every key and every TUNE row is a pad key');
ok(/if \(locked\) p\.onLocked\(p\.lockWhy\); else setOpen\(!open\)/.test(st), 'the TUNE chip is refused with the reason while locked');
ok(/if \(r\.why\) \{ tune\.onRefused\(r\.why\); return; \}/.test(st) && /if \(!r\.live\) tune\.onPick\(r\.id\)/.test(st), 'a row the server greys says why; a pick tunes (not the live one)');
ok(/TUNE_ICON_PATH\[r\.icon\]/.test(st) && /TUNE_GROUP_LABEL\[r\.group\]/.test(st), 'rows: icon, grouped');
ok(!/toggle|setToggles|onToggle/.test(st), 'nothing of the map\'s own view (chips, tabs) is in the strip — never locked');

console.log('── SDRScreen ──');
ok(/const mapLine = useMemo\(\(\) => mapSharedLine\(\{\n    shared: !!sharedDial, standalone: shownMap === 'airsea' && asViewer,/.test(sc), 'the line from the bar\'s own inputs (sharedDial, dialState, the ask lock, scan, said); not in the viewer');
ok(/shared=\{mapLine\.show \? \{ line: mapLine, chatDisabled: isKiwi \|\| \(isVibeServer && !sharedDial\), chatUnread: chatUnread \|\| askNudge, onChat: openChat \} : null\}/.test(sc),
   'CHAT is the bar\'s chat: the same opener, greyed and pulsing as the bar\'s');
ok(/\{chatOpen && mapLine\.show \? chatDrawerEl : null\}/.test(sc) && /\{!\(shownMap !== null && mapLine\.show\) && chatDrawerEl\}/.test(sc), 'the chat drawer opens OVER the map (inside its Modal), once');
ok(/dialLock=\{mapLockWhy \? \{ why: mapLockWhy, exitMoves: mapExitMoves, onDialLocked: \(\) => \{ nudgeAsk\(\); \} \} : null\}/.test(sc), 'the dial lock: the reason, whether EXIT moves the radio, the ask nudge');
ok(/airSeaTuneRows\(\{/.test(sc) && /only: onlyModeHere \|\| ''/.test(sc), 'TUNE rows from THIS radio\'s caps (and the only mode)');
ok(/const airSeaTune = useCallback\(\(id: string\) => \{\n    markInteract\(\);\n    if \(askGate\(\)\) return;/.test(sc), 'a TUNE pick asks first (ask before tuning)');

console.log('── the Mode sheet ──');
ok(/WHOLE_RADIO_IDS: readonly string\[\] = \['airsea',/.test(bands) && /case 'airsea': return \{ id, node: airSeaShown \? airSeaKey\(\) : null \};/.test(ms), 'AIR & SEA LIVE first in TUNES THE RADIO ITSELF');
for (const k of ['adsbAvail', 'aisAvail', 'marAvail', 'avdataAvail', 'sondesAvail', 'vormapAvail', 'ndbAvail'])
  ok(new RegExp(`${k}: !asMenuOne && `).test(sc), `${k} not passed where AIR & SEA LIVE stands in`);
ok(/\{ id: 'airseaview', node: [\s\S]{0,400}\n    \{ id: 'gallery'/.test(ms), 'VIEWS\' AIR & SEA right beside GALLERY');
ok(/onAirSeaView: \(\) => \{ markInteract\(\); setModeSelOpen\(false\); setAsViewer\(true\);/.test(sc), 'the viewer opens the map with no TUNE and no shared line');
ok(/avMapAvail: !asMenuOne && /.test(sc) && /shipMapAvail: !asMenuOne && /.test(sc), 'AVIATION and SHIPPING merged into it');

console.log(`test_map_shared_app: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
