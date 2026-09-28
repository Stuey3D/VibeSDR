/**
 * The executable spec for src/services/audioSelfHeal.ts — and for its Swift and Kotlin ports
 * (spike/WristSDR/WristSDR/AudioSelfHeal.swift, android/.../AudioSelfHeal.kt), which must make the
 * same decisions on the same inputs. scripts/test_audioSelfHeal_swift.sh runs the Swift port
 * against the SAME scenarios below (they are exported as JSON by this script).
 *
 * Run: npx tsx scripts/test_audioSelfHeal.ts            (asserts)
 *      scripts/test_audioSelfHeal_swift.sh / _kotlin.sh    (the ports, same scenarios)
 *      npx tsx scripts/test_audioSelfHeal.ts --json     (prints the scenario table for the ports)
 */
import { AudioSelfHeal, type HealConfig, type HealAction } from '../src/services/audioSelfHeal';

interface Step { t: number; rx: number; played: number; expected: boolean; hold?: number }
interface Scenario { name: string; cfg?: Partial<HealConfig>; steps: Step[]; want: Array<[number, HealAction]> }

/** Build a 1 Hz timeline. `f(t)` returns what happened in second t: rx/played deltas + expected. */
function timeline(secs: number, f: (t: number) => { rx?: number; pl?: number; exp?: boolean; hold?: number }): Step[] {
  const out: Step[] = [];
  let rx = 0, played = 0;
  for (let t = 0; t <= secs; t++) {
    const s = f(t);
    rx += s.rx ?? 0; played += s.pl ?? 0;
    out.push({ t: t * 1000, rx, played, expected: s.exp ?? true, hold: s.hold });
  }
  return out;
}
const OK = { rx: 50, pl: 48000 };           // one healthy second: 50 frames in, 48 k samples out
const RNP = { rx: 50, pl: 0 };               // receiving, not playing
const DEAF = { rx: 0, pl: 0 };

