/* ★★★ VibeServer's OWN protocol client — a full COPY, not a shared base (2026-09-22).
 *  Stuart: "Even if we have to duplicate the code ... I would rather have fully separate VibeServer
 *  and UberSDR components like Kiwi and OWRX and FM-DX are separate codebases inside the app."
 *  This began as an identical copy of the old shared SdrWsClient. From here it changes for
 *  VibeServer only; UberSDRWsClient.ts changes for UberSDR only. A fix needed on both is made TWICE,
 *  deliberately — the shared base is what let a shared-dial fix break UberSDR's audio (4530cb06).
 *  ✗ Do not import behaviour from UberSDRWsClient.ts, and it must not import from here. */
// SdrWsClient.ts — the shared WebSocket client for the UberSDR-family protocol.
//
// ★★★ WHY THIS IS A BASE CLASS NOW. VibeServer's protocol was DERIVED from UberSDR's, so for a
//     year one class served both, told apart by a mutable `isVibeServer` flag. That flag was the
//     defect, not the design: the client cannot know which server it is talking to until a
//     MESSAGE arrives (hwinfo), so everything decided at connect time — the reconnect ladder, the
//     bin count, which rate lever to pull — was decided as UberSDR and then retrofitted. Its own
//     comments recorded the damage: "THE LADDER RACE, KILLED AT SOURCE", and a diagnostic reading
//     "set_rate divisor=2 RECEIVED  <- UberSDR lever, on a VibeServer".
//
// ★★★ AND IT LEAKED WELL BEYOND THIS FILE. Because a VibeServer connected AS an UberSDR, the app
//     showed the UberSDR logo on VibeServers, offered UberSDR admin pages that 404, sent the watch
//     to "a screen that does not speak the protocol", and shipped repairVibeserverFavourites() —
//     a migration whose whole job was to DELETE the 'vibeserver' server type as corruption,
//     because nothing downstream could handle it. Five workarounds for one missing case.
//
// ★★ THE SPLIT IS A PRECONDITION, NOT A TIDY-UP. Stuart, 2026-09-08: "VibeSDR is truly its own
//    thing and needs to be separate because everytime we seem to make changes to VibeServer it
//    causes issues on UberSDR and Vice Versa" — and VibeServer is about to gain DAB, ADS-B, AIS,
//    ACARS and DRM, none of which UberSDR has. Every one of those would have been another branch
//    on that flag.
//
// ★ AND THE SHARING IS DELIBERATE, WHICH IS WHY A BASE CLASS IS THE RIGHT SHAPE RATHER THAN TWO
//   COPIES. The website states the formula: VibeDSP + UberSDR = VibeServer. The borrowed half
//   belongs here, in one place, where a fix to it reaches both. What was wrong was never the
//   sharing — it was expressing the sharing as a mutable flag instead of as a type.
//
// ★ SO: this file holds only what BOTH speak. The four places they genuinely differ are protected
//   members overridden by the two subclasses, which know what they are at CONSTRUCTION — so the
//   flag is gone, and with it linkBuiltForVibe, which existed only because the flag flipped late.
//     UberSDRClient.ts    — UberSDR, and nothing else
//     VibeServerClient.ts — VibeServer, and the home for everything above
//
// Audio WS is owned by native VibePowerModule (runs on background thread, survives JS suspension).
// JS only manages the spectrum WS for display.
//
// Binary SPEC frame format (from user_spectrum_websocket.go):
//   Header 22 bytes:
//     [0..3]  magic "SPEC"
//     [4]     version 0x01
//     [5]     flags: 0x01=full float32, 0x02=delta float32, 0x03=full uint8, 0x04=delta uint8
//     [6..13] timestamp uint64 LE (nanoseconds)
//     [14..21] frequency uint64 LE (Hz)
//   Body:
//     full:  binCount × float32 LE
//     delta: uint16 changeCount, then changeCount × {uint16 index, float32 value}
//   8-bit variants: same layout but values are uint8 (0..255 mapped to dBFS range)

import type { DabState } from './dabTypes';
import 'react-native-get-random-values'; // polyfill for crypto.getRandomValues
import { ungzip } from 'pako';
import { VibePowerModule } from '../components/AudioPlayer';
import { noteUnhandled, noteDecision } from './protocolLog';
import { resolveStationIso, receiverIso } from './rdsCountry';
import { LinkManager, LADDERS, type LinkMode } from './vibeLinkManager';

/** ★ Re-exported so each subclass names its own ladder without reaching past this file. */
export const LADDERS_FOR = LADDERS;
import { USER_AGENT, APP_PROTO } from '../constants/version';

/**
 * ★★★ NAME OURSELVES IN THE QUERY, because the header is not ours to set. React Native's WebSocket
 *     does not reliably send a User-Agent on the upgrade — the platform owns that header — so every
 *     VibeSDR session appeared in an owner's connection log as "—", indistinguishable from a bot,
 *     beside browsers naming themselves in full. Setting the header was the obvious fix and was NOT
 *     enough (Stuart, 2026-08-13: "also no user agent from VibeSDR or Jr still").
 * ★★ The server prefers a real User-Agent and falls back to this, so nothing changes for clients
 *    that can send one. It is the client's own word about itself either way — exactly what a
 *    User-Agent has always been — and it grants nothing.
 */
const CLIENT_Q = `&client=${encodeURIComponent(USER_AGENT)}&proto=${APP_PROTO}`;

/** Powersave target, in frames/sec — an absolute floor, not a divisor. */
const POWERSAVE_FPS = 5;

import type { SDRMode, SDRStatus, SDRCallbacks, RadioCaps, RdsExt, IdlePolicy, IqOutState } from './sdrProtocol';
import { MODE_BANDWIDTHS, UPDATE_APP_MESSAGE } from './sdrProtocol';

// ── Constants ─────────────────────────────────────────────────────────────────

const SPEC_MAGIC    = 0x43455053; // "SPEC" in little-endian uint32
const FLAG_FULL_F32  = 0x01;
const FLAG_DELTA_F32 = 0x02;
const FLAG_FULL_U8   = 0x03;
const FLAG_DELTA_U8  = 0x04;

// binary8 encoding (user_spectrum_websocket.go sendBinary8Spectrum):
//   uint8 = clamp(dBFS, -256, 0) + 256  →  decode: dBFS = uint8 - 256
// (0 = -256 dB, 255 = -1 dB). The previous -160..0 linear mapping was WRONG
// and distorted every dB value entering the waterfall/auto-range/SNR pipeline.
const U8_DBFS_OFFSET = -256;

// View-prediction tuning (anti-thrash — see view/getView below):
// coalesce zoom/pan sends to ≤1 per VIEW_SEND_MS (a fast drum gesture fires at
// 60–120Hz; every request triggers a config echo, so unthrottled gestures flood
// the link), and treat the view as "in flight" until VIEW_SETTLE_MS of send
// quiet, after which the server's acked state is adopted in one step.
const VIEW_SEND_MS   = 33;
const VIEW_SETTLE_MS = 300;

// ── Spectrum starvation watchdog ────────────────────────────────────────────
// The audio WS has a native watchdog (VibePowerModule.reviveIfDead) that runs in
// the background. The spectrum WS had none: its only recovery paths were onclose
// (which a half-open socket never fires) and SDRScreen's AppState `active`
// handler (which never runs while the phone is locked in a pocket). On cellular
// — CGNAT rebinds on cell handover, RRC idle transitions, IP changes — the TCP
// flow is silently invalidated with no FIN/RST, so the socket sat OPEN and
// starving forever. _evalLink() below has ALWAYS computed the `starving`
// condition; it just never acted on it. Audio and tuning healed themselves, the
// waterfall stayed dead. That is the whole bug (field-confirmed 2026-07-13).
//
// Detection is PONG-FIRST and deliberately so. A frame-cadence threshold has to
// distinguish "socket is dead" from "the feed is legitimately slow" — and the
// feed is legitimately slow on purpose (the idle saver's rate divisor, shared
// channels' hardcoded ÷3, and any future battery-saving divisor). The pong is an
// active probe: it does not care what the frame rate is.
const WATCHDOG_TICK_MS  = 2_000;
// Two missed 5s ping cycles plus margin. This is the PRIMARY detector.
const PONG_TIMEOUT_MS   = 12_000;
// Frame-staleness backstop, in case a server answers pings but stops sending.
// Scaled off the divisor we ourselves commanded (see _staleLimitMs) rather than
// off observed gap statistics — which, when the feed has stopped, are describing
// a feed that has stopped.
const STALE_MIN_MS      = 10_000;
// A socket that opened but never delivered a single frame is the reconnect-race
// (Hole 2): the spectrum re-attached to a session the server had already reaped.
// It connects fine, receives nothing, and — being OPEN — never retries.
const NO_FIRST_FRAME_MS = 10_000;
// A flapping cellular path must not thrash the server with session churn: the
// server counts connections. One forced reopen per window, whatever asks for it.
const FORCE_REOPEN_MS   = 15_000;
// How long a reopen waits for audio to confirm the session is alive before going
// ahead regardless (the session may be fine and only the spectrum flow dead).
const AUDIO_WAIT_MS     = 5_000;
// connect() gives native audio 1s to register the session before the spectrum
// subscribes. Every recovery path must honour the same ordering or it races.
const AUDIO_SETTLE_MS   = 1_000;
// Reopens that produced no frames before we stop trusting the session itself and
// re-POST /connection. _openSpectrumWs never re-registers — only connect() does.
const REOPENS_BEFORE_RECHECK = 2;

// ── Client class ──────────────────────────────────────────────────────────────

export abstract class VibeServerWsClient {
  /** The last eye grids — kept between the rdsx messages that omit them (eyeEvery). */
  private lastEye = { P: '', S: '', R: '' };
  /* ── THE FOUR PLACES THE TWO PROTOCOLS GENUINELY DIVERGE ────────────────────────────────
   *  Every one of these used to be `this.isVibeServer ? … : …` evaluated at a moment when the
   *  answer was not yet known. A subclass answers them by existing. */

  /** `&bins=N` — VibeServer honours a requested bin count; UberSDR sends its own. */
  protected abstract binsSuffix(): string;
  /** Which rung ladder the link controller starts on. Chosen at construction, never rebuilt. */
  protected abstract ladderFor(): number[];
  /** The frame-rate lever this server actually has. UberSDR speaks divisors off its own full
   *  rate; VibeServer takes an absolute fftRate. Sending the wrong one is the bug that read as
   *  "Auto crawled while Full reached 20 on the same server". */
  protected abstract sendRateLever(): void;
  /** Powersave, in whichever lever this server understands. */
  protected abstract sendPowersaveLever(targetFps: number): void;
  /** True for a VibeServer. Read by the UI to offer advanced RDS and the right admin pages. */
  abstract get isVibe(): boolean;

  private baseUrl:   string;
  readonly uuid:     string; // shared with native audio WS
  protected callbacks: SDRCallbacks;   // ★ protected: a subclass's own messages report through them

  protected spectrumWs:   WebSocket | null = null;   // ★ protected: the subclasses send their own rate lever
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private destroyed = false;

  private bins: Float32Array = new Float32Array(1024);
  private status: SDRStatus = {
    frequency:     14_074_000,
    mode:          'usb',
    bandwidthLow:  50,    // usb defaults — kept in sync via MODE_BANDWIDTHS
    bandwidthHigh: 2700,
    binCount:       1024,
    binBandwidth:   0,
    centerHz:       0,
    bwHz:           0,
  };

  // ── View prediction (anti-thrash) ───────────────────────────────────────
  // The server echoes a config after EVERY zoom/pan (sendStatus fires even for
  // no-ops). During a fast gesture many requests are in flight at once and the
  // echoes replay every intermediate state one RTT late — applied directly to
  // the UI they thrash the band plan/needle (the "multi-colour flash"), and
  // gestures that re-base on this stale acked state compute wrong targets, so
  // the view can land at an old zoom/tune. Fix: keep a *predicted* view that
  // updates synchronously on every send. Gestures read getView(), frames are
  // rendered under the predicted geometry while in flight, and the acked truth
  // (server snaps binBandwidth to a ladder, so its answer always wins) is
  // adopted in one clean step once sends go quiet.
  // Tunable frequency range (Hz). Default = UberSDR HF limits (10 kHz–30 MHz).
  // V4 local hardware widens this to the RTL-SDR's range.
  minHz = 10_000;
  maxHz = 30_000_000;
  // Max zoom-OUT span (Hz). 0 = use maxHz. Local hardware reports its full
  // device bandwidth via config.maxBandwidth so you can't zoom out past it.
  private maxSpanHz = 0;

  // VFO lock / waterfall panning (see SDRBackend.setFollowMode/panSpan).
  // followVfo=true reproduces today's behaviour: tune() recentres the view.
  private followVfo = true;
  /** ★ Does this receiver share one dial? Set from the server's `dial` message. On a shared dial a centre
   *  change we did not ask for is another listener tuning, and must be ADOPTED rather than argued with. */
  private sharedDial = false;
  // Local hardware only — set by the adapter from the device config. Drives the
  // movable Fs pan window in panSpan(). Default = the 2.4 MS/s RTL-SDR rate.
  /**
   * The on-device shim OR a LAN VibeServer — both are the same shim binary, and
   * both speak `fftRate` rather than UberSDR's `set_rate` divisor.
   *
   * ★★ SETTING THIS ALSO SETS isVibeServer, and that is the whole fix. The
   * backend-specific policy (which ladder, which rate lever, whether to ask for
   * fewer bins) was being decided from `isVibeServer`, which only became true
   * when `hwinfo` ARRIVED — a message, so necessarily after the socket opened and
   * after the controller had already acted. Measured on the wire 2026-07-26:
   *
   *   WS UPGRADE /ws/user-spectrum?...&mode=binary8     ← no bins=, flag still false
   *   DIAG set_rate divisor=2 RECEIVED                  ← UberSDR lever, on a VibeServer
   *   fft rate: 5.0 (engine 20.0)  →  emit 2.5 fps      ← server halving, obediently
   *
   * The controller started on UberSDR's ladder at rung 2, sent divisor 2, and the
   * server dropped every other frame FOREVER — nothing resets that divisor. Then
   * hwinfo arrived, the ladder was rebuilt and fftRate started being used
   * correctly, so everything downstream looked right while the output stayed
   * halved. Days of "the link controller is broken" traced to one flag set late.
   *
   * ★ `isLocal` has been known since construction all along. Decide from what you
   * already know, never from what is still in flight.
   */
  /* ★ This used to also set isVibeServer, "and that is the whole fix" for a race that no longer
   *  exists: a local shim is a VibeServer because VibeServerClient is what gets constructed. */
  set isLocal(v: boolean) { this._isLocal = v; }
  get isLocal(): boolean { return this._isLocal; }
  private _isLocal = false;
  localSampleRate = 2_400_000;
  // VibeServer PIN: a pre-computed "&vs_nonce=&vs_auth=" suffix appended to the
  // spectrum WS URL so a PIN-protected server accepts the upgrade. Empty otherwise.
  authSuffix = '';
  /**
   * ★★★ ADMIN ON THE CONNECT URL, NOT AFTER THE SOCKET IS OPEN.
   *
   *     `adminUnlock()` proves admin over an ALREADY-OPEN socket, which is fine for unlocking
   *     controls mid-session and useless for the two cases that matter most: a receiver that is
   *     BUSY, and one that is holding us on a COOLDOWN. Both are refused during the handshake —
   *     before any message can be sent — so the owner is turned away from their own radio and the
   *     password they hold never gets read. The server checks these query parameters at accept
   *     time, ahead of both refusals, which is precisely why they have to be here.
   * ★ Challenge-response (`vs_admin_nonce` + `vs_admin_auth`) or a minted `vs_admin_ticket`; the
   *   password itself never crosses the link. Empty for everyone who is not an admin.
   */
  adminSuffix = '';

  /**
   * Carry an admin credential on every socket this client opens from now on.
   *
   * ★★★ SDRScreen HAS BEEN CALLING `setAdminAuth` SINCE THE PICKER'S ADMIN BOX WAS BUILT, AND IT
   *     DID NOT EXIST. The call site is `(c as any).setAdminAuth?.(…)` — optional-chained, so a
   *     missing method is not an error, not a warning, and not a crash: it is simply nothing
   *     happening, for ever. The credential never reached the connect URL, so an owner who
   *     unlocked at the picker was still refused by a FULL radio — the one job that box exists to
   *     do (found 2026-08-13 while chasing a connection error at that very box).
   * ★★ THE LEADING `&` IS NORMALISED HERE. resolveVibeAdminAuth returns "&vs_admin_nonce=…" WITH
   *    one and a minted ticket comes back as "vs_admin_ticket=…" WITHOUT — and these are pasted
   *    straight into a query string. One of the two would have produced
   *    "…&mode=binary8vs_admin_ticket=…": a malformed URL, refused at the handshake, which reads
   *    to a user as exactly the connection error they reported. Fixing it at the join means
   *    neither caller has to remember.
   */
  setAdminAuth(q: string) {
    const t = (q || '').trim();
    this.adminSuffix = !t ? '' : (t.startsWith('&') ? t : '&' + t);
  }

  private view = { centerHz: 0, binBandwidth: 0 };
  private pendingView: { frequency: number; binBandwidth: number } | null = null;
  private lastSendAt   = 0;
  private sendTimer:   ReturnType<typeof setTimeout> | null = null;
  private settleTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(baseUrl: string, uuid: string, callbacks: SDRCallbacks, password?: string) {
    this.baseUrl   = baseUrl.replace(/\/+$/, '');
    this.uuid      = uuid;
    this.callbacks = callbacks;
    this.password  = password ?? null;
  }

  /** Bypass password (rate-limit/ban bypass) — appended to every WS URL,
   *  exactly like the skin's window.bypassPassword. */
  private password: string | null = null;
  private _binsSuffix(): string { return this.binsSuffix(); }

  private _pwSuffix(): string {
    return this.password ? `&password=${encodeURIComponent(this.password)}` : '';
  }

  // ── Public API ─────────────────────────────────────────────────────────────

  /** Where this receiver's OWNER wants you to start. Every UberSDR publishes it at
   *  `/api/description` as `default_frequency` (Hz) + `default_mode`, and they differ per site —
   *  648 kHz AM on one, 7.1 MHz LSB on another, 14.1 MHz CWU on a third.
   *
   *  ★ This is the ONLY source. Two things look like it and are not:
   *    - the spectrum `config.centerFreq` is the WINDOW CENTRE (15 MHz on a 0-30 MHz span);
   *    - the instances directory has no such field and its `public_url` is bare.
   *    The web client's `?freq=…&mode=…` URL is written by its own JS from this same endpoint —
   *    a symptom, not the source.
   *
   *  Best-effort: a slow, silent or incomplete answer just leaves the caller's tune standing. */
  private async _serverDefault(): Promise<{ frequency?: number; mode?: SDRMode }> {
    // Local hardware and the VibeServer shim publish no such endpoint, and their tune is per-device
    // rather than operator-chosen. Skip rather than spend the timeout on a request that cannot
    // succeed. (The watch guards the same case as `!isVibe`.)
    if (this.isLocal) return {};
    try {
      const ctl = new AbortController();
      const t = setTimeout(() => ctl.abort(), 5000);
      const r = await fetch(`${this.baseUrl}/api/description`, { signal: ctl.signal });
      clearTimeout(t);
      if (!r.ok) return {};
      const j = await r.json() as { default_frequency?: unknown; default_mode?: unknown };
      const out: { frequency?: number; mode?: SDRMode } = {};
      if (typeof j.default_frequency === 'number' && j.default_frequency > 0) {
        out.frequency = Math.round(j.default_frequency);
      }
      if (typeof j.default_mode === 'string' && j.default_mode.toLowerCase() in MODE_BANDWIDTHS) {
        out.mode = j.default_mode.toLowerCase() as SDRMode;
      }
      return out;
    } catch { return {}; }
  }

