// test_recordBuffer.ts — the RECORD BUFFER's retune rule (src/services/recordBuffer.ts), on a fake clock.
// "Clear the buffer on retune a large amount but keep it when a user is fine tuning on a signal" — measured from
// the ANCHOR (the dial when the buffer's oldest audio was heard), so creeping across a band still clears it.
import { RecordBufferAnchor, retuneAllowanceHz, parseRecordBufferSeconds } from '../src/services/recordBuffer.ts';

let pass = 0, fail = 0;
const eq = (a: unknown, b: unknown, m: string) => {
  if (a === b) pass++; else { fail++; console.log(`  FAIL ${m}: ${String(a)} ≠ ${String(b)}`); }
};
const S = 'https://rx.example|r0';

// ── allowances ──
eq(retuneAllowanceHz('cwu'), 1000, 'CW-U 1 kHz');
eq(retuneAllowanceHz('CWL'), 1000, 'CW-L, any case');
eq(retuneAllowanceHz('usb'), 3000, 'USB 3 kHz');
eq(retuneAllowanceHz('lsb'), 3000, 'LSB 3 kHz');
eq(retuneAllowanceHz('am'), 10000, 'AM 10 kHz');
eq(retuneAllowanceHz('sam'), 10000, 'SAM 10 kHz');
eq(retuneAllowanceHz('nfm'), 15000, 'NFM 15 kHz');
eq(retuneAllowanceHz('fm'), 15000, "'fm' is narrow FM in this app");
eq(retuneAllowanceHz('wfm'), 150000, 'WFM 150 kHz');
eq(retuneAllowanceHz('dab'), 0, 'DAB: any dial change');
eq(retuneAllowanceHz('iq'), 5000, 'unknown 5 kHz');
eq(retuneAllowanceHz(''), 5000, 'empty 5 kHz');

// ── setting ──
eq(parseRecordBufferSeconds('30'), 30, '"30"');
eq(parseRecordBufferSeconds('60'), 60, '"60"');
eq(parseRecordBufferSeconds('45'), 0, 'junk → Off');
eq(parseRecordBufferSeconds(null), 0, 'missing → Off');

// ── the first update clears (a previous screen's audio is not this audio) ──
{
  const a = new RecordBufferAnchor(30_000);
  eq(a.update({ freqHz: 14_200_000, mode: 'usb', source: S }, 0), true, 'first update clears');
  eq(a.update({ freqHz: 14_200_000, mode: 'usb', source: S }, 100), false, 'same dial keeps');
}

// ── fine tuning on SSB keeps; a big jump clears ──
{
  const a = new RecordBufferAnchor(30_000);
  a.update({ freqHz: 14_200_000, mode: 'usb', source: S }, 0);
  eq(a.update({ freqHz: 14_200_150, mode: 'usb', source: S }, 1000), false, 'USB +150 Hz keeps');
  eq(a.update({ freqHz: 14_202_900, mode: 'usb', source: S }, 2000), false, 'USB +2.9 kHz from anchor keeps');
  eq(a.update({ freqHz: 14_203_100, mode: 'usb', source: S }, 3000), true, 'USB +3.1 kHz from anchor clears');
  eq(a.update({ freqHz: 14_203_200, mode: 'usb', source: S }, 3100), false, 'after the clear, the new anchor is here');
}

// ── creeping across a band in small steps still clears (anchor, not last step) ──
{
  const a = new RecordBufferAnchor(30_000);
  a.update({ freqHz: 7_100_000, mode: 'lsb', source: S }, 0);
  let cleared = -1;
  for (let i = 1; i <= 20; i++) {
    if (a.update({ freqHz: 7_100_000 + i * 500, mode: 'lsb', source: S }, i * 200)) { cleared = i; break; }
  }
  eq(cleared, 7, 'LSB 500 Hz steps: clears on the step that passes 3 kHz from the anchor (3.5 kHz)');
}

// ── …but a slow retune over minutes keeps, because nothing in the buffer predates it ──
{
  const a = new RecordBufferAnchor(30_000);
  a.update({ freqHz: 7_100_000, mode: 'lsb', source: S }, 0);
  let cleared = false;
  // 1 kHz every 40 s — longer than the window, so the ring has rolled over between steps.
  for (let i = 1; i <= 10; i++) if (a.update({ freqHz: 7_100_000 + i * 1000, mode: 'lsb', source: S }, i * 40_000)) cleared = true;
  eq(cleared, false, 'slow walk, one window per step: never clears');
  eq(a.anchorHz(400_000), 7_109_000, 'the anchor followed the window');
}

// ── a 60 s window remembers further back than a 30 s one ──
{
  const mk = (w: number) => { const a = new RecordBufferAnchor(w); a.update({ freqHz: 14_000_000, mode: 'cwu', source: S }, 0); return a; };
  const a30 = mk(30_000), a60 = mk(60_000);
  for (const a of [a30, a60]) a.update({ freqHz: 14_000_800, mode: 'cwu', source: S }, 10_000);
  // 45 s later, +1.6 kHz from the first position, +0.8 kHz from the second.
  eq(a30.update({ freqHz: 14_001_600, mode: 'cwu', source: S }, 45_000), false, '30 s window: anchor is the second position');
  eq(a60.update({ freqHz: 14_001_600, mode: 'cwu', source: S }, 45_000), true, '60 s window: the first position is still in the ring');
}

// ── WFM: a 100 kHz nudge keeps, the next station clears ──
{
  const a = new RecordBufferAnchor(60_000);
  a.update({ freqHz: 97_700_000, mode: 'wfm', source: S }, 0);
  eq(a.update({ freqHz: 97_800_000, mode: 'wfm', source: S }, 500), false, 'WFM +100 kHz keeps');
  eq(a.update({ freqHz: 97_900_000, mode: 'wfm', source: S }, 900), true, 'WFM +200 kHz from anchor clears');
}

// ── mode and source changes always clear ──
{
  const a = new RecordBufferAnchor(30_000);
  a.update({ freqHz: 9_410_000, mode: 'am', source: S }, 0);
  eq(a.update({ freqHz: 9_410_000, mode: 'sam', source: S }, 100), true, 'AM → SAM clears');
  eq(a.update({ freqHz: 9_410_000, mode: 'SAM', source: S }, 200), false, 'mode compared case-blind');
  eq(a.update({ freqHz: 9_410_000, mode: 'sam', source: S + '|r1' }, 300), true, 'another receiver clears');
  eq(a.update({ freqHz: 225_648_000, mode: 'dab', source: 'x|dab:12B:0xc221' }, 400), true, 'into DAB clears');
  eq(a.update({ freqHz: 225_648_000, mode: 'dab', source: 'x|dab:12B:0xc222' }, 500), true, 'another DAB service clears');
  eq(a.update({ freqHz: 225_648_000, mode: 'dab', source: 'x|dab:12B:0xc222' }, 600), false, 'same service keeps');
}

console.log(`recordBuffer: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
