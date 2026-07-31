// protocolLog.ts — a ring buffer of SERVER MESSAGES WE DO NOT HANDLE.
//
// ★★★ WHY THIS EXISTS. Three separate investigations in one day came down to "a server said
// something and we ignored it in silence":
//   • UberSDR dropping a listener with no warning.
//   • KiwiSDR booting some clients after ~30 seconds — still unexplained.
//   • Kiwi's rn/rt session countdown, which we only found by reading their JavaScript.
// Each cost hours and would have taken minutes with one line of evidence.
//
// ★★ The adapters already logged these through `dbg()` — but `onDbg` IS NOT CONSUMED ANYWHERE IN
// THE UI, so on a release build that logging goes nowhere. The reports that matter come from
// TestFlight and from users, never from a dev console, so evidence has to survive into a build a
// real person is running. This buffer is read by services/diagnostics.ts, which the About screen
// can already export and share.
//
// ★ Local only, and deliberately small: message TYPES and a short prefix, never payload bodies,
// so nothing sensitive is captured and the export stays readable.

export interface ProtoLine { ts: number; backend: string; text: string; }

const MAX = 40;
const ring: ProtoLine[] = [];

/** Record a message the client received and had no handler for. */
export function noteUnhandled(backend: string, text: string): void {
  ring.push({ ts: Date.now(), backend, text: text.slice(0, 160) });
  if (ring.length > MAX) ring.shift();
}

/** Newest last. Used by buildDiagnostics(). */
export function unhandledLog(): ProtoLine[] { return ring.slice(); }