  async connect(frequency = 14_074_000, mode: SDRMode = 'usb', opts?: { allowServerDefault?: boolean }) {
    this.destroyed = false;
    // No remembered tune for this instance, so ask the receiver where it wants us. Only ever on a
    // first visit — a saved tune always wins and the screen does not set this flag then.
    if (opts?.allowServerDefault) {
      const d = await this._serverDefault();
      if (this.destroyed) return;
      if (d.frequency) frequency = d.frequency;
      if (d.mode) mode = d.mode;
    }
    this.status.frequency = frequency;
    this.status.mode = mode;
    // ★★★ WHAT WE ASKED FOR, SO IT CAN BE RE-ASSERTED. The server LANDS a new session on the
    //     owner's chosen frequency — deliberately — and does it during the handshake, i.e. AFTER
    //     this. So a client restoring its remembered tune ended up showing one frequency while the
    //     radio sat on another: the readout said 106.8 MHz WFM and the spectrum was the AM
    //     broadcast band, playing Radio Caroline on 648 kHz. One manual tune fixed it, which is
    //     exactly the shape of a client that never told the server where it wanted to be
    //     (Stuart, 2026-08-15, on both the app and the browser, every multi-radio server).
    // ★★ Only when the caller has a tune to restore. With allowServerDefault the server's landing
    //    IS the answer and must not be argued with — that is a new listener with no memory, which
    //    is the case the landing frequency exists for.
    this.wantTune = opts?.allowServerDefault ? null : { frequency, mode };
    // Mirror the server's per-mode bandwidth defaults for the CONNECT mode
    // too (setMode already does) — without this, connecting in a restored
    // non-USB mode kept the constructor's USB edges and the first emission
    // overwrote the screen's correct values (AM showing only one sideband).
    const cbw = MODE_BANDWIDTHS[mode];
    if (cbw) { this.status.bandwidthLow = cbw[0]; this.status.bandwidthHigh = cbw[1]; }
    // The screen already set its own state from the tune it passed in, so if the receiver sent us
    // somewhere else it has to be told — otherwise the readout shows one frequency while the audio
    // plays another.
    if (opts?.allowServerDefault) this.callbacks.onStatus({ ...this.status });

    try {
      await this._checkConnection();
      // Native VibePowerModule opens the audio WS — give it 1s to register the
      // session on the server before the spectrum WS subscribes.
      setTimeout(() => {
        if (!this.destroyed) this._openSpectrumWs();
      }, 1000);
    } catch (e: unknown) {
      const msg = e instanceof Error ? e.message : String(e);
      this.callbacks.onError('Connection check failed: ' + msg);
    }
  }

  /** ★★★ DAB OWNS THE DIAL AND THE VIEW, AND THE HOLD LIVES HERE — ONE READER.
   *
   *  An ensemble is one 1.536 MHz block: there is nothing to tune inside it and nothing to zoom
   *  into, and on an RTL the IF filter FOLLOWS the view, so a zoom cuts the multiplex out from
   *  under the decoder. The web client learned this the expensive way (Stuart, 2026-09-07: "the
   *  zoom still worked instead of being locked out… the spectrum can be clicked which knocks the
   *  multiplex tuning off") — the server refused `tune` but never `zoom`, so a stray tap on the
   *  waterfall parked the dongle on the view and the lock was gone.
   *
   *  ★ EVERY path that moves the dial or the view — drum, arrows, drag, tap, bookmark, band
   *    button, search — ends in tune()/zoom()/pan()/resetView(). Gating those four is gating all
   *    of them; gating call sites is how the web client missed one. AGENTS.md, "ONE RULE, TWO
   *    READERS": there is one reader here on purpose.
   *
   *  ★ Base value is false and only VibeServerClient ever sets it, so UberSDR carries no branch. */
  protected dabHeld = false;

  /** True while a DAB multiplex is being decoded — the UI locks zoom and the VFO out. */
  get inDab(): boolean { return this.dabHeld; }

  /** ★ A message only ONE server speaks. Return true if handled; the base speaks neither DAB nor
   *  anything else UberSDR lacks, so it returns false and the unhandled-type log still fires for
   *  a message genuinely nobody expected. */
  protected handleServerMessage(_msg: Record<string, unknown>): boolean { return false; }

  /** Send one control message on the spectrum socket, or drop it if the socket is not up.
   *  ★ DROPPING IS THE RIGHT ANSWER HERE and the wrong one elsewhere: these are commands the user
   *  just issued about the state the radio is in NOW. Queueing one across a reconnect replays a
   *  stale intent onto a session that has moved on. Anything that MUST survive a reconnect is
   *  re-sent from the onopen path, deliberately, by name. */
  protected sendSpectrum(msg: Record<string, unknown>): boolean {
    if (!this.spectrumWs || this.spectrumWs.readyState !== WebSocket.OPEN) return false;
    try { this.spectrumWs.send(JSON.stringify(msg)); return true; } catch { return false; }
  }

  /** Tune to a new frequency (and optionally mode). Sends to native audio WS + spectrum WS. */
  tune(frequency: number, mode?: SDRMode, opts?: { recenter?: boolean }) {
    if (this.dabHeld) return;            // ★ see dabHeld — the multiplex IS the tuning
    const prevFreq = this.status.frequency;   // ★ read BEFORE it is overwritten — see sameSpot below
    this.lastLocalTuneAt = Date.now();   // ★ so the server's echo is not read as somebody else
    this._armTuneSettleAsk();
    if (frequency) this.status.frequency = frequency;
    if (mode)      this._adoptMode(mode);      // ★ the passband travels with it — see _adoptMode
    this._routeTune(frequency, mode ?? this.status.mode);
    // Re-centre spectrum on new frequency so waterfall follows the VFO — only
    // when locked (followVfo) or a discrete jump forces it (opts.recenter).
    // Unlocked continuous tuning leaves the view put so the user can pan freely.
    // Goes through the coalesced view sender — a fast VFO drum spin fires per
    // step, and per-step recentres flood the link with config echoes. (Audio
    // tune above stays per-event via native, so tuning feel is unaffected.)
    /* ★★★ A TUNE TO WHERE WE ALREADY ARE IS NOT A TUNE, AND MUST NOT MOVE THE CAPTURE.
     *
     *  The recentre below is right for a real move — the waterfall follows the dial. But it also
     *  fired for a tune to the SAME frequency, and on a shared receiver that is not harmless: the
     *  zoom reaches the server's pan handler, which moves rtlCenter and PHYSICALLY RETUNES THE
     *  DONGLE. The VFO never changes, so every readout still says the right number while the
     *  capture window slides underneath everybody listening.
     *
     *  ★★ MEASURED ON STUART'S XCOVER, 2026-09-11, in the server's own log:
     *       audio WS connected … shared
     *       spectrum WS connected — listener 2 of 10
     *       retune -> 96100.000 kHz (was 96100.000, centre 95720.000)
     *       deferred retune applied: dongle -> 96100000 Hz
     *     A tune to 96.100 while already on 96.100 — and the capture centre moved 380 kHz. Both
     *     listeners heard analogue mistuning with a correct frequency box: "soon as the iphone
     *     connected they both had static like the iphone nudged the tuning when it connected". He
     *     had not retuned anything.
     *
     *  ★★ AND IT EXPLAINS THE CURE. "it needs to move a few hundred KHz to get it to produce clean
     *     signal again" — a small nudge re-runs the same no-op-ish path, while a big one crosses
     *     the server's recentre threshold and re-derives the placement from scratch.
     *
     *  ★ The server's own note says this "bites hardest exactly where it is least acceptable: ON
     *    CONNECT. The client restores its zoom and its tune milliseconds apart" — the fault was
     *    known from that end and never closed from this one.
     *  ★ A REAL move still recentres, including on a shared dial: there the whole room follows the
     *    dial by design, which is the point of sharing it. Only the no-op is suppressed, so nothing
     *    a person actually does changes. `mode` counts as a move too — a new demodulator wants its
     *    passband in view even at the same frequency. */
    const sameSpot = !!frequency && Math.abs(frequency - prevFreq) < 1 && !mode;
    /* ★★★ OUTSIDE THE CAPTURED WINDOW THERE IS NOTHING TO PAN TO (Stuart, 2026-09-20). Unlocked means "leave
     *  the view where it is so I can pan", and that is right INSIDE the window — but a tune beyond the captured
     *  span asks the radio for something it is not receiving, so the server keeps its centre, the VFO sits off
     *  the end of the view, and the waterfall goes black. On a SHARED dial it goes black for everybody: he
     *  tuned the iPhone from 96.1 to 99.7, the capture stayed at 96.1, and both his screens emptied.
     *  ★★ So the lock governs panning, not reachability: past the edge we always re-centre. */
    const captureSpan = Number(this.status.bwHz) || 0;
    const viewBb = this.view.binBandwidth || this.status.binBandwidth;
    const viewSpan = viewBb ? viewBb * (this.status.binCount || 0) : 0;
    const refCentre = this.view.centerHz || this.status.centerHz;
    const beyondCapture = !!frequency && captureSpan > 0 && refCentre > 0
                       && Math.abs(frequency - refCentre) > captureSpan * 0.45;
    const beyondView    = !!frequency && viewSpan > 0 && refCentre > 0
                       && Math.abs(frequency - refCentre) > viewSpan * 0.5;
    if ((this.followVfo || opts?.recenter || beyondCapture || beyondView) && !sameSpot) {
      const bb = this.view.binBandwidth || this.status.binBandwidth;
      if (bb) this.zoom(frequency, bb);
      else    this.pan(frequency); // no geometry known yet — let server keep its bin_bw
    }
  }

  /** Locked (true) = view follows the VFO. See SDRBackend.setFollowMode. */
  setFollowMode(follow: boolean) { this.followVfo = follow; }

  /** Real captured bandwidth (Hz) of the local device. The shim reports it as
   *  config.maxBandwidth → maxSpanHz, which tracks the ACTUAL sample rate /
   *  bandwidth mode. localSampleRate (from JS hw config) is only a fallback —
   *  it can lag the device, which made the pan wall land in the wrong place. */
  captureBandwidth(): number {
    return this.maxSpanHz > 0 ? this.maxSpanHz : this.localSampleRate;
  }

  /** Margin keeping the VFO inside the usable capture — MUST match the shim's
   *  viewDongleMargin(): above the 50 kHz auto-retune threshold AND clear of the
   *  RTL anti-alias rolloff (~10%, e.g. a 250 kHz mode is only ~192 kHz usable). */
  private localMargin(fs: number): number { return Math.max(fs * 0.10, 60_000); }

  /** Current display half-span (Hz). */
  private viewHalfSpan(): number {
    const bw = (this.view.binBandwidth || this.status.binBandwidth) * (this.status.binCount || 1);
    return (bw > 0 ? bw : this.status.bwHz) / 2;
  }

  /** The dongle / RF-centre frequency the shim is parked at, derived the same
   *  way the shim does (dongleForView): the dongle follows the view but is
   *  clamped so the VFO stays captured, then locks. Drives the RF-centre marker
   *  and the capture-window walls. (No protocol field — pure mirror.) */
  rfCenterHz(): number {
    if (!this.isLocal) return this.status.centerHz;
    const fs = this.captureBandwidth();
    const lim = fs / 2 - this.localMargin(fs);
    const vfo = this.status.frequency;
    return Math.max(vfo - lim, Math.min(vfo + lim, this.status.centerHz));
  }

  panSpan(): { loHz: number; hiHz: number; movable: boolean } {
    if (this.isLocal) {
      // The DISPLAY centre can roam across the whole captured band: the dongle
      // follows the view to VFO ± (Fs/2 − M), then locks, and the shim's crop
      // offset carries the view on to the capture edge (a further Fs/2 − cropHalf
      // beyond the dongle). So the reachable view centre = VFO ± (Fs − M − cropHalf).
      // Clamped directly (centre-bounds) — see SDRScreen.onWfPanDelta movable branch.
      const fs = this.captureBandwidth();
      const reach = Math.max(0, fs - this.localMargin(fs) - this.viewHalfSpan());
      const vfo = this.status.frequency;
      return { loHz: vfo - reach, hiHz: vfo + reach, movable: true };
    }
    return { loHz: this.minHz, hiHz: this.maxHz, movable: false }; // full HF range
  }

  /** Update internal state only — used when native already sent the tune (e.g. lock screen skip). */
  syncFrequency(frequency: number, mode?: SDRMode) {
    if (frequency) this.status.frequency = frequency;
    if (mode)      this._adoptMode(mode);      // ★ see _adoptMode
  }

  /* ★★★ A MODE IS NEVER JUST A MODE — IT CARRIES ITS PASSBAND. setMode() has always mirrored
   *     MODE_BANDWIDTHS because the server applies that table on every mode change and never
   *     reports the result back. Every OTHER place that changed `status.mode` set the mode ALONE,
   *     so the edges stayed at whatever the last mode used.
   *  ★★★ WHAT THAT LOOKED LIKE: connect to a WFM receiver while the app is restoring a remembered
   *     USB session, the server lands us on 96.6 WFM, we adopt its mode — and the VFO stays drawn
   *     at USB's few kHz while the audio plays broadcast FM perfectly. Stuart: "it shows and is
   *     decoding WFM but the VFO itself is super narrow as if it was in AM mode or NFM until I
   *     click WFM again." Clicking the mode button worked because THAT path went through setMode.
   *  ★ One helper, used by every path that can change the mode, so a future one cannot forget. */
  private _adoptMode(mode: SDRMode) {
    this.status.mode = mode;
    const bw = MODE_BANDWIDTHS[mode];
    if (bw) { this.status.bandwidthLow = bw[0]; this.status.bandwidthHigh = bw[1]; }
  }

  setMode(mode: SDRMode) {
    this.status.mode = mode;
    // Server applies these defaults on every mode change (websocket.go —
    // "These match the defaults in app.js setMode()"). It never reports
    // bandwidth back, so mirror the exact table to stay in sync.
    const bw = MODE_BANDWIDTHS[mode];
    if (bw) { this.status.bandwidthLow = bw[0]; this.status.bandwidthHigh = bw[1]; }
    this._routeTune(this.status.frequency, mode);   // ★ one route — see _routeTune
  }

  setBandwidth(low: number, high: number) {
    this.status.bandwidthLow  = low;
    this.status.bandwidthHigh = high;
    // Local shim (on-device or a remote VibeServer): the demod runs server-side
    // and its audio WS is NOT owned by VibePowerModule, so sendBandwidth() is a
    // no-op there. Send over the spectrum WS control channel — handleControl's
    // 'bandwidth' case applies it. UberSDR keeps the native audio-WS path.
    if (this.isLocal) this._sendCtl({ type: 'bandwidth', bandwidthLow: low, bandwidthHigh: high });
    else VibePowerModule?.sendBandwidth(low, high);
  }

  // Frequency MUST be an integer — server unmarshals into uint64 and rejects
  // fractional JSON numbers. Centre clamp 10kHz–30MHz per server limits.
  // binBandwidth clamped so total span never exceeds the 30MHz HF range —
  // the server ladder passes large values through unchecked and a runaway
  // zoom-out wedges the session.
  zoom(frequency: number, binBandwidth: number) {
    if (this.dabHeld) return;            // ★ see dabHeld — a zoom narrows the IF under the mux
    /* ★★★ THE VIEW CENTRE IS NOT A TUNE REQUEST — DO NOT CLAMP IT TO THE TUNER'S RANGE.
     *
     *  This was `Math.max(this.minHz, …)`, and for every VibeServer connection minHz is
     *  LOCAL_CAPS.freqRange[0] = 100 kHz — the RTL-SDR's floor, applied to whatever radio is
     *  actually on the far end. On the Pi's Airspy HF+ (1 kHz up) a view centred on MSF at 60 kHz
     *  was silently rewritten to 100 kHz, so every zoom closed in on 100 kHz and walked the VFO
     *  off the left edge until MSF and DCF77 could not be zoomed into at all (Stuart, 2026-09-03).
     *  ★★ AGENTS.md's "ELSE MEANS DONGLE", exactly: a limit that belongs to ONE radio applied to
     *     all three because the local path was written when the RTL-SDR was the only one.
     *
     *  ★★★ MEASURED, not assumed. Probing the shim directly: ask for centre 60000 with a 48 kHz
     *      span and it answers `centerFreq: 60000` and puts 60000 in every frame header — it
     *      honours a window running below the radio's lowest tunable frequency and simply sends
     *      the dead space, which is what the web client shows and what Stuart asked for ("the web
     *      client isnt afraid to show dead space to keep the VFO in the centre"). The device log
     *      then showed anchor=60000 going in and {"frequency":100000} going out, which is where
     *      the 40 kHz went.
     *  ★ The SPAN is still capped below — that is a real client-side limit (render artefacts and a
     *    runaway zoom-out wedging the session). Only the CENTRE clamp goes, and the server remains
     *    the authority on what it will serve.
     */
    const f = Math.round(frequency);
    const n  = this.status.binCount || 1024;
    // Max-zoom floor: 6 kHz total span (3 kHz per sideband — one SSB
    // channel both sides). The server goes deeper but past this the
    // spectrum shows artefacts and looks frozen even though it isn't
    // (device-confirmed on both platforms 2026-06-12).
    const spanCap = this.maxSpanHz > 0 ? this.maxSpanHz : this.maxHz;
    const bb = Math.max(6_000 / n, Math.min(binBandwidth, spanCap / n));
    this.view.centerHz     = f;
    this.view.binBandwidth = bb;
    this._sendView(f, bb);
  }

  pan(frequency: number) {
    if (this.dabHeld) return;            // ★ see dabHeld
    // ★ Same rule as zoom(): panning the VIEW is not tuning, so the tuner's range does not bound
    //   it. Clamping here hid the low end of every radio that reaches below the RTL-SDR's floor.
    const f = Math.round(frequency);
    this.view.centerHz = f;
    this._sendView(f, this.view.binBandwidth || this.status.binBandwidth);
  }

  // ── VibeServer hardware controls (client drives the remote radio) ─────────
  // Sent over the spectrum WS control channel; the shim applies them server-side.
  private _sendCtl(obj: Record<string, unknown>) {
    const ws = this.spectrumWs;
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
  }
  setHwGain(tenthDb: number, auto: boolean) {
    this._sendCtl(auto ? { type: 'gain', auto: true } : { type: 'gain', value: tenthDb });
  }
  setHwBiasT(on: boolean) { this._sendCtl({ type: 'biasT', on }); }
  /** Say one of the canned phrases on a shared-dial receiver.
   *  ★★ AN ID, NEVER TEXT. The vocabulary is fixed server-side (`chatPhrases()` in the shim) and
   *  an unknown id is dropped there, so this channel cannot carry free text in either direction —
   *  which is what removes the moderation burden, the abuse vector and the translation problem in
   *  one go, and is the only reason a one-person operator can run this at all.
   *  ★ The server also rate-limits to one phrase per 3s per session; that is flood control, not
   *    moderation. Nothing here can be offensive, but anything can be repeated. */
  say(id: string) { this._sendCtl({ type: 'say', id }); }
  setHwAgc(on: boolean)   { this._sendCtl({ type: 'agc', on }); }
  setHwPpm(ppm: number)   { this._sendCtl({ type: 'ppm', value: Math.round(ppm) }); }
  /** ★★★ FM DE-EMPHASIS — tau in SECONDS (0 = off, 50e-6 EU/UK, 75e-6 Americas).
   *
   *  These two were MISSING, and their absence is why both controls did nothing on a networked
   *  server: SDRScreen called the LOCAL USB module for them, which on a remote VibeServer is an
   *  idle shim while the server does the decoding. The server has always accepted them and the web
   *  client has always sent them — the app was the only client that never did. Found on a Mac
   *  against VibeServer with an Airspy (Stuart, 2026-07-30), and de-emphasis is the first thing an
   *  FM-DXer reaches for: Hans picked it up immediately the first time he used VibeServer. */
  setDeemph(tau: number)  { this._sendCtl({ type: 'deemph', tau }); }
  setStereo(on: boolean)  { this._sendCtl({ type: 'stereo', on }); }

