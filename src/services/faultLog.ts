// faultLog.ts — ONE BAD THING MUST COST ONE THING.
//
// ★★★ WHY THIS EXISTS. A broken RDS message once took down the whole app, and an AdvRdsPanel
// render throw unmounted the entire SDR screen and dumped the user back at the directory
// (1adcd21b). In both cases the radio was fine — audio still flowing, spectrum still arriving — and
// the one thing that had gone wrong was a single message or a single panel. Stuart, 2026-09-28:
// "the hardening we discussed to prevent a bad packet from taking down the whole app".
//
// So every socket handler and every independently mountable panel routes its failures HERE:
//   • the failure is CONTAINED by the caller (the message is dropped, the panel is closed);
//   • it is COUNTED per source+kind, so a type that fails every frame is visible as such;
//   • it is LOGGED — console.error with the stack — but RATE-LIMITED per key, so a flood of bad
//     frames (twenty spectrum frames a second) cannot drown the console or the JS thread;
//   • and it lands in the Diagnostics report (services/diagnostics.ts), because the reports that
//     matter come from TestFlight and from users, never a dev console.
//
// ★★ NEVER A SILENT CATCH. `catch {}` around a message handler is how the old code hid a whole
//    class of fault: the message vanished, nothing counted it, and the symptom surfaced somewhere
//    unrelated. A dropped message is fine; an UNRECORDED dropped message is not.
// ★ Pure TS, no react-native import — so scripts/test_faultLog.ts can exercise it under tsx, and so
//   nothing in here can itself throw during a fault.

export interface FaultEntry {
  /** Where: 'vibe-spec', 'uber-audio', 'panel:AdvRds', … */
  source: string;
  /** What: the message type ('rds', 'config', 'binary'), or 'render' for a panel. */
  kind: string;
  count: number;
  firstTs: number;
  lastTs: number;
  /** The first failure's message — the first sighting is the whole diagnostic value. */
  firstError: string;
  /** And the most recent, in case it changed. */
  lastError: string;
  stack?: string;
}

/** A log line for the same key at most this often; the count keeps climbing in between. */
export const LOG_INTERVAL_MS = 10_000;
const MAX_KEYS = 60;

const faults = new Map<string, FaultEntry & { loggedAt: number; suppressed: number }>();
let total = 0;
type Sink = (line: string, err: unknown) => void;
// ★ Overridable for the unit test, which must not print a wall of expected errors.
// ★ The line carries text from the network (a message type, a bad frame's summary), so it is an
//   ARGUMENT, never the format string: a stray %s or %c in it would otherwise garble the one log
//   this module exists to keep, and swallow `err` as its substitution (CodeQL js/tainted-format-string).
let sink: Sink = (line, err) => { console.error('%s', line, err); };
export function _setFaultSink(s: Sink | null): void {
  sink = s ?? ((line, err) => { console.error('%s', line, err); });
}
let now: () => number = () => Date.now();
export function _setFaultClock(fn: (() => number) | null): void { now = fn ?? (() => Date.now()); }

function describe(err: unknown): string {
  if (err instanceof Error) return `${err.name}: ${err.message}`;
  try { return String(err); } catch { return 'unprintable error'; }
}

