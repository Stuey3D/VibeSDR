/**
 * satSchedule.ts — ★★★ beta96 SCHEDULE A PASS (Stuart 2026-11-26; docs/v12/SAT-SCHEDULE.md). One copy for both clients
 * (the web's satellites.ts, the app's SatellitePanel + SDRScreen), so they decide and say the same thing.
 *
 * Stuart: "single user and locked range local hardware whereby other users are not affected, the remind button should be
 * a SCHEDULE button, and if a pass overlaps another then a button to PRIORITISE the pass should be present. Shared VFOs
 * keep the remind system."
 *
 *   · WHICH KEY (satSession / satPassKey): decided by whether tuning moves anyone else — read from what the server says
 *     (sat_list source.pickPrivate, the dial message's listeners, config's locked), never guessed:
 *       shared dial with others on it → REMIND (a pop-up; LISTEN is the listener's own pick, through ask-before-tuning)
 *       single-user radio, a locked range (each listener has their own channel), this app's own radio, or a shared
 *       dial with nobody else on it now → SCHEDULE
 *       an older server that says nothing → neither (a control that can never act is not drawn)
 *     A row that cannot be heard (greyed: out of the locked range, a dead orbit) gets no key at all.
 *   · SCHEDULE: at AOS the client picks the pass exactly as the row's tap does (tune, AUTO DOPPLER, the decoder); after
 *     LOS it goes back to what the listener was on (frequency, mode, or the satellite they had picked) — unless another
 *     scheduled pass starts within SAT_SCHED_BRIDGE_S, when it stays and the next one picks. A listener who tunes away
 *     during the pass has taken over: nothing is restored. Kept per server in the listener's own storage; it runs only
 *     while the client is open and connected.
 *   · OVERLAPS (satSchedPlan): the PRIORITISED pass wins; without a choice the HIGHER pass (max elevation) wins. The other
 *     runs only for what is left (a remainder under SAT_SCHED_MIN_S is not worth a retune: skipped), said on its row.
 *   · DEGRADE: if the dial becomes shared with someone else, scheduled passes stop tuning and fire as reminders
 *     (satSchedRemindDue); a pass running at that moment is left as it is (no return). Alone again, they schedule again.
 */
import { ageText, hhmm, hmIn, satHzText, type SatList, type SatPass, type SatReminder, type SatRow } from './satellites';

// ── WHICH KEY ───────────────────────────────────────────────────────────────────────────────────────────────────────
export type SatSessionKind = 'schedule' | 'remind' | 'none';
export type SatSessionWhy = 'local' | 'single' | 'locked' | 'alone' | 'shared' | 'old';
export interface SatSessionFacts {
  /** sat_list source.pickPrivate — the server's word (true: a pick disturbs nobody; false: a shared dial; absent: not said) */
  pickPrivate?: boolean;
  /** this app's own radio (Local Hardware through this device's engine — not a server across the network) */
  localHw?: boolean;
  /** config `locked`: the owner pinned the range; each listener tunes their own channel inside it */
  locked?: boolean;
  /** the dial message says the tuner is shared (open / spectator tuning) */
  sharedDial?: boolean;
  /** listeners on this radio now, this one included (the dial's / users' count); absent = not known */
  listeners?: number;
}
export function satSession(f: SatSessionFacts): { kind: SatSessionKind; why: SatSessionWhy } {
  const shared = f.sharedDial === true || f.pickPrivate === false;
  if (shared) {
    if (f.listeners != null && f.listeners <= 1) return { kind: 'schedule', why: 'alone' };
    return { kind: 'remind', why: 'shared' };
  }
  if (f.pickPrivate === true) return { kind: 'schedule', why: f.localHw ? 'local' : f.locked ? 'locked' : 'single' };
  if (f.localHw) return { kind: 'schedule', why: 'local' };
  return { kind: 'none', why: 'old' };
}
/** The key on a pass row: SCHEDULE / REMIND / none. Hearable rows only, a pass still to come. REMIND stays on the
 *  current-or-next pass of a downlink (one reminder a downlink, as since 11-24); SCHEDULE may take ANY listed pass — the
 *  good 20:42 METEOR with the low 19:03 one left alone (Mac Ha's "select those interesting"). */
