/* ★★★ THE PAGE'S HEARTBEAT RUNS ON A WORKER'S CLOCK (docs/v12/WEB-KEEPALIVE.md, Stuart's Pi 500, 2026-11-22).
 *
 * The server releases a proto ≥ 1 session that sends no APPLICATION message for 30 s (local_sdr_shim.cpp kAppLivenessMs —
 * the rule that frees the single-occupant slots a forgotten Jr or watch used to hold). The page's only such message while
 * nobody touches anything is the 5 s ping, and that ran on window.setInterval. A hidden tab that is not playing audible
 * sound has its main-thread timers THROTTLED by the browser:
 *   • WebKit (Safari): Page::updateTimerThrottlingState — a visually idle page that is not Visible / Audible / Loading /
 *     Capturing gets TimerThrottlingState::EnabledIncreasing: the DOM timer alignment grows with the time since it was
 *     hidden (Page::updateDOMTimerAlignmentInterval, "alignment interval increases exponentially"), up to the client's
 *     limit. A muted Web Audio graph counts as SILENT there (DefaultAudioDestinationNode::render → setIsSilent on the
 *     bus's silent flag — a GainNode at 0 zeroes the bus). Audible pages keep a 1 s alignment.
 *   • Chromium: intensive throttling — a page hidden 5 min, silent 30 s, wakes timer chains once a MINUTE.
 * Either way the ping fell to about one a minute, and the server called the listener dead: the freeze-and-reconnect
 * once a minute. ★ Dedicated WORKER timers are not part of that: WebKit's alignment is the Document's (Page →
 * Document::domTimerAlignmentInterval); a worker's context uses ScriptExecutionContext's default, 0 — no hidden-page
 * alignment. Chromium's intensive throttling is for the page's own timers too. So the tick comes from a tiny Blob worker
 * and is posted here; message events are not timers and are not aligned.
 * ★ A worker that cannot start (no Worker, a policy that refuses blob: workers) falls back to the old window timer —
 *   never worse than before.
 * ★ ONE worker for every subscriber on the page (a spectrum socket per VFO, the pocket links) — not one per socket. */

type Sub = { every: number; next: number; fn: () => void };

const SRC = `let t = null;
self.onmessage = (e) => {
  const ms = e.data && e.data.ms;
  if (t !== null) { clearInterval(t); t = null; }
  if (ms > 0) t = setInterval(() => self.postMessage(0), ms);
};`;

/** The worker ticks at this rate; each subscriber runs when its own period is up (checked against performance.now()). */
const TICK_MS = 1000;

let worker: Worker | null = null;
let fallback: number | null = null;
let tried = false;
const subs = new Set<Sub>();

function onTick() {
  const now = performance.now();
  for (const s of Array.from(subs)) {
    if (now < s.next - TICK_MS / 2) continue;   // ★ half a tick early is on time — else a 5 s beat drifts to 6 s on a 1 s clock
    // ★ the next due time from NOW, not from the missed one: a tick that came late (a busy main thread) sends one
    //   heartbeat, not a burst of the ones it missed
    s.next = now + s.every;
    try { s.fn(); } catch (err) { console.warn('[keepAlive] a heartbeat threw', err); }
  }
}

function startClock() {
  if (worker || fallback !== null) return;
  if (!tried) {
    tried = true;
    try {
      if (typeof Worker !== 'undefined' && typeof Blob !== 'undefined' && typeof URL !== 'undefined' && URL.createObjectURL) {
        const url = URL.createObjectURL(new Blob([SRC], { type: 'application/javascript' }));
        const w = new Worker(url);
        URL.revokeObjectURL(url);
        w.onmessage = onTick;
        // ★ a worker that dies (a policy refusing it after construction) hands over to the window timer
        w.onerror = () => { try { w.terminate(); } catch { /* gone */ } if (worker === w) { worker = null; useFallback(); } };
        worker = w;
      }
    } catch { worker = null; }
  }
  if (worker) { worker.postMessage({ ms: TICK_MS }); return; }
  useFallback();
}
function useFallback() {
  if (fallback === null && subs.size) fallback = window.setInterval(onTick, TICK_MS);
}
function stopClock() {
  if (worker) worker.postMessage({ ms: 0 });
  if (fallback !== null) { clearInterval(fallback); fallback = null; }
}

/** Run `fn` every `everyMs` (≥ 1 s) on the worker's clock, for as long as the returned stop() is not called. */
export function heartbeat(everyMs: number, fn: () => void): () => void {
  const s: Sub = { every: Math.max(TICK_MS, everyMs), next: performance.now() + Math.max(TICK_MS, everyMs), fn };
  subs.add(s);
  startClock();
  return () => {
    if (!subs.delete(s)) return;
    if (!subs.size) stopClock();
  };
}

/** Which clock is running (tests and the diagnostics): 'worker', 'window', or '' when nothing subscribes. */
export function heartbeatClock(): 'worker' | 'window' | '' {
  if (!subs.size) return '';
  return worker ? 'worker' : fallback !== null ? 'window' : '';
}
