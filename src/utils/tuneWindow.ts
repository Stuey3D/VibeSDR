/**
 * ★★★ WHERE A LISTENER MAY PUT THE DIAL — the backend's range, narrowed to the owner's LOCKED
 * WINDOW when the VibeServer has its centre pinned.
 *
 * Stuart, 2026-10-18 (V12 484, Mac): "the app seems to be ignoring the boundaries of a locked vfo
 * centre and is allowing me to tune to black space outside of the locked range even though its not
 * doing anything, the web client is not effected." Every tune path in SDRScreen clamped to
 * `caps.freqRange` — the RADIO's coverage (100 kHz–1.766 GHz on an RTL) — and nothing narrowed it
 * to the window the owner had pinned. The server clamped the request into the window and said
 * nothing back, so the readout sat on a frequency the radio was not on, out in the black past the
 * capture walls. Never right on main either: the app has drawn those walls since 2026-09-09 but
 * no tune ever looked at them.
 *
 * ★ THE RULE IS THE SERVER'S, mirrored, because the server is the one that decides
 *   (local_sdr_shim.cpp, retune: "LOCKED CENTRE … CLAMP the VFO into it"): the dial may sit within
 *   lockedCentre ± (Fs/2 − min(50 kHz, 0.15·Fs)). The guard band is the server's — it keeps the VFO
 *   off the anti-alias roll-off — and a client that clamped to the bare ±Fs/2 (the web client does)
 *   could still show a figure up to 50 kHz from where the server actually parked it.
 * ★ CLAMPED, NOT REFUSED, for every continuous gesture — drum, keys, drag, crown, a tap on the
 *   waterfall — matching both the server and the web client: asking past the edge gets you the
 *   edge, and the walls on the waterfall show why. A TYPED frequency is the exception (the
 *   frequency card refuses it with the range, as the web's does) — see rangeMessage.
 * ★ The DIAL is clamped, not the passband: a 10 kHz AM filter on the edge hangs half over the
 *   wall, exactly as the server runs it. Same as the web client.
 * ★ Pure functions, so the rule can be tested without a screen (scripts/test_tune_window.ts).
 */

/** The server's guard band inside a locked window (local_sdr_shim.cpp retune: `limit`). */
export function lockedGuardHz(fsHz: number): number {
  return Math.min(50_000, fsHz * 0.15);
}

/** The owner's locked window — where the server will let the dial sit — or null when the centre
 *  is not locked or the capture width is not known yet. */
export function lockedWindow(lockedCentreHz: number, fsHz: number): [number, number] | null {
  if (!(lockedCentreHz > 0) || !(fsHz > 0)) return null;
  const half = fsHz / 2 - lockedGuardHz(fsHz);
  if (!(half > 0)) return null;
  return [lockedCentreHz - half, lockedCentreHz + half];
}

/** The range every tune clamps into: the backend's range, narrowed to the locked window when there
 *  is one. ★ The window WINS — it is the stricter of the two and the only one the server enforces
 *  on a listener — but it is intersected with the radio's range so a window pinned near 0 Hz cannot
 *  offer a negative frequency. Should the two not overlap at all (a stale freqRange), the window
 *  is still what the server will honour, so it is returned whole. */
export function tuneBounds(freqRange: [number, number],
                           win: [number, number] | null): [number, number] {
  if (!win) return freqRange;
  const lo = Math.max(freqRange[0], win[0]);
  const hi = Math.min(freqRange[1], win[1]);
  return lo <= hi ? [lo, hi] : win;
}

export function clampHz(hz: number, bounds: [number, number]): number {
  return Math.max(bounds[0], Math.min(bounds[1], hz));
}

/** What the frequency card says when a typed frequency is outside what this receiver covers. */
export function rangeMessage(bounds: [number, number]): string {
  return `This receiver covers ${(bounds[0] / 1e6).toFixed(3)}–${(bounds[1] / 1e6).toFixed(3)} MHz`;
}

/** ★ V12: can this receiver be TUNED to `hz` — inside the clamp range (`tuneBounds`) and, where the server lists them
 *  (vibeserver.json `tunable`, after the owner's allow/block lists), inside one of its tunable windows. The web's
 *  `clampTune(hz) === hz` test for its channel / band keys (DSC, JTTY): a key whose frequency the radio cannot reach is
 *  not drawn (AGENTS.md: no dead controls). An empty `tunable` = the server did not say: the bounds alone decide. */
export function hzReachable(hz: number, bounds: [number, number], tunable?: ReadonlyArray<readonly [number, number]> | null): boolean {
  if (!(hz >= bounds[0] && hz <= bounds[1])) return false;
  if (!tunable || !tunable.length) return true;
  return tunable.some(([lo, hi]) => hz >= lo && hz <= hi);
}

/** vibeserver.json's `tunable` ([[lo, hi], …] Hz), checked: pairs of finite numbers, ordered, at most 32. */
export function parseTunable(raw: unknown): Array<[number, number]> {
  if (!Array.isArray(raw)) return [];
  const out: Array<[number, number]> = [];
  for (const r of raw.slice(0, 32)) {
    if (!Array.isArray(r) || r.length < 2) continue;
    const lo = Number(r[0]), hi = Number(r[1]);
    if (Number.isFinite(lo) && Number.isFinite(hi) && lo >= 0 && hi > lo) out.push([lo, hi]);
  }
  return out;
}
