/**
 * spectrum.ts — VibeServer spectrum WebSocket client (browser).
 *
 * Browser port of src/services/UberSDRClient.ts, trimmed to what the shim
 * actually speaks. Shim reference: android/app/src/main/cpp/local_sdr_shim.cpp
 *
 *   /ws/user-spectrum
 *     <- text  {"type":"config"|"hwinfo"|"rds"|"pong", ...}
 *     <- bin   'SPEC' frames (22-byte header + binCount u8 bins)
 *     -> text  {"type":"zoom"|"tune"|"mode"|"bandwidth"|"gain"|... }
 *
 * Two things here are load-bearing and easy to get wrong:
 *   1. SPEC bins arrive in FFT order (DC first) — they MUST be rotated by
 *      binCount/2 to draw left-to-right.
 *   2. Zoom/pan sends are coalesced. Every view request triggers a config echo
 *      from the server; an un-throttled drag fires at 60–120 Hz and floods the
 *      link. Keep _sendView.
 */

export type SDRMode = 'usb' | 'lsb' | 'am' | 'sam' | 'fm' | 'nfm' | 'cwu' | 'cwl' | 'wfm';

/** The server applies these on every mode change and never reports bandwidth
 *  back, so the client mirrors the table to stay in sync. */
export const MODE_BANDWIDTHS: Record<SDRMode, [number, number]> = {
  usb: [50, 2700],     lsb: [-2700, -50],
  am:  [-5000, 5000],  sam: [-5000, 5000],
  cwu: [-200, 200],    cwl: [-200, 200],
  fm:  [-6000, 6000],  nfm: [-5000, 5000],
  wfm: [-100000, 100000],
};

const SPEC_MAGIC    = 0x43455053; // 'SPEC' little-endian
const FLAG_FULL_U8  = 0x03;
const U8_DBFS_OFFSET = -256;      // dBFS = u8 - 256

const VIEW_SEND_MS   = 33;
const VIEW_SETTLE_MS = 300;
// Gain is a USB CONTROL TRANSFER on the same bus as the IQ stream, so it is far
// more expensive than a view change — throttle it much harder. See setHwGain().
const GAIN_SEND_MS   = 120;
const MIN_SPAN_HZ    = 6_000;     // max-zoom floor; deeper looks frozen/artefacted

export interface Config {
  centerFreq: number;
  binCount: number;
  binBandwidth: number;
  totalBandwidth: number;
  maxBandwidth: number;
  /** The server's own demodulator, if it reported one — authoritative for a fresh client's mode. */
  serverMode?: string;
  /** ★ Where the RADIO is actually tuned (Hz), from the server. Distinct from centerFreq, which
   *  is the VIEW centre — on a locked receiver the view sits at the locked centre while the VFO
   *  is wherever the owner's landing frequency put it. A first-time visitor has nothing
   *  remembered, so without this it parked on the view centre instead of where it was sent. */
  serverVfo?: number;
}

export interface RdsMeta {
  stereo: boolean;
  ps: string;
  radiotext: string;
  pi: number;
  ecc: number;
  /** Block error rate %, -1 = the decoder has not seen a full window yet. */
  ber: number;
  /** Recovered 57 kHz level relative to the pilot, dB (-99 = nothing). */
  sig: number;
}

/** The fields the normal RDS path discards, plus the constellation. */
export interface RdsExt {
  /** ★ TEF-style automatic demodulator bandwidth: whether it is on, and the width it settled on
   *  (Hz). Carried beside mpxSnr because that is the figure that decides it. */
  autobw?: boolean;
  autobwHz?: number;
  /** ★ What IMS's multipath suppression is asking of the L-R corner (Hz); 0 = not acting, or the
   *  noise curve had already asked for narrower, in which case IMS is not the reason for it. */
  imsBlend?: number;
  /** Why IMS is not acting: 1 off · 2 CEQ has the job · 3 nothing to treat · 4 NR already narrower. */
  imsWhy?: number;
  pty: number; tp: number; ta: number; ms: number; di: number;
  // ★ The same five UNCONFIRMED — what is arriving right now, whether or not it passed
  // confirmation. Always sent; the RAW/CONFIRMED choice is entirely the client's.
  ptyRaw: number; tpRaw: number; taRaw: number; msRaw: number; diRaw: number;
  ct: number;            // minutes since UTC midnight, -1 = none
  ctoff: number;         // local offset, half-hours
  af: number[];          // kHz, CONFIRMED only
  afseen: number;        // distinct frequencies glimpsed, confirmed or not
  /** Every AF glimpsed as [kHz, confirmed] — so the list can be drawn with a tick each rather
   *  than reduced to a score. On a noisy station the unconfirmed entries are usually phantoms
   *  manufactured by block errors, and only the list shows WHICH frequencies they are. */
  afAll: [number, number][];
  grp: number[];         // 32 counters, gtype*2+version
  gtot: number;
  rtpTitle: string;      // RT+ tagged title (ODA 4BD7)
  rtpArtist: string;     // RT+ tagged artist
  longPs: string;        // 32-character Long PS (group 15A)
  ptyn: string;          // programme type NAME (10A) — the station's own words
  lang: number;          // RDS language code (1A variant 3)
  pinDay: number; pinHour: number; pinMin: number;
  eon: { pi: string; ps: string; af: number; ta: number }[];
  oda: { aid: string; grp: number }[];
  phase: number;         // RDS-to-pilot phase, degrees (-1 = no lock)
  phaseCoh: number;
  /** deg/s the RDS-to-pilot phase is turning. >0 means the station's encoder is not locked
   *  to its own pilot — a transmitter fault, not a reception one. */
  phaseDrift: number;      // 0..1 — how steady that phase is; below ~0.35 it is meaningless
  pilotDev: number;      // pilot injection, kHz deviation (spec 6.0–7.5)
  /** ★ Is the stereo PLL locked. Distinct from the constellation's lock, which is RDS —
   *  one word for two different failures misled the panel's own author. */
  pilotLock: boolean;
  rdsDev: number;        // RDS injection, kHz deviation (typical 2–4, max 5.6)
  /** Pilot against the transmitted-silence gap at 15–19 kHz, dB. NOT a textbook SNR — the
   *  measuring filter's own leakage caps it near 34 dB — but a real figure of merit, and the
   *  thing that drives high-blend and the audio high-cut. */
  mpxSnr: number;
  /** Envelope-AM depth: 0 for a clean carrier, rising with a REFLECTION. An FM carrier leaves the
   *  transmitter at constant amplitude, so this is damage done by the channel — multipath, not
   *  weakness. The two want opposite treatments, which is why it is measured separately. */
  multipath: number;
  /** Whether `multipath` means anything. Below ~12 dB MPX S/N the noise correction is subtracting
   *  nearly everything it measured, so the residual is the difference of two large numbers — a
   *  confident-looking figure about nothing. "Cannot tell" is an honest answer; a number is not. */
  multipathOk: boolean;
  /** Whether there is a usable 19 kHz pilot at all. Both mpxSnr and multipath are ratios against
   *  it, so with no pilot they are arithmetic rather than measurement — 70 dB and 580% together. */
  snrOk: boolean;
  hiCutLmr: number;      // where high-blend has rolled the stereo difference off, Hz
  hiCutAud: number;      // where the audio high-cut is sitting, Hz
  /** dB the signal would GAIN by narrowing the IF to ifCand. Positive = narrowing helps. Measured
   *  on a shadow copy of the live signal, so it is this aerial's answer, not a lab's. */
  ifGain: number;
  ifCand: number;        // the candidate IF width being evaluated, Hz
  /** The IF width in use now, Hz. 0 = wide open. `ifGain` is always "the OTHER option minus the
   *  current one", so this says which way round to read it. */
  ifBw: number;
  /** CEQ engaged, and the multipath depth measured AFTER it — against `multipath`, which is what
   *  arrived. Two numbers so the equaliser is judged on what it achieved, not on having run. */
  ceqOn: boolean;
  ceqAfter: number;
  /** Fraction of samples the noise blanker is excising, 0..1. Says whether impulse noise is even
   *  a problem here — otherwise a guess. */
  nbRate: number;
  /** Why CEQ is not running: 0 running, 1 off, 2 signal too weak, 3 nothing to correct. */
  ceqWhy: number;
  xy: number[];          // interleaved x,y as signed bytes (x100)
  mpx: number[];         // MPX spectrum, dB per bin, DC..100 kHz
}