export function satPassKey(l: SatList | null, r: Pick<SatRow, 'grey' | 'up' | 'next' | 'later'>, kind: SatSessionKind): 'schedule' | 'remind' | null {
  if (kind === 'none' || !l || r.grey || r.up || !r.next) return null;
  if (kind === 'remind') return r.later ? null : 'remind';
  return 'schedule';
}
/** ★★★ beta96 (Stuart 11-26: "on shared vfo radios … if you are alone on the receiver it will tune and open the pass for
 *  you, however if anybody else is on then its a reminder only"): THE KEY on a shared dial is always SCHEDULE — the
 *  pass is booked, and who is on the receiver WHEN IT RISES decides: alone → it tunes and opens the pass; anyone else
 *  on → a reminder only. (A REMIND pressed while others were on never tuned, even if they had left by AOS.) The
 *  session's `kind` stays the runtime rule (schedTick); this is the key a row shows. */
export function satKeyKind(s: { kind: SatSessionKind; why: SatSessionWhy }): SatSessionKind {
  return s.why === 'shared' ? 'schedule' : s.kind;
}
/** A shared dial (with or without anyone else on it now) — where SCHEDULE may end up as a reminder. */
export function satOnSharedDial(s: { why: SatSessionWhy }): boolean { return s.why === 'shared' || s.why === 'alone'; }
/** Said when SCHEDULE is pressed on a shared dial, and the key's tooltip there. */
export const SAT_SCHED_SHARED_NOTE = 'Scheduled — if you are alone on this receiver when it rises, it tunes and opens the pass for you; if anyone else is on, it is a reminder only';
/** The line under the list that says why it is SCHEDULE or REMIND here, and the honest limit. `where` 'web' | 'app'. */
export function satSessionLine(s: { kind: SatSessionKind; why: SatSessionWhy }, where: 'web' | 'app'): string {
  const lim = where === 'web' ? 'runs only while this page is open and connected'
                              : 'runs only while the app is open and connected — nothing fires once it is closed';
  switch (s.why) {
    case 'local': return `This device's own radio: SCHEDULE tunes it for the pass and goes back afterwards · ${lim}`;
    case 'single': return `Only you on this radio: SCHEDULE tunes it for the pass and goes back afterwards · ${lim}`;
    case 'locked': return `A locked range — your own channel: SCHEDULE moves only yours, and only passes inside the range are offered · ${lim}`;
    case 'alone':
    case 'shared': return `A shared receiver: SCHEDULE tunes and opens the pass for you if you are alone on it when the pass rises — if anyone else is on, it is a reminder only · ${lim}`;
    default: return '';
  }
}
export const SAT_SCHED_OVERLAP_RULE = 'Overlapping passes: the higher pass wins unless you PRIORITISE one';
/** Said where scheduled passes have become reminders (someone else is on the dial). */
export function satSchedDegradedText(n: number): string {
  return n ? `Another listener is on this dial — your ${n} scheduled pass${n === 1 ? '' : 'es'} will remind you instead of tuning (they tune again if you are alone)` : '';
}

// ── THE BOOK ───────────────────────────────────────────────────────────────────────────────────────────────────────
export interface SatSchedEntry {
  key: string; server: string; norad: number; tx: number; aos: number; los: number; maxEl: number; name: string; label: string;
  /** PRIORITISED: wins every overlap with another scheduled pass */
  prio?: boolean;
}
export const SAT_SCHED_KEY = 'vibe.satSchedule';
export const SAT_SCHED_MAX = 20;          // per server
export const SAT_SCHED_MAX_ALL = 60;      // every server — the stored blob stays small
/** a remainder shorter than this is not worth a retune (and back): skipped */
export const SAT_SCHED_MIN_S = 60;
/** the next scheduled pass this close after LOS: stay on, do not go back in between */
export const SAT_SCHED_BRIDGE_S = 120;
export const satSchedKeyText = (on: boolean): string => (on ? 'CANCEL' : 'SCHEDULE');
export const satSchedPrioText = (on: boolean): string => (on ? 'PRIORITISED' : 'PRIORITISE');
export const SAT_SCHED_SET_WHAT = 'Tune to this pass at AOS (Doppler and decoder as a tap on the row) and go back to what you were on after LOS';
export const SAT_SCHED_CANCEL_WHAT = 'Scheduled — tap to cancel';
export const SAT_SCHED_PRIO_WHAT = 'This pass wins its overlap with another scheduled pass';
export const SAT_SCHED_FULL = 'Twenty passes are scheduled on this server already — cancel one first';

