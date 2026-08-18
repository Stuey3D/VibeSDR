/**
 * audioPathLog.ts — what the audio path is actually doing, for the diagnostics report.
 *
 * ★★★ WHY THIS EXISTS. A whole night went into a fault whose entire visible symptom was "no audio,
 *     and tuning does nothing" — and every diagnosis was inferred from the SERVER's log, because
 *     the app could not say anything about itself. The server can only report sockets that ARRIVE;
 *     it has nothing to say about one that was never opened, which was the actual fault twice over
 *     (first the wrong component, then a gate that closed it). Four fixes were aimed at the wrong
 *     layer for want of one line of local truth.
 *
 * ★★ THE TUNING DEPENDS ON IT TOO, which is why it degrades so badly. In this app the audio socket
 *    carries the tune and every control message — the spectrum socket is display-only. So a single
 *    refused audio connection takes sound, tuning and the hardware panel with it, and presents as
 *    "the radio is ignoring me" rather than as an audio fault. Recording which component is
 *    mounted, what its gates evaluated to, and what its socket did, names that in one glance.
 *
 * ★ Deliberately tiny and synchronous: a ring of the last few transitions plus the current gate
 *   values. No timers, no I/O, nothing that can itself fail while the thing it is watching fails.
 */

interface AudioPathState {
  /** Which component owns audio: LocalAudioPlayer (a VibeServer or local hardware) or AudioPlayer
   *  (a plain UberSDR server). "none" means NEITHER is mounted, which is a fault in itself. */
  component: string;
  /** The gates that decide whether a socket is opened at all. */
  gates: Record<string, string | number | boolean | null>;
  /** Last few socket transitions, newest last. */
  events: string[];
}

const state: AudioPathState = { component: 'none', gates: {}, events: [] };
const kMaxEvents = 12;

function stamp(): string {
  const d = new Date();
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`
       + `:${String(d.getSeconds()).padStart(2, '0')}`;
}

/** Which component is mounted, and the values its socket gate depends on. Called on render, so it
 *  must stay cheap — it is a couple of assignments. */
export function noteAudioPath(component: string, gates: AudioPathState['gates']): void {
  state.component = component;
  state.gates = gates;
}

/** A socket transition worth remembering: opening, open, closed, refused, error. */
export function noteAudioEvent(what: string): void {
  state.events.push(`${stamp()} ${what}`);
  while (state.events.length > kMaxEvents) state.events.shift();
}

/** Lines for the diagnostics report. */
export function audioPathDump(): string[] {
  const out: string[] = [];
  out.push(`component : ${state.component}`);
  const g = Object.entries(state.gates);
  out.push(`gates     : ${g.length ? g.map(([k, v]) => `${k}=${String(v)}`).join('  ') : '(none)'}`);
  // ★ The gates are the point. "component=LocalAudioPlayer port=null" says the socket was never
  //   opened and WHY, which no amount of server-side log can tell you.
  // ★★★ SAY "NOTHING WAS RECORDED", NOT "NOTHING HAPPENED". This line used to read "the socket was
  //     never opened" whenever the ring was empty — and only LocalAudioPlayer ever called
  //     noteAudioEvent, so a report from the UberSDR path stated, as a finding, something it had no
  //     instrumentation to know. It sent us to the wrong layer on issue #20 while the real fault
  //     (a session the server had never registered) was three sockets away.
  // ★★ An absence of evidence printed as evidence of absence is worse than a blank line: the blank
  //    line makes you go and look.
  if (!state.events.length) out.push('events    : NONE RECORDED — no instrumentation reported in');
  else for (const e of state.events) out.push(`  ${e}`);
  return out;
}
