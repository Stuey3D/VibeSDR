/* wideRowBus.ts — where a VibeServer's full-capture row (SPWB, V12 — src/utils/wfWide.ts) goes once the spectrum socket
 * has it: to the SDR screen's WaterfallView, which keeps it under the waterfall's history (JS path) or hands it to the
 * Android native engine (engine.pushWide). One sink — the mounted waterfall; nothing mounted = the row is dropped (it is
 * only ever the newest picture of the band, never history to catch up on).
 * ★ The Android NATIVE socket never comes through here: spec_socket.cpp gives the engine the row on the socket's thread.
 * ★ A row only arrives when the socket asked `&wide=1`, which it does only for a server whose hwinfo said "wide" ≥ 2
 *   (wideCaps.ts) — every other backend is untouched. */

type WideSink = (frame: Uint8Array) => void;
let sink: WideSink | null = null;

/** The waterfall registers (and on unmount clears — only its own). */
export function setWideSink(s: WideSink | null, owner?: WideSink): void {
  if (s === null && owner && sink !== owner) return;
  sink = s;
}
/** The spectrum socket's SPWB frame (the bytes as received). */
export function emitWide(frame: Uint8Array): void { sink?.(frame); }