/** Record one contained failure. Never throws. */
export function noteFault(source: string, kind: string, err: unknown, detail?: string): void {
  try {
    total++;
    const key = source + '|' + kind;
    const t = now();
    const text = (describe(err) + (detail ? ` [${detail}]` : '')).slice(0, 300);
    let e = faults.get(key);
    if (!e) {
      if (faults.size >= MAX_KEYS) {
        // ★ Evict the stalest key, never refuse the new one: a NEW failure is the interesting one.
        let oldK: string | null = null; let oldT = Infinity;
        for (const [k, v] of faults) if (v.lastTs < oldT) { oldT = v.lastTs; oldK = k; }
        if (oldK) faults.delete(oldK);
      }
      e = { source, kind, count: 0, firstTs: t, lastTs: t, firstError: text, lastError: text,
            stack: err instanceof Error && typeof err.stack === 'string' ? err.stack.slice(0, 2000) : undefined,
            loggedAt: -Infinity, suppressed: 0 };
      faults.set(key, e);
    }
    e.count++;
    e.lastTs = t;
    e.lastError = text;
    if (t - e.loggedAt >= LOG_INTERVAL_MS) {
      const more = e.suppressed ? ` (+${e.suppressed} more since last log, ${e.count} total)` : '';
      e.loggedAt = t;
      e.suppressed = 0;
      sink(`[fault] ${source} ${kind}: dropped — ${text}${more}`, err);
    } else {
      e.suppressed++;
    }
  } catch {
    /* ★ The ONE deliberate empty catch in the whole hardening: the fault recorder failing must
     *   never become a second fault. There is nowhere left to report it to. */
  }
}

/** Run `fn`; if it throws, record the fault and return false. The message/work is dropped, the
 *  caller carries on. Use per message TYPE so one bad handler cannot stop the others. */
export function guard(source: string, kind: string, fn: () => void, detail?: string): boolean {
  try { fn(); return true; } catch (err) { noteFault(source, kind, err, detail); return false; }
}

/** Best-effort type of a parsed message, for keying — never throws. */
export function msgKind(msg: unknown): string {
  try {
    const t = (msg as { type?: unknown } | null)?.type;
    return typeof t === 'string' && t ? t.slice(0, 32) : 'untyped';
  } catch { return 'untyped'; }
}

/** Parse JSON text and hand it to `handle`, guarding the parse and the handler separately so a
 *  parse failure and a handler failure are counted as what they are. Returns false if dropped. */
export function guardJson(source: string, text: string, handle: (msg: Record<string, unknown>) => void): boolean {
  let msg: Record<string, unknown>;
  try { msg = JSON.parse(text) as Record<string, unknown>; }
  catch (err) { noteFault(source, 'bad-json', err, `len=${text.length} head=${JSON.stringify(text.slice(0, 40))}`); return false; }
  if (!msg || typeof msg !== 'object') { noteFault(source, 'bad-json', new Error('not an object'), typeof msg); return false; }
  return guard(source, msgKind(msg), () => handle(msg));
}

/** Snapshot, most frequent first. */
export function faultSummary(): FaultEntry[] {
  return [...faults.values()]
    .map(({ loggedAt: _l, suppressed: _s, ...e }) => e)
    .sort((a, b) => b.count - a.count);
}
export function faultTotal(): number { return total; }
export function _resetFaults(): void { faults.clear(); total = 0; }

/** Wrap every function on a callbacks object so a throw inside one (a UI section updating from a
 *  message) is contained to that callback: logged, counted under `source`/`cb:<name>`, and the
 *  caller — the socket client, mid-way through its own state update — carries on.
 *  ★ A Proxy, so a callback assigned AFTER construction is guarded too. Wrappers are cached so a
 *    callback read twice is the same function (identity matters to add/removeEventListener). */
export function guardCallbacks<T extends object>(source: string, cb: T): T {
  const cache = new Map<PropertyKey, { fn: unknown; wrapped: unknown }>();
  return new Proxy(cb, {
    get(target, key, recv) {
      const v = Reflect.get(target, key, recv) as unknown;
      if (typeof v !== 'function') return v;
      const hit = cache.get(key);
      if (hit && hit.fn === v) return hit.wrapped;
      const name = String(key);
      const wrapped = function (this: unknown, ...args: unknown[]) {
        try { return (v as (...a: unknown[]) => unknown).apply(this === recv ? target : this, args); }
        catch (err) { noteFault(source, 'cb:' + name, err); return undefined; }
      };
      cache.set(key, { fn: v, wrapped });
      return wrapped;
    },
  });
}