  /** ★★★ THE AUDIO DSP CONTROLS — THE SAME BUG AS DE-EMPHASIS ABOVE, ONE LAYER DEEPER.
   *  Noise reduction, squelch and the auto-notch worked on rtl_tcp and local hardware (where the
   *  DSP is OURS, on-device) and in the web client (which sends these) — and did NOTHING from the
   *  app on a VibeServer, because there was no wire path at all. Stuart, 2026-07-31.
   *
   *  ★★ The server has implemented them all along, and its own comment says who for:
   *    local_sdr_shim.cpp:2437 — "…the on-device JS via JNI, so a REMOTE client (web or phone) had
   *    no way to touch them. Exposing them here gives BOTH remote clients the same audio controls
   *    the local app has."
   *  Built for both remote clients; only the web one was ever connected.
   *
   *  ★ TEST THESE ON A VIBESERVER, NOT ON rtl_tcp. rtl_tcp passes whether or not the wire command
   *  exists, which is exactly how this got through twice. */
  /** ★ NOT `setNr` — that name is already taken on SDRBackend by OWRX's setNr(threshold: number),
   *  which means something else entirely. Two meanings on one name is how the next bug gets
   *  written; the distinct name is deliberate. */
  setNrEnabled(on: boolean, strength?: number) {
    const m: Record<string, unknown> = { type: 'nr', on };
    // Server clamps to 0..1 itself; send only when we have a value so "toggle" stays a toggle.
    if (typeof strength === 'number' && Number.isFinite(strength)) m.strength = strength;
    this._sendCtl(m);
  }
  /** db <= -100 means OFF — the server's convention, and the app's own. */
  setSquelchDb(db: number) { this._sendCtl({ type: 'squelch', db }); }
  setNotch(on: boolean)    { this._sendCtl({ type: 'notch', on }); }
  // ── The broadcast-FM treatments (VibeServer 3.1) ────────────────────────────────────────────
  // ★★★ FOUR FAULTS, FOUR SWITCHES, and they are NOT interchangeable: NR answers continuous NOISE,
  //     IMS answers a REFLECTION too weak for CEQ, CEQ answers a REFLECTION, NB answers IMPULSES. Measured on
  //     the server, they want opposite actions — narrowing the IF costs up to 10 dB against noise
  //     and gains 10 dB against a close neighbour — so one combined control would be actively
  //     wrong as well as unhelpful.
  // ★★ SHARED, like NR and the notch have always been: one DSP chain per radio, so these change
  //    what every listener on that receiver hears. The server gates them accordingly.
  /** Weak-signal noise treatment: stereo high-blend + audio high-cut. Works in mono too. */
  setWeakProc(on: boolean) { this._sendCtl({ type: 'wsp', on }); }
  /** IMS — multipath suppression (blends L-R by measured multipath depth); see the shim. */
  setIms(on: boolean)      { this._sendCtl({ type: 'ims', on }); }
  setAutoBw(on: boolean)   { this._sendCtl({ type: 'autobw', on }); }
  /** ★ The tuner's IF filter. -1 = AUTO (follows the zoom); 0 = wide open; else a width in Hz. */
  setTunerBandwidth(hz: number) { this._sendCtl({ type: 'tunerbw', value: Math.round(hz) }); }
  /** CEQ — blind channel equaliser, for a reflection. */
  setCeq(on: boolean)      { this._sendCtl({ type: 'ceq', on }); }
  /** Noise blanker — impulse noise only. */
  setNoiseBlanker(on: boolean) { this._sendCtl({ type: 'nb', on }); }
  /** ★ The audio-menu NOISE BLANKER — every mode but WFM (which has its own in the Broadcast FM
   *  row). The web client has had it since the listener NB landed; the app did not (Stuart,
   *  2026-09-15: "noise blanker missing"). Same wire word as the web: `nbx`. */
  setNoiseBlankerHf(on: boolean) { this._sendCtl({ type: 'nbx', on }); }
  /** ★ Direct sampling on a REMOTE RTL — the web's dsSeg. The app's handler went only to its own
   *  local dongle, so on a networked server the control did nothing. 0 off, 1 I, 2 Q. */
  setHwDirectSampling(v: 0 | 1 | 2) { this._sendCtl({ type: 'directSampling', value: v }); }
  /** ★ AUTO direct sampling (the engine switches the tuner out below the crossover). Admin-gated on
   *  the server, as the web client's setHwAutoDirectSampling is. */
  setHwAutoDirectSampling(on: boolean) { this._sendCtl({ type: 'autoDirectSampling', value: on ? 1 : 0 }); }

  /** ★★★ TELL THE SERVER SOMEONE IS ACTUALLY HERE — on BOTH sockets, on ACTIVITY.
   *
   *  UberSDR counts down `session_timeout` (read in _checkConnection) and `{"type":"ping"}` resets
   *  it. Their own `idle-detector.js` sends that ping to the AUDIO socket AND the spectrum socket,
   *  on real user activity, throttled to ~10 s. We only ever pinged the SPECTRUM socket, on a blind
   *  5-second timer — so the session timer never saw us and WESSEX dropped Stuart at 4 minutes
   *  while he was sitting there listening (2026-07-30/31).
   *
   *  ★★ NOT a second blind timer. An unconditional keepalive would hold a public receiver's slot
   *  for ever and DEFEAT the operator's own idle kick — precisely the discourtesy recorded in
   *  memory/third_party_receiver_etiquette.md, and the reason the app's 30-minute hand-back exists.
   *  So this is driven by EVIDENCE OF A HUMAN: touches, and decoder output.
   *
   *  ★ The distinction that makes this honest: `session_timeout` is a LIVENESS check — "is anyone
   *  still there?" — so answering it when someone IS there is using the mechanism as intended.
   *  `max_session_time` is a FAIRNESS limit and must never be answered automatically. This only
   *  ever touches the former; the four-hour cap is untouched and still ends the session. */
  noteActivity(): void {
    const now = Date.now();
    if (now - this.lastActivityPing < 10_000) return;   // their throttle, matched
    this.lastActivityPing = now;
    const msg = JSON.stringify({ type: 'ping' });
    // ★ The AUDIO socket is owned by NATIVE (VibePowerModule / VibeStreamModule), not by JS — which
    // is exactly why it was never pinged. `sendAudioCommand` already exists on both platforms and
    // is already typed in AudioPlayer.tsx, so this needs no native change.
    try { VibePowerModule?.sendAudioCommand?.(msg); } catch {}
    try { if (this.spectrumWs?.readyState === WebSocket.OPEN) this.spectrumWs.send(msg); } catch {}
  }
  private lastActivityPing = 0;
  /** Set when the server refused this app as too old — the receiver's web client to offer instead. */
  updateAppWebUrl = '';
  // Capture sample rate = the spectrum span the server sends. The shim restarts
  // the IQ stream and pushes a fresh config, so the waterfall span self-updates.
  setHwSampleRate(rate: number) { this._sendCtl({ type: 'sampleRate', value: Math.round(rate) }); }

  /** Coalesced view sender — keeps only the latest target, sends ≤1/VIEW_SEND_MS
   *  with the final state always delivered (trailing edge). */
  /** ★ Coalesced twin of the native tune — see the note in tune(). Same rhythm as the view sender, so a fast
   *  VFO drum sends a handful of messages rather than one per step. */
  /** ★★★ THE TUNE GOES DOWN THE NATIVE PATH. ONLY. Twice in one evening I tried to be cleverer than
   *  that and made it worse, so the reasoning is written out here rather than rediscovered.
   *
   *  ★★ Attempt 1 — send on BOTH sockets, so whichever is up carries it. Both are normally up, and they
   *     raced: "the tuning doesnt work at all its hyper erratic", the dial snapping back to where it began.
   *  ★★ Attempt 2 — alternate: spectrum when open, audio otherwise (Stuart's own suggestion, and it reads
   *     right). It was worse, and the reason is a thing neither of us was looking at: VibeStreamService keeps
   *     `currentFreq`, set ONLY by sendTuneCommand, and it puts that frequency in the URL of every audio
   *     reconnect (VibeStreamService.kt — `"$s/ws?...&frequency=$currentFreq"`). Route the tune past the
   *     native side and its copy goes stale, so the next reconnect RE-TUNES THE SERVER BACKWARDS. With two
   *     apps connected, each holding a different stale value, the dial ping-ponged 96.5 ↔ 96.1 about three
   *     times a second with nobody touching either device, and both connection meters churned with it.
   *     Measured on the Pi 2 from a third socket, which is the only reason we know it was the clients.
   *
   *  ★ So: the native path owns the tune, because it owns the state that OUTLIVES the tune. The known cost
   *    is the one Stuart found this afternoon — with the audio socket down (muted, or the AirPods moving to
   *    another device) `ws?.send` swallows it and the server never hears. That is a real bug and it is still
   *    open; it is not fixed by giving the tune a second way out. Whatever fixes it has to keep
   *    `currentFreq` in step — that, not the socket, is what the reconnect reads. */
  private _routeTune(frequency: number, mode: string) {
    if (!(frequency > 0)) return;
    VibePowerModule?.sendTuneCommand(frequency, mode);
  }

  private _sendView(frequency: number, binBandwidth: number) {
    this.pendingView = { frequency, binBandwidth };
    const wait = this.lastSendAt + VIEW_SEND_MS - Date.now();
    if (wait <= 0) { this._flushView(); return; }
    if (!this.sendTimer) {
      this.sendTimer = setTimeout(() => { this.sendTimer = null; this._flushView(); }, wait);
    }
  }

  /** ★ Turn the Advanced RDS analyser on or off. Costs the server real CPU and ~5 frames a
   *  second of extra traffic, so it is switched by the panel being OPEN — there is no setting
   *  for a user to find, forget, and leave running. Remembered across reconnects, because the
   *  server forgets on a new socket and the panel would otherwise go quietly blank. */
  setAdvRds(on: boolean) {
    this.advRds = on;
    if (this.spectrumWs?.readyState === WebSocket.OPEN) {
      // ★ eyeEvery: the eye grids in every 2nd message (~2/s at the ~3.9/s rdsx rate) — see the 'rdsx' parse, which keeps the last ones.
      this.spectrumWs.send(JSON.stringify({ type: 'rdsx', on: on ? 1 : 0, eyeEvery: 2 }));
    }
  }
  private advRds = false;
  private adminSet = false;
  /** ★★ The server has DELIBERATELY turned us away (time up, cooldown). Terminal:
   *  the 3-second reconnect below is right for a dropped link and utterly wrong
   *  here — it would hammer a receiver that is telling us not to come back yet,
   *  which is exactly the reconnect war the shim's own comments warn about. Only
   *  a fresh user-initiated connect clears it. */
  private refused = false;

  /** ★ Unlock the protected controls. Challenge-response: the caller has already turned the
   *  password into an HMAC over a server-issued nonce, so the password never crosses the link
   *  — the same scheme as the PIN, and it inherits the same brute-force lockout. */
  adminUnlock(nonce: string, token: string) {
    this._sendCtl({ type: 'admin_unlock', nonce, token });
  }

  /** Airspy HF+ controls. Keys are optional — send only what changed.
   *  ★ AGC LAST, matching the server's own apply order: it owns the gain path, so applying it
   *  before a manual attenuation would immediately override it. */
  /** ★ Airspy R2 / Mini stages — see airspy_source.h. Only the keys present are applied, the same
   *  shape as its siblings. `curve` false = linearity (Airspy's own default), true = sensitivity;
   *  lna/mixer/vga are 0-15 each and leave preset mode; biast is owner-only, enforced server side. */
  airspyControl(o: { mode?: number; curve?: boolean; lna?: number; mixer?: number; vga?: number;
                     lnaAgc?: boolean; mixerAgc?: boolean; biast?: boolean; packing?: boolean }) {
    const m: Record<string, unknown> = { type: 'airspy_control' };
    /* ★★★ THE GAIN MODE WAS NEVER PUT ON THE WIRE. The panel sends { mode } for Sensitive /
     *  Linear / Free and the shim reads `jsonNum(msg, "mode", v)`, but this builder copied every
     *  field EXCEPT that one — so the three-way control did nothing on a remote receiver even
     *  once the adapter forwarded the call. A field-by-field copy is a list, and a list goes
     *  stale the moment a control is added. */
    if (o.mode     !== undefined) m.mode     = o.mode;
    if (o.curve    !== undefined) m.curve    = o.curve ? 1 : 0;
    if (o.lna      !== undefined) m.lna      = o.lna;
    if (o.mixer    !== undefined) m.mixer    = o.mixer;
    if (o.vga      !== undefined) m.vga      = o.vga;
    if (o.lnaAgc   !== undefined) m.lnaAgc   = o.lnaAgc ? 1 : 0;
    if (o.mixerAgc !== undefined) m.mixerAgc = o.mixerAgc ? 1 : 0;
    if (o.biast    !== undefined) m.biast    = o.biast ? 1 : 0;
    if (o.packing  !== undefined) m.packing  = o.packing ? 1 : 0;
    this._sendCtl(m);
  }

  ahfControl(o: { att?: number; lna?: boolean; thresh?: boolean; ppb?: number; agc?: boolean }) {
    const m: Record<string, unknown> = { type: 'ahf_control' };
    if (o.att    !== undefined) m.att    = o.att;
    if (o.lna    !== undefined) m.lna    = o.lna ? 1 : 0;
    if (o.thresh !== undefined) m.thresh = o.thresh ? 1 : 0;
    if (o.ppb    !== undefined) m.ppb    = o.ppb;
    if (o.agc    !== undefined) m.agc    = o.agc ? 1 : 0;
    this._sendCtl(m);
  }

  /** ★★★ HackRF One controls. Same shape — only the keys present are applied.
   *  ★★ amp and biast are OWNER-ONLY and the SERVER enforces that (adminGate in the
   *  hackrf_control handler), not this method: sending them without the admin unlock is refused
   *  and logged server-side. lna and vga are ordinary gain on sharedGate. The distinction the
   *  server draws is DAMAGE, not gain — a +14 dB amp in front of an unprotected 8-bit front end
   *  can destroy somebody else's radio, and a baseband stage cannot.
   *  ★ dB here, not tenths: these are the radio's own stage units (LNA 0-40 in 8s, VGA 0-62 in
   *  2s), unlike the single `gain` field which is tenths everywhere. */
  hackrfControl(o: { amp?: boolean; lna?: number; vga?: number; biast?: boolean }) {
    const m: Record<string, unknown> = { type: 'hackrf_control' };
    if (o.amp   !== undefined) m.amp   = o.amp ? 1 : 0;
    if (o.lna   !== undefined) m.lna   = o.lna;
    if (o.vga   !== undefined) m.vga   = o.vga;
    if (o.biast !== undefined) m.biast = o.biast ? 1 : 0;
    this._sendCtl(m);
  }

  /** SDRplay RSP controls. Same shape — only the keys present are applied. */
  /** ★ Ask the server to reset a FROZEN SDRplay gain API. Queued on the radio thread there, and
   *  gated like the gain controls — see rsp_agc_restart. Never automatic: the freeze is often
   *  inaudible and the reset costs everyone a moment of audio. */
  rspAgcRestart() { this.sendSpectrum({ type: 'rsp_agc_restart' }); }

  rspControl(o: { lna?: number; ifgr?: number; ifagc?: boolean; agcset?: number;
                  /** ★ OUR RF loop (the LNA), distinct from the radio's own IF AGC. Wire key is
                   *  `rfagc`, lower case, like its siblings — the server reads exactly that. */
                  rfagc?: boolean;
                  rfNotch?: boolean; dabNotch?: boolean }) {
    const m: Record<string, unknown> = { type: 'rsp_control' };
    if (o.lna      !== undefined) m.lna      = o.lna;
    if (o.ifgr     !== undefined) m.ifgr     = o.ifgr;
    if (o.ifagc    !== undefined) m.ifagc    = o.ifagc ? 1 : 0;
    if (o.rfagc    !== undefined) m.rfagc    = o.rfagc ? 1 : 0;
    if (o.agcset   !== undefined) m.agcset   = o.agcset;
    /* ★★★ LOWERCASE, BECAUSE THAT IS WHAT THE SERVER READS. The shim looks for "rfnotch" and
     *  "dabnotch" (jsonNum is an exact key match); this sent "rfNotch" and "dabNotch", so both
     *  notches did nothing on every VibeServer — silently, with no log line and no refusal,
     *  because a key that never matches runs no code at all. The WEB client has always sent them
     *  lowercase, which is why it worked there and not in the app. */
    if (o.rfNotch  !== undefined) m.rfnotch  = o.rfNotch ? 1 : 0;
    if (o.dabNotch !== undefined) m.dabnotch = o.dabNotch ? 1 : 0;
    this._sendCtl(m);
  }


  private _flushView() {
    const p = this.pendingView;
    if (!p) return;
    this.pendingView = null;
    // WS down (reconnecting): drop — the first config after reopening re-sends the predicted
    // view. ★ Remembered as a USER action, which is the one thing entitled to reach a shared dial.
    if (!this.spectrumWs || this.spectrumWs.readyState !== WebSocket.OPEN) {
      this.viewTouchedWhileDown = true;
      return;
    }
    this.lastSendAt = Date.now();
    // Server treats zoom and pan as one case; binBandwidth ≤ 0 = keep current.
    const msg: Record<string, unknown> = { type: 'zoom', frequency: p.frequency };
    if (p.binBandwidth > 0) msg.binBandwidth = p.binBandwidth;
    this.spectrumWs.send(JSON.stringify(msg));
    this.lastResubAt = Date.now();   // frames pause across this — not a bad link
    this._armSettle();
  }

  /** In flight = a send happened < VIEW_SETTLE_MS ago, or one is queued. */
  private _inFlight(): boolean {
    return this.settleTimer !== null || this.pendingView !== null;
  }

  private _armSettle() {
    if (this.settleTimer) clearTimeout(this.settleTimer);
    this.settleTimer = setTimeout(() => {
      this.settleTimer = null;
      if (this.destroyed) return;
      // Quiet — adopt the server's acked state (ladder-snapped) in one step.
      if (this.status.binBandwidth > 0) {
        this.view.centerHz     = this.status.centerHz;
        this.view.binBandwidth = this.status.binBandwidth;
      }
      this.callbacks.onStatus({ ...this.status });
    }, VIEW_SETTLE_MS);
  }

  resetView() {
    if (this.dabHeld) return;            // ★ see dabHeld
    if (!this.spectrumWs || this.spectrumWs.readyState !== WebSocket.OPEN) return;
    this.spectrumWs.send(JSON.stringify({ type: 'reset' }));
  }

  /**
   * Poll-rate divisor (set_rate, 1–8): the server polls radiod at 1/N rate so
   * spectrum frames arrive at 1/N — the idle battery saver. Ignored on shared
   * channels (hardcoded ÷3 server-side). Zoom/pan can migrate the session
   * shared↔private, which RESETS the divisor — so it is re-sent whenever a
   * config reports a binBandwidth change and on reconnect (skin app.js
   * onConfig parity).
   */
  /** Called when the view changes — frames pause across the re-subscription, and the controller
   *  must not read that as starvation. */
  noteResubscribe() { this.lastResubAt = Date.now(); }

