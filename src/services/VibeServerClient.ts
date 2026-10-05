// VibeServerClient.ts — VibeServer, and the home for everything it is about to grow.
//
// ★★★ WHY THIS EXISTS AT ALL. VibeServer's protocol was derived from UberSDR's — the website puts
//     it as VibeDSP + UberSDR = VibeServer — so for a year one class served both, told apart by a
//     mutable `isVibeServer` flag. The flag could only become true when a MESSAGE arrived, so
//     everything decided at connect time was decided as UberSDR and patched afterwards, and the
//     two products broke each other in both directions for a year. Stuart, 2026-09-08: "everytime
//     we seem to make changes to VibeServer it causes issues on UberSDR and Vice Versa".
//
// ★★★ AND IT IS A PRECONDITION, NOT A TIDY-UP. VibeServer is getting DAB (already in the web
//     client and the server), then ADS-B, AIS, ACARS and DRM. UberSDR has none of those. Every one
//     of them would have been another branch on that flag, in a file UberSDR also has to run.
//     They belong here, where UberSDR cannot see them and cannot be broken by them.
//
// ★★ WHAT IS *NOT* HERE, DELIBERATELY: everything both servers speak. That is SdrWsClient, one
//    copy, so a fix to the shared half still reaches both — which is the good half of the old
//    arrangement and worth keeping. Only the divergence is below.
import { VibeServerWsClient, LADDERS_FOR } from './VibeServerWsClient';
import { parseDabMessage, dabSafeText, type DabState } from './dabTypes';
import { holdNativeHealing } from '../components/AudioPlayer';
import { DabExitGuard } from './dabStepper';

export {
  MODE_BANDWIDTHS,
  type SDRMode,
  type SDRStatus,
  type IdlePolicy,
  type RdsExt,
  type RadioCaps,
  type SDRCallbacks,
} from './sdrProtocol';

export class VibeServerClient extends VibeServerWsClient {
  /** ★ How many spectrum bins to ask for. A VibeServer honours the request — that is what makes
   *  one waterfall serve tens of listeners at a quarter of the bandwidth. UberSDR ignores it. */
  static readonly BINS = VibeServerWsClient.VIBE_BINS;
  protected binsSuffix(): string { return `&bins=${VibeServerClient.BINS}`; }

  /** VibeServer's rungs. Its full rate is 20 fps, not UberSDR's 10 — asking a 20 fps server for
   *  10 and reading the difference as a failing link is exactly what the old late-flipping flag
   *  caused ("is auto link management set to UberSDR standards?"). */
  protected ladderFor(): number[] { return LADDERS_FOR.vibeserver; }

  /** ★★★ ONE LEVER, AND IT IS NOT A DIVISOR. VibeServer takes an absolute fftRate. The divisor
   *  compounds with whatever rung the controller already chose — ÷3 of the 3.3 floor is 1 fps —
   *  so the worse the link, the harder it was hit. A VibeServer must never be sent set_rate, and
   *  now it cannot be: this class does not implement it. */
  protected sendRateLever(): void { /* no divisor lever on a VibeServer — see sendPowersaveLever */ }

  protected sendPowersaveLever(targetFps: number): void {
    if (this.spectrumWs?.readyState !== WebSocket.OPEN || targetFps <= 0) return;
    this.rateDivisor = 1;                       // one lever only
    this.spectrumWs.send(JSON.stringify({ type: 'fftRate', value: targetFps }));
  }

  // ─── DAB ────────────────────────────────────────────────────────────────────────────────────
  //
  // ★★★ THIS IS WHY THE SPLIT HAD TO HAPPEN FIRST. UberSDR has no DAB, and every line below would
  //     otherwise have been another branch on the old mutable flag, in a file UberSDR also runs.
  //     ADS-B, AIS, ACARS and DRM land here next, for the same reason.

  /** Enter or leave DAB, optionally naming the multiplex (channel index) and service.
   *
   *  ★ THE SERVER OWNS THE DIAL IN DAB. It sets the radio to the block centre and the DAB sample
   *    rate itself; the client never tunes, because there is no VFO inside a multiplex. Setting
   *    `dabHeld` is what locks tune/zoom/pan/resetView out — see SdrWsClient.dabHeld, which is the
   *    single reader every one of those paths passes through.
   *
   *  ★★ THE HOLD IS SET BEFORE THE SEND AND CLEARED BEFORE THE SEND. A `dab off` that raced its
   *     own reply used to leave the view locked until the next connect; the lock is a client-side
   *     rule about what the user may ask for, so it belongs to the REQUEST, not to the answer. */
  /* ★★★ EVERY DAB TRANSITION HOLDS THE NATIVE SELF-HEAL — HERE, NOT AT THE CALL SITES. Entering,
   *     leaving and changing a multiplex, and picking a service, are seconds of frames arriving
   *     with nothing to play (acquisition + priming), which the heal judged a stall and "repaired"
   *     mid-lock (Stuart, 2026-09-29: "DAB worked previously to the new audio watchdog"). Every
   *     sender — the DAB button, the block drum, a DAB bookmark, a mode pick that leaves DAB, the
   *     wrist — ends in dab()/dabService(), so holding here holds for all of them (ONE RULE, ONE
   *     READER). 15 s for a multiplex change: a 32-bit server can take that long to lock. 6 s for
   *     a service inside the tuned multiplex. */
  static readonly DAB_MUX_HOLD_MS = 15_000;
  static readonly DAB_SERVICE_HOLD_MS = 6_000;
  /** The last multiplex / service the SERVER reported — a change we did not ask for (the owner's
   *  landing after a connect, another listener on the shared dial) is the same pause. */
  private dabSeenChannel: string | null = null;
  private dabSeenSid: number | null = null;

