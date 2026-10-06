/**
 * The DAB reception meter (src/utils/dabQuality.ts) — three bars and a sentence in place of the signal bar.
 *
 * ★★★ Stuart, 2026-10-06: a US listener sat on Coventry (12C) through the Pi 2 for an hour — station list
 *     loaded, far too many errors before Viterbi, never a sound — while the ordinary bar read S9. The cases
 *     below are the on-air figures from the DAB repair sessions (memory notes 2026-09-07 → 09-21), so the
 *     thresholds are held to what those multiplexes actually sounded like.
 *
 * Run: node --no-warnings scripts/test_dab_quality.ts   (run-tests.sh does)
 */
import {
  classifyDabWindow, DabQualityMeter, dabQualityLine, DAB_SEARCHING, type DabWindow,
} from '../src/utils/dabQuality.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const W = (o: Partial<DabWindow>): DabWindow => ({
  locked: true, fibRate: 1, fibNow: 1, sfTried: 0, sfOk: 0, mp2In: 0, mp2Bad: 0, frames: 0, erased: 0, ...o,
});
const lvl = (o: Partial<DabWindow>) => classifyDabWindow(W(o)).level;

// ── No signal ──
eq('not locked → 0', lvl({ locked: false }), 0);
eq('locked, no FIC (server false lock) → 0', lvl({ fibNow: 0.0, fibRate: 0.0 }), 0);
eq('locked, no FIC detail', classifyDabWindow(W({ fibNow: 0 })).detail, 'no station list');

// ── Prediction (no service decoding yet) ──
eq('clean mux, MER 23 → strong', lvl({ mer: 23.2 }), 3);
eq('12B MER 16.1, FIB 100 % → strong', lvl({ mer: 16.1 }), 3);
eq('MER 12 → moderate', lvl({ mer: 12 }), 2);
eq('9A MER 8.5, FIB 98-99 % → weak (MER outranks a good FIC)', lvl({ mer: 8.5, fibRate: 0.985 }), 1);
eq('10C FIB 23-55 % → weak', lvl({ fibRate: 0.4, fibNow: 0.4, mer: 0 }), 1);
eq('FIB 95 % → moderate', lvl({ fibRate: 0.95, fibNow: 0.95, mer: 20 }), 2);
eq('raw BER 9 % (10D) → weak', lvl({ mer: 9.1, mscBer: 0.09 }), 1);
eq('raw BER 4.1 % detail', classifyDabWindow(W({ mer: 20, mscBer: 0.041 })).detail, 'BER 4.1 %');
eq('raw BER 0.5 % (12B, 0 bad frames in 8 min) → strong, not moderate', lvl({ mer: 20, mscBer: 0.005 }), 3);
eq('raw BER 0.8 % (11D, clean) → strong', lvl({ mer: 18, mscBer: 0.008 }), 3);
eq('raw BER 2 % → moderate', lvl({ mer: 18, mscBer: 0.02 }), 2);
eq('12 % frames erased → weak', lvl({ frames: 50, erased: 6 }), 1);
eq('3 % frames erased → moderate', lvl({ frames: 100, erased: 3 }), 2);

// ── Audio evidence outranks prediction ──
eq('DAB+ 7D: ~20 % super frames lost → weak', lvl({ sfTried: 40, sfOk: 32 }), 1);
eq('DAB+ Pi 2 wire: 95.7 % OK → moderate', lvl({ sfTried: 1111, sfOk: 1063 }), 2);
eq('DAB+ all OK → strong', lvl({ sfTried: 42, sfOk: 42 }), 3);
eq('DAB+ 14 % lost detail', classifyDabWindow(W({ sfTried: 50, sfOk: 43 })).detail, '14 % frames lost');
eq('DAB+ all OK, no detail', classifyDabWindow(W({ sfTried: 42, sfOk: 42 })).detail, undefined);
eq('DAB+ clean ACCESS UNITS beat a poor MER/BER prediction (bursty errors)',
   lvl({ sfTried: 42, sfOk: 42, auIn: 168, auBad: 0, mer: 9, mscBer: 0.09 }), 3);
// ★★★ 2026-10-06 — the two "Strong on bubbling mud" reports
eq('DAB+ every super frame "OK" but 30 % of access units lost → weak',
   lvl({ sfTried: 42, sfOk: 42, auIn: 168, auBad: 50, mer: 9, mscBer: 0.09 }), 1);
eq('DAB+ 3 % access units lost → moderate', lvl({ sfTried: 42, sfOk: 42, auIn: 168, auBad: 5 }), 2);
eq('older server (no AU counter): all frames OK but BER 9 % → not strong',
   lvl({ sfTried: 42, sfOk: 42, mer: 9, mscBer: 0.09 }), 2);
eq('older server: Reed-Solomon failures inside OK frames → not strong',
   lvl({ sfTried: 42, sfOk: 42, rsLost: 3, mer: 20 }), 2);
