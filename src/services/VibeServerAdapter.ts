/**
 * VibeServerAdapter — VibeServerClient behind the SDRBackend contract.
 *
 * ★★★ A SEPARATE CODEBASE FROM UberSDRAdapter, NOT A SUBCLASS OF IT (2026-09-22). It began as a
 *  copy. Stuart: "Something that we need on VibeServer should not effect UberSDR" — and a subclass
 *  inherits every change made to its parent. Same arrangement as OwrxAdapter / KiwiAdapter /
 *  FmdxAdapter: they share the SDRBackend CONTRACT and nothing that runs.
 */

import { VibeServerClient } from './VibeServerClient';
import type { SDRMode, SDRStatus } from './sdrProtocol';
import type { BackendCallbacks, BackendCapabilities, BackendKind, SDRBackend } from './SDRBackend';

const VIBESERVER_CAPS: BackendCapabilities = {
  profiles:       false,
  serverSideZoom: true,
  smeter:         'derived',
  freqRange:      [0, 30_000_000],
  chat:           true,
  serverNR:       true,
  /* ★★★ WFM IS BROADCAST-WIDE AND THE DEFAULT IS NOT. Without a per-mode entry a remote
   *  VibeServer fell back to 6 kHz on WFM — the filter slider could not even reach a usable FM
   *  passband. The local block below has always carried the modes; this one was never given them.
   *  ★ ±250 kHz matches the web client (its own WFM ceiling is 250000), which is where the figure
   *    came from: "the bandwith slider in the app reaches a maximum of +-100k, whereas in
   *    vibeserver it reaches a maximum of +-250k" (Onfliner, 2026-09-24). One receiver should not
   *    offer two different passbands depending on which client is looking at it. */
  maxBandwidth:   { default: 6000, nfm: 8000, fm: 8000, am: 10000, wfm: 250000 },
};

// V4 local hardware (RTL-SDR Blog V4): HF direct ~0.1 MHz up to ~1766 MHz.
// Per-mode bandwidth ceilings — WFM is broadcast-wide, so the slider must reach ±250 kHz (without
// a wfm entry it fell back to default=6k and snapped narrow). ★ 250, not 100: the web client has
// always allowed 250 and the app stopped at 100, so the same radio offered two different maximum
// passbands depending on which client you looked at it through (Onfliner, 2026-09-24).
const LOCAL_CAPS: BackendCapabilities = {
  ...VIBESERVER_CAPS,
  freqRange: [100_000, 1_766_000_000],
  maxBandwidth: { default: 6000, nfm: 8000, fm: 8000, am: 10000, wfm: 250000 },
};

/**
 * ★★★ EVERY CONTROL THE CLIENT CAN SEND, REACHABLE — WITHOUT A LIST TO KEEP.
 *
 * SDRScreen holds the ADAPTER, not the client, and calls hardware controls as
 * `(client.current as any)?.x?.()`. A method that exists on VibeServerWsClient but was never
 * copied onto the adapter is therefore `undefined`, the optional call swallows it, and the control
 * moves in the UI while nothing is sent. There is no error anywhere.
 *
 * ★★★ THE FILE ALREADY WARNED ABOUT THIS — "Every hardware control needs BOTH halves. If a control
 *     does nothing, check here first" (2026-07-30, de-emphasis) — and it happened FIVE more times
 *     anyway, because the warning asks a person to remember something. Audited 2026-09-23, all
 *     silent, all shipped:
 *       setTunerBandwidth   — Stuart's Pi 2 IF filter control did nothing at any setting, even as
 *                             admin; the request never left the phone.
 *       airspyControl       — an Airspy tester found gain worked and no other control did, because
 *                             gain is setHwGain (present) and the rest are airspyControl (absent).
 *       hackrfControl, rspAgcRestart, setHwDirectSampling, setHwAutoDirectSampling,
 *       setAutoBw, setNoiseBlankerHf — same shape.
 *
 * ★★ SO THE LIST IS GONE. Anything the client can do that the adapter has not deliberately
 *    overridden is forwarded automatically — the same prototype walk ConverterBackend already uses
 *    one layer up (wrapWithConverter), which is why THAT layer never had this bug.
 * ★ The adapter's own methods always win: several transform their arguments or apply the converter
 *   offset, so they must not be replaced by the raw client versions.
 * ★ `_`-prefixed members are internals and are not exposed.
 */