  /** ★★★ Reports that arrive after our own `dab off` are ghosts — see DabExitGuard (dabStepper.ts). */
  private dabExit = new DabExitGuard();

  dab(on: boolean, channel?: number, sid?: number) {
    holdNativeHealing(VibeServerClient.DAB_MUX_HOLD_MS,
      on ? `DAB on${channel !== undefined ? ' block #' + channel : ''}` : 'DAB off');
    if (!on) { this.dabSeenChannel = null; this.dabSeenSid = null; }
    if (on) this.dabExit.entered(); else this.dabExit.left(Date.now());
    if (on) this.cancelPacedTune();   // ★ a held tune must not land on top of the multiplex (tunePace.ts)
    this.dabHeld = on;
    const m: Record<string, unknown> = { type: 'dab', on: on ? 1 : 0 };
    if (channel !== undefined) m.channel = channel;
    if (sid     !== undefined) m.sid     = sid;
    this.sendSpectrum(m);
  }

  /** Switch service WITHIN the tuned multiplex — no retune, no re-acquire. */
  dabService(sid: number) {
    holdNativeHealing(VibeServerClient.DAB_SERVICE_HOLD_MS, `DAB service ${sid}`);
    this.sendSpectrum({ type: 'dab_service', sid });
  }

  /** ★ Raw IQ out for THIS session — the audio sheet's row. The server answers with `iqout`. */
  iqOut(on: boolean, rate = 48000) { this.sendSpectrum({ type: 'iqout', on: on ? 1 : 0, rate }); }

  /** ★ `dab` arrives about once a second with the whole measured state; `dab_off` ends it;
   *  `dab_error` is a refusal (the receiver has no DAB, or the owner switched it off) and is an
   *  EXPLANATION, not a protocol fault — the panel says why rather than showing a dead button. */
  protected handleServerMessage(msg: Record<string, unknown>): boolean {
    switch (msg.type) {
      case 'dab': {
        /* ★★★ NOT A REPORT WE ASKED TO STOP. Dropped before it touches anything — dabHeld above all,
         *  which would lock the dial out on a receiver that is back on MW (DabExitGuard says why). */
        if (!this.dabExit.accept(Date.now())) return true;
        const st = parseDabMessage(msg);
        /* ★★ A TRANSITION NOBODY HERE ASKED FOR — the server landing on DAB after a connect, or
         *    another listener moving the shared dial. Held on a CHANGE only: `dab` arrives every
         *    second, and holding on each one would switch the heal off for the whole of DAB. (Our
         *    own requests already held in dab()/dabService(); a second hold only extends it.) */
        const ch = st.channel || null;
        const sid = st.sid > 0 ? st.sid : null;
        if (!this.dabHeld) {
          holdNativeHealing(VibeServerClient.DAB_MUX_HOLD_MS, `server reports DAB${ch ? ' ' + ch : ''}`);
        } else if (ch !== null && this.dabSeenChannel !== null && ch !== this.dabSeenChannel) {
          holdNativeHealing(VibeServerClient.DAB_MUX_HOLD_MS, `server reports DAB block ${ch}`);
        } else if (sid !== null && this.dabSeenSid !== null && sid !== this.dabSeenSid) {
          holdNativeHealing(VibeServerClient.DAB_SERVICE_HOLD_MS, `server reports DAB service ${sid}`);
        }
        if (ch !== null) this.dabSeenChannel = ch;
        if (sid !== null) this.dabSeenSid = sid;
        this.dabHeld = true;
        this.callbacks.onDab?.(st);
        return true;
      }
      case 'dab_off':
        this.dabExit.offConfirmed(Date.now());
        if (this.dabHeld) holdNativeHealing(VibeServerClient.DAB_MUX_HOLD_MS, 'server reports DAB off');
        this.dabHeld = false;
        this.dabSeenChannel = null; this.dabSeenSid = null;
        this.callbacks.onDab?.(null);
        return true;
      case 'iqout':
        this.callbacks.onIqOut?.({
          on: Number(msg.on) === 1, rate: Number(msg.rate) || undefined,
          host: typeof msg.host === 'string' ? dabSafeText(msg.host, 64) : undefined,
          port: Number(msg.port) || undefined,
          code: typeof msg.code === 'string' ? dabSafeText(msg.code, 16) : undefined,
          token: typeof msg.token === 'string' ? dabSafeText(msg.token, 40) : undefined,
          public: msg.public === true,
          why: typeof msg.why === 'string' ? dabSafeText(msg.why, 160) : undefined,
        });
        return true;
      case 'dab_error':
        this.dabHeld = false;
        this.callbacks.onDab?.(null,
          dabSafeText(msg.why, 160) || 'DAB is not available on this receiver');
        return true;
    }
    return false;
  }

  get isVibe(): boolean { return true; }
}
