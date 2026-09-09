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
import { SdrWsClient, LADDERS_FOR } from './SdrWsClient';
import { parseDabMessage, dabSafeText, type DabState } from './dabTypes';

export {
  MODE_BANDWIDTHS,
  type SDRMode,
  type SDRStatus,
  type IdlePolicy,
  type RdsExt,
  type RadioCaps,
  type SDRCallbacks,
} from './SdrWsClient';

export class VibeServerClient extends SdrWsClient {
  /** ★ How many spectrum bins to ask for. A VibeServer honours the request — that is what makes
   *  one waterfall serve tens of listeners at a quarter of the bandwidth. UberSDR ignores it. */
  static readonly BINS = SdrWsClient.VIBE_BINS;
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
  dab(on: boolean, channel?: number, sid?: number) {
    this.dabHeld = on;
    const m: Record<string, unknown> = { type: 'dab', on: on ? 1 : 0 };
    if (channel !== undefined) m.channel = channel;
    if (sid     !== undefined) m.sid     = sid;
    this.sendSpectrum(m);
  }

  /** Switch service WITHIN the tuned multiplex — no retune, no re-acquire. */
  dabService(sid: number) { this.sendSpectrum({ type: 'dab_service', sid }); }

  /** ★ Raw IQ out for THIS session — the audio sheet's row. The server answers with `iqout`. */
  iqOut(on: boolean, rate = 48000) { this.sendSpectrum({ type: 'iqout', on: on ? 1 : 0, rate }); }

  /** ★ `dab` arrives about once a second with the whole measured state; `dab_off` ends it;
   *  `dab_error` is a refusal (the receiver has no DAB, or the owner switched it off) and is an
   *  EXPLANATION, not a protocol fault — the panel says why rather than showing a dead button. */
  protected handleServerMessage(msg: Record<string, unknown>): boolean {
    switch (msg.type) {
      case 'dab':
        this.dabHeld = true;
        this.callbacks.onDab?.(parseDabMessage(msg));
        return true;
      case 'dab_off':
        this.dabHeld = false;
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