/** What the RUNNING receiver can actually do. A dongle and an RSP are different radios with
 *  different controls, and a client that assumes one will misrepresent the other. */
export interface RadioCaps {
  driver: 'rtl' | 'sdrplay' | 'airspyhf' | string;
  model?: string;
  lnaStates?: number;
  ifGrMin?: number;
  ifGrMax?: number;
  rfNotch?: boolean;
  dabNotch?: boolean;
  biasT?: boolean;
  // ── Airspy HF+ ────────────────────────────────────────────────────────────
  attSteps?: number;      // count of attenuator positions (9 = 0..8)
  attStepDb?: number;     // dB per step (6)
  hfLna?: boolean;        // has the +6 dB preamp
  hfAgc?: boolean;        // has its own AGC
  agcThreshold?: boolean; // ...with a low/high threshold
  calPpb?: boolean;       // calibration is in parts per BILLION, not ppm
  rates?: number[];       // the rates THIS radio offers, ascending
  /** ★ Tunable windows, Hz. An HF+ Discovery has a REAL GAP at 31-60 MHz — not a weak spot,
   *  absent — so a client that does not know will let a user park on a dead frequency. */
  ranges?: [number, number][];
}

export interface SpectrumCallbacks {
  onBins?:   (bins: Float32Array, centerHz: number, bwHz: number) => void;
  onConfig?: (cfg: Config) => void;
  /** The server's radio was unplugged (false) or came back (true). */
  onDevice?: (present: boolean) => void;
  /** The owner's notice to listeners, pushed when it is posted or cleared ('' = nothing). */
  onNotice?: (text: string) => void;
  /** The person at the server is looking for this window. */
  onSummon?: () => void;
  /** ★★ lockedRate is an UP-TO CEILING (offer rates at or below it). lockedCentre is a real
   *  LOCK — the operator pinned the captured window, so the rate is fixed too and the picker
   *  must go entirely. Two different meanings, deliberately two different fields: reusing the
   *  ceiling as a lock hid the picker in the app and merely filtered it here. */
  onHwInfo?: (gains: number[], rates: number[], lockedRate: number, maxFftRate: number,
              forceIdleSaver?: boolean, radio?: RadioCaps | null, lockedCentre?: number,
              /** ★★★ The owner's gain CEILING in force at the frequency being listened to, in the
               *  radio's own units, or -1 for none. A cap the client does not know about is a
               *  control that springs back — the listener drags the slider, the server clamps it,
               *  and the UI shows a value the radio is not using, which reads as a broken
               *  receiver rather than as somebody's rule. */
              gainCap?: number,
              /** The owner has locked the AGC on: the switch must show as locked rather than
               *  appear to ignore the tap. */
              agcLocked?: boolean,
              /** ★★★ WHERE THE GAIN ACTUALLY IS, in the radio's own units; -1 = auto/AGC. The
               *  slider follows the RADIO, because the radio is the authority on its own gain and
               *  we may not be the only listener. Restoring a remembered value and pushing it on
               *  connect overrode the owner's resting gain and re-gained a shared receiver for
               *  everyone already on it. */
              gainNow?: number,
              /** RTL only: is VibeSDR's AGC running, and how many steps below the ceiling is it? */
              agc?: boolean, ovlSteps?: number,
              /** Peak ADC level in dBFS — what the AGC is actually aiming at. */
              adcPeak?: number) => void;
  /** ★★★ Demodulators/decoders the owner has switched off on this receiver. The server refuses
   *  them anyway; this exists so the client can HIDE them. Per AGENTS.md, a control that is
   *  visible and refused reads as a broken feature, not a blocked one. */
  /** How many are listening, and the owner's cap. Sent when the count CHANGES. */
  onUsers?: (n: number, max: number) => void;
  /** ★ Global server-side DSP state. These SURVIVE a listener leaving, so the next
   *  listener inherits them — the client must render what the server says rather
   *  than its own saved prefs, or the control lies about the radio.
   *  ★★ EVERY STICKY CONTROL, not just the two that got reported. `nr`/`notch` were
   *  added alone and the RSP notches and the NR STRENGTH kept lying for another
   *  fortnight. Fields are OPTIONAL: absent means the server has no opinion, which
   *  is not the same as off — render nothing rather than forcing a default. */
  onDspState?: (s: {
    nr: boolean; notch: boolean;
    /** FM weak-signal processing: stereo high-blend + audio high-cut, one switch. Reported so a
     *  reconnecting client restores the button rather than showing a state the radio is not in. */
    wsp: boolean;
    /** IMS — multipath suppression. Separate from wsp: noise and a reflection want opposite cures. */
    ims: boolean;
    /** CEQ — the blind channel equaliser. Separate again: it corrects a REFLECTION. */
    ceq: boolean;
    /** Noise blanker — impulses, the fourth fault and the fourth switch. */
    nb: boolean;
    /** ★ TEF-style automatic demodulator bandwidth, and the width it has settled on (Hz). */
    autobw?: boolean;
    autobwHz?: number;
    imsBlend?: number;
    nrStrength?: number;      // 0..1.4 (>1 = over-subtraction), absent = never set
    rfNotch?: boolean; dabNotch?: boolean;
    /** ★ TWO SEPARATE BIAS-TEES: `biasT` is the dongle's, `rspBiasT` the RSP's. Different
     *  hardware, different setter, different button — never fold them into one field. */
    biasT?: boolean; rspBiasT?: boolean;
  }) => void;
  /** ★ Demodulators the owner has switched off, from hwinfo. Called since it was added and never
   *  DECLARED here — so every caller was an implicit-any under a strict check. Harmless at run
   *  time, invisible in the narrow build gate, and exactly the kind of gap that hides a real
   *  signature mismatch later. */
  onBlockedModes?: (list: string[]) => void;
  onRds?:    (meta: RdsMeta) => void;
  /** Advanced RDS payload — only sent while the RDS decoder is attached. */
  onRdsX?:   (x: RdsExt) => void;
  /** Admin unlock result, or a refusal of a protected control. */
  onAdmin?:  (ok: boolean, refused: boolean) => void;
  /** Live RSP gain state — the AGC moves the IF reduction, so slider positions are not it. */
  /** Channel power and noise floor in the engine's dBFS, measured on the FULL-RATE FFT and so
   *  independent of zoom, view and bin width. The meter must prefer these over anything it
   *  derives from a spectrum frame — a frame's resolution is whatever the user zoomed to. */
  onSigStat?: (chanDb: number, floorDb: number) => void;
  /** ★ The CONVERTER, once a second — see the 'adc' message. `clip` is the percentage of samples
   *  ON THE RAIL, which is the figure that decides harm; the peak can read a comfortable -1 dBFS
   *  a moment before the front end is 7% railed. */
  onAdcStat?: (peakDbfs: number, clipPct: number) => void;
  /** ★ The tuner's IF filter in Hz, 0 = automatic. Its own callback rather than a fourteenth
   *  argument on onHwInfo — that signature is already at the length where a caller gets one
   *  positional wrong and nothing complains. */
  onTunerBw?: (hz: number, rfCentreHz: number, auto: boolean) => void;
  onRspStat?: (systemGainDb: number, lna: number, ifgr: number, overload: boolean,
               settling: boolean) => void;
  onStatus?: (s: 'connecting' | 'open' | 'closed' | 'error', detail?: string) => void;
  /** The server is already serving someone else — do not retry. */
  onBusy?: (q?: { queuePos?: number; queueLen?: number; freeIn?: number; queueFull?: boolean }) => void;
  /** ★★ THE OCCUPANCY STRIP on a shared-dial receiver: which mode this radio runs, how many are
   *  listening, who moved the dial last (an ordinal — never an identity), and whether somebody is
   *  mid-decode. Absent on an exclusive receiver, which is every existing one. */
  onDial?: (d: { mode: string; tuner: number; mine: boolean; you: number;
                 listeners: number; decoding?: boolean }) => void;
  /** Spectator mode refused a control. ★ Said out loud, because a control that silently does
   *  nothing reads as a broken radio rather than as a receiver somebody has parked deliberately. */
  /** The RTL overload protection stepped the gain: `steps` below the owner's ceiling, `dir` -1
   *  when backing off and +1 when recovering. */
  onOverload?: (steps: number, dir: number, gainTenthDb: number, agc: boolean,
                adcPeak?: number) => void;
  onDialRefused?: () => void;
  /** Somebody said one of the canned phrases. `id` is a phrase id, never text. */
  onSaid?: (from: number, id: string) => void;
  onYourTurn?: (withinSec: number) => void;
  /** Session limit: seconds remaining (fires at 2 min and 30 s). Still connected. */
  onSessionWarning?: (secs: number) => void;
  /** The limit expired; we have been disconnected. `cooldown` = seconds before we may return. */
  onSessionEnded?: (cooldown: number, fresh?: number) => void;
  /** Refused because we are still inside our cooldown. */
  onCooldown?: (secs: number) => void;
  /** The owner took the radio back using the admin password. */
  onEvicted?: () => void;
  /** This address is on the receiver's ban list. Terminal — there is no waiting it out. */
  onBanned?: () => void;
  onElsewhere?: (radio: string) => void;
  onHandover?: (secs: number) => void;
  onIdleCheck?: (secs: number) => void;
  onIdleClosed?: () => void;
  onHandoverOff?: () => void;
  /** ★ The dial has JUMPED far enough that anything already buffered is a different signal.
   *  main.ts uses this to drop queued audio — see AudioPlayer.flush(). */
  onRetuneJump?: () => void;
  /** ★★ SOMEBODY ELSE TURNED THE DIAL. Only ever fires on a shared receiver: the page redraws its
   *  readout, its mode and its VFO marker from values it did not choose. */
  onDialMoved?: (hz: number, mode: SDRMode) => void;
  /** ★ Admin controls were re-locked after an idle period. NOT a disconnection: the session,
   *  the audio and any decoder are all still running. `idleMin` is how long it waited. */
  /** Another session proved the owner's password more recently; this one is now an ordinary
   *  listener. Still connected, still listening — only the controls are gone. */
  onAdminSuperseded?: () => void;
  onAdminRelocked?: (idleMin: number) => void;
  onRtt?:    (ms: number) => void;
  /** Bytes received on the spectrum socket — the BIGGER half of the link. */
  onBytes?:  (n: number) => void;
}