export class SatSchedule {
  private m = new Map<string, SatSchedEntry>();
  static key(server: string, norad: number, tx: number, aos: number) { return `${server}|${norad}|${tx}|${Math.round(aos)}`; }
  get size() { return this.m.size; }
  all(server?: string): SatSchedEntry[] {
    return [...this.m.values()].filter((e) => server == null || e.server === server).sort((a, b) => a.aos - b.aos || a.key.localeCompare(b.key));
  }
  get(key: string): SatSchedEntry | undefined { return this.m.get(key); }
  /** the entry on that downlink's pass (its AOS within a minute — an element-set refresh moves it a little) */
  find(server: string, norad: number, tx: number, aos: number): SatSchedEntry | undefined {
    for (const e of this.m.values()) if (e.server === server && e.norad === norad && e.tx === tx && Math.abs(e.aos - aos) < 60) return e;
    return undefined;
  }
  /** Schedule the row's pass (its `next`). false = refused: no pass to come, AOS gone, already held, or the server's book is full. */
  add(server: string, row: Pick<SatRow, 'sat' | 'tx' | 'i' | 'next'>, nowSec: number): boolean {
    const p = row.next;
    if (!p || p.aos <= nowSec) return false;
    if (this.find(server, row.sat.norad, row.i, p.aos)) return false;
    if (this.all(server).length >= SAT_SCHED_MAX) return false;
    const key = SatSchedule.key(server, row.sat.norad, row.i, p.aos);
    this.m.set(key, { key, server, norad: row.sat.norad, tx: row.i, aos: p.aos, los: p.los, maxEl: p.maxEl, name: row.sat.name,
                      label: row.sat.tx.length > 1 ? row.tx.label : '' });
    return true;
  }
  cancel(key: string): boolean { return this.m.delete(key); }
  clearServer(server: string) { for (const [k, e] of this.m) if (e.server === server) this.m.delete(k); }
  /** ★ PRIORITISE is a toggle: on = this pass wins, and every pass it overlaps loses its own priority (one winner per
   *  overlap); off again = back to the default (the higher pass). */
  prioritise(key: string): boolean {
    const e = this.m.get(key);
    if (!e) return false;
    const on = !e.prio;
    if (on) for (const o of this.m.values()) if (o !== e && o.server === e.server && satSchedOverlaps(o, e)) o.prio = false;
    e.prio = on;
    return true;
  }
  /** Passes that are over go. */
  prune(nowSec: number) { for (const [k, e] of this.m) if (e.los <= nowSec) this.m.delete(k); }
  /** ★ Every sat_list: a new element set moves a pass a little — the entry follows it (nearest AOS within 15 min on the
   *  same downlink), keeping its priority. A bird the list no longer has is kept (the fire-time check says why). */
  refresh(server: string, l: SatList | null) {
    if (!l) return;
    for (const e of [...this.m.values()]) {
      if (e.server !== server) continue;
      const sat = l.sats.find((s) => s.norad === e.norad);
      if (!sat) continue;
      let best: SatPass | undefined, bd = 15 * 60;
      for (const p of sat.passes) { const d = Math.abs(p.aos - e.aos); if (d <= bd) { bd = d; best = p; } }
      if (!best || (best.aos === e.aos && best.los === e.los && best.maxEl === e.maxEl)) continue;
      const key = SatSchedule.key(server, e.norad, e.tx, best.aos);
      if (key !== e.key && this.m.has(key)) continue;   // two entries collapsing onto one pass: keep the one there
      this.m.delete(e.key);
      this.m.set(key, { ...e, key, aos: best.aos, los: best.los, maxEl: best.maxEl });
    }
  }
  /** The stored form (every server's), bounded. */
  toJSON(): string { return JSON.stringify({ v: 1, e: this.all().slice(-SAT_SCHED_MAX_ALL) }); }
  /** Read the stored form back — anything malformed is dropped, passes over by `nowSec` are not loaded. */
  static fromJSON(s: string | null | undefined, nowSec: number): SatSchedule {
    const out = new SatSchedule();
    let o: unknown;
    try { o = s ? JSON.parse(s) : null; } catch { return out; }
    const arr = (o as { e?: unknown } | null)?.e;
    if (!Array.isArray(arr)) return out;
    for (const x0 of arr.slice(0, SAT_SCHED_MAX_ALL)) {
      const x = x0 as Record<string, unknown>;
      const n = (v: unknown) => (typeof v === 'number' && isFinite(v) ? v : NaN);
      const server = typeof x?.server === 'string' ? x.server.slice(0, 200) : '';
      const norad = n(x?.norad), tx = n(x?.tx), aos = n(x?.aos), los = n(x?.los);
      if (!server || !(norad > 0) || !(tx >= 0) || !(los > aos) || los <= nowSec) continue;
      if (out.all(server).length >= SAT_SCHED_MAX) continue;
      const key = SatSchedule.key(server, norad, tx, aos);
      const txt = (v: unknown, m: number) => (typeof v === 'string' ? v.replace(/[\u0000-\u001f\u007f]/g, '').slice(0, m) : '');
      out.m.set(key, { key, server, norad, tx, aos, los, maxEl: Number.isFinite(n(x.maxEl)) ? n(x.maxEl) : 0,
                       name: txt(x.name, 40) || `NORAD ${norad}`, label: txt(x.label, 40), ...(x.prio === true ? { prio: true } : {}) });
    }
    return out;
  }
}
export const satSchedOverlaps = (a: { aos: number; los: number }, b: { aos: number; los: number }): boolean => a.aos < b.los && b.aos < a.los;