  setRate(divisor: number) {
    this.rateDivisor = Math.max(1, Math.min(8, Math.round(divisor)));
    this.gapHist.length = 0; // legit frame-rate change — don't read as stalls
    if (this.spectrumWs?.readyState !== WebSocket.OPEN) return;

    // ★★ ONE LEVER PER SERVER, NEVER TWO. The shim honours BOTH `set_rate`
    // (a frame-dropping divisor) and `fftRate` (the real rate) — so sending a
    // divisor to a VibeServer MULTIPLIES with whatever rate LinkManager has
    // already asked for, and nothing reconciles them:
    //
    //     rung 5 fps ÷ idle divisor 3 = 1.7 fps      (seen as "2 fps")
    //     Full 20 fps ÷ 3             = 6.7 fps      ("Full did nothing")
    //
    // On UberSDR the divisor IS the only lever, so the two agreed and this
    // stayed invisible for as long as VibeServer has existed. Express the
    // divisor as an fftRate instead, so a VibeServer only ever hears one.
    // ★★ ON VIBESERVER, setRate() DOES NOTHING. The rate has exactly ONE owner
    // here — LinkManager (and setPowersaveRate for idle) — and every attempt to
    // give it a second one has produced the same bug in a new place:
    //
    //   • the shim honours BOTH set_rate and fftRate, so the raw divisor
    //     multiplied the controller's rate           (fixed 276)
    //   • apply() divided the rung by rateDivisor, so the controller measured
    //     its own reduction as starvation             (fixed 277)
    //   • ws.onopen re-asserted a stale divisor       (fixed 284)
    //   • and HERE: dividing the ladder rate by a leftover rateDivisor sent
    //     HALF the rung — rung 2 → 5, rung 3 → 2.5 — so Auto crawled while Full
    //     (which goes through apply(), undivided) reached 20 on the SAME server
    //     seconds later. Stuart: "starting rung 2 but divided."
    //
    // ★ Keep the divisor recorded for UberSDR, but never let it reach a
    // VibeServer. One owner, no exceptions.
    this.sendRateLever();
  }
  /**
   * Powersave: drop to an ABSOLUTE frame rate, never a divisor.
   *
   * ★★ The idle saver used setRate(3) — a DIVISOR, which compounds with whatever
   * rung the controller had already chosen. ÷3 of 20 fps is a sensible 6.7; ÷3 of
   * the 5 fps floor is 1.7, and ÷3 of UberSDR's 3.3 emergency rung is 1. So the
   * worse the link already was, the harder powersave hit it — precisely backwards,
   * and how the waterfall ended up at 1 fps.
   *
   * ★ 5 fps is the established floor for a rate anyone CHOOSES (Stuart, on Low
   * Data: "the interpolation can hide 5; 3.3 is reserved for connection issues
   * only"). Powersave is chosen behaviour, so it gets the same floor — and it can
   * never make things worse than the rung it replaces.
   */
  setPowersaveRate() {
    const target = Math.min(POWERSAVE_FPS, this.ladderFps > 0 ? this.ladderFps : POWERSAVE_FPS);
    if (this.spectrumWs?.readyState !== WebSocket.OPEN) return;
    /* ★ The FLOOR is shared policy and stays here; the LEVER is the one thing that differs, so
     *  it is the only part the subclass supplies. */
    this.sendPowersaveLever(target);
  }

  /** The fps of the rung LinkManager currently holds — the base an idle-saver
   *  divisor is applied to. Without it a divisor would have nothing to divide. */
  protected ladderFps = 0;
  protected rateDivisor = 1;   // ★ UberSDR's lever; a VibeServer pins it to 1
  private lastRateBinBw = 0;

  // NOTE (2026-06-12): set_audio_gate / set_squelch / set_dsp / set_nr_mode
  // are AUDIO-WS message types — the spectrum WS this client owns doesn't
  // know them. They now go through VibePowerModule.sendAudioCommand (native
  // socket); client NR/NR2/NB run natively in VibeDSP.swift.

  getStatus(): SDRStatus { return { ...this.status }; }

  /** Geometry for gesture math: predicted while zoom/pan requests are in
   *  flight, server truth once settled. NEVER re-base a gesture on
   *  getStatus() — its centerHz/binBandwidth are one RTT stale during
   *  interaction, which is how fast gestures used to land on old states. */
  getView(): SDRStatus {
    const s = { ...this.status };
    if (this.view.binBandwidth > 0) {
      s.centerHz     = this.view.centerHz;
      s.binBandwidth = this.view.binBandwidth;
      s.bwHz         = this.view.binBandwidth * s.binCount;
    }
    return s;
  }

