/**
 * ★★★ THE SHARED VOCABULARY OF THE UberSDR-FAMILY PROTOCOL — TYPES AND CONSTANTS ONLY (2026-09-22).
 *
 * Stuart: "I would rather have fully separate VibeServer and UberSDR components like Kiwi and OWRX
 * and FM-DX are separate codebases inside the app." The two clients (UberSDRWsClient in
 * SdrWsClient.ts, VibeServerWsClient.ts) are now separate copies and share NO behaviour. What they
 * share is this: the words the rest of the app uses to talk about a radio — SDRMode, SDRStatus,
 * the callbacks — exactly as Kiwi, OWRX and FM-DX already share SDRBackend.
 * ✗ Nothing that DOES anything belongs in this file. A function here is a shared component again.
 */
import type { DabState } from './dabTypes';

/** The one sentence a refused-as-too-old client shows; SDRScreen matches on it to add the button. */
export const UPDATE_APP_MESSAGE = 'This server needs a newer VibeSDR. Update the app, or open the receiver in your browser for now.';

// ── Types ─────────────────────────────────────────────────────────────────────

// 'wfm' = broadcast FM (stereo); local-hardware (RTL-SDR) only — UberSDR is HF.
export type SDRMode = 'usb' | 'lsb' | 'am' | 'sam' | 'fm' | 'nfm' | 'cwu' | 'cwl' | 'wfm';

/** Server-side mode bandwidth defaults (websocket.go, verbatim). */
export const MODE_BANDWIDTHS: Record<SDRMode, [number, number]> = {
  usb: [50, 2700],     lsb: [-2700, -50],
  am:  [-5000, 5000],  sam: [-5000, 5000],
  cwu: [-200, 200],    cwl: [-200, 200],
  fm:  [-6000, 6000],  nfm: [-5000, 5000],
  wfm: [-100000, 100000],
};

export interface SDRStatus {
  frequency: number;    // Hz
  mode: SDRMode;
  bandwidthLow: number;  // Hz, negative = below carrier
  bandwidthHigh: number; // Hz, positive = above carrier
  binCount: number;
  binBandwidth: number;  // Hz per bin
  centerHz: number;      // center of spectrum display (may be PREDICTED during a gesture)
  bwHz: number;          // total spectrum bandwidth
  /** The ACTUAL centre of the bins in THIS frame (from the frame header), never the
   *  predicted display centre. Consumers that INDEX INTO the bins (the watch crop) must use
   *  this — using the predicted centerHz points at the wrong bin and draws the signal offset
   *  from the VFO (the "signal next to the VFO" bug). The full-spectrum display can keep using
   *  centerHz for gesture continuity. */
  trueCenterHz?: number;
}

/** ★ ONE FRAME OF THE ADVANCED RDS ANALYSER, exactly as VibeServer computes it.
 *  ★★ NOTHING HERE IS DERIVED ON THE CLIENT — not the constellation, not the MPX curve, not
 *  the confirmations. The analyser runs beside the decoder on the server, where the baseband
 *  actually is; every client just draws this. That is why the phone can show the same panel
 *  as the browser without a line of DSP, and why a fix to the decoder reaches all of them.
 *  ★ Every gated field appears TWICE: the plain name is CONFIRMED BY REPETITION, the `*Raw`
 *  one is what arrived this instant. The server always sends both and never picks — a viewer
 *  switching to RAW must not change what another listener on the same receiver sees. */
/** ★★ What POST /connection tells us about OUR standing with this receiver — per-IP, read fresh on
 *  every connect. Measured against WESSEX 2026-07-31; see _checkConnection for the traps.
 *   idleSecs        seconds of inactivity before the server drops us. **0 = NO LIMIT** (valid).
 *   maxSessionSecs  hard session cap (WESSEX: 14400 = 4 h). 0 = none advertised.
 *   dailyUsedSecs / dailyLeftSecs   per-IP daily quota. **−1 = unlimited.** */
export interface IdlePolicy {
  idleSecs: number;
  maxSessionSecs: number;
  dailyUsedSecs: number;
  dailyLeftSecs: number;
}

