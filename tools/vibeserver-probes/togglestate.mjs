// Does a RECONNECTING listener get told the radio's ACTUAL sticky DSP state?
//
// The bug: NR/notch/RF+DAB notch/bias-T all survive a listener leaving, but hwinfo only ever
// mentioned `nr` and `notch`, and only as bare booleans — so a fresh page drew its own saved
// prefs and the control lied about the radio. Stuart, 2026-08-10: auto notch enabled on the
// Airspy still read OFF after switching to the RSP1B, "as toggling the switch refreshed it.
// Same with the RF/DAB notches and any NR figure set with the slider too".
//
// ★★★ THE STRENGTH IS THE HALF A BOOLEAN CANNOT CARRY, and it is the half that hid. With
//     `nr:true` alone the switch agreed with the radio and the NUMBER did not — which is far
//     harder to spot than a switch in the wrong position, because nothing looks wrong.
//
// ★★ RUN IT AGAINST A DIRECT SERVER, not a shared one. On a shared receiver NR and the notch
//    are behind sharedGate() — an unauthenticated probe's writes are REFUSED, so the test
//    fails for a reason that has nothing to do with what it is testing. Ask me how I know.
//        vibeserver --tcp 127.0.0.1:1234 --port 48078      # then:
//        node togglestate.mjs ws://127.0.0.1:48078
//
// Usage: [RADIO=<id>] node togglestate.mjs [ws://host:port]
import crypto from 'node:crypto';

// ★★★ SAY WHO WE ARE. Node's WebSocket sends a bare `User-Agent: node`, so every probe run
//     against a server lands in its connection log looking like an unidentified client — 32 of
//     them on the public demo in one afternoon, filed under "other", inflating the connection
//     history and the client mix an owner uses to judge real usage (Stuart, 2026-08-11: "what is
//     a node connection?").
// ★★ Which is this repo's own lesson biting again: A PROBE IS PART OF THE SYSTEM IT MEASURES.
//    Last time it was asking for 4096 bins and loading the shared FFT for ten other people.
// ★ `headers` is undici's non-standard extension to the WHATWG WebSocket — verified reaching the
//   server, which is the only thing that matters here.
const BASE = process.argv[2] || 'ws://127.0.0.1:48077';
const RADIO = process.env.RADIO || '';
const pre = `${BASE}${RADIO ? `/r/${RADIO}` : ''}/ws/user-spectrum`;
const KEYS = ['nr','notch','nrStrength','rfNotch','dabNotch','rspBiasT','biasT'];

const open = () => new Promise((res, rej) => {
  const sid = 'tog' + crypto.randomBytes(4).toString('hex');
  const ws = new WebSocket(`${pre}?user_session_id=${sid}&bins=1024&mode=binary8`, { headers: { 'User-Agent': 'VibeSDR-probe/1.0 (togglestate)' } });
  ws.binaryType = 'arraybuffer';
  const t = setTimeout(() => rej(new Error('no hwinfo in 8s')), 8000);
  ws.onmessage = m => {
    if (typeof m.data !== 'string') return;
    let j; try { j = JSON.parse(m.data); } catch { return; }
    if (j.type === 'hwinfo') { clearTimeout(t); res({ ws, info: j }); }
  };
  ws.onerror = e => { clearTimeout(t); rej(new Error('socket error')); };
});
const pick = m => Object.fromEntries(KEYS.filter(k => k in m).map(k => [k, m[k]]));
const sleep = ms => new Promise(r => setTimeout(r, ms));

const a = await open();
console.log('first connect  :', JSON.stringify(pick(a.info)));
a.ws.send(JSON.stringify({ type: 'nr', on: true, strength: 0.77 }));
a.ws.send(JSON.stringify({ type: 'notch', on: true }));
await sleep(800);
a.ws.close();                      // the listener LEAVES — the state must survive
await sleep(600);
const b = await open();            // a fresh page, as a reload is
console.log('after reconnect:', JSON.stringify(pick(b.info)));
const i = b.info;
const ok = i.nr === true && i.notch === true &&
           typeof i.nrStrength === 'number' && Math.abs(i.nrStrength - 0.77) < 0.02;
console.log(ok ? '\nPASS — the server states nr, notch AND the strength it is really using'
               : '\nFAIL — a reconnecting client still cannot render the truth');
b.ws.close();
process.exit(ok ? 0 : 1);
