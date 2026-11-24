/**
 * test-vfo-dsp.ts — the Audio panel's NR / auto notch / blanker for VFO A and VFO B (web/client/src/vfoDsp.ts), and the
 * ECHO RING that made the NR slider "have a fit" (Stuart, 2026-11-24, XCover web page: 38 → 33 → 31 % with nobody
 * touching it, then 30 → 2 % with one VFO; the spectrum socket at ~1 MB/s).
 *
 * Proves:
 *   1. THE FIGHT, REPRODUCED: two pipelines (VFO A on hwinfo, VFO B on the watch state) at DIFFERENT strengths, a server
 *      that answers every set with its report (the beta68–beta92 behaviour, the worst case), messages in flight with
 *      latency. The OLD apply path (report → set → re-send) circulates for ever — measured here, so this test cannot pass
 *      vacuously. The NEW one (ReportedControl: a report paints, only a press sends) sends exactly the presses, and both
 *      values stay put over 2,000 rounds of reports.
 *   2. One set with VFO A only (and with an older server whose B reports no DSP); two with B on and reported.
 *   3. B's set is wired to B: the wire is {"type":"watch","id":1,…}, never an A message; A's report never moves B's
 *      control and B's never moves A's.
 *   4. A press holds off that control's reports for DSP_PRESS_HOLD_MS (a drag is not dragged back by its own echoes), and
 *      after it the server's word wins again.
 *   node --no-warnings scripts/test-vfo-dsp.ts
 */
import assert from 'node:assert';
import { nrStrength, nrPercent, nrSliderFromReport, ReportedControl, vfoBDspOf, dspSets, vfoBDspMsg, DSP_PRESS_HOLD_MS }
  from '../web/client/src/vfoDsp.ts';
import { VfoABCore, msgDspB, nrStrengthOfLevel, nrLevelOfReport, ADOPT_AFTER_OFF_MS, type TuneStatus } from '../src/services/vfoAB.ts';

let pass = 0;
const ok = (m: string) => { console.log('  ✓ ' + m); pass++; };

// ── 1. The fight ──────────────────────────────────────────────────────────────────────────────────────────────────────
type Msg = { to: 'A' | 'B'; pct: number };
/** A server with two pipelines that answers EVERY set (old behaviour) after `lag` ticks, reporting A in hwinfo and B in the
 *  watch state — and a beat that re-reports both every 5 ticks (the watch heartbeat / any other hwinfo). */
function simulate(echo: boolean, rounds: number) {
  const srv = { A: nrStrength(38), B: nrStrength(70) };
  const lag = 3;
  const up: Array<{ at: number; m: Msg }> = [], down: Array<{ at: number; which: 'A' | 'B'; strength: number }> = [];
  // the client: old = a plain slider value per VFO; new = ReportedControl per VFO
  const oldVal = { A: 38, B: 70 };
  const ctl = { A: new ReportedControl<number>(38, 0), B: new ReportedControl<number>(70, 0) };
  let sends = 0, presses = 0;
  const send = (t: number, m: Msg) => { sends++; up.push({ at: t + lag, m }); };
  // the listener drags A 38 → 30 and B 70 → 55 at t=0..3, several values in flight each
  const dragA = [36, 34, 32, 30], dragB = [66, 62, 58, 55];
  for (let t = 0; t < rounds; t++) {
    if (t < 4) {
      presses += 2;
      if (echo) { oldVal.A = dragA[t]; oldVal.B = dragB[t]; }
      else { ctl.A.press(dragA[t], t); ctl.B.press(dragB[t], t); }
      send(t, { to: 'A', pct: dragA[t] }); send(t, { to: 'B', pct: dragB[t] });
    }
    // the server: apply what arrived, answer each (old behaviour), and beat
    for (const u of up.filter((x) => x.at === t)) {
      srv[u.m.to] = nrStrength(u.m.pct);
      down.push({ at: t + lag, which: u.m.to, strength: srv[u.m.to] });
    }
    if (t % 5 === 0) { down.push({ at: t + lag, which: 'A', strength: srv.A }); down.push({ at: t + lag, which: 'B', strength: srv.B }); }
    // the client: apply what arrived
    for (const d of down.filter((x) => x.at === t)) {
      const want = nrSliderFromReport(true, d.strength, 50);
      if (echo) {
        if (oldVal[d.which] !== want) { oldVal[d.which] = want; send(t, { to: d.which, pct: want }); }   // ★ the old dispatch
      } else ctl[d.which].report(want, t);   // ★ paints only — there is no send path
    }
  }
  return { sends, presses, srvA: Math.round(nrPercent(srv.A)), srvB: Math.round(nrPercent(srv.B)),
           shownA: echo ? oldVal.A : ctl.A.value, shownB: echo ? oldVal.B : ctl.B.value };
}
{
  const old = simulate(true, 2000);
  assert.ok(old.sends > 200, `the old apply path should circulate (it sent ${old.sends}) — else this test proves nothing`);
  console.log(`    (old apply path: ${old.presses} presses became ${old.sends} sends in 2,000 ticks — the ring)`);
  const now = simulate(false, 2000);
  assert.strictEqual(now.sends, now.presses, `new apply path sent ${now.sends} for ${now.presses} presses`);
  assert.strictEqual(now.srvA, 30); assert.strictEqual(now.srvB, 55);
  assert.strictEqual(now.shownA, 30); assert.strictEqual(now.shownB, 55);
  ok(`two pipelines at different strengths, answers in flight: sends = presses (${now.sends}), A stays 30 %, B stays 55 % over 2,000 rounds`);
}

