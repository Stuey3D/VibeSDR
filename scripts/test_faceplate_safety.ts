/**
 * ★★★ A FACEPLATE SETTING CAN NEVER LOCK SOMEONE OUT (src/constants/faceplate.ts CRASH SAFETY).
 *
 * 11 B7: LED VU (and, on Stuart's iPhone, VCR) crashed the app on every launch that reached the deck —
 * with a default server that is every launch, so not even the server list could be reached to change
 * it back. This drives the guard's whole life with a fake disk and clock: arm / clear / the window,
 * a crash between launches, a clean exit, leaving the foreground, a torn mark, a disk that refuses.
 *
 * Run: node --no-warnings scripts/test_faceplate_safety.ts   (run-tests.sh does)
 */
import {
  ARMED_WINDOW_MS, decideLaunch, isRiskyFaceplate, makeFaceplateGuard, safeFaceplate, type GuardIO,
} from '../src/constants/faceplate.ts';
import { DEFAULT_SETTINGS, withDisplay, TEXT_ALLOWED, type FaceplateSettings } from '../src/constants/faceplate.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);

// ── A fake disk + clock that outlive a "process" ───────────────────────────────
class Disk { mark: string | null = null; refuseWrite = false; refuseRead = false; }
function processOn(disk: Disk) {
  let now = 1_000_000;
  const timers = new Map<number, { at: number; fn: () => void }>();
  let nextId = 1;
  const io: GuardIO = {
    write: (m) => { if (disk.refuseWrite) throw new Error('EROFS'); disk.mark = m; },
    read: () => { if (disk.refuseRead) throw new Error('EIO'); return disk.mark; },
    remove: () => { disk.mark = null; },
    now: () => now,
    setTimer: (fn, ms) => { const id = nextId++; timers.set(id, { at: now + ms, fn }); return id; },
    clearTimer: (t) => { timers.delete(t as number); },
  };
  const advance = (ms: number) => {
    now += ms;
    for (const [id, t] of [...timers]) if (t.at <= now) { timers.delete(id); t.fn(); }
  };
  return { guard: makeFaceplateGuard(io), advance, timers };
}

const S = (patch: Partial<FaceplateSettings>): FaceplateSettings => ({ ...DEFAULT_SETTINGS, ...patch });
const LEDVU = S({ meter: 'vu' });
const VCR   = withDisplay(S({ text: 'green' }), 'seg');
const NIXIE = withDisplay(S({ chassis: 'silver', meter: 'edge', transparency: 'off', transparencyExplicit: true }), 'nixie');

// ── What counts as risky, and what safe is ────────────────────────────────────
ok('the default faceplate is safe', !isRiskyFaceplate(DEFAULT_SETTINGS));
ok('LED VU is on trial', isRiskyFaceplate(LEDVU));
ok('VCR is on trial', isRiskyFaceplate(VCR));
ok('a chassis is on trial', isRiskyFaceplate(S({ chassis: 'black' })));
ok('a colour alone is not (it only changes values the safe faceplate draws with)', !isRiskyFaceplate(S({ controls: 'red', text: 'amber' })));
{
  const safe = safeFaceplate(NIXIE);
  eq('safe: display HYPER', safe.display, 'hyper');
  eq('safe: meter BAR', safe.meter, 'bar');
  eq('safe: chassis DEFAULT', safe.chassis, 'default');
  eq('safe: transparency untouched', [safe.transparency, safe.transparencyExplicit], ['off', true]);
  ok('safe: leaving Nixie takes the controls off neon', safe.controls !== 'neon');
  ok('safe: the text colour is one HYPER can show', TEXT_ALLOWED.hyper.includes(safe.text));
  ok('safe is safe', !isRiskyFaceplate(safe));
}

// ── decideLaunch ──────────────────────────────────────────────────────────────
eq('no mark: the stored faceplate stands', decideLaunch(VCR, null), { settings: VCR, crashed: null });
{
  const d = decideLaunch(VCR, '{"chassis":"default","display":"seg","meter":"bar","at":1}');
  eq('a mark + VCR stored: comes up on HYPER', d.settings.display, 'hyper');
  eq('…and keeps what was chosen for the "last crashed" slot', d.crashed, VCR);
  ok('…and what it applies is safe', !isRiskyFaceplate(d.settings));
}
eq('a mark + LED VU stored: the bar', decideLaunch(LEDVU, 'x').settings.meter, 'bar');
eq('a TORN mark still counts (the crash tore it)', decideLaunch(LEDVU, '{"chas').crashed, LEDVU);
eq('an EMPTY mark still counts', decideLaunch(LEDVU, '').crashed, LEDVU);
eq('a mark with a safe faceplate stored: nothing to undo', decideLaunch(DEFAULT_SETTINGS, 'x'), { settings: DEFAULT_SETTINGS, crashed: null });

