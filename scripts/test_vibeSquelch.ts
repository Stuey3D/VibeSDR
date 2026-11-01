/**
 * VIBESERVER SQUELCH ON/OFF (src/services/vibeSquelch.ts) — a threshold below -100 dBFS is a threshold.
 *
 * Stuart, 2026-10-14: auto squelch on an Airspy HF+ (127.875 MHz AM, quiet channel ≈ -110 dBFS, needle ~S3)
 * drew its red line in the right place and never closed the gate; on an RSP1A on MW a hand-set threshold in
 * the bottom third of the bar snapped to OFF. The wire said "db <= -100 = off", and the app clamped every
 * threshold to -100 (= off) while the web clamped to -99 (= shut on everything quieter).
 *
 * Proves:
 *   • the airband numbers: the OLD pipeline turns auto squelch's threshold into OFF; the new one closes on
 *     the noise and opens on a signal;
 *   • on/off travel in words, and OFF still reads as off to an older server (db -100);
 *   • against a server without hwinfo `sqlOn` a threshold at or below -100 is OFF here too (honest UI);
 *   • remembered thresholds: a pref saved before `squelchOn` existed still reads -100 as off;
 *   • no client keeps a second copy of the old rule (the clamps that were the bug).
 * The server half (vibe_squelch_msg.h) is proved by vibeserver/test-squelch-msg.cpp.
 *
 * Run: node --no-warnings scripts/test_vibeSquelch.ts
 */
import { readFileSync } from 'node:fs';
import { VS_SQL_OFF, vsSqlOn, vsSqlEffective, vsSquelchMsg, vsSqlRestore } from '../src/services/vibeSquelch.ts';

let fails = 0;
const ok = (c: boolean, m: string) => { if (c) console.log('  ✓ ' + m); else { fails++; console.log('  ✗ ' + m); } };

/** The server's rule (vibe_squelch_msg.h), mirrored: explicit `on` wins, else the legacy db > -100. */
const serverOn = (m: { db: number; on?: boolean }, speaksOn = true) =>
  speaksOn && typeof m.on === 'boolean' ? m.on && Number.isFinite(m.db) : m.db > -100;
/** The server's gate: closed while on and the channel is below the threshold. */
const gateClosed = (m: { db: number; on?: boolean }, chan: number, speaksOn = true) =>
  serverOn(m, speaksOn) && chan < m.db;

// The app's bar map for a VibeServer, and its exact inverse (SDRScreen posOf / onSquelchDrag), no trim.
const posOf = (db: number) => Math.max(0, Math.min(1, (db + 130) / 90));
const fromPos = (c: number) => c * 90 - 130;

console.log('── the airband case (Airspy HF+, 127.875 MHz AM) ──');
// A quiet channel at -110 ± 3.5 dB; the auto tracker follows the noise MINIMUM, so its baseline sits near
// -113.5 and threshold = baseline + 12 = -101.5 → -102 on the wire. Every quiet sample is below it.
const noise = Array.from({ length: 200 }, (_, i) => -110 + 3.5 * Math.sin(i * 1.7) * Math.cos(i * 0.31));
const base = Math.min(...noise);
const decide = Math.round(base + 12);
ok(decide <= -100 && decide > -110, `auto threshold lands at ${decide} dBFS — inside the old "off" range`);
// OLD: onLocalSquelch(Math.max(-100, fromPos(posOf(decide)))) → {db} → server: <= -100 = OFF.
const oldMsg = { db: Math.max(-100, fromPos(posOf(decide))) };
ok(!serverOn(oldMsg), 'OLD pipeline: the threshold went out as OFF (the bug, reproduced)');
ok(noise.every(n => !gateClosed(oldMsg, n)), 'OLD pipeline: the gate never closed on the noise');
// NEW: the inverse is exact and the message says on.
const newMsg = vsSquelchMsg(vsSqlEffective(fromPos(posOf(decide)), true));
ok(newMsg.on === true && Math.abs(newMsg.db - decide) < 1e-9, `NEW: {db:${newMsg.db}, on:true}`);
ok(noise.every(n => gateClosed(newMsg, n)), 'NEW: the gate is CLOSED on every quiet sample');
ok(!gateClosed(newMsg, -92), 'NEW: a -92 dBFS voice opens it');
// The hang's "held open" is a genuine OFF on both servers, not a threshold.
const hang = vsSquelchMsg(VS_SQL_OFF);
ok(!serverOn(hang) && !serverOn(hang, false), 'the hang sends OFF, and an older server reads it as OFF too');

