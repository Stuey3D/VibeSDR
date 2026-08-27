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
  /** ★★★ THE ADAPTER IS THE OBJECT SDRScreen HOLDS, not the client. Adding setDeemph/setStereo to
   *  UberSDRClient alone left them unreachable: SDRScreen calls them on `client.current`, which is
   *  THIS, and `rc.setDeemph?.(tau)` on an object without the method silently does nothing. The
   *  optional call is what made it silent — de-emphasis appeared fixed and still did nothing, on a
   *  build that genuinely contained the fix (2026-07-30).
   *  ★ Every hardware control needs BOTH halves. If a control does nothing, check here first. */
  setDeemph(tau: number)  { this.client.setDeemph(tau); }
  setStereo(on: boolean)  { this.client.setStereo(on); }

  /** ★★ The audio DSP trio — same story as de-emphasis, found 2026-07-31: they worked on rtl_tcp
   *  (our DSP, on-device) and in the web client, and did nothing from the app on a VibeServer.
   *  Both halves, again — client method AND this passthrough. See UberSDRClient.setNrEnabled. */
  setNrEnabled(on: boolean, strength?: number) { this.client.setNrEnabled(on, strength); }
  setSquelchDb(db: number)              { this.client.setSquelchDb(db); }
  setNotch(on: boolean)                 { this.client.setNotch(on); }
  // ★ Broadcast-FM treatments. Forwarded to the client rather than reimplemented — four faults,
  //   four switches, and they are not interchangeable (see UberSDRClient).
  setWeakProc(on: boolean)              { this.client.setWeakProc(on); }
  setIms(on: boolean)                   { this.client.setIms(on); }
  setCeq(on: boolean)                   { this.client.setCeq(on); }
  setNoiseBlanker(on: boolean)          { this.client.setNoiseBlanker(on); }
  /** ★ Presence, forwarded — see UberSDRClient.noteActivity. Adapter half, as ever. */
  noteActivity()                        { this.client.noteActivity(); }

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

  /** Admin credentials for the CONNECT URL — set BEFORE connect(), or the handshake goes out
   *  without them and a busy or cooling-down receiver refuses us as an ordinary listener. */
  setAdminAuth(q: string) { this.client.setAdminAuth(q); }

  /** ★★★ THE ADAPTER IS WHAT THE SCREEN HOLDS, so a method that exists only on the client below it
   *  might as well not exist. The audio recovery reached for `reregisterSession` on
   *  `client.current`, found an UberSDRAdapter that had never heard of it, and returned — silently,
   *  because the guard that missed it was written as an early return with nothing to say. The
   *  native watchdog asked for help, the help was three lines away, and the diagnostics report
   *  showed the ask and no answer (Stuart, 2026-08-18, 18:30:36).
   *  ★★ THE SAME SHAPE AS THE FAULT IT WAS FIXING. Twice in one afternoon: a path that cannot
   *     work, failing quietly. Anything that can decline must say so — see the throw below and the
   *     noteAudioEvent on the calling side. */
  async reregisterSession(): Promise<void> {
    await this.client.reregisterSession();
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

  /** ★★ FORWARD, or the feature is unreachable. Same trap as `linkMode` above: SDRScreen holds
   *  the ADAPTER, not the client, so a method that exists only on UberSDRClient is simply
   *  absent — and both of these are read with `?.`, so the failure is SILENT. `isVibe` gates
   *  whether the ADV RDS button is offered at all and `setAdvRds` is the switch, so missing
   *  them meant the analyser could never be shown and never be turned on, with nothing
   *  anywhere to say why (Stuart: "it was in WFM and indicating stereo", 2026-07-27).
   *  ★ Anything added to UberSDRClient that the screen calls needs a line here. */
  get isVibe(): boolean { return this.client.isVibe; }
  setAdvRds(on: boolean) { this.client.setAdvRds(on); }
  /** Radio-specific hardware controls. Forwarded for the same reason as above — the screen
   *  holds the adapter, and an absent method on an `any`-cast call fails silently. */
  ahfControl(o: Parameters<UberSDRClient['ahfControl']>[0]) { this.client.ahfControl(o); }
  rspControl(o: Parameters<UberSDRClient['rspControl']>[0]) { this.client.rspControl(o); }
  adminUnlock(nonce: string, token: string) { this.client.adminUnlock(nonce, token); }
  /** Freeze/unfreeze the link controller during idle powersave so it doesn't fight the saver's rate. */
  setLinkPaused(p: boolean) { this.client.setLinkPaused(p); }
  /** ★ See UberSDRClient.resumeRate — waking from powersave must re-ask for the rate. */
  resumeRate() { this.client.resumeRate(); }
  /** ★★★ THE MISSING HALF OF POWERSAVE, and its absence made the saver SPEED THE SPECTRUM UP.
   *  Idle powersave does two things: pause the link controller so it stops re-asserting its rung,
   *  then drop to an absolute 5 fps. Only the FIRST was forwarded. So the controller let go of
   *  whatever rung it was holding, nothing replaced it with the idle rate, and the waterfall ran
   *  at the FULL rate under a pill reading "POWER SAVE · spectrum slowed" — measured by Stuart at
   *  10 fps before the saver engaged and 20 fps after (2026-08-02, two screenshots).
   *  ★ Note WHICH way this failed. The two calls are adjacent in SDRScreen and adjacent here; the
   *  one that got forwarded was the one that REMOVES a brake. A half-applied change is worse than
   *  an unapplied one — "not slowing" would merely have been the feature missing. */
  setPowersaveRate() { this.client.setPowersaveRate(); }
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
    // Same adapter for both Kiwi dialects — only the WebSocket path differs, and it self-corrects
    // if the guess was wrong (KiwiAdapter.tryOtherWsPrefix).
    case 'kiwi':    return new KiwiAdapter(baseUrl, uuid, callbacks, password, 'kiwi');
    case 'web888':  return new KiwiAdapter(baseUrl, uuid, callbacks, password, 'web888');
    case 'fmdx':    return new FmdxAdapter(baseUrl, uuid, callbacks);
    default: throw new Error(`backend '${kind}' not implemented`);
  }
}
