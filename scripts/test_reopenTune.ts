/**
 * THE REOPEN TUNE (src/services/reopenTune.ts) — does a reopened spectrum socket carry this
 * client's own VFO back to the server?
 *
 * Stuart, 2026-10-05, iOS, the Pi 500's RSP on a locked range with independent VFOs: "putting the
 * app in the background to reply on Discord killed the socket and when I went back to it the
 * frequency reset to 4778 every time."
 *
 * Proves:
 *   • a reopen on a PER-LISTENER dial restores our own frequency and mode (the bug);
 *   • a reopen on a SHARED dial restores NOTHING — recovery is not a user action;
 *   • the FIRST open restores nothing of its own (connect() decides, incl. a first visit that
 *     takes the server's landing);
 *   • DAB holds the dial — nothing restored;
 *   • a pending tune wins (connect()'s own, or newer) — even on a shared dial it is passed through
 *     unchanged, because the config handler is the gate that drops it there;
 *   • no frequency known → nothing.
 *
 * Run: node --no-warnings scripts/test_reopenTune.ts
 */
import { reopenRestoreTune, type ReopenTuneInput } from '../src/services/reopenTune.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.log(`  FAIL ${what}\n       got  ${g}\n       want ${w}`);
}

const base: ReopenTuneInput = {
  hadConfig: true, sharedDial: false, inDab: false, pending: null, frequency: 5_975_000, mode: 'am',
};

eq('per-listener reopen restores our own VFO',
   reopenRestoreTune(base), { frequency: 5_975_000, mode: 'am' });
eq('shared dial: a reopen restores nothing',
   reopenRestoreTune({ ...base, sharedDial: true }), null);
eq('first open: nothing of our own (connect() decides)',
   reopenRestoreTune({ ...base, hadConfig: false }), null);
eq('first visit with no memory stays null (server landing is the answer)',
   reopenRestoreTune({ ...base, hadConfig: false, pending: null }), null);
eq('DAB holds the dial',
   reopenRestoreTune({ ...base, inDab: true }), null);
eq('a pending tune wins over our status',
   reopenRestoreTune({ ...base, pending: { frequency: 7_074_000, mode: 'usb' } }),
   { frequency: 7_074_000, mode: 'usb' });
eq('a pending tune passes through on the first open',
   reopenRestoreTune({ ...base, hadConfig: false, pending: { frequency: 7_074_000, mode: 'usb' } }),
   { frequency: 7_074_000, mode: 'usb' });
eq('no frequency known',
   reopenRestoreTune({ ...base, frequency: 0 }), null);
eq('NaN frequency',
   reopenRestoreTune({ ...base, frequency: Number.NaN }), null);

console.log(`reopenTune: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
