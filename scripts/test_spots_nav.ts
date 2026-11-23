// ★★★ beta92 THE SPOTS LIST BY PAD / KEYBOARD / REMOTE (Stuart, 2026-11-23: "make the spots navigatable by controller
// but make it so that you dont have to scroll a list of hundreds to get to the header buttons or to the control deck").
// The rules (src/utils/spotsNav.ts) on a 500-spot list, then the wiring in DecoderPanel that makes them the box's keys.
// The web page's half is scripts/test-web-spots-nav.mjs.
import { readFileSync } from 'node:fs';
import { spotRowKey, skimWaitKey, spotsEnterList, spotsNavKey, spotsNavFollow, type SpotsNav } from '../src/utils/spotsNav';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
console.log('test_spots_nav');

// 500 spots, newest first (as the list draws them), plus 3 band-skimmer "waiting for callsign" rows above them.
const T0 = 1_790_000_000_000;
const mk = (n: number, from = 0) => Array.from({ length: n }, (_, i) => ({ time: T0 - (from + i) * 1000, call: `G${from + i}ABC`, freqHz: 14_074_000 + (from + i) }));
const spots = mk(500);
const wait = [{ mode: 'CW', id: 1, hz: 14_020_000 }, { mode: 'CW', id: 2, hz: 14_031_000 }, { mode: 'RTTY', id: 3, hz: 14_083_000 }];
const keys = [...wait.map(skimWaitKey), ...spots.map(spotRowKey)];
ok(keys.length === 503 && new Set(keys).size === 503, 'every row has its own key (503 rows)');

// Entering lands on the TOP row, the newest — never a remembered deep row.
const top = spotsEnterList(keys);
ok(top.zone === 'list' && top.idx === 0 && top.key === keys[0], 'entering the list: the top row');
let nav: SpotsNav = { zone: 'list', idx: 400, key: keys[400] };
const fromHeader = spotsNavKey({ zone: 'header', idx: 400, key: keys[400] }, 'ArrowDown', keys);
ok(fromHeader.nav?.zone === 'list' && fromHeader.nav.idx === 0, '↓ from the header: the TOP row, not row 400 where it was');

// BACK from row 400: the header in ONE press; BACK again: out of the box.
const b1 = spotsNavKey(nav, 'Back', keys);
ok(b1.nav?.zone === 'header', 'BACK from row 400 → the header keys in one press');
const b2 = spotsNavKey(b1.nav!, 'Back', keys);
ok(b2.nav === null, 'BACK again → the box lets go (the page and the control deck)');

// UP from row 0: the header, never wrapping to the bottom.
const u0 = spotsNavKey({ zone: 'list', idx: 0, key: keys[0] }, 'ArrowUp', keys);
ok(u0.nav?.zone === 'header', '↑ from the first row → the header');
const u1 = spotsNavKey({ zone: 'list', idx: 1, key: keys[1] }, 'ArrowUp', keys);
ok(u1.nav?.zone === 'list' && u1.nav.idx === 0, '↑ from row 1 → row 0');
const uh = spotsNavKey({ zone: 'header', idx: 0, key: null }, 'ArrowUp', keys);
ok(uh.nav?.zone === 'header', '↑ in the header goes nowhere (no wrap to the bottom row)');
const dEnd = spotsNavKey({ zone: 'list', idx: 502, key: keys[502] }, 'ArrowDown', keys);
ok(dEnd.nav?.zone === 'list' && dEnd.nav.idx === 502, '↓ on the last row stays there (no wrap to the top)');
const d = spotsNavKey(nav, 'ArrowDown', keys);
ok(d.nav?.idx === 401 && d.nav.key === keys[401], '↓ moves one row');

// ENTER tunes the highlighted row (the caller maps the index onto the row's own tap).
const e = spotsNavKey(nav, 'Enter', keys);
ok(e.tune === 400, 'ENTER on row 400 → tune row 400');
ok(spots[e.tune! - wait.length].freqHz === 14_074_000 + 397, '…which is spot 397 (the 3 skimmer rows come first)');
const ew = spotsNavKey({ zone: 'list', idx: 2, key: keys[2] }, 'Enter', keys);
ok(ew.tune === 2 && wait[ew.tune!].hz === 14_083_000, 'ENTER on a "waiting for callsign" row → its frequency');