// ── 2. Which sets ─────────────────────────────────────────────────────────────────────────────────────────────────────
{
  const bNew = vfoBDspOf({ id: 1, freq: 7e6, nr: true, nrStrength: 0.5, notch: false, nbx: true });
  const bOld = vfoBDspOf({ id: 1, freq: 7e6, mute: false });   // an older server's B: no DSP of its own
  assert.deepStrictEqual(dspSets(false, null), ['A']);
  assert.deepStrictEqual(dspSets(false, bNew), ['A'], 'B off: one set even if a stale state carried B');
  assert.deepStrictEqual(dspSets(true, bOld), ['A'], 'an older server: no dead B set');
  assert.deepStrictEqual(dspSets(true, bNew), ['A', 'B']);
  assert.deepStrictEqual(bNew, { nr: true, nrStrength: 0.5, notch: false, nbx: true });
  assert.strictEqual(vfoBDspOf({ nr: true, nrStrength: NaN }).nrStrength, undefined);
  ok('A only = one set; A+B = two sets; an older server (B without DSP) = one set');
}

// ── 3. Wired to the right channel ─────────────────────────────────────────────────────────────────────────────────────
{
  const m = vfoBDspMsg({ nr: true, nrStrength: nrStrength(42), notch: true, nbx: false });
  assert.deepStrictEqual(m, { type: 'watch', id: 1, nr: true, nrStrength: 0.525, notch: true, nbx: false });
  assert.deepStrictEqual(vfoBDspMsg({ notch: false }), { type: 'watch', id: 1, notch: false });
  // two independent controls: A's report never moves B's and vice versa
  const a = new ReportedControl<number>(20, 0), b = new ReportedControl<number>(60, 0);
  a.report(25, 10);
  assert.strictEqual(b.value, 60); assert.strictEqual(a.value, 25);
  b.report(61, 11);
  assert.strictEqual(a.value, 25); assert.strictEqual(b.value, 61);
  ok('B set → {"type":"watch","id":1,…} (VFO B\'s channel id), never `nr`/`notch`/`nbx`; each set reads only its own report');
}

// ── 4. The press hold, and the server's word after it ─────────────────────────────────────────────────────────────────
{
  const c = new ReportedControl<number>(10);
  c.press(40, 1000);
  assert.strictEqual(c.report(35, 1000 + DSP_PRESS_HOLD_MS - 1), false, 'an older echo inside the hold does not drag it back');
  assert.strictEqual(c.value, 40);
  assert.strictEqual(c.report(40, 1000 + DSP_PRESS_HOLD_MS + 1), false, 'equal: nothing to paint');
  assert.strictEqual(c.report(12, 1000 + DSP_PRESS_HOLD_MS + 2), true, 'after the hold the server wins (another listener on a shared dial)');
  assert.strictEqual(c.value, 12);
  assert.strictEqual(nrSliderFromReport(false, 0.8, 50), 0);
  assert.strictEqual(nrSliderFromReport(true, 0.0, 50), 1, 'ON never reads as OFF');
  assert.strictEqual(nrSliderFromReport(true, undefined, 37), 37);
  ok(`a press holds reports off for ${DSP_PRESS_HOLD_MS} ms; afterwards the server's word wins; ON never paints as 0`);
}