// ── THE PLAN — who gets which minutes ─────────────────────────────────────────────────────────────────────────────────
export interface SatSchedSlot {
  key: string;
  /** the minutes this pass actually runs: [start, end) pieces, in time order */
  segs: Array<[number, number]>;
  state: 'full' | 'part' | 'skipped';
  /** every scheduled pass it overlaps (PRIORITISE is offered where this is not empty) */
  over: string[];
  /** the passes that took some of its time, and why they won */
  lostTo: Array<{ key: string; by: 'prio' | 'higher' }>;
}
/** Rank: PRIORITISED first, then the higher pass, then the earlier, then the key (stable). */
function rank(a: SatSchedEntry, b: SatSchedEntry): number {
  return (b.prio ? 1 : 0) - (a.prio ? 1 : 0) || b.maxEl - a.maxEl || a.aos - b.aos || a.key.localeCompare(b.key);
}
function subtract(iv: [number, number], taken: Array<[number, number]>): Array<[number, number]> {
  let out: Array<[number, number]> = [iv];
  for (const [ts, te] of taken) {
    const next: Array<[number, number]> = [];
    for (const [s, e] of out) {
      if (te <= s || ts >= e) { next.push([s, e]); continue; }
      if (ts > s) next.push([s, ts]);
      if (te < e) next.push([te, e]);
    }
    out = next;
  }
  return out.sort((a, b) => a[0] - b[0]);
}
export function satSchedPlan(entries: SatSchedEntry[], minSeg = SAT_SCHED_MIN_S): Map<string, SatSchedSlot> {
  const plan = new Map<string, SatSchedSlot>();
  const taken: Array<[number, number]> = [];
  const ranked = [...entries].sort(rank);
  ranked.forEach((e, i) => {
    const segs = subtract([e.aos, e.los], taken).filter(([s, t]) => t - s >= minSeg);
    taken.push(...segs);
    const over = entries.filter((o) => o.key !== e.key && satSchedOverlaps(o, e)).map((o) => o.key);
    const lostTo = ranked.slice(0, i).filter((o) => satSchedOverlaps(o, e))
      .map((o) => ({ key: o.key, by: (o.prio && !e.prio ? 'prio' : 'higher') as 'prio' | 'higher' }));
    const got = segs.reduce((n, [s, t]) => n + (t - s), 0);
    plan.set(e.key, { key: e.key, segs, over, lostTo, state: !segs.length ? 'skipped' : got >= e.los - e.aos - 0.5 ? 'full' : 'part' });
  });
  return plan;
}
/** Every piece of every pass, in time order — what the runner walks. */
export function satSchedTimeline(plan: Map<string, SatSchedSlot>): Array<{ key: string; start: number; end: number }> {
  const out: Array<{ key: string; start: number; end: number }> = [];
  for (const sl of plan.values()) for (const [start, end] of sl.segs) out.push({ key: sl.key, start, end });
  return out.sort((a, b) => a.start - b.start || a.key.localeCompare(b.key));
}

