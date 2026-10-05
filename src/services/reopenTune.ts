/**
 * reopenTune — when the SPECTRUM socket is reopened (resume from background, an auto-reconnect,
 * a re-registered session), does this client carry its own tune back to the server?
 *
 * ★★★ THE BUG IT ANSWERS (Stuart, 2026-10-05, iOS, the Pi 500's SDRplay RSP on a LOCKED RANGE with
 *     several independent VFOs): "putting the app in the background to reply on Discord killed the
 *     socket and when I went back to it the frequency reset to 4778 every time."
 *     A per-listener VFO lives on the server's ClientDsp, which is keyed by the spectrum SOCKET and
 *     dies with it. The returning socket built a fresh channel at the owner's landing frequency,
 *     the config said vfo=landing, and this client — holding no remembered tune, because only
 *     connect() ever set one — adopted it as if another listener had moved the dial. On a
 *     per-listener VFO nobody else CAN move it; the only thing that moved it was the reconnect.
 *
 * ★★★ THE SHARED-DIAL CONTRACT IS UNTOUCHED, and that is the whole of the safety argument:
 *     "RECOVERY IS NOT A USER ACTION" is a rule about ONE dial shared by a room. On a shared dial
 *     a reopen restores nothing — `sharedDial` refuses here, and the config handler drops any
 *     remembered tune the moment the server's `shared` says so (VibeServerWsClient, "the server
 *     owns this VFO; dropping the remembered tune"). Two gates, either of which is enough.
 *     On an INDEPENDENT VFO the dial is the listener's own — exactly the case that handler's own
 *     comment keeps the remembered tune for ("✗ Left in place for a PER-LISTENER VFO").
 *
 * ★ Not on the FIRST open: connect() has already decided, and a first visit with no memory
 *   (allowServerDefault) must take the server's landing without argument.
 * ★ Not in DAB: the multiplex is the tuning, and tune() refuses there anyway.
 * ★ A tune already pending wins — it is newer, or it is connect()'s own.
 * ★ The server is the PRIMARY fix (it now resumes a returning session's VFO — local_sdr_shim.cpp,
 *   sessionVfo); this is the client half, for servers that have not been updated (the two
 *   Android-hosted ones have no automatic update path). Against an updated server the restore
 *   finds the server already on our frequency and sends nothing (the >500 Hz test in the handler).
 */
import type { SDRMode } from './sdrProtocol';

export interface ReopenTuneInput {
  /** This client has had at least one `config` — i.e. this is a REopen, not the first open. */
  hadConfig:  boolean;
  /** The server has told us (dial message) this receiver shares one dial. */
  sharedDial: boolean;
  /** A DAB multiplex holds the dial. */
  inDab:      boolean;
  /** A tune already waiting to be restored, if any. */
  pending:    { frequency: number; mode: SDRMode } | null;
  /** Where this client believes its own VFO is. */
  frequency:  number;
  mode:       SDRMode;
}

export function reopenRestoreTune(s: ReopenTuneInput): { frequency: number; mode: SDRMode } | null {
  if (s.pending) return s.pending;
  if (!s.hadConfig || s.sharedDial || s.inDab) return null;
  if (!Number.isFinite(s.frequency) || s.frequency <= 0) return null;
  return { frequency: s.frequency, mode: s.mode };
}