// ── 5. The app (src/services/vfoAB.ts): B's set from the watch state, its wire, its slider's exact inverse ──────────────
{
  // ★ the app's slider runs to 20 but its curve reaches AudioNR's 1.4 ceiling at 15 (SDRScreen onLocalNR, unchanged): 16–20
  //   send the same strength, so their report reads back as 15 — every level that MEANS something round-trips exactly
  for (let l = 0; l <= 20; l++) assert.strictEqual(nrLevelOfReport(l > 0, nrStrengthOfLevel(l)), Math.min(l, 15),
    `app level ${l} round trip`);
  const status: TuneStatus = { frequency: 7074000, mode: 'usb', bandwidthLow: 100, bandwidthHigh: 2800 };
  const sent: Record<string, unknown>[] = [];
  let t = 1000;
  const ab = new VfoABCore(() => status, (m) => sent.push(m), () => {}, () => t);
  ab.setOffered(1);
  ab.setDspB({ nr: true });
  assert.strictEqual(sent.length, 0, 'B off: nothing to set');
  ab.setOn(true); t += 100;
  ab.onWatch({ type: 'watch', max: 1, chans: [{ id: 1, freq: 7074000, mode: 'usb', bw: 2700, open: true, lvl: -95 }] });
  assert.strictEqual(ab.info().dspB, null, 'an older server (B reports no DSP): no B set');
  ab.onWatch({ type: 'watch', max: 1, chans: [{ id: 1, freq: 7074000, mode: 'usb', bw: 2700, open: true, lvl: -95,
                                               nr: true, nrStrength: 0.75, notch: false, nbx: true }] });
  assert.deepStrictEqual(ab.info().dspB, { nr: true, nrStrength: 0.75, notch: false, nbx: true });
  sent.length = 0;
  ab.setDspB({ nr: true, nrStrength: nrStrengthOfLevel(9) });
  assert.deepStrictEqual(sent, [{ type: 'watch', id: 1, nr: true, nrStrength: 0.75 }]);
  assert.deepStrictEqual(msgDspB({ notch: true }), { type: 'watch', id: 1, notch: true });
  ok('app: B\'s set appears only when the server reports B\'s DSP; it sends {"type":"watch","id":1,…}; level ⟷ strength exact');

  // ★★★ VFO B OFF stays off on a slow link: the beat still carrying B after ADOPT_AFTER_OFF_MS is NOT adopted — the off goes again
  sent.length = 0;
  ab.setOn(false);
  assert.strictEqual(sent.at(-1)!.off, true);
  t += ADOPT_AFTER_OFF_MS + 500;
  const o = ab.onWatch({ type: 'watch', max: 1, chans: [{ id: 1, freq: 7074000, mode: 'usb', bw: 2700, open: true, lvl: -95 }] });
  assert.ok(!ab.on && !o.adopted, 'a late beat carrying B does not bring B back');
  assert.strictEqual(sent.filter((m) => m.off === true).length, 2, 'the off is said again');
  t += 300;
  ab.onWatch({ type: 'watch', max: 1, chans: [{ id: 1, freq: 7074000, mode: 'usb', bw: 2700, open: true, lvl: -95 }] });
  assert.strictEqual(sent.filter((m) => m.off === true).length, 2, '…at most once a second');
  t += 200;
  ab.onWatch({ type: 'watch', max: 1, chans: [] });
  t += 5000;
  const o2 = ab.onWatch({ type: 'watch', max: 1, chans: [{ id: 1, freq: 7074000, mode: 'usb', bw: 2700, open: true, lvl: -95 }] });
  assert.ok(ab.on && o2.adopted, 'once the server has closed it, a B it later reports (a reconnect) is adopted again');
  ok('app: VFO B OFF on a flooded link — not re-adopted, the off re-sent (≤ 1/s) until the server says B is gone');
}

console.log(`\nVFO DSP sets and the NR echo ring: ${pass} checks passed`);
