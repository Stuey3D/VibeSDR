/**
 * COMING BACK TO A WHOLE-RADIO MODE (src/services/wholeModeResume.ts) — the app half of the 2026-10-27 fix.
 *
 * Stuart, iPhone, V12 498/499: "a couple of times I was in the map view for NDB or ADS-B and when I moved to another app
 * leaving VibeSDR running in the background when I switched back the map dropped back to a waterfall view on the
 * previous frequency, this may have been poor cellular connection though."
 *
 * Proves, on a simulated clock:
 *   • ★ REGRESSION: backgrounded for a minute with the spectrum socket CLOSED by the app, the hold does NOT lapse — the
 *     old rule (ten seconds since the last report) dropped the map the moment the app came back;
 *   • the reopened socket's first report within the first-report window keeps everything as it was — no ask at all;
 *   • the server having left the mode: one `mode_resume` ask, ~4 s after the socket opened (ISM: 7 s);
 *   • yes → wait for the reports (no give-up while they come); a yes followed by nothing gives up after the window;
 *   • no → give up AT ONCE with the server's words; no answer (an older server) → give up with our own words;
 *   • could not ask (the dial is somebody else's) → give up with the reason given;
 *   • steady silence on a socket that HAS reported is still ten seconds (the old rule, unchanged);
 *   • a stale answer for another mode does nothing; reset clears a pending ask.
 *
 * Run: npx tsx scripts/test_wholeModeResume.ts   (run-tests.sh does)
 */
import {
  WholeModeResume, holdLapsed, firstReportMs, defaultGoneWhy, RESUME_ANSWER_MS, WHOLE_STEADY_SILENCE_MS,
  type WholeKind, type ResumeStep,
} from '../src/services/wholeModeResume.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const ok = (what: string, c: boolean) => eq(what, c, true);

/** The screen's once-a-second tick, run over a span of the simulated clock; returns every non-'none' step. */
function run(r: WholeModeResume, kind: WholeKind, from: number, to: number, last: () => number, open: (t: number) => number): ResumeStep[] {
  const out: ResumeStep[] = [];
  for (let t = from; t <= to; t += 1000) {
    const st = r.step(kind, holdLapsed({ kind, now: t, lastReportAt: last(), openSince: open(t) }), t);
    if (st.do !== 'none') out.push({ ...st, at: t } as ResumeStep);
  }
  return out;
}

const T0 = 1_000_000;
for (const kind of ['adsb', 'ndb', 'ais', 'ism'] as WholeKind[]) {
  // ── ★ REGRESSION: the background minute ────────────────────────────────────────────────────────────────────────
  {
    const r = new WholeModeResume();
    const lastReport = T0;            // the last report before the app went to the background at T0
    const steps = run(r, kind, T0, T0 + 60_000, () => lastReport, () => 0);   // socket closed by the app throughout
    eq(`${kind}: no lapse while the app has the socket closed`, steps, []);
    ok(`${kind}: …where the OLD rule (10 s since the last report) would already have dropped it`, T0 + 60_000 - lastReport > 10_000);
  }
  // ── the server still in the mode (inside its grace): the first report comes, nothing is asked ───────────────────
  {
    const r = new WholeModeResume();
    const opened = T0 + 60_000;
    let last = T0;
    const steps: ResumeStep[] = [];
    for (let t = opened; t <= opened + 20_000; t += 1000) {
      if (t === opened + 1000) last = t;                 // the server's next report, a second after the socket opened
      if (t > opened + 1000 && (t - opened) % 1000 === 0 && kind !== 'ism') last = t;   // …and one a second after
      if (kind === 'ism' && (t - opened) % 5000 === 1000) last = t;                     // ISM's busy-band 5 s cadence
      const st = r.step(kind, holdLapsed({ kind, now: t, lastReportAt: last, openSince: opened }), t);
      if (st.do !== 'none') steps.push(st);
    }
    eq(`${kind}: reports on the reopened socket — no ask, nothing dropped`, steps, []);
  }
  // ── the server left the mode; it says YES and goes back in ────────────────────────────────────────────────────────
  {
    const r = new WholeModeResume();
    const opened = T0 + 300_000;
    let last = T0;
    const steps = run(r, kind, opened, opened + firstReportMs(kind) + 1000, () => last, () => opened);
    eq(`${kind}: one ask, just past the first-report window`, steps.map((s) => s.do), ['ask']);
    ok(`${kind}: …not before the window (${firstReportMs(kind)} ms)`, (steps[0] as { at?: number }).at! - opened > firstReportMs(kind));
    const askedAt = (steps[0] as { at?: number }).at!;
    eq(`${kind}: yes → nothing to do yet`, r.answered(kind, true, undefined, askedAt + 300), { do: 'none' });
    // the entry takes a moment, then the reports come — one a second from 3 s after the ask
    const more = run(r, kind, askedAt + 1000, askedAt + 3000, () => last, () => opened);
    eq(`${kind}: …no give-up while the entry is under way`, more, []);
    const r2 = new WholeModeResume();
    r2.step(kind, true, askedAt); r2.answered(kind, true, undefined, askedAt + 300);
    const steps2: ResumeStep[] = [];
    for (let t = askedAt + 1000; t <= askedAt + 30_000; t += 1000) {
      const lastRep = t >= askedAt + 3000 ? t : 0;   // the reports, once the server is back in the mode
      const st = r2.step(kind, holdLapsed({ kind, now: t, lastReportAt: lastRep, openSince: opened }), t);
      if (st.do !== 'none') steps2.push(st);
    }
    eq(`${kind}: yes, then reports → nothing more, ever`, steps2, []);
    eq(`${kind}: …and nothing pending`, r2.pending, null);
  }
  // ── YES, then nothing (the entry failed silently) → give up with our words ─────────────────────────────────────
  {
    const r = new WholeModeResume();
    const opened = T0;
    const steps = run(r, kind, opened, opened + firstReportMs(kind) + 1000, () => 0, () => opened);
    const askedAt = (steps[0] as { at?: number }).at!;
    r.answered(kind, true, undefined, askedAt + 500);
    const later = run(r, kind, askedAt + 1000, askedAt + 500 + RESUME_ANSWER_MS + 1000, () => 0, () => opened);
    eq(`${kind}: a yes with no reports gives up after the window`, later.map((s) => s.do), ['giveUp']);
    eq(`${kind}: …in our own words`, (later[0] as { why: string }).why, defaultGoneWhy(kind));
  }
  // ── NO → give up at once, with the server's words ────────────────────────────────────────────────────────────────
  {
    const r = new WholeModeResume();
    const steps = run(r, kind, T0, T0 + 9000, () => 0, () => T0);
    eq(`${kind}: one ask`, steps.map((s) => s.do), ['ask']);
    const why = 'The receiver left it while you were away, and somebody else is listening now.';
    eq(`${kind}: no → give up with the server's reason`, r.answered(kind, false, why, T0 + 9500), { do: 'giveUp', kind, why });
    eq(`${kind}: …and nothing pending`, r.pending, null);
  }
  // ── no answer at all (an older server ignores the type) → give up with our words ───────────────────────────────
  {
    const r = new WholeModeResume();
    const steps = run(r, kind, T0, T0 + firstReportMs(kind) + RESUME_ANSWER_MS + 3000, () => 0, () => T0);
    eq(`${kind}: no answer → ask, then give up`, steps.map((s) => s.do), ['ask', 'giveUp']);
    eq(`${kind}: …in our own words`, (steps[1] as { why: string }).why, defaultGoneWhy(kind));
  }
}

