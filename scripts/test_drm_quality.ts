/**
 * The DRM reception meter (src/utils/drmQuality.ts) — DAB's three bars and a sentence, for DRM, in the signal bar's place;
 * the decoder's own SNR in the reading box.
 *
 * ★★★ Stuart, 2026-11-12 (Music4Joy, xHE-AAC): the decoder said 17–20 dB SNR on the server while the meter read S9+20, and
 *     the R36S's meter read 42 dB SNR while it dropped out more. Both are right: the meter is strength over the noise
 *     beside it, the decoder's SNR is what is left after the echoes and fading are undone. So the cases below pin:
 *       • the box shows the DECODER's SNR (the report's `snr`), never the meter's;
 *       • the verdict is judged on the programme's MER against its QAM (the Signal pane's lines) and on the audio frames;
 *       • no lock (and an older server's report, which carries the same fields) falls back cleanly to "No DRM signal".
 *
 * Run: node --no-warnings scripts/test_drm_quality.ts   (run-tests.sh does)
 */
import {
  classifyDrmWindow, DrmQualityMeter, drmQualityLine, drmBoxReading, DRM_SEARCHING, DRM_SNR_EXPLAIN, type DrmWindow,
  DRM_FOUND_LABEL, DRM_UNPLAYABLE_ADVICE,
} from '../src/utils/drmQuality.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const W = (o: Partial<DrmWindow>): DrmWindow => ({ sync: true, locked: true, facIn: 0, facBad: 0, unplayable: false, ...o });
const lvl = (o: Partial<DrmWindow>) => classifyDrmWindow(W(o)).level;

// ── No lock ──
eq('no sync → 0', lvl({ sync: false, locked: false }), 0);
eq('timing found, not locked → 0 (found)', classifyDrmWindow(W({ locked: false })), { level: 0, detail: 'found' });

// ── Prediction: the programme's MER against its QAM (the Signal pane's merRow) ──
eq('16-QAM, MSC MER 12 → strong', lvl({ mscQam: 16, mscMer: 12 }), 3);
eq('16-QAM, MSC MER 7 → moderate (under the 8 dB need)', lvl({ mscQam: 16, mscMer: 7 }), 2);
eq('16-QAM, MSC MER 4 → weak (3 dB under)', lvl({ mscQam: 16, mscMer: 4 }), 1);
eq('64-QAM, MSC MER 12 → moderate (14 dB need)', lvl({ mscQam: 64, mscMer: 12 }), 2);
eq('64-QAM, MSC MER 9.5 → weak (5970 kHz TDF, ~9–10 dB, MSC never decoded)', lvl({ mscQam: 64, mscMer: 9.5 }), 1);
eq('64-QAM, MSC MER 9.5 detail', classifyDrmWindow(W({ mscQam: 64, mscMer: 9.5 })).detail, 'MSC MER 9.5 dB (64-QAM needs 14)');
eq('FAC 12 % errors → weak', lvl({ facIn: 25, facBad: 3, mscQam: 16, mscMer: 14 }), 1);
eq('FAC 4 % errors → moderate', lvl({ facIn: 25, facBad: 1, mscQam: 16, mscMer: 14 }), 2);
eq('no MSC MER yet, SNR 18 → strong', lvl({ snr: 18 }), 3);
eq('no MSC MER yet, SNR 11 → moderate (the SNR row: 15 / 9)', lvl({ snr: 11 }), 2);

// ── Audio evidence outranks prediction ──
eq('audio all good → strong', lvl({ audioIn: 30, audioBad: 0, mscQam: 16, mscMer: 12 }), 3);
eq('audio 3 % lost → moderate', lvl({ audioIn: 100, audioBad: 3, mscQam: 16, mscMer: 12 }), 2);
eq('audio 20 % lost → weak', lvl({ audioIn: 50, audioBad: 10, mscQam: 16, mscMer: 12 }), 1);
eq('audio 20 % lost detail', classifyDrmWindow(W({ audioIn: 50, audioBad: 10 })).detail, '20 % audio lost');
eq('RRI: 16-QAM at MSC MER 7–9 played clean → moderate, not strong (the margin is a ceiling)',
   lvl({ audioIn: 30, audioBad: 0, mscQam: 16, mscMer: 7.5 }), 2);
eq('a bad FAC does not outrank clean audio (the audio is what is heard)', lvl({ audioIn: 30, audioBad: 0, facIn: 9, facBad: 2, mscQam: 16, mscMer: 12 }), 3);

