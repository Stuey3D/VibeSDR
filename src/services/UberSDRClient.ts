// UberSDRClient.ts — UberSDR, and nothing else.
//
// ★★★ THIS FILE USED TO BE 2,400 LINES SERVING TWO PROTOCOLS. The shared half now lives in
//     SdrWsClient.ts (see the note at the top of it for why), and this is what is true of an
//     UberSDR server and not of a VibeServer. Four things, which is all the divergence there ever
//     was — it just used to be expressed as a mutable flag that flipped mid-connection.
//
// ★★ THE TYPE RE-EXPORTS BELOW ARE LOAD-BEARING. Eleven files import SDRMode / SDRStatus / RdsExt
//    / RadioCaps / SDRCallbacks / MODE_BANDWIDTHS from this path and care nothing about which
//    server they came from. Re-exporting them keeps every one of those imports working unchanged,
//    so the split touches the client layer and stops there.
import { SdrWsClient, LADDERS_FOR } from './SdrWsClient';

export {
  MODE_BANDWIDTHS,
  type SDRMode,
  type SDRStatus,
  type IdlePolicy,
  type RdsExt,
  type RadioCaps,
  type SDRCallbacks,
} from './SdrWsClient';

export class UberSDRClient extends SdrWsClient {
  /** UberSDR sends its own bin count and ignores a request for one. */
  protected binsSuffix(): string { return ''; }

  /** [10, 5, 3.3] — UberSDR's rungs. Chosen HERE, at construction, which is the whole point of
   *  the split: the controller used to be built on this ladder for every server and then rebuilt
   *  when a VibeServer's hwinfo arrived, if it arrived in time. */
  protected ladderFor(): number[] { return LADDERS_FOR.ubersdr; }

  /** ★ UberSDR's frame-rate lever is a DIVISOR off its own full rate. Sending this to a
   *  VibeServer was the "set_rate divisor=2 RECEIVED — UberSDR lever, on a VibeServer" diagnostic
   *  that started all of this. It can no longer reach one: a VibeServer is a different class. */
  protected sendRateLever(): void {
    if (this.spectrumWs?.readyState !== WebSocket.OPEN) return;
    this.spectrumWs.send(JSON.stringify({ type: 'set_rate', divisor: this.rateDivisor }));
  }

  /** Powersave in divisors: 10 fps full ⇒ divisor 2 for 5 fps. Clamped at 8, as before. */
  protected sendPowersaveLever(targetFps: number): void {
    if (targetFps <= 0) return;
    const full = LADDERS_FOR.ubersdr[0];
    this.setRate(Math.max(1, Math.min(8, Math.round(full / targetFps))));
  }

  get isVibe(): boolean { return false; }
}
