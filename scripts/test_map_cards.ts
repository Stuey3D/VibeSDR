// test_map_cards.ts — ★★★ beta96 THE MAP CONTROL CARDS' RULES (src/utils/mapCards.ts; docs/v12/MAP-CONTROL-CARDS.md): the
// window decides how many cards (one on a phone, never the whole width), the oldest gives way, LIST alone in its column,
// the defaults, FIT's outlier rule and its default layer.   npx tsx scripts/test_map_cards.ts
import { mapBarLine } from '../src/services/dialChat';
import { acTagsFor, acTagHtml } from '../src/utils/acTags';
import { CARD_IDS, cardDefaults, cardLimit, cardPlace, cardSide, cardToggle, fitDefault, oneSide, rateWords, trimOutliers, MAP_HELP, MAP_HELP_NOTES, type CardId } from '../src/utils/mapCards';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };

// ── how many: the window, not the device ──
ok(cardLimit(390, 844) === 1, 'a 390 px phone: one card');
ok(cardLimit(599, 1200) === 1, '599 px (still phone-width): one card');
ok(cardLimit(1440, 900) === 6 && cardLimit(1440, 600) === 4, `wide: every chip's card when tall, fewer when short (${cardLimit(1440, 900)} / ${cardLimit(1440, 600)})`);
// ── opening: the oldest gives way; a short window keeps one card per side ──
let open: CardId[] = [];
for (const id of CARD_IDS) open = cardToggle(open, id, true, 390, 844);
ok(open.length === 1 && open[0] === CARD_IDS[CARD_IDS.length - 1], `phone: every chip opened in turn leaves the last (${open})`);
open = [];
for (const id of ['servers', 'tune', 'layers'] as CardId[]) open = cardToggle(open, id, true, 1440, 600);
ok(open.join() === 'layers', `1440 × 600: the left edge holds one card (${open})`);
open = cardToggle(open, 'map', true, 1440, 600); open = cardToggle(open, 'list', true, 1440, 600);
ok(open.join() === 'layers,map,list', `…one on the right and the LIST beside it (${open})`);
open = cardToggle(open, 'map', false, 1440, 600);
ok(open.join() === 'layers,list', `shut one: ${open}`);
// ── where they open: the sides ──
const pl = cardPlace(['servers', 'tune', 'layers', 'map', 'list', 'help'], 1440, 900, false);
ok(pl.L.join() === 'servers,tune,layers' && pl.R.join() === 'map' && pl.B.join() === 'list' && pl.H.join() === 'help', `flanking: ${JSON.stringify(pl)}`);
const pl1 = cardPlace(['layers', 'map'], 1440, 900, true);
ok(pl1.L.join() === 'layers,map' && !pl1.R.length, `one side: MAP CONTROLS joins the left (${JSON.stringify(pl1)})`);
ok(cardSide('map', false) === 'R' && cardSide('map', true) === 'L' && cardSide('list') === 'B' && cardSide('servers') === 'L', 'the sides');
// ── one side or flanking: the MAP's height against the measured stack (Stuart's phones, 11-27) ──
const STACK = 5 * 56 + 16;   // ≈ SERVERS, TUNE, LAYERS, MAP, LIST and their gaps on a wide window
ok(!oneSide(550 - 110, STACK), 'iPhone SE portrait in Safari (≈375×550, a map ≈440): flanks');
ok(oneSide(830 - 110, STACK), 'iPhone 17 Pro Max portrait (≈440×830, a map ≈720): all on the left');
ok(!oneSide(400 - 60, STACK), 'a phone in landscape (≈956×400): flanks');
ok(oneSide(900 - 42, STACK), '1440 × 900: all on the left');
ok(!oneSide(700, 600), 'a stack too tall for the map: flanks, whatever the window');
// ── the help's words: one line per chip, in one place ──
ok(MAP_HELP.map((h) => h.id).join() === 'servers,tune,layers,map,list,stats,rate,chat' && MAP_HELP.every((h) => h.words.length > 10 && h.words.length < 110) && MAP_HELP_NOTES.length >= 2, 'MAP_HELP: a short line per chip, and the notes');
// ── defaults ──
ok(cardDefaults(390).length === 0, 'a phone opens with nothing (the map is the point)');
ok(cardDefaults(1024).join() === 'list', 'a wide window opens with LIST');
// ── FIT: outliers ──
const rx = { lat: 52.3125, lon: -0.875 };
const uk = Array.from({ length: 40 }, (_, i) => ({ lat: 51 + (i % 8) * 0.3, lon: -2 + Math.floor(i / 8) * 0.5, id: i }));
const tallinn = { lat: 59.437, lon: 24.7536, id: 99 };
const t = trimOutliers([...uk, tallinn], rx);
ok(!t.includes(tallinn) && t.length === 41 - 3, `Tallinn (949 NM) among 41 UK aircraft is left out — the furthest 5 % (${41 - t.length} dropped)`);
ok(trimOutliers(uk.slice(0, 7), rx).length === 7, 'fewer than 8: nothing is an outlier');
ok(trimOutliers([...uk.slice(0, 7), tallinn], rx).length === 7, '8 points: the furthest one goes');
const noRx = trimOutliers([...uk, tallinn], null);
ok(!noRx.includes(tallinn), 'with no receiver, measured from the median point: Tallinn still goes');
// ── FIT: the default layer ──
ok(fitDefault({ adsb: 140, datalink: 5, ships: 3 }, 'datalink') === 'adsb', 'ADS-B where there is ADS-B (even in the DATALINK mode)');
ok(fitDefault({ datalink: 5, ships: 3 }, 'datalink') === 'datalink', 'no ADS-B: the live mode\'s own layer');
ok(fitDefault({ ships: 3, navst: 25, warnings: 19 }, '') === 'ships', 'no ADS-B, nothing live: the ships, never the continent of NAVTEX stations');
ok(fitDefault({ navst: 25, warnings: 19 }, '') === '', 'only stations and warnings: no fit — the map keeps its view (Stuart 12:03)');
ok(fitDefault({ other: 9 }, '') === '', 'only unnamed items: none');
// ── the rate's words ──
ok(rateWords(1425.4) === '1,425' && rateWords(98.6) === '99' && rateWords(4.25) === '4.3', `the rate: ${rateWords(1425.4)} · ${rateWords(98.6)} · ${rateWords(4.25)}`);
// ── tweak 2: the bar's shared line, in Stuart's words ──
const L = (o: any) => mapBarLine({ shared: true, listeners: 2, max: 5, exitMoves: true, ...o });
ok(L({}).text === 'Shared VFO: Ask before changing mode or exiting 2/5', `others listening: "${L({}).text}"`);
ok(L({ listeners: 1 }).text === 'Shared VFO: Free to change mode or exit 1/5', `alone: "${L({ listeners: 1 }).text}"`);
ok(L({ exitMoves: false }).text === 'Shared VFO: Ask before changing mode 2/5', `opened from a key: "${L({ exitMoves: false }).text}"`);
ok(L({ ask: { askFirst: true, tuneLocked: true, askState: 'idle' } }).text === 'Shared VFO: Locked: ask in chat to change mode or exit 2/5' && L({ ask: { askFirst: true, tuneLocked: true, askState: 'idle' } }).dialLocked, `ask-locked: "${L({ ask: { askFirst: true, tuneLocked: true, askState: 'idle' } }).text}"`);
ok(L({ shared: false }).text === '2/5' && !L({ shared: false }).dialLocked, `own VFO each (not shared): just the count "${L({ shared: false }).text}"`);
ok(!L({ standalone: true }).show && !L({ max: 1 }).show, 'the viewer and a one-listener radio: nothing');
ok(L({ said: 'Tuning now' }).text === 'Tuning now', 'a tuning line in the chat is shown as it is');
// ── the aircraft tags (acTags.ts): one source for both lists and the app ──
ok(acTagsFor({ mil: true }).join() === 'mil' && acTagsFor({ squawk: '7700' }).join() === 'emerg' && acTagsFor({ squawk: 7600 }).join() === 'radio' && acTagsFor({ squawk: '7500', mil: true }).join() === 'hijack,mil', 'MIL, and 7700 EMERG · 7600 RADIO · 7500 HIJACK first');
ok(acTagsFor({ emergency: 'general' }).join() === 'emerg' && acTagsFor({ emergency: 'none', squawk: '1200' }).length === 0, 'the ADS-B emergency status counts too; none and 1200 wear nothing');
ok(/class="tag mil"[^>]*>MIL</.test(acTagHtml('mil')) && /class="tag em"[^>]*>EMERG</.test(acTagHtml('emerg')), 'the box is the ADS-B list\'s own .tag (mil / em)');
console.log(`test_map_cards: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