export class SpectrumClient {
  private ws: WebSocket | null = null;
  private url: string;
  private cb: SpectrumCallbacks;
  private closedByUs = false;

  // Server-reported geometry.
  cfg: Config = {
    centerFreq: 0, binCount: 4096, binBandwidth: 0,
    totalBandwidth: 0, maxBandwidth: 0,
  };

  // VFO state (client-owned; the server never reports it back).
  frequency = 0;
  mode: SDRMode = 'nfm';
  bandwidthLow = -5000;
  bandwidthHigh = 5000;

  /** Locked = the view follows the VFO. Unlocked = pan freely. */
  followVfo = true;

  // Predicted view (what we've asked for), vs cfg (what the server acked).
  /** What we last believed about being admin, so the LOSS of it can be noticed. See the hwinfo
   *  handler: reporting only the positive left a relocked client showing UNLOCKED for ever. */
  private adminBelief = false;
  private view = { centerHz: 0, binBandwidth: 0 };
  private pendingView: { frequency: number; binBandwidth: number } | null = null;
  private sendTimer: number | null = null;
  /** When this client last tuned itself — see the dial-follow in the config handler. */
  private lastLocalTuneAt = 0;
  private lastSendAt = 0;
  private pingTimer: number | null = null;
  private lastPingAt = 0;
  private pendingGain: { tenthDb: number; auto: boolean } | null = null;
  private gainTimer: number | null = null;
  private lastGainAt = 0;
  private reconnectTimer: number | null = null;
  /** Set when the server told us it is busy — suppresses the auto-reconnect. */
  private refused = false;
  /**
   * ★★★ WHEN DID ANYTHING LAST ARRIVE? A WebSocket that has gone dead WITHOUT CLOSING is the
   *     failure this exists for: `onclose` never fires, so the 3-second reconnect below never
   *     runs, and the client sits there believing it is connected. Stuart, 2026-08-23, on the
   *     XCover through its tunnel: "I've been connected to the xcover listening to a station and
   *     I wanted to change the station but now its still playing audio but not tuning or showing
   *     the spectrum properly" — 0.0 fps in the status bar, and a page refresh cured it, which is
   *     what says client rather than server.
   * ★★★ AND IT EXPLAINS BOTH HALVES AT ONCE. **Every control rides this socket** — tune, mode,
   *     bandwidth, gain, squelch, the lot — while audio has one of its own. So a spectrum socket
   *     that dies quietly takes the waterfall AND the dial with it and leaves the music playing,
   *     which reads as "the server has half broken" and is nothing of the kind. The rule from
   *     memory/spectrum_socket_is_not_the_listener applies in reverse here: ask of anything that
   *     looks server-side, would it still be true with only the AUDIO socket alive?
   * ★★ The 5-second ping was never a watchdog. It measures round-trip time for the readout and
   *    nothing ever checked that an answer came back, so it could not notice silence.
   */
  private lastRxAt = 0;
  private aliveTimer: number | null = null;

  // Scratch buffers, resized on bin-count change.
  private bins: Float32Array | null = null;

  constructor(url: string, cb: SpectrumCallbacks) {
    this.url = url;
    this.cb = cb;
  }

  connect() {
    this.closedByUs = false;
    this.cb.onStatus?.('connecting');
    const ws = new WebSocket(this.url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;

    ws.onopen = () => {
      this.cb.onStatus?.('open');
      // Ask for the view we want (server echoes a config back).
      if (this.view.centerHz && this.view.binBandwidth) {
        this._flushView();
      }
      this.lastRxAt = performance.now();
      this.pingTimer = window.setInterval(() => {
        this.lastPingAt = performance.now();
        this._send({ type: 'ping' });
      }, 5000);
      // ★★ THE WATCHDOG. A ping every 5s and a frame rate of at least a few per second mean a
      //    silence of RX_DEAD_MS cannot be normal — so treat it as dead and close, which drops
      //    into the reconnect path that already exists and is already careful about refusals.
      //  ★ Closing rather than reconnecting directly keeps ONE recovery path: everything that
      //    happens after a drop (status, timers, the busy check) is written once, in onclose.
      this.aliveTimer = window.setInterval(() => {
        if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
        if (performance.now() - this.lastRxAt < SpectrumClient.RX_DEAD_MS) return;
        this.cb.onStatus?.('error', 'no data — reconnecting');
        try { this.ws.close(); } catch { /* already going */ }
      }, 2000);
    };

    ws.onmessage = (e) => {
      // ★ ANY inbound traffic counts as life, not just spectrum frames: a paused or squelched
      //   radio still answers pings and still sends state, and calling that dead would reconnect
      //   a perfectly good socket every fifteen seconds.
      this.lastRxAt = performance.now();
      if (typeof e.data === 'string') {
        this.cb.onBytes?.(e.data.length);
        this._handleText(e.data);
      } else {
        const buf = e.data as ArrayBuffer;
        this.cb.onBytes?.(buf.byteLength);
        this._handleBinary(buf);
      }
    };

    ws.onerror = () => this.cb.onStatus?.('error', 'websocket error');

    ws.onclose = (e) => {
      this._stopTimers();
      this.cb.onStatus?.('closed', e.code === 1006 ? 'connection lost' : `closed (${e.code})`);
      if (!this.closedByUs && !this.refused) {
        // ★ Clear the handle as it fires: it is now also read as "a reconnect is already pending"
        //   by _send, and a stale handle there would suppress the immediate retry for ever.
        this.reconnectTimer = window.setTimeout(() => { this.reconnectTimer = null; this.connect(); }, 3000);
      }
    };
  }

  close() {
    this.closedByUs = true;
    this._stopTimers();
    if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null; }
    this.ws?.close();
    this.ws = null;
  }

  private _stopTimers() {
    if (this.pingTimer) { clearInterval(this.pingTimer); this.pingTimer = null; }
    if (this.aliveTimer) { clearInterval(this.aliveTimer); this.aliveTimer = null; }
    if (this.sendTimer) { clearTimeout(this.sendTimer); this.sendTimer = null; }
  }