const scenarios: Scenario[] = [
  { name: 'healthy stream never repairs',
    steps: timeline(120, () => OK), want: [] },
  { name: 'receiving-not-playing: rebuild at 3 s, reopen 3 s later, then back-off',
    steps: timeline(40, (t) => (t < 5 ? OK : RNP)),
    // last play at t=4; quiet ≥ 3 s at t=7 → rebuild; +3 s → reopen at 10; then 3 s back-off
    // (stage 2 → 3000·2^0) → rebuild 13; stage 3 → 6 s → reopen 19; 4 in the minute → capped
    // until 7+60 = 67.
    want: [[7000, 'rebuild-pipeline'], [10000, 'reopen-socket'], [13000, 'rebuild-pipeline'], [19000, 'reopen-socket']] },
  { name: 'a rebuild that works: one repair, then healthy',
    steps: timeline(60, (t) => (t >= 5 && t <= 7 ? RNP : OK)),
    want: [[7000, 'rebuild-pipeline']] },
  { name: 'muted: receiving, nothing played, NEVER fires',
    steps: timeline(60, (t) => (t < 5 ? OK : { ...RNP, exp: false })), want: [] },
  { name: 'paused then resumed: fresh window, no repair',
    steps: timeline(60, (t) => (t < 5 ? OK : t < 30 ? { ...RNP, exp: false } : OK)), want: [] },
  { name: 'resume into a stall is judged from the resume, not from the pause',
    steps: timeline(40, (t) => (t < 5 ? OK : t < 20 ? { ...RNP, exp: false } : RNP)),
    // expected again at t=20; window starts there; 3 s quiet → t=23
    want: [[23000, 'rebuild-pipeline'], [26000, 'reopen-socket'], [29000, 'rebuild-pipeline'], [35000, 'reopen-socket']] },
  { name: 'DAB priming held: 8 s of receive-without-play under a hold does not fire',
    steps: timeline(40, (t) => (t === 5 ? { ...RNP, hold: 9000 } : t > 5 && t < 13 ? RNP : OK)), want: [] },
  { name: 'nothing received and no owner for it (native): never fires',
    steps: timeline(60, (t) => (t < 5 ? OK : DEAF)), want: [] },
  { name: 'nothing received, web owns it (notRecvMs 5 s): reopen the socket, with back-off',
    cfg: { notRecvMs: 5000 },
    steps: timeline(40, (t) => (t < 5 ? OK : DEAF)),
    // last rx at t=4 → deaf ≥ 5 s at t=9; each reopen then gets its own 5 s window (longer than
    // the 3 s back-off) → 14, 19; by then the back-off (stage 3 → 6 s) is the longer → 25.
    want: [[9000, 'reopen-socket'], [14000, 'reopen-socket'], [19000, 'reopen-socket'], [25000, 'reopen-socket']] },
  { name: 'never started (autoplay gate, not expected): silent',
    steps: timeline(30, () => ({ ...RNP, exp: false })), want: [] },
  { name: 'fresh start, frames but no output yet: waits the full stall window',
    steps: timeline(30, (t) => (t < 2 ? RNP : OK)), want: [] },
  { name: 'escalation is forgotten after 10 s healthy',
    steps: timeline(60, (t) => ((t >= 5 && t <= 7) || (t >= 30 && t <= 32) ? RNP : OK)),
    // both episodes get a FIRST-rung rebuild, not a reopen
    want: [[7000, 'rebuild-pipeline'], [32000, 'rebuild-pipeline']] },
  { name: 'rate cap + back-off: 4 in the first minute, then slowing to one a minute — never stops',
    steps: timeline(330, (t) => (t < 5 ? OK : RNP)),
    // 4 fast, capped until 67; then the back-off has grown: 3000·2^3 = 24 s → 91, 48 s → 139, then
    // the 60 s ceiling for ever: 199, 259, 319.
    want: [[7000, 'rebuild-pipeline'], [10000, 'reopen-socket'], [13000, 'rebuild-pipeline'], [19000, 'reopen-socket'],
           [67000, 'rebuild-pipeline'], [91000, 'reopen-socket'], [139000, 'rebuild-pipeline'], [199000, 'reopen-socket'],
           [259000, 'rebuild-pipeline'], [319000, 'reopen-socket']] },
  { name: 'a counter that goes backwards (rebuilt node) is a re-base, not a stall',
    steps: (() => { const s = timeline(30, () => OK); for (const x of s) if (x.t >= 10000) x.played -= 400000; return s; })(),
    want: [] },
];

function run(sc: Scenario): Array<[number, HealAction]> {
  const m = new AudioSelfHeal(sc.cfg);
  const got: Array<[number, HealAction]> = [];
  for (const s of sc.steps) {
    if (s.hold) m.hold(s.t, s.hold);
    const d = m.tick({ now: s.t, rx: s.rx, played: s.played, expected: s.expected });
    if (d.action !== 'none') got.push([s.t, d.action]);
  }
  return got;
}

if (process.argv.includes('--json')) {
  process.stdout.write(JSON.stringify(scenarios));
} else if (process.argv.includes('--lines')) {
  // Flat form for the Kotlin port's harness (no JSON library on a bare JVM):
  //   S <notRecvMs|-> <name>   ·   T <t> <rx> <played> <0|1> <hold|0>   ·   W <t>:<action> …
  const out: string[] = [];
  for (const sc of scenarios) {
    out.push(`S ${sc.cfg?.notRecvMs ?? '-'} ${sc.name}`);
    for (const st of sc.steps) out.push(`T ${st.t} ${st.rx} ${st.played} ${st.expected ? 1 : 0} ${st.hold ?? 0}`);
    out.push('W ' + sc.want.map(([t, a]) => `${t}:${a}`).join(' '));
  }
  process.stdout.write(out.join('\n') + '\n');
} else {
  let fail = 0;
  for (const sc of scenarios) {
    const got = run(sc);
    const ok = JSON.stringify(got) === JSON.stringify(sc.want);
    if (!ok) fail++;
    console.log(`${ok ? 'ok  ' : 'FAIL'} ${sc.name}${ok ? '' : `\n     want ${JSON.stringify(sc.want)}\n     got  ${JSON.stringify(got)}`}`);
  }
  console.log(fail ? `${fail} FAILED` : `all ${scenarios.length} scenarios pass`);
  process.exit(fail ? 1 : 0);
}
