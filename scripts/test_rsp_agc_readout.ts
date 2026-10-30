// The SDRplay AGC readout's rules (src/services/rspAgcReadout.ts) — 2026-10-30, Stuart: "do we get to see a readout
// of what the AGC is doing for both the RF and IF so I can see if it is working or got stuck".
// ★★ The point of the latch: NO FLICKER on a healthy radio (rsp_frozen_readout_warning_design), amber on a rail held
//    10 s and on a hand-back, held once raised, cleared only after the cause has been gone a while.
import { RspAgcWatch, rspAgcCause, rspAgcLine, rspAgcStatus, rspAgcSampleOf, HOLD_MS, CLEAR_MS,
         type RspAgcSample } from '../src/services/rspAgcReadout.ts';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
console.log('test_rsp_agc_readout');

const healthy = (t: number): RspAgcSample => ({
  lna: 3 + (Math.floor(t / 7000) % 2), ifgr: 38 + Math.round(4 * Math.sin(t / 900)), ifAgc: true, rfAgc: true,
  adcPeak: -20 + 3 * Math.sin(t / 400), vagc: 1, vst: (t % 20000) < 3000 ? 3 : 2, vrs: 3, vtgt: -20, vwpm: 14,
  vmiss: 0, vblind: 0, vrail: 0,
});

// 1. A healthy trace, 10 minutes at 10 Hz: never amber, and the status is the plain fragment.
{
  const w = new RspAgcWatch(); let amber = 0;
  for (let t = 0; t < 600_000; t += 100) { w.push(healthy(t), t); if (w.warn) amber++; }
  ok(amber === 0, 'healthy VibeAGC trace (RF steps, IF moving, on target): never amber in 10 min');
  ok(/^RF \d · IF \d\d$/.test(w.read().status), `status fragment is "RF n · IF nn" (${w.read().status})`);
  ok(w.read().line.startsWith('VibeAGC: ') && w.read().line.includes('target −20') && w.read().line.includes('14 gain writes in the last minute'),
     `panel line: ${w.read().line}`);
}

// 2. A rail that comes and goes in short spells (the server's vrail restarts at 0 each time): never reaches 10 s → never amber.
{
  const w = new RspAgcWatch(); let amber = 0;
  for (let t = 0; t < 300_000; t += 100) {
    const spell = (t % 12_000) / 1000;                   // 0..12 s cycle; at the rail for the first 8 s of it
    w.push({ ...healthy(t), ifgr: spell < 8 ? 59 : 50, vrail: spell < 8 ? Math.floor(spell) : 0 }, t);
    if (w.warn) amber++;
  }
  ok(amber === 0, 'rail spells of 8 s with breaks: never amber (a rail must be HELD 10 s)');
}

// 3. A rail held: amber at 10 s, not before; the reason says which rail; status carries the short word.
{
  const w = new RspAgcWatch(); let firstAmber = -1;
  for (let t = 0; t <= 15_000; t += 100) {
    w.push({ ...healthy(t), ifgr: 59, vrail: Math.floor(t / 1000) }, t);
    if (w.warn && firstAmber < 0) firstAmber = t;
  }
  ok(firstAmber === 10_000, `rail held: amber at 10 s exactly (${firstAmber} ms)`);
  ok(w.read().status.endsWith('· IF RAIL') && /59 dB rail for 1\d s/.test(w.read().why), `rail reason: ${w.read().status} / ${w.read().why}`);
  // …and it clears: the rail goes at t=15 s; held to t ≥ 10+30 s and until 10 s quiet.
  let cleared = -1;
  for (let t = 15_100; t <= 60_000; t += 100) { w.push(healthy(t), t); if (!w.warn && cleared < 0) cleared = t; }
  ok(cleared === 10_000 + HOLD_MS, `rail gone at 15 s: amber held to raise + ${HOLD_MS / 1000} s, then clears (${cleared} ms)`);
}

