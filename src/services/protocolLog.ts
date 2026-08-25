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
/** ★ Per-type tally, so a repeat updates its existing line instead of adding one. */
const counts = new Map<string, { n: number; line: ProtoLine; base: string }>();

/** Record a message the client received and had no handler for. */
/* ★★★ ONE ENTRY PER MESSAGE TYPE, NOT PER MESSAGE — THE BUFFER WAS EATING ITSELF.
 *     This was written for messages that arrive occasionally: a liveness probe, a countdown, a
 *     field a server added. Two of ours arrive TWENTY TIMES A SECOND EACH — `sig` and `adc`, the
 *     signal and converter meters — so forty entries a second poured through a forty-deep ring and
 *     nothing survived in it for longer than a second. Stuart's diagnostic dump was ONE HUNDRED
 *     PER CENT sig/adc: the section whose whole purpose is to reveal the unexpected showed only
 *     the utterly expected, and every genuinely unhandled message of that session was gone.
 * ★★★ AND IT WAS NOT FREE. Each one cost a JSON.stringify, a slice, a push and — once full — an
 *     Array.shift of the whole ring, on the JS thread, forty times a second, for ever.
 *  ★★ KEEP THE FIRST AND COUNT THE REST. The first sighting is the whole diagnostic value; the
 *     ten-thousandth adds nothing. A repeat only refreshes the count and the timestamp, so a
 *     high-rate type occupies exactly one slot however long it runs — and the fact that it IS
 *     high-rate is itself reported, which the old design hid.
 *  ★ Keyed on backend+type, taken from the text up to the first quote-delimited type. */
export function noteUnhandled(backend: string, text: string): void {
  const key = backend + '|' + (text.match(/^unhandled "([^"]+)"/)?.[1] ?? text.slice(0, 24));
  const seen = counts.get(key);
  if (seen) {
    seen.n++;
    seen.line.ts = Date.now();
    seen.line.text = `${seen.base}  (x${seen.n})`;
    return;
  }
  const base = text.slice(0, 140);
  const line: ProtoLine = { ts: Date.now(), backend, text: base };
  counts.set(key, { n: 1, line, base });
  ring.push(line);
  if (ring.length > MAX) {
    const dropped = ring.shift();
    // ★ Forget the counter with the line, or a type that scrolls off can never be logged again.
    if (dropped) for (const [k, v] of counts) if (v.line === dropped) { counts.delete(k); break; }
  }
}

/** Newest last. Used by buildDiagnostics(). */
export function unhandledLog(): ProtoLine[] { return ring.slice(); }