// ── could not ask ──────────────────────────────────────────────────────────────────────────────────────────────────
{
  const r = new WholeModeResume();
  r.step('adsb', true, T0);
  eq('could not ask → give up with the reason given', r.sendFailed('adsb', 'somebody else has the dial'),
     { do: 'giveUp', kind: 'adsb', why: 'somebody else has the dial' });
  eq('…or our own words', new WholeModeResume().sendFailed('ndb'), { do: 'giveUp', kind: 'ndb', why: defaultGoneWhy('ndb') });
}
// ── the steady rule is unchanged ──────────────────────────────────────────────────────────────────────────────────────
{
  const open = T0, last = T0 + 5000;
  eq('steady: 10 s since the last report is not yet a lapse', holdLapsed({ kind: 'adsb', now: last + WHOLE_STEADY_SILENCE_MS, lastReportAt: last, openSince: open }), false);
  eq('steady: past 10 s it is', holdLapsed({ kind: 'adsb', now: last + WHOLE_STEADY_SILENCE_MS + 1, lastReportAt: last, openSince: open }), true);
  eq('no socket open: never a lapse', holdLapsed({ kind: 'adsb', now: last + 1e9, lastReportAt: last, openSince: 0 }), false);
  eq('a report from BEFORE this socket opened counts as none on it', holdLapsed({ kind: 'adsb', now: T0 + 10_000 + 4001, lastReportAt: T0, openSince: T0 + 10_000 }), true);
}
// ── an ask on a socket that was then replaced is asked again on the new one, not given up on ─────────────────────────
{
  const r = new WholeModeResume();
  const s1 = T0, s2 = T0 + 6000;
  eq('ask on the first socket', r.step('adsb', true, T0 + 4500, s1).do, 'ask');
  eq('…the half-open socket is replaced: asked again on the new one', r.step('adsb', true, s2 + 4500, s2).do, 'ask');
  eq('…then waits for its answer', r.step('adsb', true, s2 + 5500, s2).do, 'none');
  eq('…and gives up only after the window on THAT socket', r.step('adsb', true, s2 + 4500 + RESUME_ANSWER_MS + 1, s2).do, 'giveUp');
}
// ── stale answers, reset ───────────────────────────────────────────────────────────────────────────────────────────
{
  const r = new WholeModeResume();
  r.step('adsb', true, T0);
  eq('an answer for another mode does nothing', r.answered('ndb', false, 'x', T0 + 1), { do: 'none' });
  eq('…the ask is still pending', r.pending, 'adsb');
  r.reset();
  eq('reset clears it', r.pending, null);
  eq('an answer after reset does nothing', r.answered('adsb', false, 'x', T0 + 2), { do: 'none' });
  eq('no mode → nothing', r.step(null, true, T0 + 3), { do: 'none' });
}

console.log(`${fails ? 'FAIL' : 'ok'} — wholeModeResume: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
