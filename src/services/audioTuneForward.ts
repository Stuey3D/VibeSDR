/**
 * ★★★ MAY THE AUDIO SOCKET SAY `tune`? — the one rule LocalAudioPlayer's forwarding effect asks.
 *
 * THE SHARED-DIAL CONTRACT: the server sends full state, the client has no vote, and the client transmits only
 * on a USER action. LocalAudioPlayer is the app's tune door on a VibeServer (Vibe.sendLocalTune / its own ws):
 * an effect on the screen's frequency, mode and passband. It could not tell a person's change from the screen
 * ADOPTING the server's dial — so once unsealed, every move somebody else made came straight back as a `tune`.
 *
 * ★★★ MEASURED ON THE LENOVO (RSP1A, shared dial, ask-before-tuning on), 2026-10-22 00:07–00:10: Safari tuned
 *     657, 666, 675, 684, 693 … and session 574d — Stuart's V12 iPhone — sent the SAME frequency back after each
 *     step, within the second, and sometimes a step BEHIND (684 after 693; 666 after 675) because the screen
 *     renders the adopted value after the next config has already arrived. That stale echo drags the radio
 *     back under the person tuning it.
 *
 * ★★ WHY THE VALUES AND NOT ONLY THE COUNTER. `userTuneSeq` is bumped by hand at the screen's tune sites, and
 *    some never bump (the drum, the sweep) — a counter-only rule made those dead (Stuart, 2026-08-25, "lost all
 *    my tuning"). So: a change is the SERVER'S OWN STATE when it equals what the server last reported, in the
 *    SAME status snapshot (VibeServerWsClient writes `serverDial` beside the adopted frequency, synchronously, so
 *    a stale render carries a stale `serverDial` with it and still matches). Anything else is somebody here.
 *  ★ A bump of the counter always counts — the only way to re-assert the SAME values on purpose (a restore).
 *  ★ A tune TO where the server already is would change nothing anyway, so declining it costs nothing…
 *  ★★ …EXCEPT RIGHT AFTER OUR OWN SEND. Spin the drum up one step and straight back inside a round trip and the
 *     screen is back on the old frequency while `serverDial` still holds it too — the server has not confirmed
 *     the step yet. That return IS a person, and declining it would leave the radio a step away from the
 *     readout. So within OWN_TUNE_WINDOW_MS of this socket's last send a match is not an adoption — and it
 *     cannot be one: the client never adopts another listener's dial within 1.5 s of our own tune
 *     (VibeServerWsClient `settled`, ROOM_SETTLE_MS), and this window is shorter than that on purpose.
 *
 * Pure, no React — scripts/test_dial_echo.ts drives it through the evidence above.
 */

/** Below the client's 1.5 s adopt settle, so inside it a match can only be a person going back (see above). */
export const OWN_TUNE_WINDOW_MS = 1200;

export interface ServerDial {
  /** Hz, in the SAME units as the values compared against it (LocalAudioPlayer speaks hardware Hz). */
  frequency: number;
  /** Present when the server's config named it. */
  mode?: string;
  /** Present only when the server's config carried the room's passband (shared dials, 2026-10-17+). */
  bandwidthLow?: number;
  bandwidthHigh?: number;
}

export interface AskedTune {
  frequency: number;
  mode: string;
  bandwidthLow: number;
  bandwidthHigh: number;
}

/** True when `asked` is the dial (frequency + mode) the server last reported — the screen adopting, not a person. */
export function isServerDial(asked: AskedTune, dial: ServerDial | null | undefined): boolean {
  if (!dial || !(dial.frequency > 0)) return false;           // nothing heard yet: no opinion
  if (Math.abs(asked.frequency - dial.frequency) >= 1) return false;
  if (dial.mode && dial.mode.toLowerCase() !== String(asked.mode).toLowerCase()) return false;
  /* ★★ THE PASSBAND IS NOT COMPARED. It has its own door — setBandwidth sends `bandwidth` on the spectrum socket
   *  (VibeServerWsClient) — so this socket carrying it too was only ever a duplicate. Comparing it would make an
   *  adopted dial look like a person whenever the room's edges were not adopted alongside it (roomPassband
   *  declines another mode's edges, or anything inside our own settle) — the echo again, by the back door. */
  return true;
}

/**
 * The forwarding decision, given what this render holds.
 *   asked       — the frequency/mode/passband the screen holds now
 *   serverDial  — what the server last reported, from the same status snapshot
 *   seqAdvanced — userTuneSeq (+ restore nonce) moved since the last time this effect ran
 *   sealed      — a shared dial whose socket has not yet heard a person (LocalAudioPlayer's seal)
 *   helloTune   — the tune the socket opened with (the seal's own comparison)
 *   askedJson   — `asked` as the frame it would send
 *   msSinceSend — since this socket last sent a tune (Infinity if it never has)
 */
export function shouldForwardTune(o: {
  asked: AskedTune; serverDial?: ServerDial | null; seqAdvanced: boolean;
  sealed: boolean; helloTune: string; askedJson: string; msSinceSend: number;
}): { send: boolean; unseal: boolean; why: string } {
  // ★★★ THE CONTRACT: the server's own dial is never sent back to it, sealed or not.
  if (!o.seqAdvanced && !(o.msSinceSend < OWN_TUNE_WINDOW_MS) && isServerDial(o.asked, o.serverDial)) {
    return { send: false, unseal: false, why: 'adopted the server\'s dial — not a person' };
  }
  if (o.sealed) {
    // The socket merely restating its opening position, with nobody having asked.
    if (!o.seqAdvanced && o.askedJson === o.helloTune) return { send: false, unseal: false, why: 'still the hello' };
    return { send: true, unseal: true, why: 'a person asked — unsealed' };
  }
  return { send: true, unseal: false, why: 'a person asked' };
}
