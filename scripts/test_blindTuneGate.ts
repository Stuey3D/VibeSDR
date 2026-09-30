/**
 * THE BLIND-TUNE GATE (src/services/blindTuneGate.ts) — may the lock-screen / headset / car ⏮⏭,
 * a car-list pick or Siri move this receiver?
 *
 * Stuart, 2026-09-30: "We don't give our shared VFO servers the same treatment as FM-DX — all the
 * media controls are currently unlocked."
 *
 * Proves, as a full table (server type × dial state × action):
 *   • FM-DX: every tune input refused, exactly as native always did (`fmdxAudio`);
 *   • an ordinary receiver (no dial message, or 'exclusive'): everything allowed, as before;
 *   • a shared dial, alone (FREE TO TUNE): allowed — nobody to disturb;
 *   • a shared dial with others listening (ASK TO TUNE): every tune refused;
 *   • spectator: refused, unless you are the unlocked admin (then the open rule applies);
 *   • listen-only (SpyServer, no control): refused;
 *   • a voice STEP choice is this client's own setting, never gated;
 *   • the ⏮⏭ switch and a press agree (one rule), and the banner's "alone" is the gate's "alone".
 *
 * Run: node --no-warnings scripts/test_blindTuneGate.ts
 */
import {
  blindTuneAllowed, blindTuneReason, mediaSkipEnabled, dialAlone, blindTuneRefusal,
  type BlindTuneAction, type BlindTuneInput,
} from '../src/services/blindTuneGate.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.log(`  FAIL ${what}\n       got  ${g}\n       want ${w}`);
}

const TUNES: BlindTuneAction[] = ['next', 'prev', 'carPick', 'voiceTune', 'voiceMode'];
const ALL: BlindTuneAction[] = [...TUNES, 'voiceStep'];
const SERVERS = ['vibeserver', 'ubersdr', 'owrx', 'kiwi', 'fmdx'];

type DialCase = { name: string; dial: BlindTuneInput['dial']; readOnly?: boolean; admin?: boolean;
                  want: 'allow' | 'deny'; reason: string };
const CASES: DialCase[] = [
  { name: 'no dial yet',               dial: null,                                     want: 'allow', reason: 'ok' },
  { name: 'exclusive',                 dial: { mode: 'exclusive', listeners: 3 },      want: 'allow', reason: 'ok' },
  { name: 'shared · free (alone)',     dial: { mode: 'open', listeners: 1 },           want: 'allow', reason: 'ok' },
  { name: 'shared · ask (2 listening)',dial: { mode: 'open', listeners: 2 },           want: 'deny',  reason: 'others-listening' },
  { name: 'shared · ask (5 listening)',dial: { mode: 'open', listeners: 5 },           want: 'deny',  reason: 'others-listening' },
  { name: 'spectator · listener',      dial: { mode: 'spectator', listeners: 1 },      want: 'deny',  reason: 'spectator' },
  { name: 'spectator · admin alone',   dial: { mode: 'spectator', listeners: 1 }, admin: true, want: 'allow', reason: 'ok' },
  { name: 'spectator · admin, others', dial: { mode: 'spectator', listeners: 3 }, admin: true, want: 'deny', reason: 'others-listening' },
  { name: 'listen-only (SpyServer)',   dial: null, readOnly: true,                     want: 'deny',  reason: 'listen-only' },
];

for (const server of SERVERS) {
  for (const c of CASES) {
    const inp: BlindTuneInput = { serverType: server, dial: c.dial, readOnly: c.readOnly, admin: c.admin };
    for (const a of ALL) {
      const label = `${server} · ${c.name} · ${a}`;
      let want: boolean, reason: string;
      if (a === 'voiceStep')      { want = true;  reason = 'not-a-tune'; }
      else if (server === 'fmdx') { want = false; reason = 'fmdx-shared-tuner'; }
      else                        { want = c.want === 'allow'; reason = c.reason; }
      eq(label + ' allowed', blindTuneAllowed(inp, a), want);
      eq(label + ' reason', blindTuneReason(inp, a), reason);
    }
    // ★ ONE RULE: the switch native is told is exactly what a press would get.
    eq(`${server} · ${c.name} · switch == press`, mediaSkipEnabled(inp),
       blindTuneAllowed(inp, 'next') && blindTuneAllowed(inp, 'prev'));
  }
}

// ★ The banner's FREE TO TUNE and the gate's "alone" are one test.
eq('alone at 1', dialAlone({ listeners: 1 }), true);
eq('not alone at 2', dialAlone({ listeners: 2 }), false);

// ★ A refused voice/car tune must say WHY — a silent no reads as a broken feature.
for (const r of ['others-listening', 'spectator', 'listen-only', 'fmdx-shared-tuner'] as const) {
  eq(`refusal text for ${r} is non-empty`, blindTuneRefusal(r).length > 0, true);
}
eq('no refusal text when allowed', blindTuneRefusal('ok'), '');

// ★ Every tune action is gated identically — no input left live on its own.
const shared = { serverType: 'vibeserver', dial: { mode: 'open', listeners: 3 } };
eq('every blind TUNE refused on a busy shared dial', TUNES.filter(a => blindTuneAllowed(shared, a)), []);

console.log(`\nblind-tune gate: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
