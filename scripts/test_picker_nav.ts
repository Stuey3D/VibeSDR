// The server list's focus rules (src/utils/pickerNav.ts) — controller audit group H, 2026-10-20.
import { pickerZone, headerAbove, directoryRowFull, directoryRowAction, revealOffset } from '../src/utils/pickerNav';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const J = JSON.stringify;

console.log('test_picker_nav');
// 4 header controls, 6 list rows, 2 footer rows.
ok(J(pickerZone(0, 4, 6, 2)) === '{"zone":"header","at":0}', 'index 0 is the first header control');
ok(J(pickerZone(4, 4, 6, 2)) === '{"zone":"list","at":0}', 'index 4 is the FIRST list row, not the fifth');
ok(J(pickerZone(9, 4, 6, 2)) === '{"zone":"list","at":5}', 'last list row');
ok(J(pickerZone(10, 4, 6, 2)) === '{"zone":"footer","at":0}', 'then the footer');
ok(pickerZone(12, 4, 6, 2) === null && pickerZone(-1, 4, 6, 2) === null, 'out of range: nowhere');
// Back collapses the group you stand in — found from the LIST position.
const kinds = ['header', 'custom', 'custom', 'header', 'inst', 'inst'];
const z = pickerZone(4 + 4, 4, 6, 2)!;           // global 8 → list row 4 (an inst under the second header)
ok(headerAbove(kinds, z.at) === 3, 'Back from list row 4 finds the second header (3) — the old code walked from 8');
ok(headerAbove(kinds, 2) === 0 && headerAbove(['inst'], 0) === -1, 'first group; no header at all');
// Enter = tap on directory rows.
ok(directoryRowFull({ url: 'u', serverType: 'ubersdr', users: 0, maxUsers: 20 }), 'UberSDR 0 free of 20 = full');
ok(!directoryRowFull({ url: 'u', serverType: 'ubersdr', users: 20, maxUsers: 20 }), 'UberSDR 20 free = empty');
ok(directoryRowFull({ url: 'u', serverType: 'kiwi', users: 4, maxUsers: 4 }), 'Kiwi 4 in use of 4 = full');
ok(!directoryRowFull({ url: 'u', serverType: 'kiwi', users: 0, maxUsers: 0 }), 'no count: never full');
ok(directoryRowAction({ url: 'u', serverType: 'kiwi', users: 4, maxUsers: 4 }, false).do === 'none', 'a FULL row does nothing on Enter, as it is disabled for a tap');
ok(J(directoryRowAction({ url: 'spyserver://h.example:5555', serverType: 'spyserver', users: 0, maxUsers: 0 }, false)) === '{"do":"spy","host":"h.example","port":5555}', 'SpyServer rows connect (they did nothing on Enter)');
ok(directoryRowAction({ url: 'http://x', serverType: 'vibeserver', users: 1, maxUsers: 0 }, false).do === 'vibeserver', 'VibeServer rows open the door');
ok(J(directoryRowAction({ url: 'http://x', serverType: 'kiwi', users: 1, maxUsers: 4 }, true)) === '{"do":"connect","compatOnly":true}', 'a restricted receiver opens in compatibility mode on Enter too');
ok(J(directoryRowAction({ url: 'http://x', serverType: 'owrx', users: 1, maxUsers: 0 }, false)) === '{"do":"connect","compatOnly":false}', 'an ordinary one connects normally');
// ★ V12 489: header and footer rows are MEASURED into view (the header used to send the list to its top, so the bottom
//   DISCOVERED servers sat below the fold while lit). The list's box: y 120, 466 tall; rows 48 tall.
const box = { y: 120, h: 466 };
ok(revealOffset(0, box, { y: 300, h: 48 }) === null, 'a row wholly inside the list: no scroll');
ok(revealOffset(0, box, { y: 120, h: 48 }) === null && revealOffset(0, box, { y: 538, h: 48 }) === null, 'touching either edge still counts as inside');
const below = revealOffset(0, box, { y: 640, h: 48 })!;
ok(below !== null && Math.abs(640 - below - (box.y + (box.h - 48) / 2)) < 0.01, 'Bravia below the fold: scrolled so it sits in the middle of the list');
const above = revealOffset(400, box, { y: 40, h: 48 })!;
ok(above !== null && above < 400 && Math.abs(40 + (400 - above) - (box.y + (box.h - 48) / 2)) < 0.01, 'a row above the top: scrolled back to the middle');
ok(revealOffset(0, box, { y: 60, h: 48 }) === 0, 'never a negative offset');
ok(revealOffset(0, { y: 120, h: 0 }, { y: 640, h: 48 }) === null, 'an unmeasured list (height 0): leave it alone');
process.exit(fails ? 1 : 0);
