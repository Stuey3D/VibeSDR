/**
 * test_freedv_app.ts — the app's own half of FreeDV as a mode (freedv.ts holds the shared rule; test_freedv.ts tests it):
 * the passband a FreeDV pick starts with and how it follows the server's report, the mode box label, the bookmark list,
 * the ask-before-tuning list, and that the screen offers the key only through freedvOffered.
 *   Run: node --no-warnings --max-old-space-size=512 scripts/test_freedv_app.ts
 * ★ SILENT: no audio, no network.
 */
import { readFileSync } from 'node:fs';
import { modeDefaultPassband, freedvPassbandOnSide, MODE_BANDWIDTHS } from '../src/services/sdrProtocol.ts';
import { composeModeLabel, modeDisplay } from '../src/constants/modeBox.ts';
import { tunedPassband } from '../src/utils/tunedStation.ts';
import { BOOKMARK_MODES } from '../src/utils/bookmarkEdit.ts';
import { ASK_SHARED_SETTINGS } from '../src/services/dialChat.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { (c ? pass++ : fail++); console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const eq = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);

console.log('── the passband a FreeDV pick starts with ──');
ok(eq(modeDefaultPassband('freedv', 7_177_000), [-2700, -50]), '40 m (below 10 MHz): LSB, as the server demodulates it');
ok(eq(modeDefaultPassband('freedv', 14_236_000), [50, 2700]), '20 m: USB');
ok(eq(modeDefaultPassband('freedv', 10_000_000), [50, 2700]), '10 MHz itself: USB (the server\'s "from 10 MHz up")');
ok(eq(modeDefaultPassband('usb', 7_177_000), MODE_BANDWIDTHS.usb) && eq(modeDefaultPassband('am', 1e6), MODE_BANDWIDTHS.am),
   'every other mode: its table row, whatever the dial');

console.log('── the report\'s sideband wins ──');
ok(freedvPassbandOnSide(50, 2700, 'usb') === null && freedvPassbandOnSide(-2700, -50, 'lsb') === null, 'already on that side: nothing to do');
ok(eq(freedvPassbandOnSide(50, 2700, 'lsb'), [-2700, -50]), 'USB drawn, LSB reported: mirrored');
ok(eq(freedvPassbandOnSide(-2400, -100, 'usb'), [100, 2400]), 'the width is kept when it moves');
ok(eq(freedvPassbandOnSide(-3000, 3000, 'lsb'), [-2700, -50]), 'a passband across the dial becomes the SSB channel on that side');

console.log('── labels, bookmarks, the shared dial ──');
ok(modeDisplay('freedv') === 'FREEDV', 'the mode box reads FREEDV');
ok(composeModeLabel('freedv', 'whisper') === 'FDV: WHISPER', 'with a decoder riding it, FDV (FREEDV: … does not fit the field)');
ok(composeModeLabel('usb', 'whisper') === 'USB: WHISPER', 'other modes unchanged');
ok(BOOKMARK_MODES.includes('freedv'), 'a bookmark can carry FreeDV (the server says no where it has no libcodec2)');
ok(ASK_SHARED_SETTINGS.has('freedv_sub'), 'the sub-mode choice asks before tuning, as the server\'s list does');
const tp = tunedPassband('freedv');
ok(tp.lo < 0 && tp.hi > 0, 'no edges known: either side of the dial (the side is the server\'s)');

console.log('── the screen offers it only through freedvOffered ──');
const scr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
const sel = readFileSync(new URL('../src/components/ModeSelector.tsx', import.meta.url), 'utf8');
ok(/setFreedvCap\(freedvOffered\(o\)\)/.test(scr), 'the per-radio answer (vibeserver.json) decides the key');
ok(/withFreeDv\(withAmSync\(/.test(scr), 'the key is added to the VibeServer list only');
ok(!/COMMON_IDS = \[[^\]]*'freedv'/.test(sel) && /m\.demod === true/.test(sel),
   'not a common id: an OpenWebRX\'s own "freedv" stays in its dropdown');
ok(/getStatus\(\)\.mode === 'freedv'\) setFreedvState/.test(scr), 'a report after leaving the mode is a ghost');

console.log('── the sideband override (AUTO / USB / LSB) ──');
ok(eq(modeDefaultPassband('freedv', 7_177_000, 'usb'), [50, 2700]) && eq(modeDefaultPassband('freedv', 14_236_000, 'lsb'), [-2700, -50]),
   'a chosen side is drawn whatever the band');
ok(eq(modeDefaultPassband('freedv', 7_177_000, 'auto'), [-2700, -50]), 'AUTO keeps the convention');
ok(/FREEDV SIDEBAND/.test(sel) && /FREEDV_SIDEBANDS\.map/.test(sel), 'the sideband row sits under the FreeDV sub-mode keys');
ok(/onSb: (askWrap\()?onFreedvSb/.test(scr) && /freedvSub\?\.\(null, v\)/.test(scr), 'the screen sends the sideband choice (gated like the sub-mode)');
ok(/sb: freedvState\?\.sbSel/.test(scr), 'the lit key is the server\'s report, not the tap');

console.log(`\n${fail ? 'FAIL' : 'ok  '} freedv app: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
