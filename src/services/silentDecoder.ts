/**
 * silentDecoder.ts — ★★ THE SILENT-DECODER RULE, one copy for the web page and the app (V12 beta91, 2026-11-22).
 *
 * Some decoders decode something nobody listens to: APRS packets are a screech (Stuart, 2026-11-10: "we need to make APRS
 * a silent decoder"), and a METEOR LRPT downlink is a hiss of data (Stuart, 2026-11-22: "i guess for meteor lrpt the
 * audio needs muting too"). Band digital spots' rule exactly:
 *   · muted on the way in, and on the way out put back EXACTLY as it was before (a listener who unmutes meanwhile may —
 *     it is their speaker; leaving still puts back the state from before the decoder);
 *   · with VFO B on, only the VFO the decoder runs on is taken out of the mix — the speaker carries the other VFO, the one
 *     being listened to; B coming or going moves the mute between that VFO and the speaker;
 *   · from one silent decoder straight to another (APRS → LRPT) the mute simply stays, and the state from before the
 *     first comes back when the last closes.
 *
 * ★ It is the CLIENT'S OWN MUTE (the web page's gain / the app's native mute in place), never a stopped stream: the audio
 *   stream keeps a background Safari tab alive (a silent tab is suspended — the ISM lesson), and the two sockets are how
 *   the server counts the listener.
 *
 * Paging (PAGE) is NOT here: its mute is the law's and cannot be undone while it runs (SDRScreen prePagingMuteRef).
 */
import type { VfoId } from './vfoAB';

/** The decoders whose audio is muted while they run. */
/** ★ 2026-11-24 + ORBCOMM (Stuart: "can the nfm from orbcomm be muted too") — its downlink is data hiss, as METEOR's. */
export const SILENT_DECODERS = ['aprs', 'lrpt', 'orbcomm'] as const;
export type SilentDecoder = typeof SILENT_DECODERS[number];

export function isSilentDecoder(d: string | null | undefined): d is SilentDecoder {
  return !!d && (SILENT_DECODERS as readonly string[]).includes(d);
}

/** Its name for the muted banner. */
export function silentDecoderName(d: SilentDecoder): string { return d === 'lrpt' ? 'METEOR LRPT' : d === 'orbcomm' ? 'ORBCOMM' : 'APRS'; }

/** The app's banner while a silent decoder holds the speaker — a key: the listener may unmute, and it stands. */
export function silentMutedBanner(d: SilentDecoder): string {
  return `\u{1F507} AUDIO MUTED WHILE ${silentDecoderName(d)} RUNS — TAP TO UNMUTE`;
}

export interface SilentMuteIO {
  /** Mute (true) or put back (false / the saved state) the client's own speaker. */
  setSpeaker(muted: boolean): void;
  /** Take one VFO out of the mix (true) or put it back (false) — the server's watch mute. */
  setVfoMute(v: VfoId, muted: boolean): void;
}

export interface SilentState {
  /** A silent decoder is running for this listener. */
  open: boolean;
  /** VFO B is on. */
  abOn: boolean;
  /** The VFO the decoder runs on. */
  vfo: VfoId;
  /** The speaker's mute now; null = no speaker yet (nothing to save, nothing to mute — the next sync does it). */
  speakerMuted: boolean | null;
  /** The app: a speaker that came back unmuted while the decoder holds it (a reconnect — every native start clears its
   *  mute) is muted again, unless the LISTENER unmuted it (listenerUnmuted). The web page's speaker is never unmuted
   *  behind its back, so it leaves this off. */
  reassert?: boolean;
}

export class SilentMute {
  /** The speaker's state from before the decoder, while the decoder holds the speaker; null = not holding it. */
  private from: boolean | null = null;
  /** The VFO taken out of the mix, while the decoder holds one. */
  private vfo: VfoId | null = null;
  /** The listener unmuted while the decoder held the speaker — it stands until the decoder closes. */
  private listenerSpoke = false;

  get holdsSpeaker(): boolean { return this.from !== null; }
  get holdsVfo(): VfoId | null { return this.vfo; }
  /** The speaker state that will come back when the decoder closes (null = not holding it). */
  get savedSpeaker(): boolean | null { return this.from; }

  /** Called on every decoder change and whenever B comes or goes (and in the app on every mute change). Idempotent. */
  sync(s: SilentState, io: SilentMuteIO): void {
    const wantV: VfoId | null = s.open && s.abOn ? s.vfo : null;
    if (this.vfo && this.vfo !== wantV) {
      const v = this.vfo; this.vfo = null;
      if (s.abOn) io.setVfoMute(v, false);   // ★ B gone: nothing to put back on a VFO that is not there
    }
    if (wantV && this.vfo !== wantV) { this.vfo = wantV; io.setVfoMute(wantV, true); }
    const wantSpk = s.open && !s.abOn;
    if (wantSpk && this.from === null) {
      if (s.speakerMuted === null) return;
      this.from = s.speakerMuted; this.listenerSpoke = false;
      io.setSpeaker(true);
    } else if (wantSpk && s.reassert && s.speakerMuted === false && !this.listenerSpoke) {
      io.setSpeaker(true);
    } else if (!wantSpk && this.from !== null) {
      const back = this.from; this.from = null; this.listenerSpoke = false;
      io.setSpeaker(back);
    }
  }

  /** The listener unmuted (a key, the lock screen, a watch) — while the decoder holds the speaker, that stands. */
  listenerUnmuted(): void { if (this.from !== null) this.listenerSpoke = true; }

  /** Leaving the screen: the speaker as it was before the decoder. */
  release(io: Pick<SilentMuteIO, 'setSpeaker'>): void {
    if (this.from === null) return;
    const back = this.from; this.from = null; this.listenerSpoke = false; this.vfo = null;
    io.setSpeaker(back);
  }
}
