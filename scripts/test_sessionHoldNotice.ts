/**
 * THE "VibeSDR paused" NOTE (src/services/sessionHoldNotice.ts) — the app half of the V12 held session (2026-10-27).
 *
 * Stuart: "when the OS suspends the app we send a little notification … your session on the server is held for 3 minutes
 * all decoders are still running for this period and no data is missed"; "if audio playing the app will happily stay
 * all day in the background so nobody ever sees that notification if audio is in use"; "if a user has a signal in
 * squelch mode waiting to hear an incoming we must not allow the OS to kill the app in the background, that is the one
 * time where silence is justified"; "if the user hits a hard time limit within that grace period then that is the hard
 * rule".
 *
 * Proves:
 *   • ★ SQUELCHED IS NOT QUIET: no map mode = the audio session is kept (audioQuietFor false) — and SDRScreen's quiet rule
 *     is audioQuietFor and names no squelch, mute or decoder state (read from the source, so a later edit cannot slip a
 *     squelch into it unnoticed);
 *   • a map mode (ADS-B, NDB, ISM, AIS) = quiet → the note is planned; live audio (squelched or not) → never;
 *   • the words: held for N minutes, come back by HH:MM; a hard limit sooner → its time, said plainly;
 *   • never on Android, never for a non-VibeServer, never when the server does not hold (holdS 0 / older server);
 *   • a deadline under 20 s is not announced; the permission is asked once, only in a quiet mode, only undetermined.
 *
 * Run: node --no-warnings scripts/test_sessionHoldNotice.ts   (run-tests.sh does)
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { audioQuietFor, noticePlan, holdWords, shouldAskPermission, NOTICE_DELAY_S, type NoticeInput } from '../src/services/sessionHoldNotice.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const yes = (what: string, c: boolean) => eq(what, c, true);

const NONE = { adsb: false, ism: false, ndb: false, ais: false };
// ── squelch keeps the session; map modes release it ───────────────────────────────────────────────────────────────────
yes('no map mode (a squelched listener waiting on a signal) → NOT quiet: the audio session stays', audioQuietFor(NONE) === false);
for (const k of ['adsb', 'ism', 'ndb', 'ais'] as const) yes(`${k} → quiet`, audioQuietFor({ ...NONE, [k]: true }));

// ★ SDRScreen's quiet rule IS audioQuietFor, and mentions no squelch / mute / decoder.
{
  const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
  const src = fs.readFileSync(path.join(root, 'src/screens/SDRScreen.tsx'), 'utf8');
  const line = src.split('\n').find((l) => /const audioQuiet\s*=/.test(l)) || '';
  yes(`SDRScreen derives audioQuiet from audioQuietFor (${line.trim()})`, /audioQuietFor\(/.test(line));
  yes('…and nothing about squelch, mute or a decoder feeds it', !/sql|squelch|mute|decod/i.test(line));
  const calls = src.match(/setNativeQuiet\(/g) || [];
  yes(`setNativeQuiet is called only from the quiet rule and the unmount release (${calls.length} calls)`, calls.length <= 3);
  // ★ the native side: setQuiet is the ONLY door that releases the session for silence, and squelch never reaches it
  const sw = fs.readFileSync(path.join(root, 'ios/VibeSDR/VibePowerModule.swift'), 'utf8');
  const at = sw.indexOf('func setSquelchOpen');
  const sqlFn = at < 0 ? '' : sw.slice(at, sw.indexOf('@objc func', at + 10));
  yes('iOS setSquelchOpen neither pauses the engine nor releases the session',
      sqlFn.length > 20 && !/setActive\(false|\.pause\(\)|\.stop\(\)|setQuiet/.test(sqlFn));
}

// ── the plan ──────────────────────────────────────────────────────────────────────────────────────────────────────────
const NOW = Date.UTC(2026, 9, 27, 14, 0, 0);
const clock = (ms: number) => { const d = new Date(ms); return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`; };
const base: NoticeInput = { platform: 'ios', isVibeServer: true, terms: { holdS: 180, hardAt: 0 }, quiet: true, now: NOW,
                            serverName: 'Pi 500', clock };
{
  const p = noticePlan(base);
  yes('ADS-B in the background on iOS → a note', p.post);
  if (p.post) {
    eq('title', p.title, 'VibeSDR paused');
    eq('body', p.body, "iOS pauses apps that aren't playing sound. Your place on Pi 500 is held for 3 minutes and its decoders "
                     + 'keep running, so nothing is missed. Come back by 14:03 to carry on.');
    eq('deadline = now + 180 s', p.deadline, NOW + 180_000);
    yes('no exclamation marks', !/!/.test(p.title + p.body));
  }
}
eq('★ audio live (squelched or playing) → never a note', noticePlan({ ...base, quiet: false }).post, false);
eq('Android → never (its media service keeps the app running)', noticePlan({ ...base, platform: 'android' }).post, false);
eq('not a VibeServer → never', noticePlan({ ...base, isVibeServer: false }).post, false);
eq('older server (no terms) → never', noticePlan({ ...base, terms: null }).post, false);
eq('idle grace 0 (holdS 0) → never', noticePlan({ ...base, terms: { holdS: 0, hardAt: 0 } }).post, false);
{
  const p = noticePlan({ ...base, terms: { holdS: 60, hardAt: 0 } });
  yes('owner grace 1 minute → "held for 1 minute", back by 14:01', p.post && /held for 1 minute and/.test(p.body) && /14:01/.test(p.body));
}
{
  const p = noticePlan({ ...base, terms: { holdS: 180, hardAt: NOW + 75_000 } });
  yes('★ a hard limit sooner → the limit, said plainly', p.post && p.byLimit && /reaches its time limit at 14:01/.test(p.body)
      && /Come back before 14:01/.test(p.body) && !/3 minutes/.test(p.body));
  if (p.post) eq('…the deadline is the limit', p.deadline, NOW + 75_000);
}
{
  const p = noticePlan({ ...base, terms: { holdS: 180, hardAt: NOW + 600_000 } });
  yes('a hard limit LATER than the hold → the hold', p.post && !p.byLimit && /held for 3 minutes/.test(p.body));
}
eq('a limit 10 s away → not announced', noticePlan({ ...base, terms: { holdS: 180, hardAt: NOW + 10_000 } }).post, false);
eq('no server name → "the server"', (noticePlan({ ...base, serverName: '' }) as { body: string }).body.includes('place on the server'), true);
eq('holdWords', [holdWords(180), holdWords(60), holdWords(30), holdWords(150)], ['3 minutes', '1 minute', '30 seconds', '3 minutes']);
yes('posted a few seconds after backgrounding', NOTICE_DELAY_S >= 2 && NOTICE_DELAY_S <= 8);

// ── permission ────────────────────────────────────────────────────────────────────────────────────────────────────────
const ask = { platform: 'ios', isVibeServer: true, quiet: true, terms: { holdS: 180, hardAt: 0 }, status: 'undetermined' as const, askedBefore: false };
yes('first quiet mode on a holding VibeServer, undetermined → ask', shouldAskPermission(ask));
eq('asked before → never again', shouldAskPermission({ ...ask, askedBefore: true }), false);
eq('denied → never', shouldAskPermission({ ...ask, status: 'denied' }), false);
eq('granted → nothing to ask', shouldAskPermission({ ...ask, status: 'granted' }), false);
eq('not quiet (listening, squelched or not) → not now', shouldAskPermission({ ...ask, quiet: false }), false);
eq('server does not hold → not now', shouldAskPermission({ ...ask, terms: { holdS: 0, hardAt: 0 } }), false);
eq('Android → never', shouldAskPermission({ ...ask, platform: 'android' }), false);

console.log(`${fails ? '✗' : '✓'} test_sessionHoldNotice: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
