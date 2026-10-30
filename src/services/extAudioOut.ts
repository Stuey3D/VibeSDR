/**
 * ★★ WHERE A KIWI / WEB-888 / OPENWEBRX ADAPTER'S AUDIO GOES — V12 TWO SERVERS AT ONCE (Stuart, 2026-10-30: "Kiwi
 * Web-888 and OWRX should have full background audio").
 *
 * They already have it on their own: KiwiAdapter and OwrxAdapter decode in JavaScript and push PCM to the native main
 * player (startExternalAudio / pushExternalPcm), which plays on with the screen locked. With two servers at once only
 * ONE side may use the main player (the VibeServer pump, the UberSDR engine, external PCM and FM-DX all feed it), so an
 * adapter on the other side pushes the same PCM to the SECOND voice (VibeDualAudio.pushAuxPcm) instead. That is the
 * whole difference: the same decoder, the same socket, the same session — only the last call changes. Moving between
 * the two (dualServer.moveToNative / moveToAux) is a change of route, not a reconnect.
 *
 * ★ 'native' is the default and is EXACTLY the old path, call for call — outside a pairing nothing here changes a byte.
 */
import { NativeModules } from 'react-native';

const Vibe = NativeModules.VibePowerModule as {
  startExternalAudio?: (rate: number, pauseMode?: string) => void;
  pushExternalPcm?: (b64: string, rate: number, channels: number) => void;
  stopExternalAudio?: () => void;
  setAudioBurstDepth?: (ms: number) => void;
} | undefined;
const Dual = (NativeModules as { VibeDualAudio?: {
  startAux?: () => void; stopAux?: () => void; pushAuxPcm?: (b64: string, rate: number, channels: number) => void;
} }).VibeDualAudio;

export type ExtRoute = 'native' | 'aux';

export class ExtAudioOut {
  private route: ExtRoute = 'native';
  private started = false;
  private pauseMode = 'release';
  private burstMs = 0;
  /** When the last buffer went out (ms epoch) — the pairing's "has its stream stopped?" for the second voice. */
  lastPushAt = 0;

  /** Where it goes now. A change while playing stops the old route; the next buffer starts the new one. */
  get current(): ExtRoute { return this.route; }
  setRoute(r: ExtRoute) {
    if (r === this.route) return;
    if (this.started) this.stopRoute(this.route);
    this.started = false;
    this.route = r;
  }

  /** The adapter's first buffer (its old `startExternalAudio`). `pauseMode` is the main player's lock-screen pause rule. */
  start(rate: number, pauseMode: string) {
    this.pauseMode = pauseMode;
    if (this.started) return;
    this.started = true;
    if (this.route === 'aux') Dual?.startAux?.();
    else {
      Vibe?.startExternalAudio?.(rate, pauseMode);
      // ★ A route change rebuilt the player: the bursty-decoder pre-roll it was given is restated (OWRX only).
      if (this.burstMs > 0) Vibe?.setAudioBurstDepth?.(this.burstMs);
    }
  }
  isStarted(): boolean { return this.started; }

  push(b64: string, rate: number, channels: number) {
    if (!this.started) this.start(rate, this.pauseMode);
    this.lastPushAt = Date.now();
    if (this.route === 'aux') Dual?.pushAuxPcm?.(b64, rate, channels === 2 ? 2 : 1);
    else Vibe?.pushExternalPcm?.(b64, rate, channels);
  }

  /** OWRX's bursty-decoder pre-roll — the MAIN player's (the second voice keeps its own cushion). */
  burst(ms: number) {
    this.burstMs = ms;
    if (this.route === 'native') Vibe?.setAudioBurstDepth?.(ms);
  }

  stop() {
    if (!this.started) return;
    this.started = false;
    this.stopRoute(this.route);
  }

  private stopRoute(r: ExtRoute) {
    if (r === 'aux') Dual?.stopAux?.();
    else Vibe?.stopExternalAudio?.();
  }
}