console.log('── the MW case (RSP1A, signal S4 ≈ -100) ──');
// Dragging to a quarter of the bar = -107.5 dBFS — just above a -110 noise floor.
const drag = fromPos(0.25);
ok(Math.abs(drag + 107.5) < 1e-9, 'a quarter of the bar is -107.5 dBFS');
ok(vsSqlOn(vsSqlEffective(drag, true)), 'NEW: that is a threshold on a current server (was OFF, handle snapped left)');
ok(!vsSqlOn(vsSqlEffective(drag, false)), 'against an older server (no sqlOn) it is OFF here too — never a gate the server is not applying');
ok(vsSqlEffective(-95, false) === -95, 'above -100 an older server applies it, so it stays');

console.log('── the message ──');
ok(JSON.stringify(vsSquelchMsg(-115)) === '{"type":"squelch","db":-115,"on":true}', 'on → {db, on:true}');
ok(JSON.stringify(vsSquelchMsg(VS_SQL_OFF)) === '{"type":"squelch","db":-100,"on":false}', 'off → db -100 (the legacy off) + on:false');
ok(!vsSquelchMsg(NaN).on && !vsSquelchMsg(Infinity).on, 'a non-finite threshold is never on');

console.log('── remembered thresholds ──');
ok(vsSqlRestore(-100, undefined) === VS_SQL_OFF, 'legacy pref -100 (no squelchOn) = off');
ok(vsSqlRestore(-110, undefined) === VS_SQL_OFF, 'legacy pref -110 = off, as the old server applied it');
ok(vsSqlRestore(-90, undefined) === -90, 'legacy pref -90 = on');
ok(vsSqlRestore(-110, true) === -110, 'new pref -110 + squelchOn:true = on at -110');
ok(vsSqlRestore(-80, false) === VS_SQL_OFF, 'squelchOn:false = off whatever the number');
ok(vsSqlRestore(undefined, undefined) === VS_SQL_OFF && vsSqlRestore('x', true) === VS_SQL_OFF, 'missing or junk = off');

console.log('── no second copy of the old rule ──');
const src = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const app = src('src/screens/SDRScreen.tsx');
// ★ Code, not prose: the notes that explain the bug quote it.
ok(!/Math\.max\(-100, (c \*|prefs\.squelch)/.test(app), 'SDRScreen: no Math.max(-100, …) clamp on a threshold');
ok(!/Squelch(Ref\.current)?\s*>\s*-100/.test(app), 'SDRScreen: no "squelch > -100" on/off test');
const web = src('web/client/src/main.ts');
ok(!/(Math\.max\(|\? )SQL_OFF \+ 1/.test(web), 'web: no SQL_OFF + 1 (-99) clamp');
ok(!/squelchDb > -100/.test(web) && !/_squelchDb > -100/.test(src('web/client/src/audio.ts')), 'web: no "squelchDb > -100" on/off test');
// ★ V12 VFO A/B (387ff7cd4 web, 94bc4954c app, 2026-10-26): each VFO has its own squelch, so the body first hands
//   VFO B's threshold to the watch channel — and A's (the only VFO when A/B is off) still goes out as vsSquelchMsg.
const body = (s: string, sig: string) => { const i = s.indexOf(sig); if (i < 0) return ''; let d = 0;
  for (let k = s.indexOf('{', i); k < s.length; k++) { if (s[k] === '{') d++; else if (s[k] === '}' && --d === 0) return s.slice(i, k + 1); }
  return ''; };
ok(/this\._send\(vsSquelchMsg\(db\)\);\s*\}$/.test(body(src('web/client/src/spectrum.ts'), 'setSquelch(db: number) {')), 'web: spectrum.setSquelch sends vsSquelchMsg');
ok(/this\._sendCtl\(vsSquelchMsg\(db\)\);\s*\}$/.test(body(src('src/services/VibeServerWsClient.ts'), 'setSquelchDb(db: number) {')), 'app: VibeServerWsClient.setSquelchDb sends vsSquelchMsg');

if (fails) { console.log(`\n${fails} FAILED`); process.exit(1); }
console.log('\nall passed');
