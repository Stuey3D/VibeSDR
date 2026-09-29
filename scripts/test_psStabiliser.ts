/**
 * The executable spec for src/services/psStabiliser.ts — what the VTS shows for an RDS PS.
 *
 * Run: npx tsx scripts/test_psStabiliser.ts
 *
 * What it must prove:
 *   1. A static (European) PS is shown at once and never changes; a one-off change shows at once.
 *   2. A Brazilian-style marquee PS ("UMUARAMA" / "ALINE" / "RADIO" a second apart; "UMUARAMA" /
 *      "MASSA" two seconds apart) produces NO BLANK FRAME, settles on ONE stable name that carries
 *      the whole rotation, and changes the display a bounded number of times — never faster than
 *      the minimum dwell.
 *   3. A character-by-character scroller is re-assembled, not shown as a flashing window.
 *   4. Empty PS frames on the same station hold the name; a new station resets.
 *   5. When the rotation stops, the static name comes back.
 */
import { PsStabiliser, MIN_DWELL_MS, mergeSegments } from '../src/services/psStabiliser';

let fails = 0;
const ok = (cond: boolean, what: string) => {
  if (cond) console.log('  ok   ' + what);
  else { console.log('  FAIL ' + what); fails++; }
};

/** Drive a PS sequence at `stepMs` intervals for `durMs`, sampling the display every 100 ms the way
 *  the app would (feed on each RDS frame, tick when nextDueIn elapses). Returns the frames. */
function run(seq: (string | '')[], stepMs: number, durMs: number, key = 'pi:4322') {
  const st = new PsStabiliser();
  const frames: { t: number; name: string }[] = [];
  let due: number | null = null;
  for (let t = 0; t <= durMs; t += 100) {
    let name: string | null = null;
    if (t % stepMs === 0) {
      name = st.feed(key, seq[(t / stepMs) % seq.length], t);
      const d = st.nextDueIn(t); due = d == null ? null : t + d;
    } else if (due != null && t >= due) {
      name = st.tick(t);
      const d = st.nextDueIn(t); due = d == null ? null : t + d;
    }
    if (name != null) frames.push({ t, name });
  }
  return { st, frames };
}

const changes = (frames: { t: number; name: string }[]) => {
  const out: { t: number; name: string }[] = [];
  for (const f of frames) if (!out.length || out[out.length - 1].name !== f.name) out.push(f);
  return out;
};
const minGap = (ch: { t: number }[]) =>
  ch.slice(1).reduce((m, c, i) => Math.min(m, c.t - ch[i].t), Number.POSITIVE_INFINITY);

console.log('1. static European PS');
{
  const { frames } = run(['BBC R2'], 1000, 60000);
  const ch = changes(frames);
  ok(ch.length === 1 && ch[0].name === 'BBC R2' && ch[0].t === 0, 'shown at once, never changes');
  ok(frames.every(f => f.name !== ''), 'no blank frame');
}
{
  const st = new PsStabiliser();
  ok(st.feed('pi:C201', 'HEART', 0) === 'HEART', 'first name immediate');
  ok(st.feed('pi:C201', 'HEART', 60000) === 'HEART', 'holds');
  ok(st.feed('pi:C201', 'HEART 80', 120000) === 'HEART 80', 'a one-off change after a settled minute shows AT ONCE');
  ok(!st.rotating, 'not treated as rotating');
}

console.log('2a. Brazilian marquee, three segments, 1 s apart (Kiko 93.7)');
{
  const { st, frames } = run(['UMUARAMA', 'ALINE', 'RADIO'], 1000, 120000);
  const ch = changes(frames);
  console.log('       display changes: ' + ch.map(c => `${c.t / 1000}s "${c.name}"`).join(', '));
  ok(frames.every(f => f.name !== ''), 'no blank frame');
  ok(ch.length <= 2, `at most 2 distinct displayed values over 2 minutes (got ${ch.length})`);
  ok(minGap(ch) >= MIN_DWELL_MS, `no change inside the minimum dwell (min gap ${minGap(ch)} ms)`);
  const last = ch[ch.length - 1];
  ok(last.name === 'UMUARAMA ALINE RADIO', `settles on the whole rotation as one name ("${last.name}")`);
  ok(last.t <= 5000, `settles within 5 s (at ${last.t} ms)`);
  ok(st.rotating, 'recognised as rotating');
}