export function forwardUnhandled(target: object, inner: object): void {
  const mine = new Set<string>();
  for (let p: object | null = target; p && p !== Object.prototype; p = Object.getPrototypeOf(p))
    for (const k of Object.getOwnPropertyNames(p)) mine.add(k);
  for (let p: object | null = inner; p && p !== Object.prototype; p = Object.getPrototypeOf(p)) {
    for (const k of Object.getOwnPropertyNames(p)) {
      if (k === 'constructor' || k.startsWith('_') || mine.has(k)) continue;
      const d = Object.getOwnPropertyDescriptor(p, k);
      if (!d || typeof d.value !== 'function') continue;
      mine.add(k);
      (target as Record<string, unknown>)[k] =
        (...args: unknown[]) => (d.value as (...a: unknown[]) => unknown).apply(inner, args);
    }
  }
}

export class VibeServerAdapter implements SDRBackend {
  readonly kind: BackendKind = 'ubersdr';
  /* ★ NOT readonly any more, and the declaration should say so: the tuning range is LEARNED from
   *  the receiver after construction (see learnTuningRange), so this genuinely changes once. It
   *  was declared readonly and assigned in a method anyway — which tsc did not flag here, and a
   *  declaration that quietly disagrees with the code is how the next reader is misled. */
  caps: BackendCapabilities;
  protected client: VibeServerClient;
  private baseUrl: string;
  private cb: BackendCallbacks;

  /** ★★★ THE ONE LINE THAT DECIDES WHICH PROTOCOL THIS CONNECTION IS. It used to be settled far
   *  later, by a message arriving, and everything chosen before then was chosen as UberSDR — see
   *  the note at the top of SdrWsClient.ts. A subclass overrides this and the decision is made
   *  before a socket is opened, which is the whole point of the split. */
  protected makeClient(baseUrl: string, uuid: string, callbacks: BackendCallbacks,
                       password?: string): VibeServerClient {
    return new VibeServerClient(baseUrl, uuid, callbacks, password);
  }

  constructor(baseUrl: string, uuid: string, callbacks: BackendCallbacks, password?: string, local = false) {
    // onSMeter/onProfiles unused: S-meter is spectrum-derived, no profiles.
    this.client = this.makeClient(baseUrl, uuid, callbacks, password);
    forwardUnhandled(this, this.client);
    this.baseUrl = baseUrl;
    this.cb = callbacks;
    /* ★★★ A COPY, NOT THE SHARED CONSTANT. The tuning range is learned from the server below,
     *  and `VIBESERVER_CAPS` is a module-level object: writing the learned ceiling into it would
     *  leak one receiver's limit onto every later connection in the same session — a 60 MHz
     *  server would leave the next 30 MHz one believing it could tune to 60. */
    // Local hardware tunes far beyond UberSDR's HF cap.
    this.caps = { ...(local ? LOCAL_CAPS : VIBESERVER_CAPS) };
    if (!local) { void this.learnTuningRange(baseUrl); void this.learnExtensions(baseUrl); }
    if (local) {
      this.client.minHz = LOCAL_CAPS.freqRange[0];
      this.client.maxHz = LOCAL_CAPS.freqRange[1];
      this.client.isLocal = true;
    }
  }