  /** ★ Fifteen seconds of nothing at all. Long enough that a stalled radio or a slow uplink is
   *  not mistaken for a dead socket, short enough that a listener who reaches for the dial gets
   *  it back before they give up and refresh the page — which is what they did instead. */
  private static readonly RX_DEAD_MS = 15000;

  /** Send an arbitrary control message. Used for radio-specific controls (RSP gain, notches)
   *  that have no place in the shared tune/gain vocabulary. */
  send(obj: Record<string, unknown>) { this._send(obj); }

  private _send(obj: Record<string, unknown>) {
    const ws = this.ws;
    if (ws && ws.readyState === WebSocket.OPEN) { ws.send(JSON.stringify(obj)); return; }
    // ★★★ A CONTROL THAT GOES NOWHERE MUST NOT GO QUIETLY. This dropped every tune, mode and gain
    //     on the floor whenever the socket was not open, so the dial moved on screen and the radio
    //     ignored it with nothing said — the exact experience that ended in a page refresh. A ping
    //     is exempt: it is our own heartbeat and saying "connection lost" because a heartbeat could
    //     not be sent, when the watchdog is already about to say it, is noise.
    if (obj && (obj as { type?: string }).type !== 'ping') {
      this.cb.onStatus?.('error', 'not connected — reconnecting');
      // ★ And ASK FOR IT BACK NOW. Somebody reaching for the dial is the best possible signal that
      //   the socket is wanted; waiting out the watchdog's remaining seconds serves nobody.
      if (!this.closedByUs && !this.refused && !this.reconnectTimer
          && (!ws || ws.readyState === WebSocket.CLOSED || ws.readyState === WebSocket.CLOSING)) {
        this.reconnectTimer = window.setTimeout(() => { this.reconnectTimer = null; this.connect(); }, 250);
      } else if (ws && ws.readyState === WebSocket.OPEN) {
        /* unreachable — kept for the reader: OPEN is handled above */
      } else if (ws && ws.readyState === WebSocket.CONNECTING) {
        /* ★ Already on its way back; the caller's control is lost but the socket is not. */
      }
    }
  }

  // ── Inbound ────────────────────────────────────────────────────────────────