// ── The guard: arm / clear / the window ───────────────────────────────────────
{
  const disk = new Disk();
  const { guard, advance, timers } = processOn(disk);
  guard.arm(VCR);
  ok('arm: the mark is on disk the moment arm() returns', disk.mark != null);
  eq('arm: it records the risky choices', JSON.parse(disk.mark!).display, 'seg');
  advance(ARMED_WINDOW_MS - 1);
  ok('still armed just inside the window', disk.mark != null);
  guard.arm(VCR);                                  // e.g. the deck mounts a moment later
  eq('re-arming restarts the window (one timer, not two)', timers.size, 1);
  advance(ARMED_WINDOW_MS - 1);
  ok('…so it is still armed where the first window would have ended', disk.mark != null);
  advance(2);
  ok('the window ran out: cleared', disk.mark == null);
  guard.arm(LEDVU);
  guard.arm(DEFAULT_SETTINGS);
  ok('going back to safe clears at once', disk.mark == null);
  eq('…and leaves no timer behind', timers.size, 0);
  guard.arm(LEDVU);
  guard.clear();                                   // AppState: background / inactive
  ok('leaving the foreground clears', disk.mark == null);
  eq('…and stops the timer', timers.size, 0);
}

// ── Across launches ───────────────────────────────────────────────────────────
{
  // Launch 1: VCR applied, and the app dies inside the window (no timer ever fires).
  const disk = new Disk();
  let p = processOn(disk);
  eq('launch 1: nothing from before', p.guard.takeLaunchMark(), null);
  p.guard.arm(VCR);
  // …abort. Launch 2:
  p = processOn(disk);
  const mark = p.guard.takeLaunchMark();
  ok('launch 2: the mark is there', mark != null);
  ok('launch 2: and taking it removed it', disk.mark == null);
  const d = decideLaunch(VCR, mark);
  eq('launch 2: comes up safe', d.settings.display, 'hyper');
  ok('launch 2: the notice is due', d.crashed != null);
  p.guard.arm(d.settings);
  ok('launch 2: the safe faceplate leaves no mark', disk.mark == null);
  // Launch 3 (the user has not chosen VCR again): no second notice.
  p = processOn(disk);
  eq('launch 3: no mark, so no notice again', decideLaunch(d.settings, p.guard.takeLaunchMark()).crashed, null);
}
{
  // A clean run: the window passes, then the app is killed (or swiped away) — never a reset.
  const disk = new Disk();
  let p = processOn(disk);
  p.guard.takeLaunchMark();
  p.guard.arm(LEDVU);
  p.advance(ARMED_WINDOW_MS + 1);
  p = processOn(disk);
  eq('a run that got past the window is not a crash', decideLaunch(LEDVU, p.guard.takeLaunchMark()).crashed, null);
}
{
  // Swiped away from the app switcher inside the window: AppState went inactive first.
  const disk = new Disk();
  let p = processOn(disk);
  p.guard.arm(VCR);
  p.guard.clear();
  p = processOn(disk);
  eq('a swipe-away inside the window is not a crash', p.guard.takeLaunchMark(), null);
}
{
  // Entering a receiver long after launch re-arms (useFaceplateOnTrial), and a crash there counts.
  const disk = new Disk();
  let p = processOn(disk);
  p.guard.takeLaunchMark();
  p.guard.arm(LEDVU);
  p.advance(60_000);                               // sat on the server list for a minute
  ok('cleared while on the server list', disk.mark == null);
  p.guard.arm(LEDVU);                              // the deck mounts…
  p = processOn(disk);                             // …and the app dies
  eq('a crash on entering a receiver brings the next launch up safe',
     decideLaunch(LEDVU, p.guard.takeLaunchMark()).settings.meter, 'bar');
}

// ── A guard that cannot use the disk must never break the display ────────────
{
  const disk = new Disk();
  disk.refuseWrite = true;
  const p = processOn(disk);
  let threw = false;
  try { p.guard.arm(VCR); } catch { threw = true; }
  ok('a refused write does not throw out of arm()', !threw);
  disk.refuseRead = true;
  threw = false;
  let m: string | null = 'unset';
  try { m = p.guard.takeLaunchMark(); } catch { threw = true; }
  ok('a refused read does not throw, and means "no mark"', !threw && m === null);
}

console.log(`${fails ? 'FAIL' : 'ok'}  faceplate safety: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