  /** ★★★ ASK THE RECEIVER HOW FAR IT TUNES — DO NOT ASSUME 30 MHz.
   *
   *  UberSDR's span is derived from the front end's sample rate, and their own source spells the
   *  arithmetic out (receiver_span.go):
   *
   *       64.8 Msps -> 30,456,000 usable -> 30,000,000 span
   *      129.6 Msps -> 60,912,000 usable -> 60,000,000 span
   *
   *  So "30 MHz" was never a property of UberSDR — it was the sample rate the receivers happened
   *  to run. A 129.6 Msps RX-888 reaches 60 MHz, and we clamped its listeners to half the radio.
   *  Stuart, with the spectrum drawn out to 45 MHz and the dial stuck on exactly 30000.000:
   *  "we show the spectrum above 30, just cannot reach it."
   *
   *  ★★ IT IS PUBLISHED, AND WE WERE ALREADY FETCHING THE ENDPOINT. `/api/description` carries a
   *     `tuning_range` object — confirmed live on a public receiver:
   *       { "min_frequency": 10000, "max_frequency": 30000000, "input_samprate": 64800000,
   *         "spectrum_span_hz": 30000000, "samprate_source": "radiod-conf" }
   *     We read `description` for the receiver name and threw the rest away.
   *
   *  ★ THE FALLBACK IS THEIRS, NOT A GUESS. receiver_span.go states the client contract outright:
   *    "Consumers must treat a missing object, or any field of it that is absent or zero, as
   *    10 kHz - 30 MHz." So an older server, a missing field or a zero all keep exactly today's
   *    behaviour — which is why this can be read eagerly without risking a regression.
   *  ★ Failure is silent and harmless: no response, bad JSON or nonsense numbers leave the
   *    defaults in place. A receiver that will not say keeps the range it has always had. */
  private async learnTuningRange(baseUrl: string): Promise<void> {
    try {
      const r = await fetch(`${baseUrl.replace(/\/+$/, '')}/api/description`);
      if (!r.ok) return;
      const tr = (await r.json())?.tuning_range;
      const lo = Number(tr?.min_frequency);
      const hi = Number(tr?.max_frequency);
      // ★ Both must be sane AND ordered before either is believed — a half-read range that
      //   clamped the dial to nothing would be worse than the assumption it replaces.
      if (!Number.isFinite(lo) || !Number.isFinite(hi) || lo <= 0 || hi <= lo) return;
      this.caps = { ...this.caps, freqRange: [lo, hi] };
      this.client.minHz = lo;
      this.client.maxHz = hi;
      /* ★ NO CALLBACK, DELIBERATELY — there is no onCaps and `caps` is declared readonly on the
       *  backend interface precisely because the UI re-reads `client.current.caps` on every
       *  render rather than being told. Spectrum frames re-render continuously, so the new
       *  ceiling is in force within a frame of it arriving.
       *  ★★ I first wrote `this.cb.onCaps?.(...)`, which does not exist. Fifth invented name this
       *     session — grep before reaching for a plausible one. */
    } catch {
      // A receiver that will not say keeps 10 kHz - 30 MHz, per their documented contract.
    }
  }

  /** ★★★ ASK WHAT THIS RECEIVER RUNS. `/api/extensions` answers
   *  `{ available: [{ slug, displayName }], default: … }` — confirmed live on a public receiver,
   *  which listed: clock, cw-spots, digital-spots, drm, dx-cluster, flexcontrol, freedv, fsk, ft8,
   *  midi-control, morse, navtex, olivia, qrss, radio-sync, soundmodem, sstv, stats, wefax,
   *  whisper.
   *  ★ Failure stays silent and the UI keeps offering everything: not being able to ask is not
   *    evidence that a decoder is absent, and hiding the lot on a fetch error would be a far worse
   *    outcome than the dead control this replaces. */
  private async learnExtensions(baseUrl: string): Promise<void> {
    try {
      const r = await fetch(`${baseUrl.replace(/\/+$/, '')}/api/extensions`);
      if (!r.ok) return;
      const j = await r.json();
      const list = Array.isArray(j?.available) ? j.available : Array.isArray(j) ? j : [];
      const slugs = list
        .map((x: any) => (typeof x === 'string' ? x : x?.slug))
        .filter((x: any): x is string => typeof x === 'string' && !!x);
      if (slugs.length) this.cb.onExtensions?.(slugs);
    } catch {
      // Could not ask — the UI keeps offering everything, as before.
    }
  }

  get uuid(): string { return this.client.uuid; }

  /** Link-management mode (Auto / Full / Low Data). FORWARD to the inner client — the app sets this on
   *  the adapter, and without this getter/setter the toggle vanished (the adapter had no linkMode and no
   *  `.inner`, so Low Data never reached the controller and held 10fps; Auto only "worked" as the
   *  default). The client's own setter reconfigures the running LinkManager live. */
  get linkMode(): VibeServerClient['linkMode'] { return this.client.linkMode; }
  set linkMode(m: VibeServerClient['linkMode']) { this.client.linkMode = m; }

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
  ahfControl(o: Parameters<VibeServerClient['ahfControl']>[0]) { this.client.ahfControl(o); }
  rspControl(o: Parameters<VibeServerClient['rspControl']>[0]) { this.client.rspControl(o); }
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

  /** ★ The cast is CHECKED, not asserted away: makeClient above is the only thing that builds this
   *  adapter's client and it builds a VibeServerClient, so the narrowing is a fact about this
   *  class. A `(c as any).dab?.()` would have been the setAdminAuth mistake again. */
  private get vibe(): VibeServerClient { return this.client; }

  dab(on: boolean, channel?: number, sid?: number): void { this.vibe.dab(on, channel, sid); }
  dabService(sid: number): void { this.vibe.dabService(sid); }
  iqOut(on: boolean, rate = 48000): void { this.vibe.iqOut(on, rate); }
  get inDab(): boolean { return this.vibe.inDab; }
}