// ── THE RUNNER (a pure step, once a second) ──────────────────────────────────────────────────────────────────────────
/** What the listener was on before the schedule took the radio — put back after LOS. */
export interface SatSchedPrev { hz: number; mode: string; sat: boolean; pick: { norad: number; tx: number } | null }
export interface SatSchedRun {
  /** the entry whose pass the radio is on now (null = idle) */
  key: string | null;
  prev: SatSchedPrev | null;
  /** its pick has been confirmed by the server at least once */
  seen: boolean;
  /** the listener moved away from it (another pick, out of SATELLITE): nothing is restored */
  took: boolean;
}
export const SAT_SCHED_IDLE: SatSchedRun = { key: null, prev: null, seen: false, took: false };
export type SatSchedAct =
  | { act: 'none' }
  | { act: 'tune'; e: SatSchedEntry }
  | { act: 'back'; prev: SatSchedPrev }
  /** the run ended with nothing to restore: the listener took over, or (degraded) the dial is shared now */
  | { act: 'end'; why: 'took' | 'shared' | 'gone' };
/**
 * One step. `cur.pick` = the pick the server has CONFIRMED now (null = none); `here` = where the listener is now (taken
 * as `prev` when a run starts from idle). `kind` other than 'schedule' ends a run without going back (degrade).
 */
export function satSchedStep(run: SatSchedRun, entries: SatSchedEntry[], nowSec: number,
                             cur: { pick: { norad: number; tx: number } | null }, here: SatSchedPrev, kind: SatSessionKind,
                             plan: Map<string, SatSchedSlot> = satSchedPlan(entries)): { run: SatSchedRun; act: SatSchedAct } {
  if (kind !== 'schedule') return run.key ? { run: SAT_SCHED_IDLE, act: { act: 'end', why: 'shared' } } : { run, act: { act: 'none' } };
  let r = run;
  const byKey = new Map(entries.map((e) => [e.key, e]));
  if (r.key) {
    const e = byKey.get(r.key);
    const on = !!e && !!cur.pick && cur.pick.norad === e.norad && cur.pick.tx === e.tx;
    if (on && !r.seen) r = { ...r, seen: true };
    else if (!on && r.seen && !r.took) r = { ...r, took: true, prev: null };
  }
  const tl = satSchedTimeline(plan);
  const seg = tl.find((s) => s.start <= nowSec && nowSec < s.end);
  if (seg) {
    if (seg.key === r.key) return { run: r, act: { act: 'none' } };
    const e = byKey.get(seg.key)!;
    // ★ from idle, or after the listener took over: where they are NOW is what goes back; back to back: the first one's
    const prev = !r.key || r.took ? here : r.prev;
    return { run: { key: seg.key, prev, seen: false, took: false }, act: { act: 'tune', e } };
  }
  if (!r.key) return { run: r, act: { act: 'none' } };
  // ★ a scheduled pass starting soon after: stay on, no back-and-forth in between
  const next = tl.find((s) => s.start > nowSec);
  if (next && next.start - nowSec <= SAT_SCHED_BRIDGE_S && !r.took) return { run: r, act: { act: 'none' } };
  if (r.took || !r.prev) return { run: SAT_SCHED_IDLE, act: { act: 'end', why: r.took ? 'took' : 'gone' } };
  return { run: SAT_SCHED_IDLE, act: { act: 'back', prev: r.prev } };
}
/** ★ DEGRADED (someone else on the dial): the scheduled passes due as reminders now — at AOS − lead, once each (`fired`
 *  remembers which; the caller keeps it for the session). Over passes never fire. */
