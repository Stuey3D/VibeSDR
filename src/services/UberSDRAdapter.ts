/**
 * UberSDRAdapter — UberSDRClient behind the SDRBackend contract (v3 brief §8
 * phase 0). Near pass-through by design: the internal model IS UberSDR-shaped,
 * so this wrapper only contributes kind/caps and the delegation plumbing.
 * Zero behaviour change from calling the client directly.
 */

import { UberSDRClient, type SDRMode, type SDRStatus } from './UberSDRClient';
import type { SDRBackend, BackendCallbacks, BackendCapabilities, BackendKind } from './SDRBackend';
import { OwrxAdapter } from './OwrxAdapter';
import { KiwiAdapter } from './KiwiAdapter';
import { FmdxAdapter } from './FmdxAdapter';

const UBERSDR_CAPS: BackendCapabilities = {
  profiles:       false,
  serverSideZoom: true,
  smeter:         'derived',
  freqRange:      [0, 30_000_000],
  chat:           true,
  serverNR:       true,
  maxBandwidth:   { default: 6000 },
};

// V4 local hardware (RTL-SDR Blog V4): HF direct ~0.1 MHz up to ~1766 MHz.
// Per-mode bandwidth ceilings — WFM is broadcast-wide, so the slider must reach
// ±100 kHz (without a wfm entry it fell back to default=6k and snapped narrow).
const LOCAL_CAPS: BackendCapabilities = {
  ...UBERSDR_CAPS,
  freqRange: [100_000, 1_766_000_000],
  maxBandwidth: { default: 6000, nfm: 8000, fm: 8000, am: 10000, wfm: 100000 },
};

export class UberSDRAdapter implements SDRBackend {
  readonly kind: BackendKind = 'ubersdr';
  readonly caps: BackendCapabilities;
  private client: UberSDRClient;
  private baseUrl: string;
  private cb: BackendCallbacks;

  constructor(baseUrl: string, uuid: string, callbacks: BackendCallbacks, password?: string, local = false) {
    // onSMeter/onProfiles unused: S-meter is spectrum-derived, no profiles.
    this.client = new UberSDRClient(baseUrl, uuid, callbacks, password);
    this.baseUrl = baseUrl;
    this.cb = callbacks;
    // Local hardware tunes far beyond UberSDR's HF 30 MHz cap.
    this.caps = local ? LOCAL_CAPS : UBERSDR_CAPS;
    if (local) {
      this.client.minHz = LOCAL_CAPS.freqRange[0];
      this.client.maxHz = LOCAL_CAPS.freqRange[1];
      this.client.isLocal = true;
    }
  }

  get uuid(): string { return this.client.uuid; }

  /** Link-management mode (Auto / Full / Low Data). FORWARD to the inner client — the app sets this on
   *  the adapter, and without this getter/setter the toggle vanished (the adapter had no linkMode and no
   *  `.inner`, so Low Data never reached the controller and held 10fps; Auto only "worked" as the
   *  default). The client's own setter reconfigures the running LinkManager live. */
  get linkMode(): UberSDRClient['linkMode'] { return this.client.linkMode; }
  set linkMode(m: UberSDRClient['linkMode']) { this.client.linkMode = m; }

  /** Local hardware: thread the live device sample rate for panSpan()'s Fs window. */
  setLocalSampleRate(hz: number) { this.client.localSampleRate = hz; }
  // VibeServer PIN: the pre-computed "&vs_nonce=&vs_auth=" WS URL suffix.
  setAuthSuffix(s: string) { this.client.authSuffix = s; }
  // VibeServer: client-driven hardware controls (applied on the serving device).
  setHwGain(tenthDb: number, auto: boolean) { this.client.setHwGain(tenthDb, auto); }
  setHwBiasT(on: boolean) { this.client.setHwBiasT(on); }
  setHwAgc(on: boolean)   { this.client.setHwAgc(on); }
  setHwPpm(ppm: number)   { this.client.setHwPpm(ppm); }
  setHwSampleRate(rate: number) { this.client.setHwSampleRate(rate); }

  /** Receiver location from /status.json (same shape as OWRX: receiver.gps.lon)
   *  → ITU region, for custom/default UberSDR hosts not carrying a directory lon. */
  private async fetchReceiverLon(): Promise<void> {
    try {
      const http = this.baseUrl.trim().replace(/^wss:\/\//, 'https://').replace(/^ws:\/\//, 'http://').replace(/\/+$/, '');
      const r = await fetch(http + '/status.json', { signal: AbortSignal.timeout(8000) });
      if (!r.ok) return;
      const lon = (await r.json())?.receiver?.gps?.lon;
      if (typeof lon === 'number') this.cb.onReceiverLon?.(lon);
    } catch {}
  }

  connect(frequency?: number, mode?: SDRMode, opts?: { allowServerDefault?: boolean }) { this.fetchReceiverLon(); return this.client.connect(frequency, mode, opts); }
  destroy()                                   { this.client.destroy(); }

  tune(frequency: number, mode?: SDRMode, opts?: { recenter?: boolean }) { this.client.tune(frequency, mode, opts); }
  syncFrequency(frequency: number, mode?: SDRMode) { this.client.syncFrequency(frequency, mode); }
  setFollowMode(follow: boolean) { this.client.setFollowMode(follow); }
  panSpan() { return this.client.panSpan(); }
  captureBandwidth() { return this.client.captureBandwidth(); }
  rfCenterHz() { return this.client.rfCenterHz(); }
  setMode(mode: SDRMode)                           { this.client.setMode(mode); }
  setBandwidth(low: number, high: number)          { this.client.setBandwidth(low, high); }

  zoom(frequency: number, binBandwidth: number) { this.client.zoom(frequency, binBandwidth); }
  pan(frequency: number)                        { this.client.pan(frequency); }
  resetView()                                   { this.client.resetView(); }

  setRate(divisor: number) { this.client.setRate(divisor); }
  pauseSpectrum()          { this.client.pauseSpectrum(); }
  resumeSpectrum()         { this.client.resumeSpectrum(); }
  forceResubscribe(reason: string) { this.client.forceResubscribe(reason); }

  getStatus(): SDRStatus { return this.client.getStatus(); }
  getView():   SDRStatus { return this.client.getView(); }
}

/** Backend factory — KiwiAdapter / OwrxAdapter register here in later phases. */
export function createBackend(
  kind: BackendKind,
  baseUrl: string,
  uuid: string,
  callbacks: BackendCallbacks,
  password?: string,
  local = false,
): SDRBackend {
  switch (kind) {
    case 'ubersdr': return new UberSDRAdapter(baseUrl, uuid, callbacks, password, local);
    case 'owrx':    return new OwrxAdapter(baseUrl, uuid, callbacks);
    case 'kiwi':    return new KiwiAdapter(baseUrl, uuid, callbacks, password);
    case 'fmdx':    return new FmdxAdapter(baseUrl, uuid, callbacks);
    default: throw new Error(`backend '${kind}' not implemented`);
  }
}
