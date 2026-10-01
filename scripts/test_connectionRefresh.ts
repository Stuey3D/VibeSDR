/**
 * CONNECTION REFRESH (src/services/connectionRefresh.ts) — what the Servers menu's refresh rebuilds,
 * and when the row is offered at all.
 *
 * Proves:
 *   • every backend the radio screen hosts gets a refresh, and the AUDIO owner matches the screen's
 *     own mount gates (native Opus for UberSDR, the native pump for VibeServer / the phone's dongle,
 *     the adapter for OWRX / Kiwi / Web-888);
 *   • a VibeServer is ALWAYS the local pump — it reaches the screen as isLocal, never as the native
 *     Opus engine (that mix-up is how a fix for one audio path missed the other, 2026-08-16);
 *   • the row is hidden where it could only be a no-op: compatibility mode, no radio chosen yet, a
 *     refusal card, Kiwi's refusal card, FM-DX;
 *   • the native Opus restart waits for the new registration, and stands aside during a takeover.
 *
 * Run: node --no-warnings scripts/test_connectionRefresh.ts
 */
import {
  connectionRefreshPlan, canRefreshConnection, restartAudioOnRegister,
} from '../src/services/connectionRefresh.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.log(`  FAIL ${what}\n       got  ${g}\n       want ${w}`);
}

// ── The plan, per backend ──────────────────────────────────────────────────────
const PLANS: Array<[string | undefined, boolean, string]> = [
  ['ubersdr',    false, 'native-after-register'],
  [undefined,    false, 'native-after-register'],   // the screen's default type is ubersdr
  ['vibeserver', true,  'local-pump'],              // a remote VibeServer AND this phone's own
                                                    // dongle (loopback shim) — both isLocal
  ['ubersdr',    true,  'local-pump'],              // local wins whatever the type says
  ['owrx',       false, 'adapter'],
  ['kiwi',       false, 'adapter'],
  ['web888',     false, 'adapter'],
  ['vibeserver', false, 'none'],                    // never routed this way; nothing plays it
];
for (const [type, local, audio] of PLANS) {
  const p = connectionRefreshPlan(type, local);
  eq(`${type ?? '(default)'} local=${local} → audio`, p.audio, audio);
  eq(`${type ?? '(default)'} local=${local} → spectrum rebuilt`, p.spectrum, true);
  eq(`${type ?? '(default)'} local=${local} → decoders refreshed`, p.decoders, true);
}

// ── Availability ───────────────────────────────────────────────────────────────
for (const t of ['ubersdr', 'vibeserver', 'owrx', 'kiwi', 'web888', undefined]) {
  eq(`${t ?? '(default)'} healthy session → offered`, canRefreshConnection({ serverType: t }), true);
}
eq('fmdx → hidden', canRefreshConnection({ serverType: 'fmdx' }), false);
eq('compatibility mode → hidden', canRefreshConnection({ serverType: 'kiwi', compatOnly: true }), false);
eq('no radio chosen yet → hidden', canRefreshConnection({ serverType: 'vibeserver', noSessionYet: true }), false);
eq('refusal card → hidden', canRefreshConnection({ serverType: 'vibeserver', refused: true }), false);
eq('kiwi refusal card → hidden', canRefreshConnection({ serverType: 'kiwi', kiwiRefused: true }), false);
eq('every flag false → offered', canRefreshConnection({
  serverType: 'ubersdr', compatOnly: false, noSessionYet: false, refused: false, kiwiRefused: false,
}), true);

// ── The native Opus restart ────────────────────────────────────────────────────
eq('pending, no admin → restart on register', restartAudioOnRegister(true, false), true);
eq('pending, admin held → onConnect restarts instead', restartAudioOnRegister(true, true), false);
eq('not pending → never (an ordinary connect)', restartAudioOnRegister(false, false), false);
eq('not pending, admin → never', restartAudioOnRegister(false, true), false);

console.log(`connectionRefresh: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