export function satSchedRemindDue(entries: SatSchedEntry[], nowSec: number, leadMin: number, fired: Set<string>): SatReminder[] {
  const out: SatReminder[] = [];
  for (const e of entries) {
    if (fired.has(e.key) || nowSec >= e.los || nowSec < e.aos - leadMin * 60) continue;
    fired.add(e.key);
    out.push({ key: e.key, server: e.server, norad: e.norad, tx: e.tx, aos: e.aos, los: e.los, name: e.name, label: e.label });
  }
  return out;
}
export const SAT_SCHED_REMIND_NOTE = 'Scheduled — but another listener is on this dial now, so it did not tune by itself';
/** ★ The fire-time check: why this pass cannot be picked any more ('' = it can). */
export function satSchedWhy(l: SatList | null, e: Pick<SatSchedEntry, 'norad' | 'tx' | 'name'>): string {
  if (!l) return '';
  const sat = l.sats.find((s) => s.norad === e.norad);
  const t = sat?.tx[e.tx];
  if (!sat || !t) return `${e.name} is no longer listed on this server`;
  if (!t.reach) return t.why || 'out of this receiver’s range';
  if (sat.ageState === 'expired' || sat.ageState === 'none') return ageText(sat);
  return '';
}

// ── THE WORDS ──────────────────────────────────────────────────────────────────────────────────────────────────────
const who = (e: Pick<SatSchedEntry, 'name' | 'label'>) => (e.label ? `${e.name} · ${e.label}` : e.name);
/** The scheduled row's own line: when it tunes, and what an overlap did to it. */
export function satSchedRowText(e: SatSchedEntry, slot: SatSchedSlot | undefined, all: SatSchedEntry[]): string {
  if (!slot) return 'SCHEDULED';
  const name = (k: string) => { const o = all.find((x) => x.key === k); return o ? who(o) : 'another pass'; };
  const why = (b: 'prio' | 'higher') => (b === 'prio' ? 'is prioritised' : 'is the higher pass');
  if (slot.state === 'skipped') {
    const w = slot.lostTo[0];
    return `SCHEDULED · skipped — ${w ? `${name(w.key)} ${why(w.by)}` : 'another pass wins'} for the whole of it`;
  }
  const pieces = slot.segs.map(([s, t]) => `${hhmm(s)}–${hhmm(t)}`).join(', ');
  if (slot.state === 'part') {
    const w = slot.lostTo[0];
    return `SCHEDULED · ${pieces} only — ${w ? `${name(w.key)} ${why(w.by)}` : 'another pass wins'} for the overlap`;
  }
  const beats = slot.over.length ? ` · wins the overlap with ${name(slot.over[0])}${e.prio ? ' (prioritised)' : ' (the higher pass)'}` : '';
  return `SCHEDULED · tunes ${pieces}, then back${beats}`;
}
/** The schedule's status line: running now, or the next one. '' = nothing scheduled. */
export function satSchedNowText(run: SatSchedRun, entries: SatSchedEntry[], plan: Map<string, SatSchedSlot>, nowSec: number): string {
  if (run.key) {
    const e = entries.find((x) => x.key === run.key);
    const seg = satSchedTimeline(plan).find((s) => s.key === run.key && s.start <= nowSec && nowSec < s.end);
    if (e) {
      const back = run.took ? 'you have taken over — nothing will be put back'
        : run.prev ? `then back to ${run.prev.sat && run.prev.pick ? 'your satellite' : `${satHzText(run.prev.hz)} ${run.prev.mode.toUpperCase()}`}` : '';
      return `SCHEDULE · on ${who(e)}${seg ? ` until ${hhmm(seg.end)}` : ''}${back ? `, ${back}` : ''}`;
    }
  }
  const next = satSchedTimeline(plan).find((s) => s.end > nowSec);
  if (!next) return '';
  const e = entries.find((x) => x.key === next.key);
  return e ? `SCHEDULE · next: ${who(e)} at ${hhmm(next.start)} (in ${hmIn(next.start, nowSec)})` : '';
}