  private _handleText(raw: string) {
    let msg: any;
    try { msg = JSON.parse(raw); } catch { return; }
    switch (msg.type) {
      case 'config': {
        const cfg: Config = {
          centerFreq:     msg.centerFreq ?? this.cfg.centerFreq,
          binCount:       msg.binCount ?? this.cfg.binCount,
          binBandwidth:   msg.binBandwidth ?? this.cfg.binBandwidth,
          totalBandwidth: msg.totalBandwidth ?? this.cfg.totalBandwidth,
          maxBandwidth:   msg.maxBandwidth ?? this.cfg.maxBandwidth,
          serverMode:     typeof msg.mode === 'string' ? msg.mode : this.cfg.serverMode,
          serverVfo:      typeof msg.vfo === 'number' && msg.vfo > 0 ? msg.vfo : this.cfg.serverVfo,
        };
        // ★★★ SOMEBODY ELSE MOVED THE DIAL — GIVE THIS LISTENER THE SAME RESET THE TUNER GETS.
        //
        //     A LOCAL tune flushes the audio buffer and re-arms it (see onRetuneJump in tune()):
        //     everything queued was demodulated at the old frequency, so playing it out is the
        //     previous station arriving late. On a shared dial the person tuning gets that reset
        //     and every MIRROR gets nothing — the retune simply lands mid-buffer, and any state
        //     that gets stuck stays stuck for the mirrors and never for the tuner.
        //
        //  ★★ Stuart put it exactly right (2026-08-20): "the stereo mono thing doesn't have an
        //     issue when the single user is controlling the VFO, so why does it if I am just
        //     listening to a mirror of it?" — because one of them is reset and the other is not.
        //     This is that asymmetry removed, not a workaround for it.
        //  ★ Same threshold as the local path, and for the same reason: below one bandwidth the
        //    buffered audio is still substantially what you are listening to, and flushing on
        //    every nudge would turn somebody else's smooth tune into stuttering silence here.
        {
          const oldVfo = this.cfg.serverVfo;
          const bw = Math.abs(this.bandwidthHigh - this.bandwidthLow) || 3000;
          if (oldVfo && cfg.serverVfo && Math.abs(cfg.serverVfo - oldVfo) > bw
              && Math.abs(cfg.serverVfo - this.frequency) > bw) {
            this.cb.onRetuneJump?.();
          }
        }
        this.cfg = cfg;
        // ★ Never let the view be WIDER than the capture. On a sample-rate DROP the device span
        // shrinks, but our old (wider) view span would persist and over-spread the axis — signals
        // bunch toward the VFO and the ticker reads a span the dongle can't actually see (Stuart
        // 2026-07-24). Clamp the held span to the new maxBandwidth and re-request so the server's
        // crop matches. Runs even mid-gesture, because an over-wide view is never valid.
        if (cfg.maxBandwidth > 0 && cfg.binCount > 0) {
          const widestBinBw = cfg.maxBandwidth / cfg.binCount;   // binBw for the full-capture span
          if (this.view.binBandwidth > widestBinBw * 1.001) {
            this.view.binBandwidth = widestBinBw;
            if (this.view.centerHz) this.zoom(this.view.centerHz, this.view.binBandwidth);
          }
        }
        // Adopt the server's view once our own sends have settled. While a
        // gesture is in flight our predicted view wins, otherwise the config
        // echoes fight the drag.
        if (!this._viewInFlight()) {
          this.view.centerHz     = cfg.centerFreq;
          this.view.binBandwidth = cfg.binBandwidth;
        }
        // ★★★ FOLLOW THE DIAL. On a shared receiver somebody ELSE moves the VFO, and this client
        //     adopted the server's frequency only on the FIRST config — so the audio moved and the
        //     readout, the VFO marker and the view all stayed where they were: "I can hear the
        //     audio moving about but my spectrum and my tuning readout are not updating" (Stuart,
        //     2026-08-20, listening to a dial I was turning).
        //  ★★ NOT WHILE OUR OWN TUNE IS IN FLIGHT. The server echoes every tune back as a config,
        //     so adopting blindly would fight a drag or a held step key with a value from before
        //     it — the same reason the view above waits for sends to settle.
        //  ★ Threshold and guard match the local path: below one bandwidth this is the same
        //    station and moving the readout would just make it jitter.
        {
          const bw = Math.abs(this.bandwidthHigh - this.bandwidthLow) || 3000;
          const settled = Date.now() - this.lastLocalTuneAt > 1500;
          // ★★★ THE READOUT FOLLOWS EVERY MOVE; only the EXPENSIVE half waits for a real jump.
          //     Gating the adopt on the demodulator's bandwidth meant 100 kHz steps on WFM (~200
          //     kHz wide) never registered, so a mirror lagged and then settled on the wrong
          //     frequency — found on the phone, fixed in both clients (2026-08-20).
          const moved = cfg.serverVfo ? Math.abs(cfg.serverVfo - this.frequency) : 0;
          if (settled && cfg.serverVfo && moved > 100) {
            this.frequency = Math.round(cfg.serverVfo);
            if (cfg.serverMode && cfg.serverMode !== this.mode) {
              this.mode = cfg.serverMode as SDRMode;
              const b = MODE_BANDWIDTHS[this.mode];
              if (b) { this.bandwidthLow = b[0]; this.bandwidthHigh = b[1]; }
            }
            // ★ Bring the VIEW with it when this client is following the VFO, exactly as a local
            //   tune does — otherwise the dial moves to a station that is off the side of the
            //   spectrum somebody is watching.
            if (this.followVfo && moved > bw) {
              const n = this.cfg.binCount || 4096;
              this.view.centerHz = this.frequency;
              this.zoom(this.frequency, this.view.binBandwidth || (this.cfg.totalBandwidth / n));
            }
            this.cb.onDialMoved?.(this.frequency, this.mode);
          }
        }
        this.cb.onConfig?.(cfg);
        break;
      }
      case 'busy':
        // Someone else has the radio. DON'T reconnect (the default 3s retry would hammer a busy
        // server forever, which is the OWRX-style bad-neighbour behaviour we avoid elsewhere).
        // ★★ The server now HOLDS this socket open rather than closing it, and re-sends this
        //    frame as our place in the queue changes — so `busy` arrives repeatedly and is an
        //    UPDATE, not just a refusal. Still terminal for reconnect purposes: the socket we
        //    already have is the thing keeping our place, and dropping it loses it.
        this.refused = true;
        this.cb.onBusy?.({ queuePos: Number(msg.queuePos) || undefined,
                           queueLen: Number(msg.queueLen) || undefined,
                           freeIn:   msg.freeIn === undefined ? undefined : Number(msg.freeIn),
                           queueFull: !!msg.queueFull });
        break;
      case 'your_turn':
        // ★★★ Our turn came up. The server has RESERVED the slot for this session for a short
        //     window, so reconnecting now is not a race — but it is also the only thing that
        //     claims it, and the window is short.
        this.refused = true;
        this.cb.onYourTurn?.(Number(msg.withinSec) || 20);
        break;
      // ★★★ ALL THREE OF THESE SET `refused`, and that is the whole safety of the feature.
      // Every one is the server DELIBERATELY turning us away, and the default 3s retry would
      // hammer it forever — the same reconnect war that made takeover the wrong default for
      // ordinary clients. A deliberate refusal must be terminal until the user acts.
      case 'session_expired':
        // Our time was up. Say so plainly, with how long before we may try again.
        this.refused = true;
        // ★ `fresh` = when a FULL turn returns — a different, longer window than the
        //   cooldown. See the server's note; 0 on a server that does not say.
        this.cb.onSessionEnded?.(Number(msg.cooldown) || 0, Number(msg.fresh) || 0);
        break;
      case 'cooldown':
        // We came back too soon after a timeout.
        this.refused = true;
        this.cb.onCooldown?.(Number(msg.secs) || 0);
        break;
      case 'evicted':
        // The owner took their radio back with the admin password. Not a fault, and not
        // something to retry into.
        this.refused = true;
        this.cb.onEvicted?.();
        break;
      // ★★★ ALREADY ON ANOTHER RADIO HERE. Terminal like the rest, and it names the radio: "in
      //     use" would send the visitor to wait for a slot that is not coming, when the fix is one
      //     click away — close the other radio.
      case 'elsewhere':
        this.refused = true;
        this.cb.onElsewhere?.(String(msg.radio || 'another radio'));
        break;
      case 'banned':
        // ★ The most terminal refusal there is: unlike busy or cooldown there is nothing to
        //   wait for, so retrying is pure noise on both ends. `refused` is what stops the
        //   auto-reconnect, and it matters most here.
        this.refused = true;
        this.cb.onBanned?.();
        break;
      // ★★★ SOFT LIMIT: SOMEBODY IS WAITING, AND THIS IS THE ONLY WARNING THERE IS. NOT terminal —
      //     the listener is still connected and still listening, and setting `refused` here would
      //     stop the reconnect they are entitled to attempt afterwards.
      // ★★★ "STILL LISTENING?" — NOT a refusal, and not terminal. The listener is connected and
      //     hearing the radio; this only asks whether anybody is there. Setting `refused` here
      //     would stop the reconnect they are perfectly entitled to.
      case 'idle_check':
        this.cb.onIdleCheck?.(Number(msg.secs) || 0);
        break;
      // ★ They did not answer. Terminal, so the client does not treat it as a dropped socket and
      //   silently reconnect into the slot it was just asked to give up.
      case 'idle_closed':
        this.refused = true;
        this.cb.onIdleClosed?.();
        break;
      case 'session_handover':
        this.cb.onHandover?.(Number(msg.secs) || 0);
        break;
      // ★ The waiter gave up before the clock ran out, so the notice is withdrawn. Without this
      //   the listener sits watching a countdown that will never fire and assumes the receiver
      //   has hung — the reprieve has to be as visible as the threat.
      case 'session_handover_off':
        this.cb.onHandoverOff?.();
        break;
      case 'session_warning':
        // Still connected — this is a countdown, not a refusal. Do NOT set refused.
        this.cb.onSessionWarning?.(Number(msg.secs) || 0);
        break;
      case 'notice':
        // ★ The owner's message to listeners, pushed the moment it is posted or cleared.
        this.cb.onNotice?.(typeof msg.text === 'string' ? msg.text : '');
        break;
      case 'device':
        // The server has lost (or regained) its radio. Without this the page just stops updating
        // and looks broken — a black waterfall with working controls, which tells the user nothing.
        this.cb.onDevice?.(msg.present !== false);
        break;
      case 'summon':
        // The host machine is looking for this tab. Handled by main.ts (flash + focus attempt).
        this.cb.onSummon?.();
        break;
      case 'hwinfo':
        if (msg.tunerBw !== undefined)
          this.cb.onTunerBw?.(Number(msg.tunerBw) || 0, Number(msg.rfCentre) || 0,
                              msg.tunerBwAuto === true);
        this.cb.onHwInfo?.(msg.gains ?? [], msg.rates ?? [], Number(msg.lockedRate) || 0,
                           Number(msg.maxFftRate) || 0, Number(msg.forceIdleSaver) === 1,
                           (msg.radio ?? null) as RadioCaps | null,
                           Number(msg.lockedCentre) || 0,
                           typeof msg.gainCap === 'number' ? msg.gainCap : -1,
                           msg.agcLocked === true,
                           typeof msg.gainNow === 'number' ? msg.gainNow : undefined,
                           msg.agc === 1 || msg.agc === true,
                           Number(msg.ovlSteps) || 0,
                           typeof msg.adcPeak === 'number' ? msg.adcPeak : undefined);
        // ★★ Demodulators the OWNER has switched off. The server also REFUSES them, so this is
        //    not the enforcement — it is what lets us leave them out of the menu entirely.
        //    Offering a mode that will be refused reads as "the feature is broken"; not offering
        //    it reads as "this receiver is for HF", which is the truth.
        if (Array.isArray(msg.blocked)) this.cb.onBlockedModes?.(msg.blocked as string[]);
        if (typeof msg.nr === 'boolean' || typeof msg.notch === 'boolean') {
          this.cb.onDspState?.({
            nr: msg.nr === true, notch: msg.notch === true,
            // ★ Defaults TRUE when absent: an older server that does not report it still HAS the
            //   treatment on, so showing OFF would be a lie about the radio.
            wsp: msg.wsp !== false,
            ims: msg.ims !== false,
            ceq: msg.ceq !== false,
            nb: msg.nb !== false,
            // ★ Only forward what the server actually stated. `undefined` travels through as
            //   "no opinion" and the renderer leaves that control alone.
            /* ★★★ DECLARED IN THE TYPE IS NOT COPIED ON THE WIRE. `autobw` sat in this callback's
             *     parameter type for hours while never being copied here, so `s.autobw` was
             *     ALWAYS undefined — and `!!undefined` is false, which pinned AUTO BW's button OFF
             *     no matter what the radio reported or how many times the default was changed.
             *     The identical omission in onRdsX kept the IMS status blank. Both are the same
             *     trap: an object literal builds the payload, TypeScript checks only the fields
             *     you DID write, and a missing one is indistinguishable from a false one.
             *  ★ Forwarded as undefined when absent — "no opinion" is not "off", exactly as the
             *    comment above this callback says, and the renderer leaves the control alone. */
            autobw:   typeof msg.autobw === 'boolean' ? msg.autobw : undefined,
            autobwHz: typeof msg.autobwHz === 'number' ? msg.autobwHz : undefined,
            nrStrength: typeof msg.nrStrength === 'number' ? msg.nrStrength : undefined,
            rfNotch:  typeof msg.rfNotch  === 'boolean' ? msg.rfNotch  : undefined,
            dabNotch: typeof msg.dabNotch === 'boolean' ? msg.dabNotch : undefined,
            biasT:    typeof msg.biasT    === 'boolean' ? msg.biasT    : undefined,
            rspBiasT: typeof msg.rspBiasT === 'boolean' ? msg.rspBiasT : undefined,
          });
        }
        // ★ Seconds left on a limited session, sent on connect so the clock starts THEN
        // rather than at the first warning. -1 = no limit / exempt.
        if (typeof msg.sessionSecsLeft === 'number' && msg.sessionSecsLeft >= 0) {
          this.cb.onSessionWarning?.(msg.sessionSecsLeft);
        }
        // ★★★ ARE WE ALREADY ADMIN? `hwinfo` has carried adminOk since the lock was built and
        //     NOTHING IN THIS CLIENT EVER READ IT — so an owner who connected as admin from the
        //     splash screen arrived with the server treating them as admin and the UI drawing
        //     every protected control greyed out. The only way back was to type the password a
        //     second time into the menu, which made the splash field look broken.
        //     ★ Same family as `doAdminUnlock` never being called: the state existed, the
        //       transport existed, and the two were never joined up.
        // ★★ ONLY THE POSITIVE. onAdmin(false) means "the password you just typed was refused"
        //    and pops an alert — but adminOk is false for every ordinary listener, so reporting
        //    it here would alert "that admin password was not accepted" on every single connect,
        //    to people who never typed one. Locked is already the default the UI draws.
        // ★★★ ...AND THE LOSS OF IT, WHICH IS THE HALF THAT WAS MISSING. Reporting only the
        //     POSITIVE is right for a listener who never typed a password — but it left a client
        //     that HAD been admin with no way to learn it no longer was. After the server's idle
        //     re-lock the menu still said UNLOCKED, the password row stayed hidden because
        //     unlocking had hidden it, and the admin page meanwhile asked for the password again:
        //     one receiver telling the owner two different things (Stuart, 2026-08-14).
        // ★★ The relock MESSAGE exists and is handled, but a message can be missed — a socket that
        //    reconnects, a client that joined after it was sent. hwinfo is the SERVER'S STANDING
        //    WORD and arrives continuously, so believing it in both directions is what makes this
        //    self-heal rather than depend on catching one packet.
        // ★ Only reported on a CHANGE, and never as a refusal, so it greys the controls without
        //   the "that password was not accepted" alert.
        if (msg.adminOk === true) {
          this.adminBelief = true;
          this.cb.onAdmin?.(true, false);
        } else if (msg.adminOk === false && this.adminBelief) {
          this.adminBelief = false;
          this.cb.onAdmin?.(false, true);
        }
        break;
      case 'rds':
        this.cb.onRds?.({
          stereo: !!msg.stereo, ps: msg.ps ?? '', radiotext: msg.radiotext ?? '',
          pi: msg.pi ?? -1, ecc: msg.ecc ?? 0,
          ber: typeof msg.ber === 'number' ? msg.ber : -1,
          sig: typeof msg.sig === 'number' ? msg.sig : -99,
        });
        break;
      case 'dial':
        this.cb.onDial?.({
          mode: String(msg.mode || 'exclusive'),
          tuner: Number(msg.tuner) || 0,
          mine: msg.mine === true,
          you: Number(msg.you) || 0,
          listeners: Number(msg.listeners) || 0,
          decoding: msg.decoding === true,
        });
        break;
      // ★★★ THE DONGLE'S OVERLOAD PROTECTION MOVED THE GAIN. Distinct from the RSP's `rspstat`
      //     overload flag, which reports a HARDWARE condition: this reports an ACTION we took, so
      //     the chip can say which way the gain went rather than only that something is wrong.
      case 'ovl':
        this.cb.onOverload?.(Number(msg.steps) || 0, Number(msg.dir) || 0,
                            Number(msg.gain) || 0, msg.agc === 1 || msg.agc === true,
                            typeof msg.adcPeak === 'number' ? msg.adcPeak : undefined);
        break;
      case 'dial_refused':
        this.cb.onDialRefused?.();
        break;
      case 'said':
        // ★ An id we do not know how to draw is DROPPED, not shown raw — the client half of the
        //   rule that keeps free text off this channel in both directions.
        this.cb.onSaid?.(Number(msg.from) || 0, String(msg.id || ''));
        break;
      case 'users':
        this.cb.onUsers?.(Number(msg.n) || 0, Number(msg.max) || 0);
        break;
      case 'admin':
        // ok=true after a correct password; refused=true when a protected control was
        // rejected, which is how a client learns it is locked without having asked.
        // ★ relocked=true is the idle re-lock. It is NOT a refusal and NOT a failed password,
        //   so it must not raise the "that password was not accepted" alert — it gets its own
        //   callback and its own quiet pill.
        if (msg.relocked === true) {
          this.cb.onAdminRelocked?.(Number(msg.idleMin) || 0);
          this.cb.onAdmin?.(false, true);   // grey the controls, without the alert
          break;
        }
        // ★★ SUPERSEDED: somebody proved the owner's password more recently, somewhere else.
        //    Admin is exclusive and the latest login wins — an owner who has left a session open
        //    on another machine must still be able to take control from the one in their hand.
        //    ★ Not a refusal and not a wrong password, so it takes the relock path's quiet
        //      treatment rather than the alert: the controls grey, and it says why.
        if (msg.superseded === true) {
          this.cb.onAdminSuperseded?.();
          this.cb.onAdmin?.(false, true);
          break;
        }
        this.cb.onAdmin?.(msg.ok === true, msg.refused === true);
        break;
      case 'rdsx':
        this.cb.onRdsX?.({
          pty: Number(msg.pty ?? -1), tp: Number(msg.tp ?? -1),
          ta: Number(msg.ta ?? -1), ms: Number(msg.ms ?? -1),
          di: Number(msg.di ?? -1),
          ptyRaw: Number(msg.ptyRaw ?? -1), tpRaw: Number(msg.tpRaw ?? -1),
          taRaw: Number(msg.taRaw ?? -1), msRaw: Number(msg.msRaw ?? -1),
          diRaw: Number(msg.diRaw ?? -1),
          ct: Number(msg.ct ?? -1), ctoff: Number(msg.ctoff ?? 0),
          af: Array.isArray(msg.af) ? msg.af : [],
          afseen: Number(msg.afseen ?? 0),
          afAll: Array.isArray(msg.afAll) ? msg.afAll : [],
          grp: Array.isArray(msg.grp) ? msg.grp : [],
          gtot: Number(msg.gtot ?? 0),
          rtpTitle: String(msg.rtpTitle ?? ''),
          rtpArtist: String(msg.rtpArtist ?? ''),
          longPs: String(msg.longPs ?? ''),
          ptyn: String(msg.ptyn ?? ''),
          lang: Number(msg.lang ?? 0),
          pinDay: Number(msg.pinDay ?? 0),
          pinHour: Number(msg.pinHour ?? -1),
          pinMin: Number(msg.pinMin ?? 0),
          eon: Array.isArray(msg.eon) ? msg.eon : [],
          oda: Array.isArray(msg.oda) ? msg.oda : [],
          phase: Number(msg.phase ?? -1),
          phaseCoh: Number(msg.phaseCoh ?? 0),
          phaseDrift: Number(msg.phaseDrift ?? 0),
          pilotDev: Number(msg.pilotDev ?? 0),
          pilotLock: Boolean(msg.pilotLock),
          rdsDev: Number(msg.rdsDev ?? 0),
          mpxSnr: Number(msg.mpxSnr ?? 0),
          multipath: Number(msg.multipath ?? 0),
          multipathOk: Number(msg.multipathOk ?? 0) === 1,
          snrOk: Number(msg.snrOk ?? 1) === 1,
          // ★ Default to 15000 (wide open), NOT 0. A server too old to send these would otherwise
          //   report a 0 Hz corner, which reads as "everything is being cut" — the alarming
          //   opposite of the truth.
          hiCutLmr: Number(msg.hiCutLmr ?? 15000),
          hiCutAud: Number(msg.hiCutAud ?? 15000),
          ifGain: Number(msg.ifGain ?? 0),
          ifCand: Number(msg.ifCand ?? 0),
          ifBw: Number(msg.ifBw ?? 0),
          /* ★★★ EVERY FIELD MUST BE COPIED HERE — THE INTERFACE DOES NOT DO IT. This builds an
           *     explicit object literal, so a field added to the server's message AND to the
           *     RdsExt type still arrives as `undefined` until it is named on this line. Nothing
           *     warns: the type says it exists, the wire carries it, the panel reads it, and it is
           *     silently absent. Three rounds of "still no IMS status" — after the message, the
           *     placement and the computation had each been fixed — came down to this. The old
           *     AUTO BW readout's permanent dash was the same omission.
           *  ★ If you add a field to RdsExt, add it here in the same edit. */
          imsBlend: Number(msg.imsBlend ?? 0),
          imsWhy: Number(msg.imsWhy ?? 1),
          ceqOn: Number(msg.ceqOn ?? 0) === 1,
          ceqAfter: Number(msg.ceqAfter ?? 0),
          nbRate: Number(msg.nbRate ?? 0),
          ceqWhy: Number(msg.ceqWhy ?? 3),
          xy: Array.isArray(msg.xy) ? msg.xy : [],
          mpx: Array.isArray(msg.mpx) ? msg.mpx : [],
        });
        break;
      case 'sig':
        // Channel power and noise floor, both measured server-side on the full-rate FFT so
        // they do NOT move with the view. See onSpectrum's signal-meter note.
        this.cb.onSigStat?.(Number(msg.chan), Number(msg.floor));
        break;
      case 'adc':
        this.cb.onAdcStat?.(Number(msg.peak), Number(msg.clip));
        break;
      case 'rspstat':
        this.cb.onRspStat?.(Number(msg.sysGain) || 0, Number(msg.lna) || 0, Number(msg.ifgr) || 0,
                            Number(msg.overload) === 1, Number(msg.settling) === 1);
        break;
      case 'pong':
        if (this.lastPingAt) this.cb.onRtt?.(performance.now() - this.lastPingAt);
        break;
    }
  }

