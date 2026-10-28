/**
 * ★ V12 `&wide=1` only for a server that advertised the full-capture row (src/services/wideCapsCore.ts).
 * Proves: an unknown / older server (no "wide", or "wide":1) gets NO suffix — its URL is unchanged; one that said
 * "wide":2 gets it on its next socket (and after a restart: the list is stored); a downgrade forgets it; servers are kept
 * apart; a broken store is the old behaviour.
 * Run: node --no-warnings scripts/test_wide_caps.ts
 */
import { WideCapsCore, serverKey, WIDE_CAPS_KEY } from '../src/services/wideCapsCore.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; if (!c) console.log(`   FAIL ${what}`); };
const mem = new Map<string, string>();
const store = { get: async (k: string) => mem.get(k) ?? null, set: async (k: string, v: string) => { mem.set(k, v); } };

const A = 'https://Stuey3d.freemyip.com:8073/', B = 'http://192.168.1.11:8073';
const c = new WideCapsCore(store);
await c.load();
ok(c.suffix(A) === '' && c.suffix(B) === '', 'an unknown server: no wide=1 (the URL as it always was)');
c.note(A, undefined); ok(c.suffix(A) === '', 'hwinfo without "wide" (an older server): no wide=1');
c.note(A, 1);         ok(c.suffix(A) === '', '"wide":1 (a format this app does not read): no wide=1');
ok(c.note(A, 2) && c.suffix(A) === '&wide=1' && c.suffix('https://stuey3d.freemyip.com:8073') === '&wide=1', '"wide":2: wide=1 on its next socket (same server, any spelling)');
ok(c.suffix(B) === '', '…and only for that server');
ok(!c.note(A, 2), 'the same answer again changes nothing (hwinfo comes on every retune)');
await new Promise((r) => setTimeout(r, 0));
const c2 = new WideCapsCore(store); await c2.load();
ok(c2.suffix(A) === '&wide=1' && JSON.parse(mem.get(WIDE_CAPS_KEY)!).length === 1, 'remembered across a restart');
c2.note(A, undefined); ok(c2.suffix(A) === '', 'a server that stops advertising it (a downgrade) is forgotten');
const bad = new WideCapsCore({ get: async () => { throw new Error('x'); }, set: async () => { throw new Error('y'); } });
await bad.load(); ok(bad.suffix(A) === '' && (bad.note(A, 2), bad.suffix(A) === '&wide=1'), 'a broken store: nothing remembered, nothing breaks');
ok(serverKey('HTTP://Host:1/x/') === 'http://host:1/x', 'the key: scheme + host lower-cased, no trailing slash');

console.log(`   wide caps: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