console.log('2b. Brazilian marquee, two segments, 2 s apart (Kiko 94.5)');
{
  const { frames } = run(['UMUARAMA', 'MASSA'], 2000, 120000);
  const ch = changes(frames);
  console.log('       display changes: ' + ch.map(c => `${c.t / 1000}s "${c.name}"`).join(', '));
  ok(frames.every(f => f.name !== ''), 'no blank frame');
  ok(ch.length <= 2, `at most 2 distinct displayed values (got ${ch.length})`);
  ok(!ch.some(c => c.name === 'MASSA'), 'the second segment never flashes up on its own');
  ok(ch[ch.length - 1].name === 'UMUARAMA MASSA', `stable combined name ("${ch[ch.length - 1].name}")`);
  ok(minGap(ch) >= MIN_DWELL_MS, 'no change inside the minimum dwell');
}

console.log('2c. tuned in mid-rotation — the name must not depend on where the rotation was');
{
  const a = run(['ALINE', 'RADIO', 'UMUARAMA'], 1000, 60000).frames;
  const name = a[a.length - 1].name;
  ok(changes(a).length <= 2 && name.split(' ').length === 3, `stable 3-part name ("${name}")`);
}

console.log('3. character scroller');
{
  const text = 'RADIO MASSA FM 94.5 ';
  const seq: string[] = [];
  for (let i = 0; i < text.length; i++) seq.push((text + text).slice(i, i + 8).trim());
  const { frames } = run(seq, 500, 90000);
  const ch = changes(frames);
  console.log('       display changes: ' + ch.map(c => `${c.t / 1000}s "${c.name}"`).join(', '));
  ok(frames.every(f => f.name !== ''), 'no blank frame');
  ok(minGap(ch) >= MIN_DWELL_MS, 'no change inside the minimum dwell');
  ok(ch.length <= 4, `bounded number of display changes over 90 s (got ${ch.length})`);
  ok(ch[ch.length - 1].name === 'RADIO MASSA FM 94.5', `re-assembled, wrap trimmed ("${ch[ch.length - 1].name}")`);
  // Update-rate bound: with a 500 ms scroll, the raw PS changed ~180 times.
  ok(ch.length < seq.length / 4, 'far fewer updates than raw PS changes');
}

console.log('4. empty frames and station changes');
{
  const st = new PsStabiliser();
  st.feed('pi:C201', 'HEART', 0);
  ok(st.feed('pi:C201', '', 500) === 'HEART', 'an empty PS on the same station holds the name');
  ok(st.feed('pi:C201', undefined, 900) === 'HEART', 'an absent PS holds the name');
  ok(st.feed('pi:C202', 'CAPITAL', 1000) === 'CAPITAL', 'a new station shows its own name at once');
  ok(st.feed('pi:C203', '', 1200) === '', 'a new station with no name yet shows nothing (not the old one)');
}

console.log('5. rotation stops');
{
  const st = new PsStabiliser();
  let t = 0;
  for (; t < 30000; t += 1000) st.feed('pi:4322', ['UMUARAMA', 'ALINE', 'RADIO'][(t / 1000) % 3], t);
  ok(st.rotating, 'rotating');
  let name = '';
  for (; t < 90000; t += 1000) name = st.feed('pi:4322', 'MASSA FM', t);
  ok(name === 'MASSA FM' && !st.rotating, `returns to the static name ("${name}")`);
}

console.log('merge');
ok(mergeSegments(['UMUARAMA', 'ALINE']) === 'UMUARAMA ALINE', 'no fusing on a single shared letter');
ok(mergeSegments(['RADIO MA', 'ADIO MAS', 'DIO MASS']) === 'RADIO MASS', 'overlap merge');

if (fails) { console.log(`\n${fails} FAILED`); process.exit(1); }
console.log('\nall passed');