  private _handleBinary(buf: ArrayBuffer) {
    if (buf.byteLength < 22) return;
    const dv = new DataView(buf);
    if (dv.getUint32(0, true) !== SPEC_MAGIC) return;
    const flags = dv.getUint8(5);
    if (flags !== FLAG_FULL_U8) return; // shim only ever emits FULL_UINT8

    const centerHz = Number(dv.getBigUint64(14, true));
    const n = buf.byteLength - 22;
    if (n <= 0) return;

    if (!this.bins || this.bins.length !== n) this.bins = new Float32Array(n);
    const bins = this.bins;
    const u8 = new Uint8Array(buf, 22, n);

    // Bins arrive in FFT order (DC first, then +f, then -f). Rotate by n/2 so
    // the array runs low→high frequency for the renderer.
    const half = n >> 1;
    for (let i = 0; i < n; i++) {
      const src = (i + half) % n;
      bins[i] = u8[src] + U8_DBFS_OFFSET;
    }

    const bwHz = this.cfg.binBandwidth * n;
    this.cb.onBins?.(bins, centerHz, bwHz);
  }

  // ── Outbound ───────────────────────────────────────────────────────────────

  /**
   * A sensible display span for a mode: roughly ten times the demod bandwidth.
   * Wide enough to see the signal in context, narrow enough that it isn't a
   * speck. (Same rule we settled on for the watch.)
   */
  private defaultSpanHz(mode: SDRMode): number {
    const [lo, hi] = MODE_BANDWIDTHS[mode];
    const bw = Math.abs(hi - lo);
    const span = bw * 10;
    const cap = this.cfg.maxBandwidth || span;
    return Math.max(MIN_SPAN_HZ, Math.min(span, cap));
  }