// 4. Flicker test on the warning itself: the cause blinks on/off every second for a minute → ONE amber spell, no gaps.
{
  const w = new RspAgcWatch(); let transitions = 0, prev = false;
  for (let t = 0; t < 60_000; t += 100) {
    const on = Math.floor(t / 1000) % 2 === 0;
    w.push({ ...healthy(t), vmiss: on ? 1 : 0 }, t);
    if (w.warn !== prev) { transitions++; prev = w.warn; }
  }
  ok(transitions === 1, `a cause blinking every second gives one steady amber, not a flicker (${transitions} transition)`);
}

// 5. Hand-back: amber at once, says why; the panel line names the old system.
{
  const w = new RspAgcWatch();
  w.push({ lna: 4, ifgr: 44, ifAgc: true, rfAgc: true, adcPeak: -31, agcSet: -30, vagc: 0, vst: 5, vrs: 13 }, 0);
  const r = w.read();
  ok(r.warn && r.status === 'RF 4 · IF 44 · AGC HANDED BACK', `hand-back: amber at once (${r.status})`);
  ok(r.why.includes('rejected 3 gain writes'), `hand-back reason: ${r.why}`);
  ok(r.line.startsWith('The SDRplay\'s own IF AGC + our RF walker'), `hand-back line names what runs now: ${r.line}`);
  for (let t = 0; t < 120_000; t += 100) w.push({ lna: 4, ifgr: 44, ifAgc: true, vagc: 0, vst: 5, vrs: 13 }, t);
  ok(w.warn, 'a hand-back stays amber while it stands (it is a state, not an event)');
}

// 6. VibeAGC off by choice (official API, no trip): the old system named, no warning.
{
  const s = { lna: 2, ifgr: 41, ifAgc: true, rfAgc: false, adcPeak: -29.6, agcSet: -30, vagc: 0, vst: 0 };
  ok(rspAgcLine(s) === 'The SDRplay\'s own IF AGC · ADC −30 dBFS · target −30', `official API line: ${rspAgcLine(s)}`);
  ok(rspAgcCause(s) === null, 'official API, healthy: no cause');
}

// 7. Manual gain: no fragment, no warning even with a rail figure.
{
  const s = { lna: 0, ifgr: 20, ifAgc: false, vrail: -40 };
  ok(rspAgcStatus(s) === '' && rspAgcCause(s) === null, 'manual gain: nothing on the status line, never amber');
  const w = new RspAgcWatch(); w.push({ ...healthy(0), vst: 5 }, 0); w.push(s, 100);
  ok(!w.warn, 'switching to manual clears an amber at once (the owner took the gain)');
}

// 8. An older server (beta44: vagc only, none of the readout fields): plain fragment, "running", never amber.
{
  const w = new RspAgcWatch();
  for (let t = 0; t < 60_000; t += 100) w.push(rspAgcSampleOf({ lna: 3, ifgr: 40, vagc: 1, adcPeak: -21 }, true), t);
  ok(!w.warn && w.read().status === 'RF 3 · IF 40' && w.read().line.startsWith('VibeAGC: running'), `beta44 server: ${w.read().line}`);
}

// 9. Blind: VibeAGC with no fresh ADC window for 3 s.
{
  const s = { ...healthy(0), vblind: 4 };
  ok(rspAgcCause(s)?.[0] === 'NO ADC LEVEL', 'no fresh ADC level 4 s: amber');
  ok(rspAgcCause({ ...healthy(0), vblind: 2 }) === null, '2 s: not yet');
}

// 10. The floor rail.
ok(rspAgcCause({ ...healthy(0), ifgr: 20, vrail: -12 })?.[0] === 'IF FLOOR', 'IF at its floor 12 s with RF to add: amber');
ok(CLEAR_MS < HOLD_MS, 'clear dwell shorter than the hold');

process.exit(fails ? 1 : 0);