export interface RdsExt {
  pty: number; tp: number; ta: number; ms: number; di: number;
  ptyRaw: number; tpRaw: number; taRaw: number; msRaw: number; diRaw: number;
  ct: number;            // minutes since midnight UTC, -1 = none
  ctoff: number;         // local offset in HALF-hours (India is +11)
  gtot: number;          // total groups decoded — 0 means no block sync at all
  afseen: number;
  rtpTitle: string; rtpArtist: string; longPs: string; ptyn: string;
  lang: number; pinDay: number; pinHour: number; pinMin: number;
  phase: number;         // RDS-to-pilot phase, degrees, folded to [0,90]
  phaseDrift: number; phaseCoh: number;
  pilotDev: number; rdsDev: number;   // kHz; rdsDev < 0 = not measurable (rdsDev is AVERAGED)
  /** ★★ MEASURED PEAK RDS deviation, no assumed crest factor — see RdsExt::rdsDevPeakKHz.
   *  `rdsDev` uses a fixed 1.520 crest where Hans's Pira implies 1.770, so it reads ~16 % low;
   *  this is the figure an analyser would agree with. 0 = not measured, so draw a dash. */
  rdsDevPeak: number;
  /** ★ Stereo PLL locked. NOT the constellation's lock, which is RDS — see AdvRdsPanel. */
  pilotLock: boolean;
  ber: number;           // block error rate %, -1 = unknown
  grp: number[];         // per-group-type counts, 32 entries (0A,0B,1A...)
  af: number[];          // alternative frequencies, kHz
  eon: { pi: string; ps: string; af: number; ta: number }[];
  oda: { aid: string; grp: number }[];
  xy: number[];          // constellation, interleaved i/q, x100, clipped +/-127
  mpx: number[];         // MPX spectrum, dB, integers in [-128, 0]
  /** The composite eye: eyeW*eyeH intensities, row 0 = top (+peak), one character per cell from
   *  a fixed 64-character alphabet (EYE_ALPHABET in AdvRdsPanel, kEyeAlphabet on the server —
   *  they must match). eyeDev is the kHz deviation full scale represents; the plot autoscales. */
  /** One grid per component — [0] pilot 19 kHz, [1] stereo L-R 38 kHz, [2] RDS 57 kHz,
   *  run-length encoded (see the encoder in local_sdr_shim.cpp). Drawn additively they
   *  reproduce the composite picture with the colour saying what makes each part of it. */
  eyeP: string;
  eyeS: string;
  eyeR: string;
  /** The eye's own per-band deviation estimate, kHz [pilot, stereo, rds] — only the stereo
   *  figure is quoted (pilot and RDS have coherent measurements of their own). */
  eyeAmp: number[];
  eyeW: number;
  eyeH: number;
  eyeDev: number;
  /** Total peak deviation of the whole composite including audio, kHz; 75 is the limit.
   *  ★★★ THREE STATISTICS OF ONE MEASUREMENT, as PIRA's P75 family shows MAX/AVE/MIN. We used to
   *  publish only the 1.5 s average and call it "deviation", which is why Onfliner reported the
   *  meter reading low on jazz/classical/speech (2026-09-25, [[BRIEF-deviation-meter]]): on
   *  processed programme average ≈ peak so it looked right, but at a 10-20 dB crest factor a
   *  75 kHz peak read about 38. Both testers were telling the truth.
   *   • mpxDev  — the TRUE PEAK: instant attack, 0.9 s decay. Fast-moving; drives the BAR FILL,
   *               where movement is information.
   *   • mpxAvg  — the 1.5 s average mpxDev used to carry. Steady context figure. 0 = an older
   *               server that does not send it, and then no avg is quoted at all.
   *   • mpxHold — instant attack, 6 s decay: the excursion memory (PIRA's MAX). This is what the
   *               DIGITS and the verdict read, because a number that yo-yos cannot be read
   *               (Stuart: "it looks like a stopwatch, how do you read that?"), and because the
   *               question a modulation monitor answers is "did it go over". */
  mpxDev: number;
  mpxAvg: number;
  mpxHold: number;
  /** What the deviation bar removed as noise, kHz rms in its measurement band (0 = not measured). */
  mpxNoise: number;
  // ── The weak-signal readings (VibeServer 3.1) ──────────────────────────────────────────────
  /** Pilot against the transmitted-silence gap at 15-19 kHz, dB. NOT a calibrated SNR — the
   *  measuring filter's own leakage caps it near 34 — but the figure that drives NR, and directly
   *  comparable between stations on one receiver. */
  mpxSnr: number;
  /** Whether there is a usable pilot at all. Both mpxSnr and multipath are ratios against it, so
   *  without one they are arithmetic rather than measurement. */
  snrOk: boolean;
  /** Envelope-AM depth with the measured noise contribution removed: what is left is a REFLECTION.
   *  An FM carrier leaves the transmitter at constant amplitude, so this is damage done by the
   *  path — and it is NOT the same fault as weakness. */
  multipath: number;
  multipathOk: boolean;  // false = too noisy to tell a reflection from the noise
  hiCutLmr: number;      // where high-blend has rolled the stereo difference off, Hz
  hiCutAud: number;      // where the audio high-cut is sitting, Hz
  nbRate: number;        // fraction of samples the noise blanker is excising
  ceqOn: boolean;        // the equaliser is engaged
  ceqAfter: number;      // multipath depth AFTER it — against `multipath`, which is what arrived
  ceqWhy: number;        // 0 running, 1 off, 2 signal too weak, 3 nothing to correct
  ifGain: number;        // dB the OTHER IF option would gain; the sign flips once narrowed
  ifCand: number;        // the candidate IF width being evaluated, Hz
  ifBw: number;          // the IF width in use now, Hz (0 = wide open)
  /** Every AF glimpsed as [kHz, confirmed]. On a noisy station the unconfirmed ones are usually
   *  phantoms manufactured by block errors, and only the list shows WHICH they are. */
  afAll: [number, number][];
}