  /**
   * Tune the VFO. Recentres the view when locked (or when forced).
   *
   * `retarget` — for a DISCRETE JUMP (frequency entry, bookmark, search, spot):
   * also reset the span to something sensible for the mode. Carrying the old span
   * across a jump is how you end up tuning from 648 kHz AM (zoomed right in) to
   * 96.6 MHz FM and finding a single station filling a 250 kHz window. A jump to
   * a different band should not inherit the zoom of the one you left.
   */
  tune(frequency: number, mode?: SDRMode, opts?: { recenter?: boolean; retarget?: boolean }) {
    // ★★★ A JUMP INVALIDATES BUFFERED AUDIO. Everything in the playout buffer was demodulated at
    //     the old frequency, so after a big move it is the PREVIOUS station arriving late — up to
    //     a second of it, which is exactly the "audio lags the waterfall when I tune" report
    //     (Stuart, 2026-08-07: frequency entry and waterfall clicks).
    // ★★ ONLY ON A JUMP, and the threshold is the demodulator's own bandwidth. Flushing on every
    //    call would ruin holding the step button down: each 100 Hz nudge would drop the buffer and
    //    re-arm it, turning a smooth tune into stuttering silence. Below one bandwidth the
    //    buffered audio is still substantially the signal you are listening to, so it is kept.
    const prev = this.frequency;
    this.lastLocalTuneAt = Date.now();   // ★ so a config echo is not mistaken for somebody else
    if (frequency) this.frequency = Math.round(frequency);
    {
      const bw = Math.abs(this.bandwidthHigh - this.bandwidthLow) || 3000;
      if (prev && Math.abs(this.frequency - prev) > bw) this.cb.onRetuneJump?.();
    }
    if (mode) this.setMode(mode);
    else this._send({
      type: 'tune', frequency: this.frequency, mode: this.mode,
      bandwidthLow: this.bandwidthLow, bandwidthHigh: this.bandwidthHigh,
    });
    if (this.followVfo || opts?.recenter) {
      const n = this.cfg.binCount || 4096;
      let bb: number;
      if (opts?.retarget) {
        // A discrete jump. DON'T slam to the tight per-mode default — that lands a broadcast in a
        // ~20 kHz window that looks blocky/low-res (Stuart 2026-07-24). Instead: keep your current
        // zoom if it's already sensible, and only zoom PART way in from a wide view — to a
        // comfortable "landing" span, never all the way to the tight default. Clamp the preserved
        // zoom to [default, landing] so a jump across bands can't carry an absurdly tight window.
        const cur = (this.view.binBandwidth || this.cfg.binBandwidth) * n;
        const dflt = this.defaultSpanHz(this.mode);
        const landing = Math.min(this.cfg.maxBandwidth || Infinity, Math.max(dflt * 8, 200_000));
        const span = Math.max(dflt, Math.min(cur || landing, landing));
        bb = span / n;
      } else {
        bb = this.view.binBandwidth || this.cfg.binBandwidth;
      }
      if (bb) this.zoom(this.frequency, bb);
    }
  }

  setMode(mode: SDRMode) {
    this.mode = mode;
    const bw = MODE_BANDWIDTHS[mode];
    if (bw) { this.bandwidthLow = bw[0]; this.bandwidthHigh = bw[1]; }
    // The shim IGNORES bandwidth fields on a tune that also changes mode
    // (local_sdr_shim.cpp:1529) — so send mode, then bandwidth separately.
    this._send({ type: 'tune', frequency: this.frequency, mode });
    this._send({ type: 'bandwidth', bandwidthLow: this.bandwidthLow, bandwidthHigh: this.bandwidthHigh });
  }

  setBandwidth(low: number, high: number) {
    this.bandwidthLow = low;
    this.bandwidthHigh = high;
    this._send({ type: 'bandwidth', bandwidthLow: low, bandwidthHigh: high });
  }

  /** Current display span (Hz). */
  spanHz(): number {
    const n = this.cfg.binCount || 4096;
    return (this.view.binBandwidth || this.cfg.binBandwidth) * n;
  }

  viewCenterHz(): number { return this.view.centerHz || this.cfg.centerFreq; }

  // ── Capture geometry (mirrors of the shim's own maths) ──────────────────────
  // No protocol field carries these — the client reproduces the server's
  // arithmetic. Ported from UberSDRClient (captureBandwidth/localMargin/
  // rfCenterHz/panSpan), which mirrors the shim's viewDongleMargin/dongleForView.

  /** Real captured bandwidth (Hz) — the shim reports it as config.maxBandwidth. */
  captureBandwidth(): number { return this.cfg.maxBandwidth || 0; }

  /** Margin keeping the VFO inside the usable capture: above the 50 kHz
   *  auto-retune threshold AND clear of the RTL anti-alias rolloff (~10%). */
  private localMargin(fs: number): number { return Math.max(fs * 0.10, 60_000); }

