// test_map_shared_app.ts — beta96 (Stuart 11-26) the app's half: the SHARED TUNER line, CHAT, the TUNE chip and the dial
// lock on the full-screen maps — ★★★ beta97 drawn by the MAP CONTROL CARDS (MapChrome in MapOverlay, fed by SDRScreen; the old
// MapTopStrip DELETED) — AIR & SEA LIVE in the Mode sheet and VIEWS' AIR & SEA beside GALLERY. Source checks (the words and
// the rows are test_air_sea_tune.ts; the layout test_map_chrome.ts; the pad order test_nav_coverage).
//   npx tsx scripts/test_map_shared_app.ts
import { existsSync, readFileSync } from 'node:fs';
let pass = 0, fail = 0;
const ok = (c: unknown, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const rd = (f: string) => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8');
const mo = rd('src/components/MapOverlay.tsx'), st = rd('src/components/MapChrome.tsx'), sc = rd('src/screens/SDRScreen.tsx'), ms = rd('src/components/ModeSelector.tsx');
const bands = rd('src/utils/decoderBands.ts');

console.log('── MapOverlay ──');
ok(/if \(d === 'close'\) return closeGuarded\(\);/.test(mo) && /onRequestClose=\{closeGuarded\}/.test(mo), 'Android back goes through closeGuarded (and the old page key\'s message still would)');
ok(/const exitLocked = !!dialLock\?\.why && dialLock\.exitMoves;/.test(mo) && /dialLock\.onDialLocked\(dialLock\.why\); return;/.test(mo), 'a locked EXIT that would leave the mode is refused with the reason');
ok(/exitLocked=\{exitLocked\} onExit=\{closeGuarded\}/.test(mo) && /exit=\{\{ label: exitLabel, words: exitWords, locked: p\.exitLocked, onPress: p\.onExit \}\}/.test(mo), 'the cards\' ✕ is closeGuarded, greyed while locked');
ok(!existsSync(new URL('../src/components/MapTopStrip.tsx', import.meta.url)) && !/MapTopStrip|__backLock|t === 'mapTop'/.test(mo), '★★★ the old strip, its bar height and the page key\'s lock are DELETED, not hidden');
ok(/isTrk \|\| CARDS \? '' : `<div id="topbar">/.test(mo), '★★★ the track maps\' (and the AMATEUR map\'s) page has NO top bar of its own');
ok(/<NavCtx\.Provider value=\{nav\.navCtx\}>/.test(st) && (st.match(/usePanelNav\(/g) || []).length === 1, 'ONE pad order for every chip, card and LIST row (never two owners)');

console.log('── MapChrome ──');
ok((st.match(/useNavButton\(onPress\)/g) || []).length >= 2 && /<NavRing/.test(st), 'every key, chip and TUNE row is a pad key');
ok(/onPress=\{\(\) => \(tuneLocked \? refuse\(\) : toggle\(id\)\)\}/.test(st) && /const refuse = \(\) => \{ setPulseAt\(Date\.now\(\)\); p\.onLocked\(p\.lockWhy\); \};/.test(st), 'the TUNE chip is refused with the reason while locked — and the pill + CHAT pulse');
ok(/if \(r\.why\) \{ t\.onRefused\(r\.why\); return; \}/.test(st) && /if \(!r\.live\) t\.onPick\(r\.id\)/.test(st), 'a row the server greys says why; a pick tunes (not the live one)');
ok(/TUNE_ICON_PATH\[r\.icon\]/.test(st) && /TUNE_GROUP_LABEL\[r\.group\]/.test(st), 'rows: icon, grouped');
ok(/<Card title="TUNE" at=\{at\} onClose=\{close\} faded=\{locked\}>/.test(st), 'TUNE\'s card fades under the ask lock');
ok(/onKey|onToggle/.test(st) && !/locked[^\n]*onToggle/.test(st), 'the map\'s own view (LAYERS, KEY) is never locked');

console.log('── SDRScreen ──');
ok(/const mapLine = useMemo\(\(\) => mapSharedLine\(\{\n    shared: !!sharedDial, standalone: shownMap === 'airsea' && asViewer,/.test(sc), 'the line from the bar\'s own inputs (sharedDial, dialState, the ask lock, scan, said); not in the viewer');
ok(/shared=\{mapLine\.show \? \{ line: mapLine, chatDisabled: isKiwi \|\| \(isVibeServer && !sharedDial\), chatUnread: chatUnread \|\| askNudge, onChat: openChat \} : null\}/.test(sc),
   'CHAT is the bar\'s chat: the same opener, greyed and pulsing as the bar\'s');
ok(/\{chatOpen && mapLine\.show \? chatDrawerEl : null\}/.test(sc) && /\{!\(shownMap !== null && mapLine\.show\) && chatDrawerEl\}/.test(sc), 'the chat drawer opens OVER the map (inside its Modal), once');
ok(/dialLock=\{mapLockWhy \? \{ why: mapLockWhy, exitMoves: mapExitMoves, onDialLocked: \(\) => \{ nudgeAsk\(\); \} \} : null\}/.test(sc), 'the dial lock: the reason, whether EXIT moves the radio, the ask nudge');
ok(/airSeaTuneRows\(\{/.test(sc) && /only: onlyModeHere \|\| ''/.test(sc), 'TUNE rows from THIS radio\'s caps (and the only mode)');
ok(/const airSeaTune = useCallback\(\(id: string\) => \{\n    markInteract\(\);\n    if \(askGate\(\)\) return;/.test(sc), 'a TUNE pick asks first (ask before tuning)');

console.log('── ★★★ beta97 SERVERS, SOURCES, LIVE elsewhere ──');
ok(/doorVisit\.stash\(\{ base: baseUrl, radioId: chosenFromListRef\.current \?\? id, pins: radioPins\.current, adminAuthQ: adminAuthQRef\.current, pick: id \}\);\s*navigation\.replace\('SDR', doorReturnParams/.test(sc),
   'SERVERS → another radio: the door\'s remount, the radio carried in the visit (never in route params)');
ok(/const id = visit\?\.pick;\s*if \(!id \|\| !door \|\| radioBase\) return;\s*if \(visit\) visit\.pick = undefined;\s*if \(door\.radios\.some\(\(x\) => x\.id === id\)\) void chooseRadio\(id\);/.test(sc),
   'the new screen chooses it once the door answers, once only');
ok(/onPick: \(id: string\) => \{ if \(id\.startsWith\('r:'\)\) onSwitchRadio\(id\.slice\(2\)\); else onBackToPicker\(\); \}/.test(sc) && /onOpen: loadRadioLive/.test(sc) && /Date\.now\(\) - radioLiveAt\.current < 10_000/.test(sc),
   'LEAVE / a PIN radio go back to the list; listeners asked only while the card is open, floored at 10 s');
ok(/hidden: asViewer \? \[\] : asSrcHidden/.test(sc) && /sources: asViewer \? undefined : sourceRows\(/.test(sc) && /onSource: asSrcSet/.test(sc), 'SOURCES: rows from the table, hidden through the scope (never on the viewer)');
ok(/live: c\.live, others: asOthersElsewhere\[c\.id\]/.test(sc), 'LAYERS\' outlined LIVE from layerOthers (the same table)');

console.log('── the Mode sheet ──');
ok(/WHOLE_RADIO_IDS: readonly string\[\] = \['airsea',/.test(bands) && /case 'airsea': return \{ id, node: airSeaShown \? airSeaKey\(\) : null \};/.test(ms), 'AIR & SEA LIVE first in TUNES THE RADIO ITSELF');
for (const k of ['adsbAvail', 'aisAvail', 'marAvail', 'avdataAvail', 'sondesAvail', 'vormapAvail', 'ndbAvail'])
  ok(new RegExp(`${k}: !asMenuOne && `).test(sc), `${k} not passed where AIR & SEA LIVE stands in`);
ok(/\{ id: 'airseaview', node: [\s\S]{0,400}\n    \{ id: 'gallery'/.test(ms), 'VIEWS\' AIR & SEA right beside GALLERY');
ok(/onAirSeaView: \(\) => \{ markInteract\(\); setModeSelOpen\(false\); setAsViewer\(true\);/.test(sc), 'the viewer opens the map with no TUNE and no shared line');
ok(/avMapAvail: !asMenuOne && /.test(sc) && /shipMapAvail: !asMenuOne && /.test(sc), 'AVIATION and SHIPPING merged into it');

console.log(`test_map_shared_app: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