/** ★ WHAT THE CONNECTED RADIO ACTUALLY HAS, straight from the server (hwinfo.radio).
 *  ★★ THE CLIENT MUST NOT GUESS. An Airspy HF+ has no tuner gain table, no LNA STATE ladder
 *  and no IF gain reduction — it has an AGC, an 8-step attenuator and a preamp. Drawing a
 *  dongle's gain slider for it is not a cosmetic error: the control does nothing useful and
 *  the ones that would work are absent. Show only what the driver reports. */
export interface RadioCaps {
  driver: 'rtl' | 'sdrplay' | 'airspyhf' | string;
  model: string;
  /** ★ An rtl_tcp stream whose server reported NO gains — relayed IQ (a VibeServer's raw-IQ output,
   *  an UberSDR), not a dongle. There is no tuner at the far end: no gain, no VibeAGC, no dongle
   *  settings. The engine refuses them; the panel does not offer them. */
  noHwGain?: boolean;
  // ── Airspy R2 / Mini (driver === 'airspy') ──
  /** 22 preset positions on either curve; each stage is 0-15. */
  gainPresets?: number;
  stageMax?: number;
  curve?: 'linearity' | 'sensitivity' | string;
  lnaAgc?: boolean;
  mixerAgc?: boolean;
  /** ★ The Airspy's mixer stage. Its LNA and VGA reuse `lna`/`vga` below — the HackRF declares
   *  those already and they mean the same kind of thing (a stage value the radio reports). */
  mixer?: number;
  packing?: boolean;
  hasPacking?: boolean;
  /** ★ 24-1800 MHz with no direct-sampling branch: the control must not be offered. */
  noDirectSampling?: boolean;
  // ── Airspy HF+ ──
  attSteps?: number;        // 9 => 0..8
  attStepDb?: number;       // 6 dB per step
  hfLna?: boolean;          // +6 dB preamp
  hfAgc?: boolean;
  agcThreshold?: boolean;   // low/high
  calPpb?: boolean;
  /** ★ Tunable windows, Hz. The HF+ has a REAL HOLE at 31–60 MHz — not a weak spot, absent.
   *  A client that does not know cannot stop someone parking on a dead frequency. */
  ranges?: [number, number][];
  rates?: number[];
  // ── SDRplay RSP ──
  // ── HackRF One ──
  /** ★★★ THE STAGES AS THE RADIO ACTUALLY HAS THEM, not as this client last set them. A HackRF is
   *  shared like every other radio here: another listener may have raised the gain, and a
   *  reconnect finds whatever the previous session left. Mirroring only our own writes would show
   *  0 dB on a radio sitting at 24 — the same class of lie as claiming maximum gain under a cap.
   *  ★ It matters MORE on this radio than any other, because it opens every stage at ZERO by
   *  design: 0 is both the safe default and a real value, so "0" is exactly what a stale mirror
   *  looks like and there is nothing to tell them apart. */
  amp?: number; lna?: number; vga?: number; biast?: number;
  hrfAmp?: boolean; hrfLna?: boolean; hrfVga?: boolean; hrfBiasT?: boolean;
  hwAgc?: boolean;
  // ── SDRplay RSP ──
  lnaStates?: number;
  ifGrMin?: number; ifGrMax?: number;
  agcSetPoint?: boolean;
  rfNotch?: boolean; dabNotch?: boolean; biasT?: boolean;
  /* ★★★ THE RSP SWEEP, 2026-09-24 (GitHub #29). CAPABILITY AND STATE TRAVEL TOGETHER, every time:
   *  publishing "this radio has three aerials" without "it is on B" is the bias-T fault this file
   *  already carries a note about — a control whose state cannot be read is not a control.
   *  ★ `antennas` is the radio's OWN list of port names, and the UI draws from it and nothing
   *    else — so a single-socket RSP1 publishes an empty list and no selector appears at all. */
  antennas?: string[]; antenna?: string; antennaLocked?: boolean;
  hdr?: boolean;     hdrOn?: boolean;        // RSPdx: high dynamic range below 2 MHz
  amNotch?: boolean; amNotchOn?: boolean;    // RSPduo: AM broadcast notch on the Hi-Z port
  extRef?: boolean;  extRefOn?: boolean;     // RSP2 / Duo: 24 MHz reference output
}