eq('Layer II Coventry 12C: 2.8 % bad but ScF-CRC 0.71/frame → weak',
   lvl({ mp2In: 6590, mp2Bad: 185, scfConcealed: 4675, mer: 8, mscBer: 0.129 }), 1);
eq('Layer II clean: no ScF concealment → strong', lvl({ mp2In: 9980, mp2Bad: 15, scfConcealed: 40 }), 3);
eq('Layer II ScF 0.05/frame → moderate', lvl({ mp2In: 1000, mp2Bad: 0, scfConcealed: 50 }), 2);
eq('Layer II 9A: 19 % bad → weak', lvl({ mp2In: 1077, mp2Bad: 205 }), 1);
eq('Layer II 12B V4L: 0.15 % bad → strong', lvl({ mp2In: 9980, mp2Bad: 15 }), 3);
eq('Layer II 5 % bad → moderate', lvl({ mp2In: 200, mp2Bad: 10 }), 2);
eq('too few super frames to judge → prediction', lvl({ sfTried: 2, sfOk: 0, mer: 22 }), 3);
eq('no FIC beats audio evidence', lvl({ fibNow: 0.01, sfTried: 40, sfOk: 40 }), 0);

// ── The meter: window, hysteresis, resets ──
{
  const m = new DabQualityMeter();
  let t = 0;
  const rep = (o: Record<string, unknown>) => m.push({ channel: '12C', locked: true, fibRate: 1, mer: 20, ...o }, (t += 1000));
  eq('first report is adopted at once', rep({ sid: 0 }).level, 3);
  // A service is picked; its super frames start failing heavily.
  let sfT = 0, sfO = 0;
  const sf = (tried: number, ok: number) => { sfT += tried; sfO += ok; return rep({ sid: 0xc0de, sfTried: sfT, sfOk: sfO }); };
  eq('one bad second does not move the bars (hold down 2)', sf(8, 4).level, 3);
  eq('the second does', sf(8, 4).level, 1);
  eq('weak line', dabQualityLine(m.current()!), 'Multiplex weak · No or heavily broken audio · 50 % frames lost');
  // Recovery: it takes three clean reports, AND the bad seconds must age out of the 5 s window first.
  const seen: number[] = [];
  for (let i = 0; i < 10; i++) seen.push(sf(8, 8).level);
  eq('recovers to strong only after the window clears and the hold passes', seen.indexOf(3) >= 4, true);
  eq('...and does get there', seen[seen.length - 1], 3);

  // The server zeroes the audio counters on a service change: the window restarts, no negative deltas.
  sfT = 0; sfO = 0;
  eq('counters going backwards restart the window (no false weak)', sf(8, 8).level, 3);

  // Losing lock: two reports, then No signal.
  rep({ locked: false }); const q = rep({ locked: false });
  eq('lock lost → No signal after the hold', q.level, 0);
  eq('No signal carries no stat', q.detail, undefined);

  // A new multiplex starts from nothing — no verdict carries over.
  const q2 = m.push({ channel: '12B', locked: true, fibRate: 1, mer: 22 }, (t += 1000));
  eq('new channel adopts its first report immediately', q2.level, 3);

  m.reset();
  eq('reset clears the verdict', m.current(), null);
}
{
  // Flicker: a mux sitting right on a line must not jitter between two levels report by report.
  const m = new DabQualityMeter();
  let t = 0, tried = 0, ok = 0;
  const levels: number[] = [];
  for (let i = 0; i < 20; i++) {
    tried += 10; ok += i % 2 ? 10 : 9;    // alternating 100 % / 90 % seconds → ~95 % over the window
    levels.push(m.push({ channel: '11D', locked: true, fibRate: 1, mer: 18, sid: 1, sfTried: tried, sfOk: ok }, (t += 1000)).level);
  }
  const changes = levels.slice(1).filter((l, i) => l !== levels[i]).length;
  eq('a borderline mux changes level at most twice in 20 s', changes <= 2, true);
  eq('...and settles on moderate', levels[levels.length - 1], 2);
}

eq('searching placeholder', [DAB_SEARCHING.level, DAB_SEARCHING.short], [0, 'No signal']);

// ★ The second box: MER, not the verdict word again (2026-10-06)
{
  const m = new DabQualityMeter();
  const q = m.push({ channel: '12C', locked: true, fibRate: 1, mer: 8.04, mp2In: 100, mp2Bad: 0, scfConcealed: 70 }, 1000);
  eq('second box carries the MER', q.merDb !== undefined && Math.abs(q.merDb - 8.04) < 1e-9, true);
  const n = new DabQualityMeter();
  eq('no MER while unlocked', n.push({ channel: '12C', locked: false, mer: 9 }, 1000).merDb, undefined);
}

console.log(`dab quality: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
