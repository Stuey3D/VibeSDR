/**
 * The executable spec for src/services/faultLog.ts — the guard every socket handler and panel
 * boundary in the app routes its failures through.
 *
 * Run: npx tsx scripts/test_faultLog.ts
 *
 * What it must prove:
 *   1. A throwing handler is contained (guard returns false, nothing escapes) and counted.
 *   2. A bad message does NOT stop the next good one — per-message isolation.
 *   3. Parse failures and handler failures are counted as different kinds, keyed by message type.
 *   4. Logging is rate-limited per key: a flood of 1000 bad frames logs ONCE in the window, and
 *      the next log line after the window reports how many were suppressed.
 *   5. Nothing is silent: every failure increments the total.
 */
import {
  guard, guardJson, noteFault, faultSummary, faultTotal, _resetFaults, _setFaultSink, _setFaultClock,
  LOG_INTERVAL_MS,
} from '../src/services/faultLog';

let fails = 0;
const ok = (cond: boolean, what: string) => {
  if (cond) console.log('  ok   ' + what);
  else { console.log('  FAIL ' + what); fails++; }
};

const logged: string[] = [];
_setFaultSink((line) => { logged.push(line); });
let t = 1_000_000;
_setFaultClock(() => t);

// 1. contained + counted
_resetFaults();
ok(guard('test', 'x', () => { throw new Error('boom'); }) === false, 'guard returns false on throw');
ok(guard('test', 'y', () => { /* fine */ }) === true, 'guard returns true on success');
ok(faultTotal() === 1, 'one fault counted');
ok(faultSummary()[0]?.firstError.includes('boom') ?? false, 'error text recorded');

// 2. a bad message does not stop the next good one
_resetFaults(); logged.length = 0;
const handled: string[] = [];
const stream = ['{"type":"rds","ps":"GOOD1"}', '{"type":"rds","ps":', '{"type":"rds","ps":42}', '{"type":"config","n":1}', 'null', '{"type":"rds","ps":"GOOD2"}'];
for (const text of stream) {
  guardJson('spec', text, (m) => {
    if (m.type === 'rds') {
      // A handler that trusts the wire, as the old code did: ps must be a string.
      handled.push((m.ps as string).trim());
    } else handled.push(String(m.type));
  });
}
ok(JSON.stringify(handled) === JSON.stringify(['GOOD1', 'config', 'GOOD2']), `good messages all handled around the bad ones (${handled.join(',')})`);
const kinds = Object.fromEntries(faultSummary().map((e) => [e.kind, e.count]));
ok(kinds['bad-json'] === 2, 'truncated JSON and a bare null counted as bad-json (2)');
ok(kinds['rds'] === 1, 'wrong-typed rds counted under its own type');
ok(faultTotal() === 3, 'three dropped, none silent');

// 4. rate-limited logging
_resetFaults(); logged.length = 0;
for (let i = 0; i < 1000; i++) noteFault('spec', 'binary', new RangeError('Offset is outside the bounds of the DataView'));
ok(logged.length === 1, `a flood of 1000 logs once (${logged.length})`);
ok(faultSummary()[0].count === 1000, 'but all 1000 are counted');
t += LOG_INTERVAL_MS;
noteFault('spec', 'binary', new RangeError('again'));
ok(logged.length === 2 && /\+999 more/.test(logged[1]), 'next window reports the suppressed count: ' + logged[1]);
noteFault('spec', 'other', new Error('different key'));
ok(logged.length === 3, 'a different key logs immediately');

// 5. the recorder itself never throws, even on a hostile error value
_resetFaults();
const hostile = { toString() { throw new Error('nope'); } };
let threw = false;
try { noteFault('x', 'y', hostile); } catch { threw = true; }
ok(!threw, 'noteFault survives an error whose toString throws');

_setFaultSink(null); _setFaultClock(null);
console.log(fails ? `\n${fails} FAILED` : '\nall passed');
process.exit(fails ? 1 : 0);