  /** Stop spectrum display (app backgrounded). Native audio continues
   *  unaffected. The paused flag is CRITICAL: without it the onclose handler
   *  auto-reconnects 3s later and the whole spectrum pipeline runs behind the
   *  locked screen forever (background audio keeps JS alive — measured ~50%
   *  CPU locked). */
  private pausedByApp = false;
  pauseSpectrum() {
    this.pausedByApp = true;
    if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null; }
    if (this.sendTimer)      { clearTimeout(this.sendTimer);      this.sendTimer = null; }
    if (this.settleTimer)    { clearTimeout(this.settleTimer);    this.settleTimer = null; }
    // The watchdog must be inert while paused: a deliberately-closed socket is
    // not a starving one, and "paused" is a state the watch is told about.
    this._stopWatchdog();
    this._setReconnecting(false);
    this.pendingView = null;
    this.spectrumWs?.close();
    this.spectrumWs = null;
  }

  /** Resume spectrum display (app foregrounded). Always opens a FRESH socket:
   *  after a deep suspension (e.g. another audio app reaped our session) the old
   *  spectrumWs can be a stale/half-open object that never fired onclose, so the
   *  previous `!this.spectrumWs` guard would skip the reopen and leave the
   *  waterfall frozen. Callers sequence this AFTER the native audio revive so the
   *  spectrum subscribes to a session that exists again (see SDRScreen AppState). */
  resumeSpectrum() {
    this.pausedByApp = false;
    if (this.destroyed) return;
    if (this.spectrumWs) { try { this.spectrumWs.close(); } catch { /* already dead */ } this.spectrumWs = null; }
    this._openSpectrumWs();
  }

  destroy() {
    this.destroyed = true;
    if (this.tuneSettleAsk) { clearTimeout(this.tuneSettleAsk); this.tuneSettleAsk = null; }
    this.stopLinkManager();
    if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null; }
    if (this.sendTimer)      { clearTimeout(this.sendTimer);      this.sendTimer = null; }
    if (this.settleTimer)    { clearTimeout(this.settleTimer);    this.settleTimer = null; }
    this._stopWatchdog();
    this._setReconnecting(false);
    this.pendingView = null;
    this.spectrumWs?.close();
    this.spectrumWs = null;
  }

  // ── Private ────────────────────────────────────────────────────────────────

  private dbg(msg: string) {
    if (__DEV__) console.warn('[UberSDR]', msg); // console is NOT free in release
    this.callbacks.onDbg?.(msg);
  }

  /** True once a radio has announced itself on this client — so a LATER hwinfo means we came
   *  back, rather than arrived. */
  private hadSession = false;
  /** ★ Set on socket open; the first `config` decides whether to restore our view or adopt the
   *  server's. See _restoreViewOnConfig. */
  private viewRestorePending = false;
  /** ★ The user zoomed or panned while the socket was down — a USER action, so it is sent even
   *  to a shared dial once the socket is back. */
  private viewTouchedWhileDown = false;

  /** ★★★ ADOPT ON A SHARED DIAL, RESTORE ELSEWHERE — decided by the server's own `shared`, which
   *  arrives in this very message ("the decision and the fact that governs it must arrive
   *  together", local_sdr_shim.cpp). A per-listener radio resets our view on a new socket, so
   *  there we put it back exactly as onopen always did; a UberSDR sends no `shared` and keeps
   *  that behaviour too. */
  private _restoreViewOnConfig(msg: Record<string, unknown>) {
    if (!this.viewRestorePending) return;
    this.viewRestorePending = false;
    const touched = this.viewTouchedWhileDown;
    this.viewTouchedWhileDown = false;
    if (msg.shared === true && !touched) {
      this.dbg('shared dial — adopting the server\'s view, not restoring ours');
      return;
    }
    const ws = this.spectrumWs;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({
      type:         'zoom',
      frequency:    Math.round(this.view.centerHz || this.status.centerHz || this.status.frequency),
      binBandwidth: this.view.binBandwidth || this.status.binBandwidth || 100,
    }));
  }

  /** ★★★ "GIVE ME YOUR FULL STATE" — the question that replaces every re-assert. The server
   *  answers unconditionally with config + hwinfo + dial (+ DAB). VibeServer only: an UberSDR
   *  would not know the message. */
  /** ★★★ ASK WHEN OUR OWN SETTLE WINDOW CLOSES — Jr's `armTuneSettle` on the phone. The adopt
   *  branch ignores every config for 1500 ms after OUR tune (so our own echo is not read as
   *  somebody else); if another listener moved the dial inside that window, the server will not
   *  say it again until it CHANGES, and we would sit on our own stale frequency for ever. So once
   *  the window closes we ask. Armed ONLY by our own tune — never by an incoming message, which
   *  is the discipline whose absence made the app deaf (2026-09-21). Shared dials only: on a
   *  per-listener VFO nobody else can move ours. */
  private tuneSettleAsk: ReturnType<typeof setTimeout> | null = null;
  private _armTuneSettleAsk() {
    if (!this.isVibe || !this.sharedDial) return;
    if (this.tuneSettleAsk) clearTimeout(this.tuneSettleAsk);
    this.tuneSettleAsk = setTimeout(() => {
      this.tuneSettleAsk = null;
      if (!this.destroyed) this.requestState();
    }, 1600);
  }

  requestState() {
    const ws = this.spectrumWs;
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ type: 'state' }));
  }
  /** The tune we connected FOR, when it came from memory rather than from the server. Re-sent once
   *  the server has told us where it actually put us; cleared the moment it is honoured. */
  private wantTune: { frequency: number; mode: SDRMode } | null = null;
  /** When this client last tuned itself — so the server's echo of our own tune is not mistaken
   *  for another listener moving a shared dial. */
  private lastLocalTuneAt = 0;
  /** The VFO the SERVER last told us it has us on (config.vfo). The only honest way to ask whether
   *  a tune took: our own status is set by tune() itself. */
  private lastServerVfo = 0;

  /** Re-run the preflight for THIS session id. Public because the fault it cures is detected by
   *  the NATIVE audio watchdog, which can reopen a socket but cannot POST — see onSessionRegistered
   *  and the VibeAudioStuck event. Proven against a live server: a session that was refused
   *  recovers on the same id the moment it is registered. */
  async reregisterSession(): Promise<void> {
    await this._checkConnection();
  }

  private async _checkConnection() {
    this.dbg('POST /connection uuid=' + this.uuid.slice(0, 8));
    // ★★ THE ID GOES IN THE QUERY STRING TOO, not just the body. VibeServer's
    // /connection preflight only ever sees the REQUEST LINE, so an id sent only
    // in the body is invisible to it — and its occupancy check then could not
    // tell the caller apart from anyone else. Result: once this client's own
    // AUDIO socket had claimed the occupant slot, its own preflight answered
    // "in-use" and the spectrum never opened. Audio playing while the app says
    // the server is busy is the signature. The body is kept for older servers.
    // ★★★ THE ADMIN CREDENTIAL RIDES THE PREFLIGHT TOO. It was on both WebSocket URLs and not on
    //     this one — and this one runs FIRST and can stop everything: an owner holding the password
    //     was told "Connection check failed: in-use" and never opened a socket, so the ticket, the
    //     eviction and the whole handshake path never got the chance to run. Three fixes at layers
    //     below this one were all correct and all invisible, because the journey ended here
    //     (Stuart, 2026-08-15).
    // ★★ adminSuffix already carries its own leading '&' (setAdminAuth normalises it), so it is
    //    appended raw — the same string, in the same shape, as the sockets use.
    const resp = await fetch(
      `${this.baseUrl}/connection?user_session_id=${encodeURIComponent(this.uuid)}&proto=${APP_PROTO}`
      + this.adminSuffix, {
      method: 'POST',
      headers: {
        'Content-Type':   'application/json',
        'User-Agent':     USER_AGENT,   // ★ was a hardcoded 'VibeSDR/2.0' long after v2
        'X-Requested-With': 'VibeSDR',
      },
      // password = bypass auth: rate-limited/blocked IPs get through with it
      // (server validates it in this body BEFORE any WS can open)
      body: JSON.stringify({
        user_session_id: this.uuid,
        ...(this.password ? { password: this.password } : {}),
      }),
    });
    if (!resp.ok) {
      const text = await resp.text().catch(() => '');
      throw new Error(`HTTP ${resp.status}: ${text.slice(0, 120)}`);
    }
    // ★★★ THE SERVER TELLS US ITS WHOLE POLICY HERE AND WE USED TO PARSE TWO FIELDS.
    // Measured against WESSEX 2026-07-31 — a single POST returns:
    //   session_timeout 240   IDLE limit, SECONDS. Their own client warns at (timeout − 30) and
    //                         allows 30 s to answer, so 210 s idle → dialog → drop at 240 s.
    //   max_session_time 14400  the four-hour cap — this is the countdown ALREADY on screen,
    //                         which reaches us as sessionSecsLeft on session_warning. Not new.
    //   daily_time_used_secs / daily_time_remaining_secs   a per-IP daily quota (−1 = unlimited).
    //
    // ★★ THREE TRAPS IN THE SEMANTICS:
    //  1. These are PER-IP, not per-server — read fresh on every connection, never cache across
    //     servers or assume constant. A receiver may be generous to a known user and strict with a
    //     stranger.
    //  2. `0` MEANS NO TIMEOUT and is a VALID value, not a missing one. Their client uses nullish
    //     coalescing precisely to avoid treating 0 as absent, and defaults to 300 only when the
    //     field is genuinely absent. Getting this backwards puts a countdown on a server that has
    //     none.
    //  3. The policy is returned EVEN WHEN allowed IS FALSE — so a refusal still carries the terms.
    const json = await resp.json() as {
      allowed: boolean; reason?: string;
      session_timeout?: number; max_session_time?: number;
      daily_time_used_secs?: number; daily_time_remaining_secs?: number;
    };
    this.idlePolicy = {
      // undefined ⇒ absent ⇒ their documented default of 300. 0 ⇒ genuinely no idle limit.
      idleSecs:      typeof json.session_timeout === 'number' ? json.session_timeout : 300,
      maxSessionSecs: Number(json.max_session_time) || 0,
      dailyUsedSecs:  typeof json.daily_time_used_secs === 'number' ? json.daily_time_used_secs : -1,
      dailyLeftSecs:  typeof json.daily_time_remaining_secs === 'number' ? json.daily_time_remaining_secs : -1,
    };
    this.dbg(`/connection → allowed=${json.allowed} reason=${json.reason ?? 'ok'} `
           + `idle=${this.idlePolicy.idleSecs}s maxSession=${this.idlePolicy.maxSessionSecs}s `
           + `dailyLeft=${this.idlePolicy.dailyLeftSecs}s`);
    this.callbacks.onIdlePolicy?.({ ...this.idlePolicy });
    /* ★★★ TOO OLD FOR THIS SERVER (BRIEF-v11 §6). The server has said so BEFORE any socket, with
     *  a reason this app understands, so the user gets an instruction rather than "Connection
     *  lost" — the failure a store user hit for a month (xavxx, 2026-09-17). `updateAppWebUrl` is
     *  the receiver's own web client, offered as the way to listen meanwhile. */
    if (!json.allowed && json.reason === 'update-app') {
      const rel = typeof (json as any).webUrl === 'string' ? String((json as any).webUrl) : '/';
      this.updateAppWebUrl = /^https?:/.test(rel) ? rel : this.baseUrl.replace(/\/+$/, '') + rel;
      throw new Error(UPDATE_APP_MESSAGE);
    }
    if (!json.allowed && json.reason === 'battery-paused') {
      const lv = (json as any).level, ra = (json as any).resumeAt;
      throw new Error(`The server is in a low power state to protect its battery (${lv}%). It comes back once the battery is above ${ra}%.`);
    }
    if (!json.allowed) throw new Error(json.reason ?? 'Server rejected connection');
    // ★★★ ONLY NOW MAY THE AUDIO SOCKET OPEN. Proved against a live UberSDR (2026-08-18): a WS
    //     carrying a session id this POST has not registered gets a 101 and 213 bytes — the
    //     handshake and nothing else — while the identical id, POSTed first, streams 28 KB in the
    //     same five seconds. The server never says why; the phone reports POSIX 53 and the user
    //     hears silence over a working waterfall.
    this.callbacks.onSessionRegistered?.();
  }

  private _wsUrl(path: string): string {
    const url = this.baseUrl.replace(/^http/, 'ws');
    return `${url}${path}`;
  }

  /** The full spectrum WS URL (incl. PIN/password suffix) so NATIVE can open its own
   *  socket when the phone locks — same URL this client uses in _openSpectrumWs. Handed to
   *  VibeWatchModule.startWatchSpectrum so the native forwarder never re-implements auth. */
  watchSpectrumUrl(): string {
    return this._wsUrl(`/ws/user-spectrum?user_session_id=${this.uuid}&mode=binary8${this._binsSuffix()}${this._pwSuffix()}${this.authSuffix}${this.adminSuffix}${CLIENT_Q}`);
  }

  private _openSpectrumWs() {
    if (this.destroyed) return;

    const url = this._wsUrl(`/ws/user-spectrum?user_session_id=${this.uuid}&mode=binary8${this._binsSuffix()}${this._pwSuffix()}${this.authSuffix}${this.adminSuffix}${CLIENT_Q}`);
    // ★★★ SAY WHO WE ARE ON THE SOCKET. The server's connection log records the User-Agent of the
    //     WS upgrade, and React Native's WebSocket sends none by default — so every app session
    //     appeared in the owner's log as "—", indistinguishable from a bot or a bare script, while
    //     browsers listed themselves in full (Stuart, 2026-08-13: "app also not reporting as
    //     VibeSDR or VibeSDR Jr either"). An owner deciding whether to BLOCK an address is exactly
    //     the person who needs to know it is our own app.
    // ★★ RN's third argument takes headers on both platforms. The /connection POST has always sent
    //    this header — the sockets, which are what the log actually records, never did.
    // ★ Cast: React Native's WebSocket takes a third `options` argument (headers, per-socket),
    //   which the DOM lib's type does not describe. The runtime honours it on both platforms.
    const ws  = new (WebSocket as unknown as {
      new (u: string, p: undefined, o: { headers: Record<string, string> }): WebSocket;
    })(url, undefined, { headers: { 'User-Agent': USER_AGENT } });
    ws.binaryType = 'arraybuffer';
    this.spectrumWs = ws;

    // Per-socket, NOT per-client: lastFrameAt carries across reopens, so it can't
    // answer "has THIS socket ever delivered anything?" — which is the question
    // that catches a reopen into a reaped session.
    this.wsOpenedAt      = Date.now();
    this.framesThisSocket = 0;
    this.pingSentAt      = 0;
    this._armWatchdog();

    let specMsgCount = 0;
    ws.onopen = () => {
      if (this.destroyed) { ws.close(); return; }
      this.dbg('Spectrum WS open');
      this.callbacks.onConnect();
      /* ★★★ THE VIEW IS NOT RESTORED HERE ANY MORE — IT WAITS FOR THE SERVER'S FIRST `config`
       *     (2026-09-22). This sent our own zoom the instant the socket opened, before we could
       *     know whether the dial is shared — so a joiner imposed its default full-span view on
       *     a room that already had one, and on an RTL that walks the IF filter open and resets
       *     the AGC: the blip Stuart heard when a second user connected. Stuart's contract: "the
       *     server should say hey we are zoomed into this level ... and the joining client should
       *     be like OK setting myself to that." See _restoreViewOnConfig. */
      this.viewRestorePending = true;
      // ★★ NO RAW DIVISOR RE-ASSERT HERE. This used to send `set_rate` directly on
      // every socket open, which was wrong twice over:
      //
      //   1. It BYPASSED setRate(), the one place that knows a VibeServer speaks
      //      `fftRate`, so an UberSDR divisor was stacked on the VibeServer rate:
      //      rung 2 (10 fps) ÷ a stale divisor 2 = 5, the controller then read 5
      //      against an expected 10, degraded, and rung 3 ÷ 2 gave ~3 fps.
      //   2. It could not be fixed by testing `isVibeServer` HERE, because hwinfo
      //      is a message and has not arrived yet at onopen — the same race that
      //      put the controller on the wrong ladder.
      //
      // It is also redundant: LinkManager.reassert() below re-applies the rung
      // through the backend-correct lever. ★ One owner for the rate, always.
      // Adaptive rate control. On a RECONNECT the server is back at its default, so re-assert
      // whatever rung we had settled on rather than silently jumping back to full rate.
      this.startLinkManager();
      this.link?.reassert();
      // ★ The server forgets the analyser on a new socket. If the panel is open, say so again
      // — otherwise a reconnect the user never noticed leaves it frozen on its last frame,
      // which reads as "the decoder died" rather than "the link blipped".
      if (this.advRds) ws.send(JSON.stringify({ type: 'rdsx', on: 1, eyeEvery: 2 }));
    };

    ws.onmessage = (e) => {
      specMsgCount++;
      if (specMsgCount <= 3) {
        this.dbg(`SpecMsg#${specMsgCount} binary=${e.data instanceof ArrayBuffer} len=${e.data instanceof ArrayBuffer ? e.data.byteLength : (e.data as string).length}`);
      }
      if (e.data instanceof ArrayBuffer) {
        this.specBytes += e.data.byteLength;   // for the connection-meter data-rate readout
        this._parseBinaryFrame(e.data);
      } else if (typeof e.data === 'string') {
        try {
          const msg = JSON.parse(e.data) as Record<string, unknown>;
          this._handleSpectrumMessage(msg);
        } catch {}
      }
    };

    // Ping doubles as the RTT probe for link quality (server excludes pings
    // from rate limiting, so 5s cadence is safe). One outstanding ping at a
    // time — pong handler computes RTT + jitter EMAs.
    //
    // pingSentAt is the timestamp of the OLDEST UNANSWERED ping, not of the last
    // ping sent. It used to be overwritten on every cycle, which meant it was
    // always fresh even when no pong had come back for minutes — so it could
    // measure an RTT but could never detect a link that had stopped answering at
    // all. Leaving it alone until a pong clears it turns it into the watchdog's
    // primary death signal, at no extra traffic.
    const ping = setInterval(() => {
      if (ws.readyState === WebSocket.OPEN) {
        if (this.pingSentAt === 0) this.pingSentAt = Date.now();
        ws.send(JSON.stringify({ type: 'ping' }));
      } else clearInterval(ping);
    }, 5_000);

    const qual = setInterval(() => {
      if (ws.readyState !== WebSocket.OPEN) { clearInterval(qual); return; }
      this._evalLink();
    }, 1_000);

    ws.onclose = (e) => {
      clearInterval(ping);
      clearInterval(qual);
      this.dbg('Spectrum WS closed code=' + e.code);
      this.lastReconnectAt = Date.now();
      this.gapHist.length = 0;
      this.rttHist.length = 0;   // ★ a new socket, a new path — old pings describe the last one
      this._evalLink(); // → 0 (down) immediately
      if (!this.destroyed && !this.pausedByApp) {
        this.callbacks.onDisconnect();
        this._scheduleReconnect();
      }
    };

    // Transient socket errors are NOT user-facing: onclose follows and
    // _scheduleReconnect recovers silently (Android's socket stack fires
    // onerror on any hiccup — surfacing it alert-booted users to the
    // instance picker while the session was actually fine; the link bars
    // already show degradation).
    ws.onerror = () => this.dbg('Spectrum WS error (reconnect handles it)');
  }

  private _parseBinaryFrame(buf: ArrayBuffer) {
    const view  = new DataView(buf);
    const bytes = new Uint8Array(buf);

    // JSON control messages (type:"config" etc.) arrive as BINARY frames of
    // gzipped JSON (server writeJSONCompressed) — NOT text frames. The web
    // client does the same gzip-magic sniff before DecompressionStream.
    if (bytes.length >= 2 && bytes[0] === 0x1f && bytes[1] === 0x8b) {
      try {
        const msg = JSON.parse(ungzip(bytes, { to: 'string' })) as Record<string, unknown>;
        this._handleSpectrumMessage(msg);
      } catch (e) {
        this.dbg('gzip JSON frame parse failed: ' + String(e));
      }
      return;
    }

    if (buf.byteLength < 22) { this.dbg('frame too short: ' + buf.byteLength); return; }

    const magic = view.getUint32(0, true);
    if (magic !== SPEC_MAGIC) {
      this.dbg('bad magic: 0x' + magic.toString(16) + ' expected 0x' + SPEC_MAGIC.toString(16) +
        ' bytes=' + Array.from(bytes.slice(0,4)).map(b=>b.toString(16)).join(','));
      return;
    }

    const flags     = bytes[5];
    const freqLo    = view.getUint32(14, true);
    const freqHi    = view.getUint32(18, true);
    const frequency = freqLo + freqHi * 0x100000000;

    /* ★ NO COPY FOR THE COMMON FRAME. binary8 full frames are the steady state (every frame on a
     *  VibeServer at 1024–4096 bins); slicing the body allocated a fresh 1–4 KB ArrayBuffer per
     *  frame for a view that can index the original at an offset. Float32 bodies still need the
     *  copy: offset 22 is not 4-byte aligned and a Float32Array view would throw. */
    if (flags === FLAG_FULL_U8) { this._applyFullU8(new Uint8Array(buf, 22), frequency); return; }
    const body = buf.slice(22);
    if (flags === FLAG_FULL_F32)  { this._applyFull(new Float32Array(body), frequency); }
    else if (flags === FLAG_DELTA_F32) { this._applyDeltaF32(body, frequency); }
    else if (flags === FLAG_DELTA_U8)  { this._applyDeltaU8(body, frequency); }
  }

  private _applyFull(floats: Float32Array, frequency: number) {
    if (floats.length !== this.bins.length) {
      this.bins = new Float32Array(floats.length);
      this.status.binCount = floats.length;
    }
    this.bins.set(floats);
    this._emitSpectrum(frequency);
  }

  private _applyDeltaF32(body: ArrayBuffer, frequency: number) {
    const view = new DataView(body);
    if (body.byteLength < 2) return;
    // ★ Same guard as _applyDeltaU8 — see the note there.
    if (this.bins.length === 0 && this.status.binCount > 0) {
      this.bins = new Float32Array(this.status.binCount);
    }
    const changeCount = view.getUint16(0, true);
    let offset = 2;
    for (let i = 0; i < changeCount; i++) {
      if (offset + 6 > body.byteLength) break;
      const idx = view.getUint16(offset, true);
      const val = view.getFloat32(offset + 2, true);
      offset += 6;
      if (idx < this.bins.length) this.bins[idx] = val;
    }
    this._emitSpectrum(frequency);
  }

  private _applyFullU8(u8: Uint8Array, frequency: number) {
    if (u8.length !== this.bins.length) {
      this.bins = new Float32Array(u8.length);
      this.status.binCount = u8.length;
    }
    for (let i = 0; i < u8.length; i++) {
      this.bins[i] = u8[i] + U8_DBFS_OFFSET; // dBFS = uint8 - 256
    }
    this._emitSpectrum(frequency);
  }

  private _applyDeltaU8(body: ArrayBuffer, frequency: number) {
    const view  = new DataView(body);
    if (body.byteLength < 2) return;
    // ★★★ A DELTA CANNOT CREATE THE ARRAY IT WRITES INTO, and this server sends NOTHING ELSE.
    //     UberSDR's current encoder emits only `flags 0x04` — a capture of a live feed is deltas
    //     from first frame to last. The write below is bounds-checked against `this.bins`, so an
    //     unsized array discards every sample in silence: frames counted, fps healthy, socket
    //     green, waterfall BLACK. That is exactly what it did to Jr, whose port of this decoder
    //     omitted the config sizing below (2026-08-18) — the phone was saved by having it.
    // ★★ So do not rely on the config having arrived FIRST either. It normally does, and on a
    //    reconnect where the server does not re-send it, "normally" is the whole bug.
    if (this.bins.length === 0 && this.status.binCount > 0) {
      this.bins = new Float32Array(this.status.binCount);
    }
    const changeCount = view.getUint16(0, true);
    let offset = 2;
    for (let i = 0; i < changeCount; i++) {
      if (offset + 3 > body.byteLength) break;
      const idx = view.getUint16(offset, true);
      const val = view.getUint8(offset + 2);
      offset += 3;
      if (idx < this.bins.length) this.bins[idx] = val + U8_DBFS_OFFSET; // dBFS = uint8 - 256
    }
    this._emitSpectrum(frequency);
  }

  private unwrapped: Float32Array = new Float32Array(0);

  // ── Link quality state ──────────────────────────────────────────────────
  private gapHist: number[] = [];   // recent frame inter-arrival gaps (ms)

  // ── Adaptive link management (ported policy — see services/linkManager.ts) ──────────────────
  /** Only the VibeServer shim sends `hwinfo`, so its arrival is an honest signal of which rate
   *  lever this server speaks: VibeServer takes `fftRate` in fps, UberSDR takes a `set_rate`
   *  divisor. Everything else about the controller is identical. */
  /** Per-IP, per-connection — never cached across servers. See _checkConnection. */
  private idlePolicy: IdlePolicy = { idleSecs: 300, maxSessionSecs: 0, dailyUsedSecs: -1, dailyLeftSecs: -1 };
  /** The receiver's terms as last read. Undefined fields never happen — see the defaults above. */
  getIdlePolicy(): IdlePolicy { return { ...this.idlePolicy }; }

  /**
   * Bins to ask a VibeServer for — the FFT/BIN lever, as Jr has always used.
   *
   * ★★ MEASURED 2026-07-26. The phone asked for NOTHING and so got the server's
   * full 4096 bins (4118 B/frame). `sendWs()` on the server is a BLOCKING send,
   * so the server can only emit as fast as the client drains — and at the same
   * configured 5 fps it sent 20.1 KB/s to a loopback probe but only 12 KB/s to
   * the phone. The server was not under-delivering; THE PHONE WAS THE BOTTLENECK,
   * and every "the rate controller is broken" symptom followed from it.
   *
   * ★ 4096 bins is detail no phone screen can show: it costs a 4096-iteration JS
   * loop per frame plus a Skia image, for a display about 1200 px wide. 1024 is
   * still roughly one bin per pixel, cuts the bytes 4x and the per-frame work
   * with it. Jr has done exactly this since it shipped (`bins=` its waterfall
   * width); the phone simply never did.
   */
  /** ★ protected: VibeServerClient asks for this; UberSDR has no use for it. */
  protected static readonly VIBE_BINS = 1024;

  /**
   * Tell the client it is talking to a VibeServer BEFORE the socket opens.
   *
   * ★★ THE LADDER RACE, KILLED AT SOURCE. `isVibeServer` used to be set only when
   * `hwinfo` ARRIVED — but hwinfo is a MESSAGE, so it cannot land before
   * ws.onopen, where startLinkManager() picks the ladder. The controller was
   * therefore ALWAYS built on LADDERS.ubersdr [10, 5, 3.3] and asked a 20 fps
   * VibeServer for 5 then 3.3 — "connects at 5, drops to 3", with no 3 anywhere
   * on VibeServer's own [20/10/5]. Rebuilding the controller when hwinfo landed
   * was a patch over the race and did not always fire.
   *
   * ★ The app KNOWS which backend it dialled before it dials it. Deciding a
   * backend-specific policy from state that arrives later is the bug; taking it
   * from the caller, who has known all along, is the fix.
   */
  /** ★ Read-only: is the far end a VibeServer? Clients use it to decide whether to OFFER a
   *  VibeServer-only control at all. ★ Advertised is not the same as supported — a button
   *  that does nothing on an UberSDR reads as "this feature is broken", which is worse than
   *  not showing it (the lesson from the keyboard hints). */
  private link: LinkManager | null = null;
  private specFrames = 0;              // frames counted in the current 1s window
  private specBytes  = 0;              // spectrum bytes in the current 1s window (audio is native)
  private linkTimer: ReturnType<typeof setInterval> | null = null;
  private serverMaxFps = 0;            // the owner's ceiling, once hwinfo tells us

  /** Which ladder the live controller was built for, so a late `hwinfo` can
   *  rebuild it against the right one. */

  private startLinkManager() {
    if (this.linkTimer) return;
    const mode = (this.linkMode ?? 'adaptive') as LinkMode;
    this.link = new LinkManager({
      ladder: this.ladderFor(),
      // 5 fps is the deepest a USER may pin — below that is adaptive-only, because the
      // interpolation can hide 5 but not 3.3.
      lowDataRung: 2,
      mode,
      apply: (rung, fps) => {
        if (this.isVibe) {
          this.ladderFps = fps;
          if (this.spectrumWs?.readyState === WebSocket.OPEN) {
            // ★★ SEND THE RUNG'S RATE, UNDIVIDED. Dividing by the idle-saver's
            // rateDivisor here made the controller FIGHT ITSELF: it asks for 20,
            // deliberately sends 6.7, then measures 6.7 against an expectation of
            // 20, reads 33% as starvation and degrades — over and over, so the
            // link glyph flapped red/green and the rate collapsed to the floor.
            //
            // The controller must only ever ask for what it expects to receive.
            // The idle saver PAUSES it before applying a divisor (setLinkPaused),
            // so the two can never be active at once and apply() needs no
            // knowledge of powersave at all.
            this.spectrumWs.send(JSON.stringify({ type: 'fftRate', value: fps }));
          }
        } else {
          this.setRate(rung);          // UberSDR: rung IS the poll divisor
        }
      },
    });
    if (this.serverMaxFps > 0) this.link.applyServerCeiling(this.serverMaxFps);
    this.linkTimer = setInterval(() => {
      const fps = this.specFrames;
      this.specFrames = 0;
      const kbps = this.specBytes / 1024;   // spectrum only — the phone's audio bytes are native
      this.specBytes = 0;
      const live = this.spectrumWs?.readyState === WebSocket.OPEN;
      // `settled` guards a tune/zoom re-subscription, where frames legitimately pause — reading
      // that as a bad link would throttle every time the user moved the dial.
      // PAUSED during idle powersave: the idle saver owns the rate then (setRate(IDLE_DIVISOR)), and a
      // running controller would re-assert its own rung every second and win the fight — powersave pill
      // showing while the wire held 10fps (Stuart 2026-07-24). Skip the tick's rate control while
      // paused; still emit the meter so the readout tracks the real idle rate.
      if (!this.linkPaused) this.link?.tick(fps, !!live, Date.now() - this.lastResubAt > 1500);
      if (this.link) this.callbacks.onLinkRate?.(this.link.adaptiveRung, this.link.settling, fps, kbps);
    }, 1000);
  }

  private stopLinkManager() {
    if (this.linkTimer) { clearInterval(this.linkTimer); this.linkTimer = null; }
    this.link = null;
  }

  /** Idle powersave freezes the adaptive/pinned controller so it stops fighting the idle saver's
   *  directly-set rate. The saver calls setRate() itself; this just stops the controller re-asserting. */
  private linkPaused = false;
  setLinkPaused(p: boolean) { this.linkPaused = p; }

  /** ★★★ COMING OUT OF POWERSAVE, THE RATE HAS TO BE ASKED FOR AGAIN.
   *
   *  Powersave goes DOWN with an absolute `fftRate: 5` (setPowersaveRate) and the wake path came
   *  back UP with setRate() — which on a VibeServer returns without sending anything, by design:
   *  "one lever per server, never two". So the pill disappeared, the controller was un-paused, and
   *  the server sat at 5 fps with nothing left to tell it otherwise. Stuart: "any interaction which
   *  got rid of the powersave message didnt restore 10fps only opening the advanced rds box did" —
   *  and that box only worked by accident, because it provokes an hwinfo that rebuilds the ladder
   *  and calls forceApply() on the way past.
   *
   *  ★★ forceApply, NOT reassert. reassert() deliberately does nothing on rung 1, which is exactly
   *     where a healthy link sits — so the one state that most needed restoring was the one state
   *     it skipped.
   *  ★ The controller is the rate's single owner (see setRate's note); this hands it back rather
   *    than introducing a second way to set the rate, which is what caused every bug in that list. */
  resumeRate() { this.link?.forceApply(); }

  /** When the view last changed — frames pause across a re-subscription. */
  private lastResubAt = 0;
  /** User preference, set by the app (Auto / Full rate / Low data). A setter so changing it LIVE also
   *  reconfigures the running controller — otherwise the LinkManager's own `mode` stays frozen at
   *  whatever it was constructed with and the toggle is ignored (Low Data held 10fps, 2026-07-24). */
  private _linkMode: LinkMode = 'adaptive';
  get linkMode(): LinkMode { return this._linkMode; }
  set linkMode(m: LinkMode) { this._linkMode = m; this.link?.setMode(m); }
  private lastFrameAt    = 0;
  private lastReconnectAt = 0;
  private pingSentAt     = 0;
  private rttAvg         = 0;       // EMA of ping RTT
  private rttJit         = 0;       // median of |rtt − rttAvg| over rttHist
  private rttHist: number[] = [];   // the last 8 ping RTTs
  private lastLink: -1 | 0 | 1 | 2 | 3 = -1;

  /** Score the link like a phone signal indicator. Stalls are judged against
   *  the MEDIAN gap, so legit rate changes (idle divisor) don't read as loss. */
  private _evalLink() {
    let q: 0 | 1 | 2 | 3;
    if (!this.spectrumWs || this.spectrumWs.readyState !== WebSocket.OPEN) {
      q = 0;
    } else {
      const now = Date.now();
      const h = this.gapHist;
      let med = 120;
      if (h.length >= 5) {
        const s = [...h].sort((a, b) => a - b);
        med = s[s.length >> 1];
      }
      let stalls = 0;
      for (let i = 0; i < h.length; i++) if (h[i] > med * 2.5 + 50) stalls++;
      const starving = this.lastFrameAt > 0 &&
        now - this.lastFrameAt > Math.max(2000, med * 4);
      if (now - this.lastReconnectAt < 8000 || stalls >= 3 || starving || this.rttJit > 250) {
        q = 1;
      } else if (stalls >= 1 || this.rttJit > 80 || this.rttAvg > 400) {
        q = 2;
      } else {
        q = 3;
      }
    }
    /* ★★★ HOLD A LEVEL BEFORE SHOWING IT — the score above has no hysteresis, and a meter with none
     *  HUNTS. One late frame makes `stalls` 1, which is 3 bars down to 2 instantly; the sample then sits
     *  in a 40-deep history for about eight seconds at the five frames a second a busy Pi 2 sends, so the
     *  next late frame arrives while the last is still counted. Stuart, watching two apps do it at once:
     *  "both connection meters going 123 321 123 321 … its constantly hunting."
     *  ★★ What is wrong is the DISPLAY, not the score: the link really is varying, and the bars are meant
     *     to answer "is this connection healthy", which is a question about the last few seconds, not the
     *     last frame. So a new level has to survive three evaluations (~3 s) before it is published.
     *  ★ EXCEPT ZERO. A closed socket is not a fluctuation and must show immediately — a meter that
     *    politely waits three seconds to admit the connection has gone is the one lie it must never tell. */
    if (q === 0 || q === this.lastLink) {
      this.linkPending = q;
      this.linkPendingN = 0;
    } else if (q === this.linkPending) {
      this.linkPendingN++;
    } else {
      this.linkPending = q;
      this.linkPendingN = 1;
    }
    const settled = q === 0 || this.lastLink === -1 || this.linkPendingN >= 3;
    if (q !== this.lastLink && settled) {
      this.lastLink = q;
      this.linkPendingN = 0;
      this.callbacks.onLink?.(q);
    }
  }
  private linkPending: 0 | 1 | 2 | 3 = 3;
  private linkPendingN = 0;

  // ── Starvation watchdog + recovery ──────────────────────────────────────
  // See the constants block for why this exists and why detection is pong-first.
  //
  // It runs in EVERY app state, not just background. The brief that specified it
  // assumed foreground self-heals via SDRScreen's AppState handler — but that
  // handler only fires on a lock/unlock TRANSITION, which is a different thing
  // from "the app is awake". A foregrounded phone whose cellular flow is silently
  // rebound has exactly the same dead socket and no transition coming. Guarding
  // on pausedByApp alone is strictly safer, and the 15s rate limit means it
  // cannot collide with resumeSpectrum()'s own force-reopen.

  private watchdogTimer: ReturnType<typeof setInterval> | null = null;
  private wsOpenedAt        = 0;
  private framesThisSocket  = 0;
  private lastForceReopenAt = 0;
  private deadReopens       = 0;   // reopens that produced no frames
  private reconnecting      = false;

  /** Frame-staleness limit, derived from the divisor WE commanded rather than
   *  from observed gaps. Shared channels also apply a hardcoded ÷3 server-side,
   *  so allow for it. Base cadence is ~20fps (50ms). */
  private _staleLimitMs(): number {
    const expectedGap = 50 * this.rateDivisor * 3;
    return Math.max(STALE_MIN_MS, expectedGap * 8);
  }

  private _armWatchdog() {
    this._stopWatchdog();
    this.watchdogTimer = setInterval(() => this._watchdogTick(), WATCHDOG_TICK_MS);
  }

  private _stopWatchdog() {
    if (this.watchdogTimer) { clearInterval(this.watchdogTimer); this.watchdogTimer = null; }
  }

  private _watchdogTick() {
    if (this.destroyed || this.pausedByApp) return;
    const ws = this.spectrumWs;
    // Not OPEN = onclose already owns the recovery. Don't double-drive it.
    if (!ws || ws.readyState !== WebSocket.OPEN) return;

    const now = Date.now();

    // 1. PONG TIMEOUT — the link stopped answering. Rate-agnostic, so it is the
    //    one detector a deliberately-slowed feed cannot fool.
    if (this.pingSentAt > 0 && now - this.pingSentAt > PONG_TIMEOUT_MS) {
      this.forceResubscribe('pong-timeout');
      return;
    }

    // 2. NO FIRST FRAME — the socket opened into a session that isn't there.
    if (this.framesThisSocket === 0 && this.wsOpenedAt > 0 &&
        now - this.wsOpenedAt > NO_FIRST_FRAME_MS) {
      this.forceResubscribe('no-first-frame');
      return;
    }

    // 3. FRAME STALENESS — backstop for a server that pongs but stops sending.
    if (this.framesThisSocket > 0 && this.lastFrameAt > 0 &&
        now - this.lastFrameAt > this._staleLimitMs()) {
      this.forceResubscribe('frames-stale');
      return;
    }
  }

  private _setReconnecting(busy: boolean) {
    if (busy === this.reconnecting) return;
    this.reconnecting = busy;
    this.callbacks.onReconnecting?.(busy);
  }

  /** Force a fresh spectrum socket. Public because the watch escalates into it
   *  (its own row-staleness knowledge is otherwise merely displayed) and the
   *  native network-path monitor fires it on a WiFi↔cellular hop.
   *
   *  Rate-limited: a flapping path must not churn server sessions. */
  forceResubscribe(reason: string) {
    if (this.destroyed || this.pausedByApp) return;
    const now = Date.now();
    if (now - this.lastForceReopenAt < FORCE_REOPEN_MS) {
      this.dbg(`force-resubscribe (${reason}) SUPPRESSED — ${Math.round((FORCE_REOPEN_MS - (now - this.lastForceReopenAt)) / 1000)}s left in window`);
      return;
    }
    this.lastForceReopenAt = now;
    this.dbg(`force-resubscribe (${reason})`);
    this._setReconnecting(true);
    this._stopWatchdog();
    // A socket we are replacing that never delivered a single frame is evidence
    // the SESSION is gone, not just the flow — and no number of reopens can fix
    // that, because only connect() re-registers. Counted HERE rather than at
    // detection: detection ticks every 2s and can fire repeatedly while a reopen
    // is rate-limited, which would have counted our own patience as failures.
    if (this.framesThisSocket === 0 && this.wsOpenedAt > 0) this.deadReopens++;
    // Same fresh-socket semantics as resumeSpectrum(): a half-open zombie never
    // fires onclose, so closing it is defensive, not a handshake.
    if (this.spectrumWs) {
      try { this.spectrumWs.close(); } catch { /* already dead */ }
      this.spectrumWs = null;
    }
    void this._reopenSequenced(reason);
  }

  /** The ordering connect() established, applied to every recovery path.
   *
   *  _scheduleReconnect used to reopen after a flat 3s with zero coordination.
   *  The native audio watchdog can take up to ~12s (8s staleness + 4s tick), so
   *  after an outage long enough for the server to reap the session, the spectrum
   *  would reopen FIRST against a session that no longer existed: the socket
   *  connects, receives nothing, and never retries. Audio confirms the session is
   *  alive; only then does the spectrum subscribe to it. */
  private async _reopenSequenced(reason: string) {
    // Escalation: the session itself is suspect. _openSpectrumWs never re-POSTs
    // /connection — only connect() does — so a reaped session can never be
    // re-registered by reopening alone, however many times we try.
    if (this.deadReopens >= REOPENS_BEFORE_RECHECK) {
      this.dbg(`${this.deadReopens} dead reopens — re-registering session`);
      try {
        await this._checkConnection();
        this.deadReopens = 0;
      } catch (e) {
        this.dbg('session re-register failed: ' + String(e));
      }
      if (this.destroyed || this.pausedByApp) { this._setReconnecting(false); return; }
    }

    await this._awaitAudioAlive();
    if (this.destroyed || this.pausedByApp) { this._setReconnecting(false); return; }

    this.dbg(`reopening spectrum (${reason})`);
    this._openSpectrumWs();
  }

  /** Resolve once native audio is confirmed carrying packets — or give up and let
   *  the caller open anyway (the session may be fine and only the spectrum dead).
   *
   *  Returns immediately on backends whose audio the native module does not own
   *  (OWRX/Kiwi push external PCM; a local/VibeServer shim runs its own socket),
   *  where there is no native liveness to wait for. */
  private async _awaitAudioAlive(): Promise<void> {
    const mod = VibePowerModule as unknown as {
      audioStaleness?: () => Promise<number>;
      revive?: () => void;
    } | null;
    if (typeof mod?.audioStaleness !== 'function') {
      await this._delay(AUDIO_SETTLE_MS);
      return;
    }

    const deadline = Date.now() + AUDIO_WAIT_MS;
    let revived = false;
    while (Date.now() < deadline) {
      let stale: number;
      try { stale = await mod.audioStaleness(); } catch { break; }
      // −1 = this backend's audio isn't the native engine's to vouch for.
      if (stale < 0) return;
      if (stale < 2) {
        this.dbg(`audio alive (${stale.toFixed(1)}s) — spectrum follows in ${AUDIO_SETTLE_MS}ms`);
        await this._delay(AUDIO_SETTLE_MS);
        return;
      }
      if (!revived) {
        revived = true;
        this.dbg(`audio stale (${stale.toFixed(1)}s) — reviving before spectrum`);
        mod.revive?.();
      }
      await this._delay(500);
    }
    this.dbg('audio never confirmed — opening spectrum anyway');
  }

  private _delay(ms: number): Promise<void> {
    return new Promise((r) => setTimeout(r, ms));
  }

  private _emitSpectrum(frequency: number) {
    // Frame inter-arrival tracking for link quality
    const fNow = Date.now();
    if (this.lastFrameAt > 0) {
      this.gapHist.push(fNow - this.lastFrameAt);
      if (this.gapHist.length > 40) this.gapHist.shift();
    }
    this.lastFrameAt = fNow;
    // FRAMES are the proof of recovery, not a socket that merely opened — a
    // reopen into a reaped session opens perfectly happily and delivers nothing.
    if (this.framesThisSocket === 0) this.deadReopens = 0;
    this.framesThisSocket++;
    this._setReconnecting(false);
    const s = this.status;
    s.centerHz = frequency;
    s.bwHz     = s.binBandwidth * s.binCount;

    // Unwrap FFT bin ordering from radiod (spectrum-display.js parity):
    // frames arrive as [positive freqs DC→+Nyquist, negative freqs −Nyquist→DC];
    // display needs [negative, positive]. Without this swap every signal is
    // drawn half the span away from its true frequency. this.bins stays in
    // WRAPPED order because delta-frame indices refer to wrapped positions.
    const n = this.bins.length;
    const half = n >> 1;
    if (this.unwrapped.length !== n) this.unwrapped = new Float32Array(n);
    const out = this.unwrapped;
    out.set(this.bins.subarray(half, half * 2), 0);
    out.set(this.bins.subarray(0, half), half);

    // While zoom/pan is in flight, render frames under the PREDICTED geometry:
    // the view goes where the finger says instantly and stays put; data from
    // intermediate states is at most one RTT misplaced. Emitting each frame's
    // own (intermediate) geometry replays the whole transition as the echoes
    // arrive — that was the band-plan flash / view-reset glitch.
    const emit = { ...s };
    // The REAL centre of these bins — never overridden below. The watch crop indexes bins by
    // this; using the predicted emit.centerHz draws the signal offset from the VFO.
    emit.trueCenterHz = frequency;
    const v = this.view;
    if (this._inFlight() && v.binBandwidth > 0) {
      emit.centerHz     = v.centerHz;
      emit.binBandwidth = v.binBandwidth;
      emit.bwHz         = v.binBandwidth * s.binCount;
    } else if (v.binBandwidth > 0 &&
               (Math.abs(frequency - v.centerHz) > 1 ||
                Math.abs(s.binBandwidth - v.binBandwidth) > v.binBandwidth * 1e-6)) {
      // Deviant frame with no request in flight — same unsolicited-change
      // treatment as configs: keep showing the stable view, let the settle
      // timer adopt whatever geometry survives the confirm window.
      emit.centerHz     = v.centerHz;
      emit.binBandwidth = v.binBandwidth;
      emit.bwHz         = v.binBandwidth * s.binCount;
      this._armSettle();
    }
    this.specFrames++;            // per-second count for the adaptive controller
    this.callbacks.onSpectrum(out, emit);
  }

  private _handleSpectrumMessage(msg: Record<string, unknown>) {
    if (msg.type === 'pong') {
      if (this.pingSentAt > 0) {
        const rtt = Date.now() - this.pingSentAt;
        this.pingSentAt = 0;
        /* ★★★ MEDIANS, NOT EMAs (2026-09-22). One ping every 5 s, and on Wi-Fi a single one can
         *  come back 200-300 ms late (the radio dozing between pings) while the STREAM is clean —
         *  measured off the Pi 2: 234 frames, zero stalls, yet the meter sat yellow on Stuart's
         *  Wi-Fi devices. An EMA at 0.3 turned that one late ping into `rttJit > 80` for the next
         *  half-minute. The median of the last eight ignores an isolated spike and still sees a
         *  link that is genuinely jittery, because then MOST of the samples are. */
        this.rttHist.push(rtt);
        if (this.rttHist.length > 8) this.rttHist.shift();
        const med = (a: number[]) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };
        this.rttAvg = med(this.rttHist);
        this.rttJit = this.rttHist.length >= 3
          ? med(this.rttHist.map(r => Math.abs(r - this.rttAvg))) : 0;
      }
      return;
    }
    // Server replies with type:"config" — sent on connect and after every
    // zoom/pan/reset/set_rate (sendStatus in user_spectrum_websocket.go):
    //   { type:"config", centerFreq, binCount, binBandwidth, totalBandwidth }
    // This is the ONLY way the client learns binBandwidth (binary frames carry
    // just the centre frequency) — without it bwHz stays 0 and the entire
    // frequency→pixel mapping (needle, band plan, gestures) is dead.
    // V4 local hardware: FM RDS + stereo → reuse the OWRX metadata display path.
    if (msg.type === 'rds') {
      const ps = typeof msg.ps === 'string' ? msg.ps.trim() : '';
      const rt = typeof msg.radiotext === 'string' ? msg.radiotext.trim() : '';
      const stereo = msg.stereo === true;
      // PI (hex) + ECC → station country (for the flag + logo lookup), same as
      // the FM-DX backend. The shim sends pi (int, -1 = none) and ecc (0 = none).
      const pi = typeof msg.pi === 'number' && msg.pi >= 0
        ? msg.pi.toString(16).toUpperCase().padStart(4, '0') : undefined;
      const ecc = typeof msg.ecc === 'number' && msg.ecc > 0 ? msg.ecc : undefined;
      (this.callbacks as any).onMetadata?.({
        stationName: ps || undefined,
        text: rt || undefined,
        // ★ Badge on ANY decoded RDS, not just a name — a text-only frame is still RDS, and
        // without the badge it would show up unlabelled, indistinguishable from a bookmark.
        badge: (ps || rt) ? 'RDS' : undefined,
        stereo,
        pi,
        // ECC + PI when the station sends an ECC; otherwise the PI's country nibble
        // VALIDATED against the receiver's own country. Most stations never transmit an
        // ECC (it rides in group 1A), which is why the flag and the station logo almost
        // never appeared — with no country, the logo lookup demanded a near-exact name
        // match and silently failed. The nibble check recovers the country for domestic
        // stations without inventing one for a foreign catch: a sporadic-E Spaniard's
        // nibble does not match a British receiver, so it stays blank.
        countryIso: resolveStationIso(ecc, pi, receiverIso()) || undefined,
        // ★★ THE RAW ECC TOO, not only the country derived from it. The SERVER's logo lookup takes
        //    the ECC and, when we send none, tries the plausible candidates itself — which is the
        //    only way a station that never transmits group 1A gets its broadcaster's artwork. The
        //    app was throwing this away here and then doing a lookup that REQUIRED one.
        ecc,
      });
      return;
    }
    if (msg.type === 'session_expired') {
      this.refused = true;                    // never auto-retry a deliberate refusal
      // ★ `fresh` = when a FULL turn returns, which is a different (and much longer) window than
      //   the cooldown — see the server's note. 0 when an older server did not say.
      this.callbacks.onSessionEnded?.(Number(msg.cooldown) || 0, Number(msg.fresh) || 0);
      return;
    }
    if (msg.type === 'cooldown') {
      this.refused = true;
      this.callbacks.onCooldown?.(Number(msg.secs) || 0);
      return;
    }
    if (msg.type === 'busy') {
      this.refused = true;                    // a busy server must not be hammered
      const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
      this.callbacks.onBusy?.({ queuePos: n(msg.queuePos), queueLen: n(msg.queueLen),
                                freeIn: n(msg.freeIn), queueFull: msg.queueFull === true });
      return;
    }
    if (msg.type === 'elsewhere') {
      // ★★★ THE SERVER EXPLAINED ITSELF AND THE APP WAS NOT LISTENING. This arrives, correctly and
      //     within microseconds, naming the radio already held — and only the BROWSER handled it.
      //     The app and Jr ignored it and sat on "waterfall initializing" until something else
      //     timed out, so a deliberate, well-explained policy looked exactly like a broken server.
      //     Found from the field: ff-mish's watch shares its iPhone's address (a watch tunnels
      //     through its paired phone), so the pair trips a rule an iPad never does (GitHub #21).
      this.refused = true;                    // nothing to wait for — retrying cannot help
      this.callbacks.onElsewhere?.(String(msg.radio || 'another radio'));
      return;
    }
    if (msg.type === 'evicted') {
      this.refused = true;
      this.callbacks.onEvicted?.();
      return;
    }
    if (msg.type === 'session_warning') {
      // ★ NOT terminal — we are still connected and still listening. Setting `refused` here
      //   would tear down a perfectly good session two minutes early.
      this.callbacks.onSessionWarning?.(Number(msg.secs) || 0);
      return;
    }
    if (msg.type === 'dial') {
      // ★ Remembered because it changes what an "unsolicited" centre MEANS — see the config handler.
      this.sharedDial = String(msg.mode || 'exclusive') !== 'exclusive';
      this.callbacks.onDial?.({
        mode: String(msg.mode || 'exclusive'),
        tuner: Number(msg.tuner) || 0,
        mine: msg.mine === true,
        you: Number(msg.you) || 0,
        listeners: Number(msg.listeners) || 0,
        decoding: msg.decoding === true,
      });
      return;
    }
    if (msg.type === 'dial_refused') { this.callbacks.onDialRefused?.(); return; }
    if (msg.type === 'said') {
      this.callbacks.onSaid?.(Number(msg.from) || 0, String(msg.id || ''));
      return;
    }
    if (msg.type === 'notice') {
      /* ★★★ ONE TYPE, TWO MESSAGES, AND THE REFUSAL WAS BEING THROWN AWAY.
       *  `text` is the owner's STANDING notice (persistent, posted from the setup page); `why` is
       *  a REFUSAL of something this listener just tried (transient). Only `text` was read, so
       *  every refusal became the empty string: "the notches are on automatic — …", "the operator
       *  has reserved the notch filters". The server explained itself and we discarded it — which
       *  is precisely why a locked control reads as a DEAD control (Stuart, 2026-09-13: "the
       *  controls just appear dead").
       *  ★★ They must not share a slot: a refusal routed into the owner's notice would clobber a
       *     message somebody deliberately posted, and leave it clobbered. */
      const why = typeof msg.why === 'string' ? msg.why : '';
      if (why) { this.callbacks.onRefused?.(why); return; }
      // ★ `vts` = a transient server line for the bar (gain start-up, sampling switch); the
      //   refusal path already lands there, with the same look.
      const vts = typeof msg.vts === 'string' ? msg.vts : '';
      if (vts) { this.callbacks.onRefused?.(vts); return; }
      this.callbacks.onNotice?.(typeof msg.text === 'string' ? msg.text : '');
      return;
    }
    if (msg.type === 'admin') {
      // ★★ `superseded` = an owner proved the same password more recently, so this session is no
      //    longer admin. Distinct from `refused` (the password was wrong): nothing was mistyped
      //    here, and the honest response is to let the credential go rather than to re-assert it.
      this.callbacks.onAdminState?.({
        set: this.adminSet, ok: msg.ok === true, refused: msg.refused === true,
        superseded: msg.superseded === true });
      return;
    }
    if (msg.type === 'rdsx') {
      // ★ Trust the shape, not the sender: this arrives on the same socket as everything
      // else and a field the server has not sent yet must not crash the panel. Numbers
      // default to -1 (the "unknown" the renderers already understand), arrays to empty.
      const num = (x: any, d = -1) => (typeof x === 'number' && isFinite(x) ? x : d);
      const str = (x: any) => (typeof x === 'string' ? x : '');
      const arr = (x: any): number[] => (Array.isArray(x) ? x.filter((n) => typeof n === 'number') : []);
      this.callbacks.onRdsExt?.({
        pty: num(msg.pty), tp: num(msg.tp), ta: num(msg.ta), ms: num(msg.ms), di: num(msg.di),
        ptyRaw: num(msg.ptyRaw), tpRaw: num(msg.tpRaw), taRaw: num(msg.taRaw),
        msRaw: num(msg.msRaw), diRaw: num(msg.diRaw),
        ct: num(msg.ct), ctoff: num(msg.ctoff, 0), gtot: num(msg.gtot, 0), afseen: num(msg.afseen, 0),
        rtpTitle: str(msg.rtpTitle), rtpArtist: str(msg.rtpArtist),
        longPs: str(msg.longPs), ptyn: str(msg.ptyn),
        lang: num(msg.lang), pinDay: num(msg.pinDay), pinHour: num(msg.pinHour), pinMin: num(msg.pinMin),
        phase: num(msg.phase), phaseDrift: num(msg.phaseDrift), phaseCoh: num(msg.phaseCoh, 0),
        pilotDev: num(msg.pilotDev), rdsDev: num(msg.rdsDev), ber: num(msg.ber),
        pilotLock: Boolean(msg.pilotLock),
        grp: arr(msg.grp), af: arr(msg.af), xy: arr(msg.xy), mpx: arr(msg.mpx),
        // ★ NAMED EXPLICITLY, like every other field here — this object literal is built by
        //   hand, so a field left out arrives undefined rather than failing to compile.
        // ★ ABSENT = UNCHANGED, not empty: with eyeEvery the server sends the grids in every 3rd message only
        //   (Stuart, 2026-09-19 — Advanced RDS took a server from 18 to 80-90 kB/s).
        eyeP: typeof msg.eyeP === 'string' ? (this.lastEye.P = msg.eyeP) : this.lastEye.P,
        eyeS: typeof msg.eyeS === 'string' ? (this.lastEye.S = msg.eyeS) : this.lastEye.S,
        eyeR: typeof msg.eyeR === 'string' ? (this.lastEye.R = msg.eyeR) : this.lastEye.R,
        eyeW: num(msg.eyeW, 0), eyeH: num(msg.eyeH, 0), eyeDev: num(msg.eyeDev, 0),
        eyeAmp: Array.isArray(msg.eyeAmp) ? (msg.eyeAmp as unknown[]).map((v) => num(v, 0)) : [0, 0, 0],
        mpxDev: num(msg.mpxDev, 0), mpxHold: num(msg.mpxHold, 0), mpxNoise: num(msg.mpxNoise, 0),
        eon: Array.isArray(msg.eon) ? msg.eon.map((e: any) => ({
          pi: str(e?.pi), ps: str(e?.ps), af: num(e?.af, 0), ta: num(e?.ta, 0) })) : [],
        oda: Array.isArray(msg.oda) ? msg.oda.map((o: any) => ({
          aid: str(o?.aid), grp: num(o?.grp, 0) })) : [],
        // ★ Defaults chosen so an OLDER SERVER reads as "nothing to report" rather than as alarming
        //   news: no pilot problem, no multipath, filters wide open. A corner of 0 would say
        //   "everything is being cut", which is the opposite of the truth on a server that simply
        //   does not have the feature.
        mpxSnr: num(msg.mpxSnr, 0),
        snrOk: Number(msg.snrOk ?? 1) === 1,
        multipath: num(msg.multipath, 0),
        multipathOk: Number(msg.multipathOk ?? 0) === 1,
        hiCutLmr: num(msg.hiCutLmr, 15000), hiCutAud: num(msg.hiCutAud, 15000),
        nbRate: num(msg.nbRate, 0),
        ceqOn: Number(msg.ceqOn ?? 0) === 1,
        ceqAfter: num(msg.ceqAfter, 0), ceqWhy: num(msg.ceqWhy, 3),
        ifGain: num(msg.ifGain, 0), ifCand: num(msg.ifCand, 0), ifBw: num(msg.ifBw, 0),
        afAll: Array.isArray(msg.afAll)
          ? msg.afAll.filter((e: any) => Array.isArray(e) && e.length >= 2)
                     .map((e: any) => [Number(e[0]) || 0, Number(e[1]) ? 1 : 0] as [number, number])
          : [],
      });
      return;
    }
    if (msg.type === 'rspstat') {
      this.callbacks.onRspStat?.({
        /* ★ −999, not 0, for "not reported". Now that the panel prints zero and NEGATIVE gains as
         *  real readings (they are — the RSP's LNA states are attenuators at medium wave), a
         *  missing field defaulting to 0 would draw a confident "0.0 dB" for a value nobody sent.
         *  −999 is the server's own sentinel for "cannot read it". */
        sysGain:  Number.isFinite(Number(msg.sysGain)) ? Number(msg.sysGain) : -999,
        lna:      Number(msg.lna) || 0,
        ifgr:     Number(msg.ifgr) || 0,
        overload: Number(msg.overload) === 1,
        settling: Number(msg.settling) === 1,
        /* ★★★ THE TWELVE FIELDS THE SERVER HAS ALWAYS SENT AND THIS CLIENT NEVER READ.
         *  rspstat carries seventeen; the web client uses all of them and the app used five.
         *  Each of the ones added here drives a control that was otherwise wrong or dead:
         *
         *  · autoNotch/userNotch — WHO OWNS THE NOTCHES. With automatic notching on, the server
         *    REFUSES a listener's notch and says so; without this the panel drew them live, the
         *    tap did nothing, and (before the refusal fix) nothing explained it. The dead control
         *    Stuart reported.
         *  · lnaN — HOW MANY LNA STATES EXIST AT THIS FREQUENCY. The count is per BAND, not per
         *    model: an RSP1A has seven on medium wave and ten higher up. Sizing the slider from
         *    the model's capability maps its top third onto states the radio clamps away — the
         *    exact bug the web client already fixed, still present here.
         *  · rfAgc/agcSet — the RF AGC and its target, both new controls (2026-09-13).
         *  · adcPeak/adcClip — the level every gain decision turns on.
         *  · gainStuck — the SDRplay gain API has frozen; offer the reset.
         *  · agcInit/agcReinit — the chip is (re)initialising, so readings are not yet meaningful.
         *  ★ Defaults chosen so a server that has not sent a field yet behaves as before rather
         *    than as "off": lnaN 0 means "fall back to the capability", the notch owners default
         *    to NOT owning, and gainStuck defaults to false. */
        rfNotch:   Number(msg.rfNotch) === 1,
        dabNotch:  Number(msg.dabNotch) === 1,
        autoNotch: Number(msg.autoNotch) === 1,
        userNotch: Number(msg.userNotch) === 1,
        rfAgc:     Number(msg.rfAgc) === 1,
        agcSet:    Number.isFinite(Number(msg.agcSet)) ? Number(msg.agcSet) : -30,
        adcPeak:   Number.isFinite(Number(msg.adcPeak)) ? Number(msg.adcPeak) : 0,
        adcClip:   Number(msg.adcClip) || 0,
        lnaN:      Number(msg.lnaN) || 0,
        agcInit:   Number(msg.agcInit) === 1,
        agcReinit: Number(msg.agcReinit) === 1,
        gainStuck: Number(msg.gainStuck) === 1,
      });
      return;
    }
    // ★★★ THE GAIN LOOP SAID SOMETHING. Sent by the VibeServer shim on every automatic change:
    //     `steps` below the ceiling, `dir` (+1 up / −1 down), the applied `gain` and whether the
    //     AGC is what moved it. The client shows a short readout rather than the server's full
    //     sentence — see the status row.
    if (msg.type === 'lx') {
      this.callbacks.onLightning?.(Number(msg.rate) || 0, Number(msg.ago));
      return;
    }
    if (msg.type === 'ovl') {
      this.callbacks.onOverload?.({
        gainTenthDb: Number(msg.gain) || 0,
        dir: Number(msg.dir) || 0,
        agc: msg.agc === 1 || msg.agc === true,
      });
      return;
    }
    if (msg.type === 'hwinfo') {
      if (typeof msg.autoDs === 'boolean')
        this.callbacks.onHwDirectSampling?.(msg.autoDs, Number(msg.dsBelowHz) || 0, Number(msg.ds) || 0);
      // ★★★ RE-ASSERT OUR OWN TUNE WHEN THE RADIO ANNOUNCES ITSELF ON A *RETURNING* SOCKET.
      //     Backgrounding pauses the spectrum, and resuming opens a FRESH socket — a new session
      //     as far as the server is concerned, so it starts the listener at the radio's landing
      //     frequency. On the way back from the background the Airspy therefore jumped from the
      //     2 m band to broadcast FM, which is not a retune anyone asked for (Stuart, 2026-08-13).
      //     ★★ onopen re-asserts the VIEW (the zoom/centre) and always has — but the view is where
      //        the waterfall is LOOKING, not what the demodulator is TUNED to. The two were never
      //        the same thing, and only one of them was being restored.
      //     ★ Only on a socket that has been here before (`this.hadSession`): on a FIRST connect
      //       the server's landing frequency is exactly what should win, and SDRScreen's own
      //       last-tune restore runs then too — re-asserting here would fight it.
      /* ★★★ AND NEVER ON A SHARED DIAL — THIS IS THE SECOND WRITER, AND IT IS HALF THE BOUNCE.
       *     Everything above is right for a PER-LISTENER VFO, where the dial is ours and a resume
       *     must put the listener back. On a shared dial the opposite is true: the room owns the
       *     frequency, the landing IS the room, and re-asserting our own is the app arguing with
       *     the server.
       *  ★★★ AND hwinfo IS NOT RARE — IT ARRIVES ON EVERY RETUNE BY ANYONE. The server re-sends it
       *      to every spectrum client whenever the RF centre, tuner bandwidth or gain cap moves
       *      ("★ Only on a CHANGE. This runs on every retune", local_sdr_shim.cpp), so while one
       *      listener works the dial, every OTHER client gets a stream of them. That made this the
       *      engine of the whole fault, not an edge case on resume:
       *        1. someone tunes -> the dongle moves -> hwinfo to everybody
       *        2. each other client re-asserts ITS OWN stale frequency here
       *        3. tune() stamps lastLocalTuneAt, so `settled` (1500 ms) is false
       *        4. the stream of hwinfos keeps it false FOR EVER, so the adopt branch in the config
       *           handler can never run — the client is structurally deaf while anyone is tuning
       *        5. and step 2 drags the room back, which is the bounce
       *      Stuart named the symptom of (4) without knowing the cause: "I used to see user xx has
       *      tuned to xxxxMHz ... I've not seen that toast in a while" — that toast is fired FROM
       *      the adopt branch, so its disappearance was the branch going dead.
       *  ★★ Which is also the honest answer to "the web client just works": it has no equivalent
       *     of this block, so nothing ever stamps its settle timer and its adopt always runs.
       *  ★★★ TWO WRITERS IS WHY IT BOUNCES RATHER THAN SITTING STALE. Stuart, 2026-09-21: "its now
       *      fucking bouncing between the 2 ... the server tells the app hey I am on 103.0 now and
       *      the app sticks its fingers in its ears". A single stale writer sits on the wrong
       *      number quietly; two that each re-assert produce a ping-pong. The adopt path (see the
       *      config handler) was already fixed — it could not win while this kept shouting over it.
       *  ★★ THE FIX IS A SUBTRACTION, as it was the first time: ONE writer of the frequency, the
       *     server, and the app transmits only on a USER ACTION. ✗ Do not add a guard, a settle
       *     timer or a reconciliation pass here — each of those is a second route by another name.
       *  ★ The web client has no equivalent of this block at all, which is the whole reason it
       *    "just works": it has nothing that can assert a frequency nobody asked for. */
      if (this.hadSession && this.status.frequency > 0 && !this.sharedDial) {
        this.tune(this.status.frequency, this.status.mode, { recenter: true });
      } else if (this.hadSession && this.sharedDial) {
        this.dbg('shared dial — not re-asserting our tune on this hwinfo; the room owns the dial');
      }
      this.hadSession = true;
      // VibeServer sent the serving device's tuner gains + offered sample rates.
      // ★ The radio describes ITSELF. Everything the hardware panel offers is decided from
      // this — see RadioCaps. Forwarded verbatim rather than normalised: a driver we do not
      // know yet must still be able to say what it is.
      if (msg.radio && typeof msg.radio === 'object') {
        this.callbacks.onRadioCaps?.(msg.radio as RadioCaps);
      }
      if (typeof msg.adminSet === 'boolean') {
        this.adminSet = msg.adminSet;
        this.callbacks.onAdminState?.({ set: msg.adminSet, ok: msg.adminOk === true });
      // ★ Absent on a server too old to send it — the clock then keeps showing the phone's time
      //   rather than going blank.
      if (typeof msg.tzOffsetMin === 'number')
        this.callbacks.onServerClock?.(Number(msg.tzOffsetMin), String(msg.tzAbbr || ''));
      }
      // ★★★ THE SESSION CLOCK RIDES HWINFO, NOT CONFIG — the exact trap that made Jr's whole
      // session-limit feature silently never happen (jr_vibeserver_release_pass). The phone was
      // deriving its countdown ONLY from route params filled in by the server-list probe, so
      // connecting by direct IP — or to a server whose limit changed after the probe — showed no
      // countdown at all. Take the server's own number whenever it speaks.
      // ★★★ PASS A NEGATIVE THROUGH — IT IS THE ONLY WAY THE SERVER CAN SAY "NO DEADLINE". The
      // shim's occupantSecsLeft() returns -1 for an admin, for loopback and for an unlimited
      // server. Filtering on `> 0` meant the one message that could STOP a countdown was the one
      // message we dropped, so a clock started from stale route params ran on for ever with
      // nothing behind it. The web client already accepts >= 0 (spectrum.ts) — this end did not,
      // and a wire value has to be read the same way at both ends.
      if (typeof msg.sessionSecsLeft === 'number') {
        this.callbacks.onSessionWarning?.(Number(msg.sessionSecsLeft));
      }
      // ★★★ THE SERVER'S WORD ON ITS OWN DSP. These are sticky AND shared, so what this phone last
      //     asked for is irrelevant — only the radio knows, and a control that misreports it is
      //     worse than a missing one because nothing tells you to look. Absent = an older server
      //     that HAS the treatment but does not talk about it, so default ON rather than OFF.
      if (typeof msg.wsp === 'boolean' || typeof msg.ims === 'boolean'
          || typeof msg.ceq === 'boolean' || typeof msg.nb === 'boolean'
          || typeof msg.autobw === 'boolean') {
        this.callbacks.onFmDsp?.({
          wsp: msg.wsp !== false, ims: msg.ims !== false,
          ceq: msg.ceq !== false, nb: msg.nb !== false,
          autobw: typeof msg.autobw === 'boolean' ? msg.autobw : undefined,
          nbx: typeof msg.nbx === 'boolean' ? msg.nbx : undefined,
        });
      }
      if (Array.isArray(msg.gains)) this.callbacks.onHwGains?.(msg.gains as number[]);
      if (typeof msg.gainNow === 'number') this.callbacks.onHwGainNow?.(msg.gainNow);
      // ★ Only when the server states it — an older one does not, and 0 must not be read as
      //   "the radio is running at nothing".
      if (typeof msg.rateNow === 'number' && msg.rateNow > 0)
        this.callbacks.onHwRateNow?.(msg.rateNow);
      // ★ Whether the server's own AGC is running — see onHwAgc.
      // ★★ The server writes it as 1/0, not true/false — a boolean-only test never fired.
      if (typeof msg.agc === 'boolean' || typeof msg.agc === 'number')
        this.callbacks.onHwAgc?.(msg.agc === true || msg.agc === 1);
      if (Array.isArray(msg.rates)) this.callbacks.onHwRates?.(msg.rates as number[]);
      // >0 = the host PINNED the capture rate. The server ignores our sampleRate
      // messages outright, so the client hides the picker rather than offer a
      // control that silently does nothing.
      this.callbacks.onHwLockedRate?.(Number(msg.lockedRate) || 0);
      // ★ Same rule as lockedRate above: the server ENFORCES this, so a client that cannot see it
      //   offers a control whose every use is refused.
      this.callbacks.onHwAgcLocked?.(msg.agcLocked === true);
      // ★ Same rule again: the server refuses, so the client must not offer. Absent = not locked,
      //   which is what every server before this one meant.
      this.callbacks.onHwGainLocked?.(msg.gainLocked === true);
      this.callbacks.onHwIfGrFloor?.(
        typeof msg.ifGrFloor === 'number' ? (msg.ifGrFloor as number) : -1);
      // ★ -1 when absent, never 0 — an older server that does not send the field must read as
      //   "no limit", and 0 would read as "no gain allowed at all".
      this.callbacks.onHwGainCap?.(
        typeof msg.gainCap === 'number' ? (msg.gainCap as number) : -1);
      /* ★★★ MODES AND DECODERS THE OWNER HAS SWITCHED OFF. Same rule as the locks above and for
       *  the same reason: the server REFUSES these, so the app must not offer them. Stuart's
       *  cases are an RSP1B locked to HF (where WFM and Adv RDS can do nothing) and an XCover set
       *  up for AM/FM only. Absent = nothing blocked, which is what every server before this one
       *  meant — never "block everything". */
      if (Array.isArray(msg.blocked))
        this.callbacks.onHwBlockedModes?.((msg.blocked as string[]).map(x => String(x).toLowerCase()));
      // ★ Only when the server actually states it — an older server has no such filter and must
      //   not be read as "wide open", which is a claim about hardware we have not been told about.
      if (msg.tunerBw !== undefined)
        this.callbacks.onHwTunerBw?.(Number(msg.tunerBw) || 0, msg.tunerBwAuto === true);
      if (msg.rfCentre !== undefined || msg.lockedCentre !== undefined)
        this.callbacks.onRfCentre?.(Number(msg.rfCentre) || 0, Number(msg.lockedCentre) || 0);
      // ★ Only the VibeServer shim sends hwinfo, and it carries the owner's FRAME-RATE CEILING.
      // Feed it to the controller: without it we would ask for the ladder's top rate, receive the
      // permitted one, and read the difference as a failing link — stepping down forever chasing a
      // limit that can never be reached. (The server's own comment makes the same point.)
      // ★★ REBUILD THE CONTROLLER ON THE RIGHT LADDER.
      //
      // startLinkManager() runs in ws.onopen, but `hwinfo` is a MESSAGE — it can
      // only arrive afterwards. So the controller was ALWAYS constructed with
      // isVibeServer false and took LADDERS.ubersdr [10, 5, 3.3], and it early-
      // returns if the timer already exists, so it never got a second chance.
      //
      // Once hwinfo landed, apply() switched to the VibeServer lever (fftRate)
      // but kept UberSDR's RUNGS — so the phone asked a 20 fps VibeServer for
      // 10 / 5 / 3.3 and could never request full rate. The browser, which just
      // asks for min(displayRate, serverCap), sat at 83 KB/s on the same server
      // while the phone crawled. (Stuart spotted this: "is auto link management
      // set to UberSDR standards?")
      //
      // ★ Deciding a backend-specific policy from state that arrives LATER than
      // the decision is the bug; rebuilding when the truth lands is the fix.
      /* ★★★ THE REBUILD THAT USED TO LIVE HERE IS GONE, AND SO IS THE RACE IT PATCHED. hwinfo set
       *  isVibeServer=true, discovered the link controller had been built on UberSDR's ladder, tore
       *  it down and built it again. None of that is reachable now: a VibeServerClient was
       *  constructed as one, so startLinkManager() took the right ladder the first time.
       *  ★ The comment above is kept because its diagnosis is the reason this class was split —
       *    "deciding a backend-specific policy from state that arrives LATER than the decision is
       *    the bug". The fix is no longer to rebuild when the truth lands; it is to know at
       *    construction. */
      this.serverMaxFps = Number(msg.maxFftRate) || 0;
      if (this.serverMaxFps > 0) this.link?.applyServerCeiling(this.serverMaxFps);
      return;
    }
    if (msg.type === 'config') {
      this._restoreViewOnConfig(msg);
      /* ★★★ THE RADIO'S IF FILTER AND GAIN, FROM THE SAME MESSAGE AS ITS FREQUENCY AND ZOOM —
       *  the server's full state in one place (2026-09-22). hwinfo still carries them for older
       *  servers; these fields are absent there, so nothing changes against one. */
      if (typeof msg.ifBw === 'number')
        this.callbacks.onHwTunerBw?.(msg.ifBw, msg.ifAuto === true);
      if (typeof msg.gainNow === 'number' && msg.gainNow >= 0)
        this.callbacks.onHwGainNow?.(msg.gainNow);
      if (typeof msg.agc === 'number' || typeof msg.agc === 'boolean')
        this.callbacks.onHwAgc?.(msg.agc === true || msg.agc === 1);      // Local hardware advertises its full span here → cap zoom-out to it.
      if (typeof msg.maxBandwidth === 'number') this.maxSpanHz = msg.maxBandwidth;
      if (typeof msg.centerFreq   === 'number') this.status.centerHz     = msg.centerFreq;
      if (typeof msg.binBandwidth === 'number') this.status.binBandwidth = msg.binBandwidth;
      if (typeof msg.binCount     === 'number') {
        this.status.binCount = msg.binCount;
        if (this.bins.length !== msg.binCount) this.bins = new Float32Array(msg.binCount);
      }
      this.status.bwHz = typeof msg.totalBandwidth === 'number'
        ? msg.totalBandwidth
        : this.status.binBandwidth * this.status.binCount;
      this.dbg(`config: ${this.status.binCount} bins @ ${this.status.binBandwidth} Hz ` +
               `centre ${this.status.centerHz} bw ${this.status.bwHz}`);
      // ★★★ THE SERVER HAS JUST TOLD US WHERE IT ACTUALLY PUT US. If we connected to restore a
      //     remembered tune, this is the moment to find out whether it took — and on a new session
      //     it will NOT have, because the server lands newcomers on the owner's chosen frequency
      //     during the handshake. Without this the app showed 106.8 MHz WFM while the radio sat in
      //     the AM broadcast band playing Radio Caroline, and one manual tune "fixed" it.
      // ★★ NOT ON A LOCKED RECEIVER. The comment on the server's own config says it: on a shared
      //    radio there is one VFO and "a joiner must adopt it rather than impose one — otherwise
      //    the last person to connect silently retunes the radio for everybody already listening".
      //    So we adopt there, and only assert where the VFO is genuinely ours.
      // ★ Once. Cleared either way, so a server that lands us somewhere on purpose is argued with
      //   exactly once and never again.
      if (Number.isFinite(Number(msg.vfo)) && Number(msg.vfo) > 0) this.lastServerVfo = Number(msg.vfo);
      /* ★★★ TELL NATIVE WHERE THE DIAL IS — SILENTLY. This is the server's word, so it is the ONE
       *  thing entitled to write native's `currentFreq`, and it must not reach the wire.
       *
       *  ★★★ WHY HERE AND NOT IN NATIVE'S OWN HANDLER: the server sends `config` to SPECTRUM clients
       *      only (`for (auto& c : allSpecClients()) sendConfig(c)` — local_sdr_shim.cpp). The audio
       *      socket, which native owns, never receives one, so native cannot learn this by itself.
       *  ★★ Native used to learn the dial ONLY from our own sendTuneCommand, so it went stale the
       *     moment anyone else tuned a shared receiver — and it then re-imposed that stale value on
       *     every reconnect and engine restart. That is the bug this whole change removes.
       *  ★ Unconditional, outside the "somebody else moved it" branch below: native wants the dial
       *    after OUR tunes too (the server's confirmation is the authoritative value), and it is a
       *    no-op when nothing changed. Cheap enough to do on every config. */
      {
        const sv = Number(msg.vfo);
        if (Number.isFinite(sv) && sv > 0) {
          try {
            VibePowerModule?.noteServerFreq?.(
              sv, typeof msg.mode === 'string' && msg.mode ? msg.mode : this.status.mode);
          } catch {}
        }
      }
      // ★★★ FOLLOW THE DIAL WHEN SOMEBODY ELSE TURNS IT. On a shared-VFO receiver the frequency
      //     moves because another listener moved it, and this client adopted the server's vfo only
      //     during the first-connect negotiation below — so the audio followed and the readout,
      //     the mode and everything keyed off them did not. The browser had the identical gap
      //     (fixed 2026-08-20); this is the same fix in the app.
      //  ★★ AND IT IS WHY RadioDNS LOGOS WERE MISSING on the phone: the lookup is keyed on the
      //     frequency (with the PI), so a stale readout asks about a station nobody is listening
      //     to and finds nothing. Stuart spotted the pair — "the moto doesnt get the RadioDNS
      //     icons which is odd but could be related to the wrong frequency". It was.
      //  ★ Guarded against our own echo: the server confirms every tune WE send as a config, and
      //    adopting inside that window would fight a drag or a held step key.
      {
        const sv = Number(msg.vfo);
        const bw = Math.abs(Number(this.status.bandwidthHigh) - Number(this.status.bandwidthLow)) || 3000;
        const settled = Date.now() - this.lastLocalTuneAt > 1500;
        // ★★★ ADOPT EVERY MOVE, HOWEVER SMALL — the READOUT is not a jitter problem. This was
        //     gated on the move exceeding the demodulator's bandwidth, which on WFM is ~200 kHz,
        //     so 100 kHz steps never qualified: the phone followed only once several steps had
        //     accumulated past the threshold, and settled wherever the arithmetic left it.
        //     Stuart, watching both screens: "on the webclient i am on 96.6 but on the moto its
        //     on 96.4" (2026-08-20).
        //  ★★ The BANDWIDTH threshold still governs the expensive half — flushing audio and
        //     recentring the view — because doing those on every 100 Hz nudge of somebody else's
        //     drum would stutter for everyone watching. Two questions, two thresholds; they were
        //     one, and the cheap one inherited the costly one's caution.
        const moved = Math.abs(sv - Number(this.status.frequency));
        /* ★★★ THE ONE CLAUSE THE WEB CLIENT DOES NOT HAVE — AND IT IS THE BUG.
         *
         *  Compared clause by clause against spectrum.ts, which does this correctly:
         *      web:  if (settled && cfg.serverVfo && moved > 100)
         *      app:  if (!this.wantTune && settled && ... && moved > 100)
         *  Same 1500 ms settle, same 100 Hz floor, same view-follow below. The ONLY difference is
         *  `!this.wantTune`, and it is why an app can sit on a stale dial for ever while the
         *  browser beside it tracks perfectly.
         *
         *  ★★★ HOW IT BITES: the adopt runs BEFORE the wantTune block below, so the config that
         *      carries the remembered tune is skipped here. The server sends `config` on connect
         *      and on CHANGE — so if nothing moves the dial afterwards, the only config that
         *      client will ever see was the one it threw away. Measured 2026-09-21 on Stuey3D
         *      SonyTV: server on 99.7 (moved there from the iPhone), Mac reading 97.8 — a
         *      frequency not even on the Mac's own axis.
         *  ★★ AND THERE IS NOTHING FOR THE GUARD TO PROTECT ON A SHARED DIAL: the wantTune block
         *     below already refuses to assert a remembered tune there ("NEVER ON A SHARED DIAL"),
         *     so blocking the adopt buys a stale readout and prevents nothing.
         *  ★ Stuart, stating the model: "The apps should simply mirror the server ... they should
         *    not fight the server and should not be holding the tuning." On a shared dial the
         *    server is the authority; a client that can disagree with it is the fault.
         *  ✗ Left in place for a PER-LISTENER VFO, where the dial genuinely is ours and a
         *    remembered tune is the listener's own answer — that is the case the guard was for. */
        const sharedNow = msg.shared === true || this.sharedDial;
        if (sharedNow && this.wantTune) {
          this.dbg('shared dial — the server owns this VFO; dropping the remembered tune');
          this.wantTune = null;
        }
        /* ★★★ SAY WHAT WAS DECIDED, WHERE A RELEASE BUILD CAN BE READ. dbg() goes to onDbg, which
         *     nothing in the UI consumes, so on a device this decision has always been invisible —
         *     and it is THE decision the shared dial turns on. Recorded into the diagnostics ring
         *     the About screen already exports, so a reproduction can be read instead of guessed
         *     at. See protocolLog.noteDecision. */
        const wouldAdopt = (sharedNow || !this.wantTune) && settled
                        && Number.isFinite(sv) && sv > 0 && moved > 100;
        if (!wouldAdopt && Number.isFinite(sv) && sv > 0 && moved > 100) {
          noteDecision('vibe', `dial NOT adopted ${sv} (moved ${Math.round(moved)})`
            + ` shared=${sharedNow ? 1 : 0} settled=${settled ? 1 : 0}`
            + ` wantTune=${this.wantTune ? 1 : 0} msgShared=${msg.shared === true ? 1 : 0}`
            + ` dialShared=${this.sharedDial ? 1 : 0}`);
        }
        if (wouldAdopt) {
          noteDecision('vibe', `dial adopted ${sv} (moved ${Math.round(moved)})`);
          this.dbg(`another listener moved the dial to ${sv}`);
          this.callbacks.onDialMoved?.(sv, typeof msg.mode === 'string' ? msg.mode : undefined);
          this.status.frequency = sv;
          if (typeof msg.mode === 'string' && msg.mode) this._adoptMode(msg.mode as SDRMode);
          // ★★★ AND BRING THE VIEW WITH IT. Adopting the frequency alone leaves the waterfall
          //     pointed where the radio ISN'T — the server is capturing around the new dial, so
          //     the old view has no data behind it and goes BLACK. Stuart hit exactly that from
          //     the phone (2026-08-20): "it made the spectrum go black and the audio stayed on
          //     greatest hits". A local tune already does this (see tune()); a remote one must.
          //  ★ Only when this client is following the VFO. Somebody who has deliberately panned
          //    away to watch another part of the band chose that view, and yanking it back on
          //    every move somebody else makes would be the opposite of helpful.
          // ★★★ AND WHEN SMALL STEPS ADD UP: an rtl_tcp app walking the dial in 100 kHz steps on
          //     WFM never crossed one bandwidth, so the view never followed and the VFO walked to
          //     the edge of a LOCKed view (web client, 2026-09-10). Also recentre once the dial
          //     has drifted more than a tenth of the view from its middle.
          const bbNow = this.view.binBandwidth || this.status.binBandwidth;
          const viewSpan = bbNow ? bbNow * (this.status.binCount || 4096) : 0;
          const drifted = viewSpan > 0 && Math.abs(sv - this.view.centerHz) > viewSpan * 0.1;
          if (this.followVfo && (moved > bw || drifted)) {
            const bb = this.view.binBandwidth || this.status.binBandwidth;
            if (bb) this.zoom(sv, bb); else this.pan(sv);
          }
          this.callbacks.onStatus({ ...this.status });
        }
      }
      if (this.wantTune) {
        const want = this.wantTune;
        this.wantTune = null;
        const serverVfo = Number(msg.vfo);
        // ★★★ CAN THE MEMORY EVEN BE HONOURED? A receiver with a LOCKED WINDOW still allows free
        //     tuning INSIDE it, so a remembered frequency in range is perfectly reachable and
        //     should be restored. One outside the window is not, and insisting on it would leave
        //     the dial showing a frequency the radio can never reach — which is the very fault
        //     this code exists to end, in a new costume.
        // ★★ SO: IN RANGE, ASSERT. OUT OF RANGE, TAKE THE LANDING FREQUENCY AND MODE, which is the
        //    owner's answer for "a listener who cannot go where they wanted". Stuart's case,
        //    exactly: "if I change the unlocked single-user radio to a fixed range with multiple
        //    users, and I connect and cannot tune to the last used memory, then I should default
        //    to the landing frequency and mode of the radio" (2026-08-15).
        // ★ Unlocked receivers have no window to be outside of, so memory always wins there —
        //   which is the two single-user radios, and the common case.
        const span = Number(this.status.bwHz) || 0;
        const centre = Number(this.status.centerHz) || 0;
        const windowed = msg.locked === true && span > 0 && centre > 0;
        const reachable = !windowed
          || Math.abs(want.frequency - centre) <= span / 2;
        /* ★★★ AND NEVER ON A SHARED DIAL, WHOEVER ELSE IS ALREADY ON IT.
         *
         *  Everything below assumes the dial is OURS to put back where we left it. On a shared
         *  receiver it is not: the frequency is the room's, a joining listener arrives into
         *  somebody else's programme, and asserting a remembered one drags them off it. The note
         *  further down already saw the danger — "it would fight a shared VFO for ever" — and
         *  guarded only the REPEAT, not the first assertion.
         *
         *  ★★ THE ASSUMPTION WAS WRITTEN DOWN AND IT WAS WRONG: "Unlocked receivers have no window
         *     to be outside of, so memory always wins there — which is the two single-user radios,
         *     and the common case." Unlocked does NOT mean single-user. Stuart's XCover is unlocked
         *     AND shared (ten listeners, dial mode "open"), so the one rule that was never meant to
         *     apply to a shared dial applied to it on every connect.
         *
         *  ★★ AND THIS IS THE "RANDOM" DISTORTION ON CONNECT. Stuart, 2026-09-11: "it will look like
         *     its sat on a signal and the correct demodulator set but the tuning will be off broken
         *     up and distorted, if i tune away then back it all lines up perfectly … this is on a
         *     shared VFO radio which should leave the tuning where it was left so new connecting
         *     clients dont trigger a retune." Random because it only bites when the remembered
         *     frequency differs from where the room is sitting — join on the same station and
         *     nothing happens at all.
         *
         *  ★ `shared` is in THIS message, beside `locked`, and always has been — the config names
         *    both and only one was read. No new field, no ordering question, no guess from the
         *    listener count. */
        if (!reachable || msg.shared === true) {
          // Adopt what the server actually did, so the readout stops claiming otherwise.
          this.dbg(msg.shared === true
            ? `shared dial — adopting the room's ${serverVfo}, not the remembered ${want.frequency}`
            : `remembered ${want.frequency} is outside the locked window — keeping the landing`);
          if (Number.isFinite(serverVfo) && serverVfo > 0) this.status.frequency = serverVfo;
          if (typeof msg.mode === 'string' && msg.mode) this._adoptMode(msg.mode as SDRMode);
          this.callbacks.onStatus({ ...this.status });
        } else if (Number.isFinite(serverVfo) && Math.abs(serverVfo - want.frequency) > 500) {
          this.dbg(`landing put us on ${serverVfo}; re-asserting remembered ${want.frequency}`);
          /* ★★★ SAY THAT THIS IS A RESTORE, BEFORE MAKING IT. The audio socket seals itself on any
           *  dial that might be shared and breaks the seal only on a value it has not seen — and
           *  the value we are about to send is exactly the one it opened with, so without this the
           *  re-assert is indistinguishable from the socket restating itself and is dropped. The
           *  note above says "deferring costs nothing"; it cost the whole restore. */
          this.callbacks.onRestoredTune?.(want.frequency, want.mode);
          this.tune(want.frequency, want.mode, { recenter: true });
          // ★★★ AND AGAIN A MOMENT LATER, BECAUSE THE AUDIO PATH IS NOT OURS TO SEE. tune() reaches
          //     the radio through VibePowerModule's native audio socket, and on this server the
          //     AUDIO socket arrives FIRST and is what triggers the landing — so at the instant the
          //     spectrum config lands, the native side has just been retuned underneath us and may
          //     not carry ours. The log showed exactly that: audio chain wfm, then the landing to
          //     am/648, then nothing for twenty-three seconds. The listener saw FM on the dial and
          //     heard Radio Caroline (Stuart, 2026-08-15).
          // ★★ ONE repeat, not a loop. If the second one does not take either, something is wrong
          //    that retrying cannot fix, and a client that keeps shoving a frequency at a server is
          //    worse than one that gets it wrong once — it would fight a shared VFO for ever.
          // ★ Judged on the SERVER's reported vfo, never on our own status — tune() sets that
          //   itself, so asking it whether the tune took is asking the question of the answer.
          setTimeout(() => {
            if (this.destroyed) return;
            if (Math.abs(this.lastServerVfo - want.frequency) > 500) {
              this.dbg(`still on ${this.lastServerVfo}; asserting ${want.frequency} once more`);
              this.callbacks.onRestoredTune?.(want.frequency, want.mode);
              this.tune(want.frequency, want.mode, { recenter: true });
            }
          }, 2500);
        }
      }
      // binBandwidth change ⇒ the session may have migrated shared↔private,
      // which resets the server-side poll divisor — re-assert ours.
      if (this.status.binBandwidth !== this.lastRateBinBw) {
        this.lastRateBinBw = this.status.binBandwidth;
        if (this.rateDivisor > 1 && this.spectrumWs?.readyState === WebSocket.OPEN) {
          this.spectrumWs.send(JSON.stringify({ type: 'set_rate', divisor: this.rateDivisor }));
        }
      }
      // In flight: echoes of intermediate requests. Internal state above must
      // track them (frames are ordered after their config on the same TCP
      // stream, so decode geometry stays consistent), but they must NOT drive
      // the UI — the settle timer adopts the final state once sends go quiet.
      if (this._inFlight()) return;
      // UNSOLICITED geometry change (no request of ours in flight): a session
      // resurrection/reconnect can briefly put the server back at full-span
      // defaults — emitting that directly flashes the band plan/ticks to
      // 0–30MHz for a frame (the idle flicker bug). This phone is the only
      // client of its session, so the server losing our geometry is always a
      // reset, never another user's tune: keep the UI pinned and RE-ASSERT our
      // view (idempotent if the server already has it). The settle timer then
      // adopts whatever the server finally acks.
      const v = this.view;
      // A remote local shim (VibeServer) recentres its capture when the VFO tunes
      // out of the captured band and pushes a fresh config — same span, new
      // centre. That is a LEGITIMATE follow, not a session reset, so adopt it
      // rather than re-asserting our stale centre (which would snap the waterfall
      // back). Distinguish it by binBandwidth being unchanged (a reset reverts to
      // full-span defaults, changing binBandwidth).
      const sameSpan = v.binBandwidth > 0 &&
        Math.abs(this.status.binBandwidth - v.binBandwidth) <= v.binBandwidth * 1e-6;
      if (this.isLocal && sameSpan && Math.abs(this.status.centerHz - v.centerHz) > 1) {
        this.view.centerHz     = this.status.centerHz;
        this.view.binBandwidth = this.status.binBandwidth;
        this.callbacks.onStatus({ ...this.status });
        return;
      }
      const unsolicitedChange = v.binBandwidth > 0 &&
        (Math.abs(this.status.centerHz - v.centerHz) > 1 ||
         Math.abs(this.status.binBandwidth - v.binBandwidth) > v.binBandwidth * 1e-6);
      /* ★★★ ON A SHARED DIAL, "UNSOLICITED" IS JUST SOMEBODY ELSE (Stuart, 2026-09-20). Re-asserting our own
       *  window is right on a PRIVATE receiver, where a centre we did not ask for means something went wrong.
       *  On a shared one it is exactly backwards: another listener tuned, and this drags the radio back to
       *  where WE were. Two app clients then fight over the capture — he tuned the iPhone to 99.7 MHz, the Mac
       *  hauled it back to 96.1, and both waterfalls emptied while the RDS followed the station nobody could
       *  see. The server says as much in its own config: "a joiner must adopt it rather than impose one".
       *  ★★ So here we adopt: fall through to the lines below, which take the server's centre as ours. */
      if (unsolicitedChange && !this.sharedDial) {
        this.dbg(`unsolicited config (centre ${this.status.centerHz} bb ${this.status.binBandwidth}) — re-asserting view`);
        this._sendView(Math.round(v.centerHz), v.binBandwidth);
        return;
      }
      if (unsolicitedChange) this.dbg(`shared dial: adopting the server's centre ${this.status.centerHz}`);
      this.view.centerHz     = this.status.centerHz;
      this.view.binBandwidth = this.status.binBandwidth;
      this.callbacks.onStatus({ ...this.status });
      return;
    }
    // ★★★ EVERYTHING ELSE FALLS THROUGH TO HERE — AND USED TO DO SO IN SILENCE.
    //
    // A server adds a message, we ignore it, and the symptom surfaces somewhere unrelated with no
    // evidence attached. That has now cost us THREE separate investigations:
    //   • UberSDR's "are you still there?" LIVENESS PROBE — never handled, so the server gave up
    //     and the drop surfaced as a bare "disconnected". A failure message for a question.
    //   • KiwiSDR booting some clients after ~30 s — still unexplained, and the only remaining
    //     route to it is seeing what the server actually said.
    //   • Kiwi's rn/rt countdown, which we only found by reading their JavaScript.
    // Each was inferred over hours; ONE LOG LINE would have named them in minutes.
    //
    // ★ Cheap on purpose: __DEV__-gated console, but ALWAYS through onDbg, so it reaches the
    // in-app debug surface on a release build where the real reports come from. Truncated because
    // an unknown message may be large, and the TYPE is the part that matters.
    // ★ Server-specific messages (DAB on a VibeServer, and whatever ADS-B/AIS/ACARS/DRM bring)
    //   get their chance BEFORE the unhandled log, or every one of them reports itself as a
    //   protocol fault on the server that does speak them.
    if (this.handleServerMessage(msg)) return;

    if (typeof msg.type === 'string') {
      /* ★★★ BUILD THE LINE ONCE PER TYPE. `sig` and `adc` arrive twenty times a second EACH, and
       *     this ran JSON.stringify on every one of them — on the JS thread, for ever, to produce
       *     a line identical to the last forty thousand. The buffer already tallies repeats (see
       *     noteUnhandled), so the only thing left to save is the stringify itself, and it is the
       *     expensive half.
       *  ★★ THE COUNT STILL RISES, so "this arrives constantly" is still visible — that fact is
       *     diagnostic and the old design destroyed it by drowning in it.
       *  ★ Per socket generation: cleared on reconnect, so a type that appears only after a
       *    reconnect is still caught the first time it does. */
      if (this._loggedUnhandled.has(msg.type)) { noteUnhandled('ubersdr', `unhandled "${msg.type}"`); return; }
      this._loggedUnhandled.add(msg.type);
      const line = `unhandled "${msg.type}": ${JSON.stringify(msg).slice(0, 140)}`;
      this.dbg(line);
      // ★ ALSO into the ring buffer — dbg() reaches nothing on a release build, and release builds
      // are where the reports come from. See services/protocolLog.ts.
      noteUnhandled('ubersdr', line);
    }
  }

  /** onclose fired — the honest case, where the socket told us it died.
   *
   *  Still goes through _reopenSequenced: the close may have been an outage long
   *  enough for the server to reap the session, and reopening into a reaped
   *  session is Hole 2. No rate limit here (a close is self-limiting), but the
   *  reconnecting flag is raised so the watch shows a recovery rather than a
   *  black screen. */
  /** ★ Types already written to the protocol log — see the note where it is used. */
  private _loggedUnhandled = new Set<string>();

  private _scheduleReconnect() {
    if (this.destroyed || this.pausedByApp) return;
    if (this.refused) return;        // a refusal is final until the user acts
    this._setReconnecting(true);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      if (this.destroyed || this.pausedByApp) return;
      void this._reopenSequenced('onclose');
    }, 3000);
  }
}
