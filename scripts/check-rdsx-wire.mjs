// ★★★ DO THE TWO ENDS OF THE ADVANCED-RDS MESSAGE AGREE ON THE FIELD NAMES?
//
// The server emitted `"R.rdsDev"` — a stray prefix, almost certainly a half-finished edit — while
// the client read `msg.rdsDev`. So the RDS deviation was `undefined`, rendered as a dash, and had
// NEVER ONCE POPULATED (Stuart, 2026-08-11, wanting it for Hans, who measures against a Pira).
//
// ★★ IT IS INVISIBLE FROM EITHER SIDE ALONE. The server "sends the deviation" and the client
//    "reads the deviation"; only the WIRE shows they are not the same word. Every neighbouring
//    field was spelled correctly, so nothing looked wrong in review — and no test touched it,
//    because both halves are individually correct.
//
// ★ So compare the two lists directly. A key sent and never read is dead weight or a typo; a key
//   read and never sent is a field that will silently stay at its default for ever.
import { readFileSync } from 'node:fs';

const cpp = readFileSync('android/app/src/main/cpp/local_sdr_shim.cpp', 'utf8');
const ts  = readFileSync('web/client/src/spectrum.ts', 'utf8');

// The rdsx builder: from `"type":"rdsx"` to the end of that JSON assembly.
const start = cpp.indexOf('\\"type\\":\\"rdsx\\"');
if (start < 0) { console.error('✗ could not find the rdsx builder'); process.exit(1); }
// ★★★ A FIXED WINDOW OVER THE BUILDER IS ITSELF A BUG WAITING TO HAPPEN. This was 6000 characters,
//     and adding four fields with their explanatory comments pushed `mpx` — the LAST thing the
//     builder emits — out to offset 6382. The checker then reported that the server "never sends
//     mpx", which was false: it was describing its own blind spot, in the confident voice it uses
//     for a real wire mismatch. A tool that cannot see the whole thing must not report on the part
//     it cannot see (2026-08-14).
// ★★ So end at the CLOSING of the JSON assembly rather than at an arbitrary length. The builder
//    finishes with the mpx array and a `sendWs`; taking everything up to that is exact, and it
//    cannot silently shrink again as comments are added.
// ★ The builder ends `j += "]}"; sendText(sock, j);` — so sendText is the exact boundary. My first
//   attempt used `sendWs`, which does not appear until well AFTER this function, so the window
//   over-ran into the session-limit code and started counting ITS fields (ok, relocked, idleMin,
//   secs) as advanced-RDS ones. Too small a window invents missing fields; too large a one invents
//   extra ones. Anchor on the real terminator.
const endMark = cpp.indexOf('sendText(sock, j)', start);
if (endMark < 0) { console.error('✗ could not find the end of the rdsx builder'); process.exit(1); }
const body = cpp.slice(start, endMark);
const sent = new Set([...body.matchAll(/\\"([A-Za-z][\w.]*)\\"\s*:/g)].map(m => m[1]));
sent.delete('type');

// The client's parser: ONLY the `case 'rdsx':` block. ★ A window of "roughly around there" swept
// in msg.* reads from the sig, rspstat, admin and hwinfo handlers and reported nineteen false
// alarms — a checker that cries wolf is one people switch off, which would have cost more than the
// bug it was written for.
const ci = ts.indexOf("case 'rdsx':");
if (ci < 0) { console.error('✗ could not find the rdsx parser'); process.exit(1); }
const cend = ts.indexOf('break;', ci);
const cbody = ts.slice(ci, cend > 0 ? cend : ci + 4000);
const read = new Set([...cbody.matchAll(/msg\.([A-Za-z][\w]*)/g)].map(m => m[1]));

// ★ Sent deliberately for OTHER readers — the phone and the watch parse these, and the nested
//   eon/oda entries carry their own pi/ps/aid. Listed so a genuinely new orphan still stands out;
//   a warning that always fires is a warning nobody reads.
const KNOWN_UNREAD = new Set(['ber', 'pi', 'ps', 'aid']);

let bad = 0;
// ★ A dotted key can only be a mistake: nothing in this protocol is namespaced.
for (const k of sent) {
  if (k.includes('.')) {
    console.error(`✗ the server sends "${k}" — a dotted key is always a typo here`);
    bad++;
  } else if (!read.has(k) && !KNOWN_UNREAD.has(k)) {
    console.warn(`  note: the server sends "${k}" and the web client never reads it`);
  }
}
for (const k of read) {
  if (!sent.has(k)) {
    console.error(`✗ the client reads msg.${k} but the server never sends it — it will stay at its default for ever`);
    bad++;
  }
}
if (bad) { console.error(`\n✗ ${bad} advanced-RDS field(s) do not agree across the wire`); process.exit(1); }
console.log(`ok   advanced RDS: ${sent.size} fields sent, and every field the client reads is one of them`);