// New spots arriving: a burst of 25 PREPENDS rows — the highlight stays on its spot (by key), not on row 400.
const burst = [...wait.map(skimWaitKey), ...mk(25, -25).map(spotRowKey), ...spots.map(spotRowKey)];
const f = spotsNavFollow(nav, burst);
ok(f.key === keys[400] && f.idx === 425 && burst[f.idx] === keys[400], 'a 25-spot burst: the highlight follows its spot (row 400 → 425)');
const eAfter = spotsNavKey(nav, 'Enter', burst);
ok(eAfter.tune === 425, 'ENTER after the burst still tunes the SAME spot');
ok(spotsNavFollow(nav, keys) === nav, 'an unchanged list leaves the highlight alone');
// The highlighted spot leaves the list (a filter / the age limit): pinned to the same place, clamped.
const gone = keys.filter((_, i) => i !== 400);
const g = spotsNavFollow(nav, gone);
ok(g.idx === 400 && g.key === gone[400], 'its spot gone: pinned to the same place (row 400, the next spot)');
const g2 = spotsNavFollow({ zone: 'list', idx: 502, key: 'gone' }, keys.slice(0, 10));
ok(g2.idx === 9 && g2.key === keys[9], '…clamped to the end of a shorter list');
// An emptied list: BACK / arrows still lead out.
ok(spotsNavKey(nav, 'ArrowDown', []).nav?.zone === 'header', 'an emptied list: the keys go to the header');

// ── The wiring: DecoderPanel uses these as the box's keys ──────────────────────────────────────────────────────────
const src = readFileSync(new URL('../src/components/DecoderPanel.tsx', import.meta.url), 'utf8');
ok(/keyExtractor=\{spotRowKey\}/.test(src), 'the FlatList keys rows with spotRowKey (the same key the highlight follows)');
ok(/isSpotsMode \? spotKeys\.length/.test(src), 'the spots list is the box\'s list (listLen)');
ok(/spotsNavKey\(\{ zone: from, idx: listIdxRef\.current, key: spotKeyRef\.current \}, k, spotKeysRef\.current\)/.test(src),
   'the key handler runs spotsNavKey on ↑ ↓ BACK ENTER');
{
  const at = src.indexOf('spotsNavKey({ zone: from');
  const back = src.indexOf("if (k === 'Back') {\n      // Deepest first");
  ok(at > 0 && back > at, 'the spots rules run BEFORE the generic BACK (which leaves the box from anywhere)');
}
ok(/if \(!st\.nav\) \{ leave\(\); return true; \}/.test(src), 'BACK from the header lets go of the box');
ok(/spotsNavFollow\(\{ zone: 'list', idx: listIdxRef\.current, key: spotKeyRef\.current \}, spotKeys\)/.test(src)
   && /useLayoutEffect\(\(\) => \{\s*if \(!isSpotsMode \|\| kbZone !== 'list'/.test(src), 'a new list re-finds the highlight by key (layout effect)');
ok(/spotKeyRef\.current = spotKeysRef\.current\[0\] \?\? null;/.test(src), 'ENTER on the box lands on the top row');
ok(/onSpotsScrollFail/.test(src) && /scrollToIndex\?\.\(\{ index: i, viewPosition: 0\.3/.test(src), 'the highlighted row is scrolled into view');
ok((src.match(/hl=\{spotHlKey !== null && (spotRowKey\(item\)|skimWaitKey\(sg\)) === spotHlKey/g) || []).length === 2,
   'both row kinds (spots and "waiting for callsign") draw the highlight');
ok(/tuneSpotRef\.current = \(i: number\) => \{[\s\S]{0,200}onTuneHz\?\.\(skimPending\[i\]\.hz\)[\s\S]{0,120}onTuneHz\?\.\(sp\.freqHz\)/.test(src),
   'ENTER tunes exactly as the rows\' taps do (onTuneHz with the row\'s frequency)');
// Everything else unchanged: the other lists still go through the generic path.
ok(/else if \(isScannerRef\.current\) scanNav\.current\.press/.test(src) && /onSelectDabRef\.current\?\.\(listIdxRef\.current\)/.test(src),
   'scanner / DAB / CW lists keep their own ENTER');

process.exit(fails ? 1 : 0);