// ── The meter: windows, the box, the explanation, fall-backs ──
{
  const m = new DrmQualityMeter();
  eq('before any report: none', m.current(), null);
  const searching = m.push({ sync: false, locked: false }, 0);
  eq('searching: level 0', searching.level, 0);
  eq('searching: the words', drmQualityLine(searching), 'No DRM signal · Searching for a DRM signal');
  eq('searching: no SNR in the box', searching.merDb, undefined);
  eq('searching: the box says the verdict\'s word', drmBoxReading(searching), 'No signal');
  eq('the explanation always rides along', searching.explain, DRM_SNR_EXPLAIN);
  const found = m.push({ sync: true, locked: false, snr: 3 }, 1200);
  eq('found, not locked: its words', [found.label, found.advice, found.level], [DRM_FOUND_LABEL, 'Reading its parameters', 0]);
  eq('found, not locked: no SNR (an unlocked figure is not the decoder\'s)', found.merDb, undefined);
}
{
  // ★★★ Stuart's case: a strong signal (meter 42 dB) whose DECODER reads 17–20 dB, 16-QAM, xHE-AAC playing clean
  const m = new DrmQualityMeter();
  let q = DRM_SEARCHING;
  let t = 0, units = 0, fac = 0;
  for (let i = 0; i < 8; i++) {
    units += 10; fac += 3; t += 1200;
    q = m.push({ sync: true, locked: true, msc: true, snr: 17 + (i % 4), mscMer: 16, mscQam: 16, facOk: fac, facBad: 0,
                 audioUnits: units, audioBad: 0, codec: 'xHE-AAC', playable: true }, t);
  }
  eq('Music4Joy: strong', q.level, 3);
  eq('Music4Joy: the box reads the DECODER\'s SNR (17–20), never the meter\'s 42', drmBoxReading(q), 'SNR 19 dB');
  eq('Music4Joy: the line names it as the decoded SNR', drmQualityLine(q), 'DRM strong · Clear audio · decoded SNR 19 dB');
  // the signal fades: audio frames start to fail — the bars drop after HOLD_DOWN (2) reports
  q = m.push({ sync: true, locked: true, snr: 9, mscMer: 6, mscQam: 16, facOk: fac + 3, facBad: 0, audioUnits: units + 5, audioBad: 5, codec: 'xHE-AAC', playable: true }, t += 1200);
  eq('one bad report is held (hysteresis)', q.level, 3);
  q = m.push({ sync: true, locked: true, snr: 9, mscMer: 6, mscQam: 16, facOk: fac + 6, facBad: 0, audioUnits: units + 10, audioBad: 10, codec: 'xHE-AAC', playable: true }, t += 1200);
  eq('two: it drops', q.level < 3, true);
  // the lock goes: at once, no hysteresis — the bars go out with the signal, the box loses its SNR
  q = m.push({ sync: false, locked: false, facOk: fac + 6, facBad: 16 }, t += 1200);
  eq('lock lost → 0 at once', q.level, 0);
  eq('lock lost → no SNR', q.merDb, undefined);
  eq('lock lost → searching words', q.label, 'No DRM signal');
}
{
  // a codec this server cannot play: the bars are the signal's, the advice says why there is no sound
  const m = new DrmQualityMeter();
  let q = DRM_SEARCHING;
  for (let i = 1; i <= 4; i++) q = m.push({ sync: true, locked: true, snr: 18, mscMer: 12, mscQam: 16, facOk: 3 * i, facBad: 0, codec: 'AAC', playable: false }, i * 1200);
  eq('unplayable: strong signal', q.level, 3);
  eq('unplayable: says so', q.advice, DRM_UNPLAYABLE_ADVICE);
}
{
  // ★ an OLDER server: the same report fields (snr, mscMer, facOk…) — this meter needs nothing new from it. One with no
  //   audio counters at all still judges on the prediction.
  const m = new DrmQualityMeter();
  let q = DRM_SEARCHING;
  for (let i = 1; i <= 4; i++) q = m.push({ sync: true, locked: true, snr: 12, mscMer: 6.5, mscQam: 16, facOk: 3 * i }, i * 1200);
  eq('older report: predicted moderate from the MSC MER', [q.level, q.detail], [2, 'MSC MER 6.5 dB (16-QAM needs 8)']);
  // a new receiver (counters went backwards): the window starts again, no negative rates
  q = m.push({ sync: true, locked: true, snr: 12, mscMer: 6.5, mscQam: 16, facOk: 1 }, 6000);
  eq('counters reset: still a sane verdict', q.level, 2);
  m.reset();
  eq('reset(): forgotten', m.current(), null);
}

console.log(`test_drm_quality: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