export type { DabState } from './dabTypes';

/** ★ Raw IQ out, as the server reports it. `public` = through the tunnel: a pairing code for
 *  the VibeIQ bridge instead of a LAN address. */
export interface IqOutState {
  on: boolean; rate?: number; host?: string; port?: number; code?: string; token?: string; public?: boolean; why?: string;
}

export interface SDRCallbacks {
  onSpectrum:   (bins: Float32Array, status: SDRStatus) => void;
  onStatus:     (status: SDRStatus) => void;
  onError:      (msg: string) => void;
  onConnect:    () => void;
  onDisconnect: () => void;
  /** Link quality: 0=down, 1=poor(red), 2=fluctuating(yellow), 3=good(green).
   *  Derived from frame inter-arrival jitter, stalls, ping RTT, reconnects. */
  onLink?:      (q: 0 | 1 | 2 | 3) => void;
  /** Adaptive link state: how far the controller has throttled (1 = full) and whether it is still
   *  working the link out. Lets the UI show a throttled-but-fine link honestly instead of red. */
  onLinkRate?:  (adaptiveRung: number, settling: boolean, fps: number, kbps: number) => void;
  /** True from the moment a recovery starts (socket torn down) until the first
   *  frame arrives on the fresh one. The watch cannot infer this — from the wrist
   *  a recovery-in-progress and a dead phone look identical — so the phone, which
   *  knows, says so. Drives the watch's "reconnecting" pill instead of a black
   *  overlay thrown over a recovery that is working. */
  onReconnecting?: (busy: boolean) => void;
  onDbg?:       (msg: string) => void;
  /** ★★★ THE SHARED DIAL, and the canned chat that goes with it. On an unlocked receiver with
   *  room for several listeners the server enforces NOTHING about who tunes — Stuart, 2026-08-20:
   *  "the dial must be like FM-DX where anybody can tune it, otherwise I would need to be on the
   *  server 24/7 to allow access to it." So the chat is not decoration beside the mechanism; it
   *  IS the mechanism by which two strangers sort the dial out between themselves.
   *  ★ `mode` is 'exclusive' on an ordinary receiver, and nothing chat-shaped should be shown. */
  onDial?:      (d: { mode: string; tuner: number; mine: boolean; you: number;
                      listeners: number; decoding: boolean }) => void;
  /** Spectator mode said no — the owner tunes this one. An explanation, not an error. */
  onDialRefused?: () => void;
  /** ★★★ THE CLIENT IS PUTTING THE DIAL BACK WHERE THIS PERSON LEFT IT — a deliberate restore of
   *  their own last tune, not the screen settling. It exists because the audio socket SEALS itself
   *  on a dial that might be shared, and that seal cannot tell a restore from the socket merely
   *  restating its opening position: both carry the same numbers. So the restore was dropped, the
   *  readout showed the remembered frequency and the radio stayed where it was opened — cured only
   *  by nudging the dial, which is the one thing that differs from `startTune`.
   *  ★ Measured on the XCover, 2026-09-24: `zoom -> 909000` twice and no tune at all, with the
   *    dongle still on the 100 MHz it was opened at (Stuart's video; Onfliner's "sound at 100.0").
   *  ★ The screen answers it by counting it as a user tune, which is what it is: a person asked
   *    for this frequency, just not in this session. */
  onRestoredTune?: (hz: number, mode?: string) => void;
  /** ★ Somebody ELSE moved the shared dial, with the frequency they moved it to. Distinct from
   *  onStatus, which cannot say WHO caused the change — and "why did it move" needs an answer on
   *  screen or the receiver looks broken. */
  onDialMoved?: (hz: number, mode?: string) => void;
  /** Somebody said one of the canned phrases. IDS TRAVEL, NOT TEXT: `from` is an ordinal ("User
   *  3"), and an id this build cannot draw must be DROPPED rather than shown raw. */
  onSaid?:      (from: number, id: string) => void;
  /** ★★★ THE SESSION IS NOW REGISTERED WITH THE SERVER (POST /connection returned allowed).
   *  UberSDR drops an audio WebSocket whose session it has never seen — it completes the
   *  handshake and closes in the same breath, which the phone reports as an abort and the user
   *  hears as silence on a perfect waterfall. The native audio engine opens its OWN socket from a
   *  separate component, so nothing made it wait for this; over a LAN it always lost the race and
   *  over a slow tunnel it usually won (issue #20). Fire it, and let the audio start here. */
  onSessionRegistered?: () => void;
  /** VibeServer: the serving device's supported tuner gains (tenths of dB), so a
   *  remote client can populate its gain slider (it can't query the HW natively). */
  /** The broadcast-FM treatments as the RADIO reports them — sticky and shared, so this is the
   *  only authority on what they are actually set to. */
  /** ★★ `autobw` is OPTIONAL and forwarded as undefined when the server did not state it — "no
   *  opinion" is not "off". The web client learned that the hard way: `!!msg.autobw` turned a
   *  missing field into a false one and pinned its AUTO BW button off for ever. */
  onFmDsp?:     (s: { wsp: boolean; ims: boolean; ceq: boolean; nb: boolean;
                      autobw?: boolean; nbx?: boolean }) => void;
  /** ★★★ DAB, and it is a VIBESERVER-ONLY callback. `s` is the whole measured state of the
   *  multiplex (see DabState — every field is MEASURED, nothing inferred); null means DAB has
   *  ended, and `err` carries the server's refusal when it could not start. */
  onDab?:       (s: DabState | null, err?: string) => void;
  /** ★ Raw IQ out: the server's answer to iqOut() — where the stream is, or why not. */
  onIqOut?:     (m: IqOutState) => void;
  onHwGains?:   (gains: number[]) => void;
  /** ★★★ WHERE THE GAIN ACTUALLY IS on the serving radio, in its own units; -1 = auto/AGC. The
   *  slider FOLLOWS this. A client cannot query a remote dongle, so before the server sent it the
   *  app showed its own idea of the gain and, on connecting, pushed it — overriding the owner's
   *  resting gain and re-gaining a shared receiver under everyone already listening. */
  onHwGainNow?: (tenthDb: number) => void;
  /** ★ Direct sampling as the RADIO has it: the owner's AUTO switch and crossover, and what is live
   *  now (0 off / 2 Q-branch). Without it the control could only offer Off/On and disagreed with a
   *  radio set to Auto (2026-09-22). */
  onHwDirectSampling?: (autoDs: boolean, belowHz: number, live: number) => void;
  /** ★★★ WHERE THE RATE ACTUALLY IS on the serving radio, in Hz — `gainNow` one field over, and
   *  the same lesson. A client cannot query a remote dongle, so with nothing to adopt the picker
   *  could only show what this phone happened to remember, and a client that shows a rate the
   *  radio is not using is worse than one with no picker at all: nothing tells you to look.
   *  ★★ ADOPTED, NEVER PUSHED BACK. Arriving at somebody's receiver is not a reason to change it,
   *     and on a shared one it re-spans the radio under everybody already listening. Stuart,
   *     2026-09-10: "the app should obey the server on initial connection not force itself upon
   *     the server and change settings blindly." */
  onHwRateNow?: (hz: number) => void;
  /**
   * ★★★ VIBEAGC, AND WHICH WAY IT JUST MOVED. The server runs its own gain loop for RTL-SDR and
   *     announces every move (`hwinfo.agc` for the state, a separate `ovl` message for the event).
   *     Neither reached this client at all, so on the phone an automatic gain was invisible: the
   *     radio changed under the listener with nothing on screen to say why.
   *  ★ `dir` is +1 when the gain went UP and −1 when it came down, so the readout can say which
   *    rather than only where it landed.
   */
  onHwAgc?: (on: boolean) => void;
  onOverload?: (o: { gainTenthDb: number; dir: number; agc: boolean }) => void;
  /** VibeServer: the sample rates (spectrum spans) THIS server offers, so the
   *  client's rate picker aligns with the server rather than a generic list. */
  onHwRates?:   (rates: number[]) => void;
  /** >0 = the serving host PINNED the capture rate; the client hides its picker. */
  onHwLockedRate?: (rate: number) => void;
  /** ★ The owner has FORCED the AGC on: a listener may not set a gain at all. The web client has
   *  always read this; the app never did, which is why its panel offered a gain the server refuses. */
  onHwAgcLocked?: (locked: boolean) => void;
  /** ★★★ The owner has FIXED the gain on the band being listened to — every gain message is
   *  refused, so the panel must show no gain controls at all rather than ones that spring back.
   *  Distinct from onHwGainCap, which is a working control with a lower ceiling. Per band: it
   *  arrives and departs with the ceiling as the listener tunes. */
  onHwGainLocked?: (locked: boolean) => void;
  /** The least IF gain reduction the owner allows on this band, in dB; -1 = none. RSP only. */
  onHwIfGrFloor?: (db: number) => void;
  /** ★★★ THE OWNER'S GAIN CEILING FOR THE FREQUENCY WE ARE ON, in TENTHS of a dB, or -1 for none.
   *  ★★★ THE SERVER ENFORCES IT AND THE CLIENT MUST SHOW IT. A panel that offers gain the server
   *  will silently clamp is a panel that LIES — the listener drags to maximum, the readout says
   *  40 dB and the radio is at 20. Stuart has seen exactly that. This is the same contract
   *  lockedRate and agcLocked already have: the server enforces, the client must not offer.
   *  ★ Frequency-dependent (per-band rules), so it can CHANGE ON A RETUNE — treat it as live
   *  state, not as a property of the radio. */
  onHwGainCap?: (capTenthDb: number) => void;
  /** Modes and decoders the owner has switched off (lower case ids). */
  onHwBlockedModes?: (list: string[]) => void;
  /** ★ The tuner's IF filter: the width in Hz (0 = wide open) and whether it is FOLLOWING THE
   *  ZOOM. The web client has had this picker since the filter existed; the app never had one. */
  onHwTunerBw?: (hz: number, auto: boolean) => void;
  /** Advanced RDS analyser frame (~5 Hz), only while setAdvRds(true). */
  onRdsExt?:    (x: RdsExt) => void;
  /** What the serving radio is and what it can do (hwinfo.radio). */
  onRadioCaps?: (caps: RadioCaps) => void;
  /** ★ RSP live state, ~10/s while an SDRplay is serving. `sysGain` is the API's own computed
   *  TOTAL system gain and `overload` is the radio's own ADC-clipping event — neither is inferred
   *  from the spectrum the way a dongle's would have to be. `settling` covers the moment after a
   *  gain change when the reading is not yet meaningful. */
  onRspStat?: (s: { sysGain: number; lna: number; ifgr: number;
                    overload: boolean; settling: boolean;
                    /** Which notches are ON right now. */
                    rfNotch: boolean; dabNotch: boolean;
                    /** ★ WHO OWNS THEM. `autoNotch` = the server is choosing them from the tuned
                     *  frequency and will refuse a listener's; `userNotch` = listeners are allowed
                     *  to. A control the server will refuse must not be drawn as live. */
                    autoNotch: boolean; userNotch: boolean;
                    /** ★ The RF AGC (our LNA loop) and the IF AGC's target level, dBFS. */
                    rfAgc: boolean; agcSet: number;
                    /** ★ Our own measurement of the ADC, not the API's — live even when the
                     *  API's event path has frozen. */
                    adcPeak: number; adcClip: number;
                    /** ★ LNA states available AT THIS FREQUENCY (per band, not per model).
                     *  0 = not reported; fall back to the capability. */
                    lnaN: number;
                    /** ★ The chip is (re)initialising — readings are not yet meaningful. */
                    agcInit: boolean; agcReinit: boolean;
                    /** ★ The SDRplay gain API has frozen: every figure above is stale and a reset
                     *  is available. See the gain-API chip. */
                    gainStuck: boolean }) => void;
  /** ★ Admin lock state. `set` = this server HAS a password; `ok` = we are through it.
   *  `refused` fires when a protected control was rejected — the honest moment to say why. */
  onAdminState?: (st: { set: boolean; ok: boolean; refused?: boolean; superseded?: boolean }) => void;
  /** ★★ THE SERVER DELIBERATELY TURNING US AWAY. Each of these is TERMINAL: the
   *  reconnect that serves a dropped link would here hammer a receiver that is
   *  busy saying "not you, not now", while showing our own user nothing but
   *  "reconnecting". The web client has always treated them as final; the phone
   *  ignored both messages entirely, so a listener whose time ran out just
   *  dropped and started retrying (2026-07-28).
   *  `cooldownSec` is the server's own number — when they may come back. */
  onSessionEnded?: (cooldownSec: number, freshSec?: number) => void;
  /** Refused because we returned inside our cooldown. */
  onCooldown?: (secs: number) => void;
  /** ★★ PARITY GAP CLOSED 2026-07-28. The web client and Jr have handled all three of these
   *  since they were built; the phone handled NONE of them, so an evicted or refused listener
   *  saw a silent dead link and a retry loop. Checked message by message against
   *  web/client/src/spectrum.ts — the failure mode of a per-client protocol is SILENCE. */
  /** Someone else holds the receiver. Terminal — do not retry into a busy server. */
  /** Refused: somebody else has the radio. The server says where you stand — position in its
   *  queue, how long it is, and when the slot frees (freeIn < 0 = no session limit). */
  onBusy?: (q?: { queuePos?: number; queueLen?: number; freeIn?: number; queueFull?: boolean }) => void;
  /** ★ LIGHTNING. The server decides — it is gated on the band this listener's VFO sits in and
   *  on whether the activity amounts to a storm — so a non-zero rate is the whole decision. */
  onLightning?: (ratePerMin: number, agoSecs: number) => void;
  /** ★ The RADIO's centre and the owner's locked centre (0 = not locked), off hwinfo. The one
   *  figure the walls and the RF-centre marker must be drawn from — the view centre is not it. */
  onRfCentre?: (rfHz: number, lockedHz: number) => void;
  /** ★★★ ONE RADIO PER ADDRESS — you are already listening on another radio of THIS server.
   *  Deliberate policy, added after one visitor held both single-user radios of the demo at once by
   *  opening a tab on each (Stuart, 2026-08-21). ★★ It is NOT a queue and must never be presented
   *  as one: there is no slot to wait for, because the slot is yours — closing the other radio
   *  frees this one instantly, which is the one thing the message has to say.
   *  ★ `radio` names the one being held, so the app can say WHICH. */
  onElsewhere?: (radio: string) => void;
  /** The owner took their radio back with the admin password. Terminal, and not a fault. */
  onEvicted?: () => void;
  /** Still connected — the server's own countdown, at T-120s and T-30s. NOT a refusal.
   *  ★ This is the AUTHORITATIVE remaining time; our local clock is only an interpolation
   *  between these, so re-base on it rather than trusting our own arithmetic. */
  onSessionWarning?: (secs: number) => void;
  /** ★★★ THE RECEIVER'S CLOCK — signed minutes from UTC, and what it calls its zone.
   *  The status row showed UTC and the PHONE's local time, and the second of those is the one thing
   *  every phone already displays on its own status bar. What it could not say is the time AT THE
   *  AERIAL, which is what explains the band you are hearing.
   *  ★ Stuart, 2026-09-21, having placed a Paraná receiver on US East Coast time and been two hours
   *    out: "this is precicely why having the server time and UTC is important". */
  onServerClock?: (offsetMin: number, abbr: string) => void;
  /** ★ The owner's notice to listeners ("antenna maintenance in progress"), pushed when it is
   *  posted or cleared. '' = nothing to show. */
  onNotice?: (text: string) => void;
  /** ★ The server REFUSED something we asked for, in its own words. Distinct from the owner's
   *  standing notice and must not displace it — see the 'notice' case. */
  onRefused?: (why: string) => void;
  /** ★ The receiver's own terms, read from POST /connection at connect (see _checkConnection).
   *  idleSecs 0 = NO idle limit (a valid value, not a missing one); daily* −1 = unlimited. */
  onIdlePolicy?: (p: IdlePolicy) => void;
}