  /** The RF (dongle) centre the shim is parked at: it follows the view but is
   *  clamped so the VFO stays captured, then locks. This is the "second VFO"
   *  marker — where the hardware actually is, vs where you're looking. */
  rfCenterHz(): number {
    const fs = this.captureBandwidth();
    if (!fs) return this.cfg.centerFreq;
    const lim = fs / 2 - this.localMargin(fs);
    const vfo = this.frequency;
    return Math.max(vfo - lim, Math.min(vfo + lim, this.viewCenterHz()));
  }

  /** How far the VIEW centre can roam before it hits the capture edge. */
  panSpan(): { loHz: number; hiHz: number } | null {
    const fs = this.captureBandwidth();
    if (!fs) return null;
    const reach = Math.max(0, fs - this.localMargin(fs) - this.spanHz() / 2);
    const vfo = this.frequency;
    return { loHz: vfo - reach, hiHz: vfo + reach };
  }

  /** Set view centre + span. binBandwidth is clamped to sane zoom limits. */
  zoom(frequency: number, binBandwidth: number) {
    const n = this.cfg.binCount || 4096;
    const spanCap = this.cfg.maxBandwidth > 0 ? this.cfg.maxBandwidth : 30e6;
    const bb = Math.max(MIN_SPAN_HZ / n, Math.min(binBandwidth, spanCap / n));
    this.view.centerHz = Math.round(frequency);
    this.view.binBandwidth = bb;
    this._sendView(this.view.centerHz, bb);
  }

  /**
   * Zoom by a factor about an ANCHOR frequency (>1 = zoom in). The anchor stays
   * pinned under the same screen position, so wheel-zooming homes in on whatever
   * you pointed at.
   *
   * Zooming about the VIEW CENTRE (the old behaviour) meant that when locked —
   * where the view centre IS the VFO — every zoom felt welded to the RF centre and
   * you couldn't zoom in on anything else.
   *
   * Omit the anchor to zoom about the VFO, which is what the +/- buttons want:
   * the thing you're listening to stays put and the span closes in around it.
   */
  zoomBy(factor: number, anchorHz?: number) {
    const bb = this.view.binBandwidth || this.cfg.binBandwidth;
    if (!bb) return;
    const n = this.cfg.binCount || 4096;

    // Clamp FIRST, so the anchor maths uses the span we'll actually get —
    // otherwise, at the zoom limit, the centre would keep sliding towards the
    // anchor while the span refused to change.
    const spanCap = this.cfg.maxBandwidth > 0 ? this.cfg.maxBandwidth : 30e6;
    const newBb = Math.max(MIN_SPAN_HZ / n, Math.min(bb / factor, spanCap / n));
    const actual = bb / newBb;               // the zoom we're really applying

    const anchor = anchorHz ?? this.frequency ?? this.viewCenterHz();
    const centre = this.viewCenterHz();
    const newCentre = anchor - (anchor - centre) / actual;

    this.zoom(newCentre, newBb);
  }

  pan(frequency: number) {
    this.zoom(frequency, this.view.binBandwidth || this.cfg.binBandwidth);
  }

  resetView() { this._send({ type: 'reset' }); }

  /** Frame decimation: server emits only every Nth frame. NB this saves BANDWIDTH
   *  ONLY — the server still computes every FFT. Use setFftRate() to save power. */
  setRateDivisor(n: number) { this._send({ type: 'set_rate', divisor: Math.max(1, Math.round(n)) }); }

  /** Live spectrum frame rate on the SERVER. Lowering it makes the serving phone
   *  skip the FFT work outright, so it saves real CPU and radio power — the point
   *  of the idle throttle. Audio is unaffected. */
  setFftRate(fps: number) { this._send({ type: 'fftRate', value: fps }); }

  // Hardware controls — the client drives the remote radio.

  /**
   * Set tuner gain — COALESCED, like the view sender, and for a harder reason.
   *
   * Every gain message becomes a synchronous USB control transfer to the dongle,
   * on the same bus that is carrying the bulk IQ stream. Dragging the slider fired
   * one per step (~10 in 200ms), and each one elbows the sample flow aside — which
   * is audible as breakup while you drag. Rate-limit to one per GAIN_SEND_MS, with
   * the trailing edge always delivered so the gain you release on is the gain the
   * radio actually ends up at.
   */
  setHwGain(tenthDb: number, auto: boolean) {
    this.pendingGain = { tenthDb, auto };
    const wait = this.lastGainAt + GAIN_SEND_MS - Date.now();
    if (wait <= 0) { this._flushGain(); return; }
    if (!this.gainTimer) {
      this.gainTimer = window.setTimeout(() => {
        this.gainTimer = null;
        this._flushGain();
      }, wait);
    }
  }

  private _flushGain() {
    const p = this.pendingGain;
    if (!p) return;
    this.pendingGain = null;
    this.lastGainAt = Date.now();
    this._send(p.auto ? { type: 'gain', auto: true } : { type: 'gain', value: Math.round(p.tenthDb) });
  }
  setHwBiasT(on: boolean)  { this._send({ type: 'biasT', on }); }
  setHwAgc(on: boolean)    { this._send({ type: 'agc', on }); }
  setHwPpm(ppm: number)    { this._send({ type: 'ppm', value: Math.round(ppm) }); }
  setHwSampleRate(r: number) { this._send({ type: 'sampleRate', value: Math.round(r) }); }
  /** ★ The tuner's IF filter in Hz; 0 = librtlsdr's automatic choice. RTL only. */
  /** ★ TEF6686-style automatic demodulator bandwidth. FM broadcast only; server-wide. */
  setAutoBw(on: boolean) { this._send({ type: 'autobw', on }); }
  setTunerBandwidth(hz: number) { this._send({ type: 'tunerbw', value: Math.round(hz) }); }
  setHwDirectSampling(v: 0 | 1 | 2) { this._send({ type: 'directSampling', value: v }); }

  // Audio DSP — runs server-side in the shim (the client stays a thin renderer).
  /** db <= -100 turns squelch off, matching the app's convention. */
  setSquelch(db: number) { this._send({ type: 'squelch', db }); }
  setNr(on: boolean, strength: number) { this._send({ type: 'nr', on, strength }); }
  setNotch(on: boolean) { this._send({ type: 'notch', on }); }
  /** tau in seconds: 0 = off, 50e-6 or 75e-6. */
  setDeemph(tau: number) { this._send({ type: 'deemph', tau }); }
  setStereo(on: boolean) { this._send({ type: 'stereo', on }); }
  /** FM weak-signal processing — high-blend + audio high-cut together. One switch because it is
   *  one treatment: A/B-ing half of it would not answer the question a DXer is asking. */
  setWeakProc(on: boolean) { this._send({ type: 'wsp', on }); }
  /** IMS — multipath suppression, TEF6686-style. NOT the same control as the noise treatment. */
  setIms(on: boolean) { this._send({ type: 'ims', on }); }
  /** CEQ — blind channel equaliser. Corrects a REFLECTION; cannot help noise and is gated so it
   *  does not try. */
  setCeq(on: boolean) { this._send({ type: 'ceq', on }); }
  /** Noise blanker — impulse noise. Not the same as NR: impulses are brief and enormous, and the
   *  cure is to remove the moments they occupy, not to filter continuously. */
  setNoiseBlanker(on: boolean) { this._send({ type: 'nb', on }); }

  // ── Coalesced view sender ──────────────────────────────────────────────────

  private _viewInFlight(): boolean {
    return Date.now() - this.lastSendAt < VIEW_SETTLE_MS;
  }

  /** Keep only the latest target; send ≤1 per VIEW_SEND_MS, trailing edge always
   *  delivered so the final position of a gesture lands. */
  private _sendView(frequency: number, binBandwidth: number) {
    this.pendingView = { frequency, binBandwidth };
    const wait = this.lastSendAt + VIEW_SEND_MS - Date.now();
    if (wait <= 0) { this._flushView(); return; }
    if (!this.sendTimer) {
      this.sendTimer = window.setTimeout(() => {
        this.sendTimer = null;
        this._flushView();
      }, wait);
    }
  }

  private _flushView() {
    const p = this.pendingView ?? {
      frequency: this.view.centerHz, binBandwidth: this.view.binBandwidth,
    };
    if (!p.frequency || !p.binBandwidth) return;
    this.pendingView = null;
    this.lastSendAt = Date.now();
    this._send({ type: 'zoom', frequency: p.frequency, binBandwidth: p.binBandwidth });
  }
}
