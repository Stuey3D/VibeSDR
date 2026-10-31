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
import { DabAssembler } from './dabAssemble';
import { parseAdsbMessage } from './adsbTypes';
import { AdsbInfoStore } from './adsbInfo';
import { ismSanitise } from './ism';
import { ndbSanitise, parseNdbFound, parseNdbStats } from './ndb';
import { drmSanitise } from './drm';   // ★ V12 DRM
import { parseAmSync } from './amSync';   // ★ V12 synchronous AM
import { parseBwAuto } from './bwAuto';   // ★★ V12 the AUTO key by the passband sliders
import { aisSanitise, aisTrailSanitise } from './tracks';   // ★ V12 AIS
import { parseDabScan } from './dabBandScan';
import type { WholeKind } from './wholeModeResume';   // ★ V12: coming back to a whole-radio mode

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
    if (this.askBlocked()) return;   // ★★ ask before tuning — the multiplex is everybody's on a shared dial
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

  private dabAsm = new DabAssembler();
  /** ★ The Signal pane is open (dab=2): the constellation + impulse response come with each report only while it is.
   *  Remembered, so a reconnect restates it. */
  private dabScopesOn = false;
  private dabScopesSaidAt = 0;
  dabScopes(on: boolean) {
    this.dabScopesOn = on;
    this.dabScopesSaidAt = Date.now();
    this.sendSpectrum({ type: 'dab_scopes', on: on ? 1 : 0 });
  }

  /** Switch service WITHIN the tuned multiplex — no retune, no re-acquire. */
  dabService(sid: number) {
    if (this.askBlocked()) return;   // ★★ ask before tuning — the station is everybody's on a shared dial
    holdNativeHealing(VibeServerClient.DAB_SERVICE_HOLD_MS, `DAB service ${sid}`);
    this.sendSpectrum({ type: 'dab_service', sid });
  }

  /** ★★ V12: start / stop a DAB band scan. The server decides who may (a single-user radio, or the admin) and answers in
   *  words; a tune while it runs ends it there (the server leaves the dial to the person tuning). */
  dabScan(on: boolean) { this.sendSpectrum({ type: 'dab_scan', on: on ? 1 : 0 }); }

  // ─── ADS-B (V12, docs/v12/ADSB.md) ───────────────────────────────────────────────────────────
  //
  // ★★★ THE SAME SHAPE AS DAB, ON PURPOSE. The server takes the whole radio to 1090 MHz, decodes on
  //     its own thread and reports once a second; the client never tunes, so `adsbHeld` locks
  //     tune/zoom/pan/resetView through the same single reader dabHeld uses (SdrWsClient). The hold
  //     is set and cleared with the REQUEST, for the reason dab() gives.
  // ★★ DAB ↔ ADS-B IS THE SERVER'S JOB: entering one leaves the other through its own door, and
  //    every listener hears `dab_off` / `adsb_off` first. Nothing here second-guesses that.

  /** ★★★ Reports that arrive after our own `adsb off` are ghosts — the DabExitGuard rule, unchanged
   *  (a report already on the wire re-opened the box over the receiver we had just been given back). */
  private adsbExit = new DabExitGuard();
  /** ★ What each aircraft is (adsb_info), per hex, merged into every report — adsbInfo.ts. */
  private adsbInfo = new AdsbInfoStore();
  /** Date.now() of the last report — see adsbSilentMs. */
  private adsbLastAt = 0;
  /** ★ When the self-heal was last held for ADS-B — see the `adsb` case. */
  private adsbHealHeldAt = 0;
  static readonly ADSB_ENTER_HOLD_MS = 15_000;
  static readonly ADSB_QUIET_HOLD_MS = 6_000;

  adsb(on: boolean) {
    if (this.askBlocked()) return;   // ★★ ask before tuning — it takes everybody's radio to 1090
    /* ★★★ THERE IS NO AUDIO IN ADS-B (the server runs the receiver spectrum-only), and the native
     *  self-heal reads a stream with no audio as a stall to repair. Held here for the entry, and
     *  kept held by the reports while the mode runs (see the `adsb` case). */
    holdNativeHealing(VibeServerClient.ADSB_ENTER_HOLD_MS, on ? 'ADS-B on' : 'ADS-B off');
    if (on) { this.adsbExit.entered(); this.cancelPacedTune(); }
    else this.adsbExit.left(Date.now());
    this.adsbHeld = on;
    if (on) this.adsbLastAt = Date.now();   // ★ the silence clock starts at the ask, not at 1970
    this.sendSpectrum({ type: 'adsb', on: on ? 1 : 0 });
  }

  /** ★ How long since the last report while we believe the receiver is in ADS-B (0 when it is not).
   *  A receiver that LEFT ADS-B while we were away (the last listener's socket closed and the
   *  server gave the radio back, telling nobody) would otherwise leave the dial locked here for
   *  ever — so the screen asks, and a long silence lets the hold go (adsbForget). */
  adsbSilentMs(now = Date.now()): number { return this.adsbHeld ? now - this.adsbLastAt : 0; }
  /** Drop the hold WITHOUT telling the server — it is not in ADS-B; we were merely told nothing. */
  adsbForget() { this.adsbHeld = false; }

  /* ── ★★★ COMING BACK TO A WHOLE-RADIO MODE (wholeModeResume.ts — Stuart, 2026-10-27: the ADS-B / NDB map "dropped back
   *  to a waterfall view on the previous frequency" after a switch to another app). ── */
  /** Date.now() of the last report of each whole-radio mode — what the screen's lapsed-hold check reads (holdLapsed). */
  private wholeLastAt: Record<WholeKind, number> = { adsb: 0, ndb: 0, ism: 0, ais: 0 };
  wholeReportAt(kind: WholeKind): number { return kind === 'adsb' ? this.adsbLastAt : this.wholeLastAt[kind]; }
  /** ★★ ASK for a whole-radio mode back after the server left it while we were away. A QUESTION, not an entry: the server
   *  grants it only if nobody else is on the radio and nobody has tuned it since (shim modeResume), and answers
   *  `mode_resume` either way. @return false if it could not be asked (no socket, or the dial is somebody else's). */
  modeResume(kind: WholeKind): boolean {
    if (this.askBlocked()) return false;   // ★★ ask before tuning — it would move everybody's radio
    return this.sendSpectrum({ type: 'mode_resume', mode: kind });
  }
  /** Drop a whole-radio hold WITHOUT telling the server (adsbForget's rule) — it has left the mode; nobody told us. */
  wholeForget(kind: WholeKind) {
    if (kind === 'adsb') this.adsbHeld = false;
    else if (kind === 'ndb') this.ndbHeld = false;
    else if (kind === 'ism') this.ismHeld = false;
    else this.aisHeld = false;
  }

  /* ── ISM (V12, docs/v12/ISM-DECODER.md) — ADS-B's pattern: a mode entered and left by name, the server owns the band
   *  (the shared-dial contract: a press sends a step, the next report says where the radio is). ── */
  /** ★ Reports after our own `ism off` are ghosts (DabExitGuard, as for DAB and ADS-B). */
  private ismExit = new DabExitGuard();
  private ismHealHeldAt = 0;
  static readonly ISM_HOLD_MS = 8_000;

  ism(on: boolean, o: { band?: string; step?: number; hz?: number } = {}) {
    if (this.askBlocked()) return;   // ★★ ask before tuning — it takes everybody's radio to the band
    /* ★★ No audio on a band (the server sends none in ISM, as in ADS-B): the native self-heal would read the silence as a
     *  stall. Held for the entry and a step, and by the reports while the mode runs. */
    holdNativeHealing(VibeServerClient.ISM_HOLD_MS, on ? 'ISM on' : 'ISM off');
    if (on) { this.ismExit.entered(); this.cancelPacedTune(); }
    else this.ismExit.left(Date.now());
    this.ismHeld = on;
    if (on) this.wholeLastAt.ism = Date.now();   // ★ the silence clock starts at the ask (wholeModeResume.holdLapsed)
    const m: Record<string, unknown> = { type: 'ism', on: on ? 1 : 0 };
    if (on && o.band) m.band = o.band;
    if (on && o.step) m.step = o.step > 0 ? 1 : -1;
    if (on && o.hz && o.hz > 0) m.hz = Math.round(o.hz);
    this.sendSpectrum(m);
  }

  /* ── NDB (V12, docs/v12/NDB-SKIMMER.md) — ADS-B's pattern: a whole-radio mode entered and left by name; the server puts
   *  the capture on the LF beacon band, skims every carrier and sends a report a second. No audio (spectrum-only). ── */
  /** ★ Reports after our own `ndb off` are ghosts (DabExitGuard, as for DAB, ADS-B and ISM). */
  private ndbExit = new DabExitGuard();
  private ndbHealHeldAt = 0;
  static readonly NDB_HOLD_MS = 8_000;

  ndb(on: boolean) {
    if (this.askBlocked()) return;   // ★★ ask before tuning — it takes everybody's radio to the beacon band
    holdNativeHealing(VibeServerClient.NDB_HOLD_MS, on ? 'NDB on' : 'NDB off');
    if (on) { this.ndbExit.entered(); this.cancelPacedTune(); }
    else this.ndbExit.left(Date.now());
    this.ndbHeld = on;
    if (on) this.wholeLastAt.ndb = Date.now();   // ★ the silence clock starts at the ask (wholeModeResume.holdLapsed)
    this.sendSpectrum({ type: 'ndb', on: on ? 1 : 0 });
  }
  /** Find NDB — published beacons nearest the receiver first (in or out of the mode); answered by `ndb_found`. */
  ndbFind(q: string, limit = 60) { this.sendSpectrum({ type: 'ndb_find', q: String(q).slice(0, 40), limit }); }
  /** The reception stats (in memory on the server, 7 days); answered by `ndb_stats`. */
  ndbStats() { this.sendSpectrum({ type: 'ndb_stats' }); }
  /* ── V12 AIS (docs/v12/AIS-APRS.md) — ADS-B's pattern: a whole-radio mode entered and left by name; the server puts the
   *  radio on 162.000 MHz, decodes both channels and ignores tune / zoom / mode while it runs. No audio is sent. ── */
  /** ★ Reports after our own `ais off` are ghosts (DabExitGuard, as for DAB, ADS-B and ISM). */
  private aisExit = new DabExitGuard();
  private aisHealHeldAt = 0;
  static readonly AIS_HOLD_MS = 8_000;

  ais(on: boolean) {
    if (this.askBlocked()) return;   // ★★ ask before tuning — it takes everybody's radio to 162 MHz
    holdNativeHealing(VibeServerClient.AIS_HOLD_MS, on ? 'AIS on' : 'AIS off');
    if (on) { this.aisExit.entered(); this.cancelPacedTune(); }
    else this.aisExit.left(Date.now());
    this.aisHeld = on;
    if (on) this.wholeLastAt.ais = Date.now();   // ★ the silence clock starts at the ask (wholeModeResume.holdLapsed)
    this.sendSpectrum({ type: 'ais', on: on ? 1 : 0 });
  }
  /** One ship's track (the map's card) — a read, answered to the asker only (`ais_trail`). */
  aisTrail(mmsi: number) {
    if (Number.isInteger(mmsi) && mmsi > 0) this.sendSpectrum({ type: 'ais_trail', mmsi });
  }

  /** ★ Raw IQ out for THIS session — the audio sheet's row. The server answers with `iqout`. */
  iqOut(on: boolean, rate = 48000) { this.sendSpectrum({ type: 'iqout', on: on ? 1 : 0, rate }); }

  /** ★ `dab` arrives about once a second with the whole measured state; `dab_off` ends it;
   *  `dab_error` is a refusal (the receiver has no DAB, or the owner switched it off) and is an
   *  EXPLANATION, not a protocol fault — the panel says why rather than showing a dead button. */
  protected handleServerMessage(msg: Record<string, unknown>): boolean {
    /* ★★★ THE REPORT IN PIECES (dab=2 — src/services/dabAssemble.ts). `dab_list` / `dab_dls` are only stored; a `dab`
     *  comes back whole, in the legacy shape, so everything below is unchanged — and an older server's full report
     *  passes straight through. */
    if (msg.type === 'dab_list' || msg.type === 'dab_dls') { this.dabAsm.ingest(msg, Date.now()); return true; }
    if (msg.type === 'dab') {
      const now = Date.now();
      const r = this.dabAsm.ingest(msg, now);
      if (r.resync) this.sendSpectrum({ type: 'dab_resync' });
      // ★ Self-healing scopes: a new socket (a reconnect) starts with them off; restate while the pane is open.
      if (this.dabScopesOn && msg.v === 2 && !('iq' in msg) && now - this.dabScopesSaidAt > 3000) {
        this.dabScopesSaidAt = now;
        this.sendSpectrum({ type: 'dab_scopes', on: 1 });
      }
      if (r.report) msg = r.report as Record<string, unknown>;
    }
    if (msg.type === 'dab_off') this.dabAsm.reset();
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
      case 'adsb': {
        const now = Date.now();
        if (!this.adsbExit.accept(now)) return true;   // ★ a ghost after our own off — see adsbExit
        const r = parseAdsbMessage(msg);
        // ★ V12 aircraft data: each aircraft with what it IS (adsb_info, sent once per aircraft, kept here per hex)
        r.aircraft = this.adsbInfo.apply(r.aircraft);
        /* ★★ HOLD THE HEAL WHILE THE MODE RUNS — every few seconds, quietly (the hold extends, never
         *  shortens; logging one a second would bury the audio log). A report we did not ask for
         *  (a second listener joining a radio already in ADS-B) is the same silence. */
        if (!this.adsbHeld || now - this.adsbHealHeldAt > 3000) {
          this.adsbHealHeldAt = now;
          holdNativeHealing(VibeServerClient.ADSB_QUIET_HOLD_MS, this.adsbHeld ? 'ADS-B running' : 'server reports ADS-B',
                            this.adsbHeld);
        }
        this.adsbHeld = true;
        this.adsbLastAt = now;
        this.callbacks.onAdsb?.(r);
        return true;
      }
      /* ★★ V12 aircraft data: registration, type, operator, country, route — new or changed aircraft only (full:1 =
       *  every one, to a socket that has just joined). Arrives just before the report it belongs to. */
      case 'adsb_info':
        if (!this.adsbExit.accept(Date.now())) return true;
        this.adsbInfo.take(msg);
        return true;
      case 'adsb_off':
        this.adsbExit.offConfirmed(Date.now());
        this.adsbInfo.clear();
        this.adsbHeld = false;
        this.callbacks.onAdsb?.(null);
        return true;
      case 'adsb_error':
        this.adsbHeld = false;
        this.callbacks.onAdsb?.(null, dabSafeText(msg.why, 200) || 'ADS-B is not available on this receiver');
        return true;
      case 'ism': {
        const now = Date.now();
        if (!this.ismExit.accept(now)) return true;   // ★ a ghost after our own off
        const r = ismSanitise(msg);
        if (!r) return true;
        if (!this.ismHeld || now - this.ismHealHeldAt > 3000) {
          this.ismHealHeldAt = now;
          holdNativeHealing(VibeServerClient.ISM_HOLD_MS, this.ismHeld ? 'ISM running' : 'server reports ISM', this.ismHeld);
        }
        this.ismHeld = true;            // ★ arrival = the receiver is in ISM, whoever put it there
        this.wholeLastAt.ism = now;
        this.callbacks.onIsm?.(r);
        return true;
      }
      case 'ism_off':
        this.ismExit.offConfirmed(Date.now());
        if (this.ismHeld) holdNativeHealing(VibeServerClient.ISM_HOLD_MS, 'server reports ISM off');
        this.ismHeld = false;
        this.callbacks.onIsm?.(null);
        return true;
      case 'ism_error':
        this.ismHeld = false;
        this.callbacks.onIsm?.(null, dabSafeText(msg.why, 200) || 'ISM is not available on this receiver');
        return true;
      /* ★★ V12 NDB — the same three messages and the ghost guard; Find NDB and the stats answer the asker only. */
      case 'ndb': {
        const now = Date.now();
        if (!this.ndbExit.accept(now)) return true;   // ★ a ghost after our own off
        const r = ndbSanitise(msg);
        if (!r) return true;
        if (!this.ndbHeld || now - this.ndbHealHeldAt > 3000) {
          this.ndbHealHeldAt = now;
          holdNativeHealing(VibeServerClient.NDB_HOLD_MS, this.ndbHeld ? 'NDB running' : 'server reports NDB', this.ndbHeld);
        }
        this.ndbHeld = true;            // ★ arrival = the receiver is in NDB, whoever put it there
        this.wholeLastAt.ndb = now;
        this.callbacks.onNdb?.(r);
        return true;
      }
      case 'ndb_off':
        this.ndbExit.offConfirmed(Date.now());
        if (this.ndbHeld) holdNativeHealing(VibeServerClient.NDB_HOLD_MS, 'server reports NDB off');
        this.ndbHeld = false;
        this.callbacks.onNdb?.(null);
        return true;
      case 'ndb_error':
        this.ndbHeld = false;
        this.callbacks.onNdb?.(null, dabSafeText(msg.why, 200) || 'NDB is not available on this receiver');
        return true;
      case 'ndb_found': this.callbacks.onNdbFound?.(parseNdbFound(msg)); return true;
      case 'ndb_stats': this.callbacks.onNdbStats?.(parseNdbStats(msg)); return true;
      /* ★★ V12 DRM — the listener's own DRM receiver's report (only while their mode is DRM; a ghost after leaving is
       *  dropped by the screen, which checks the mode as the web's onDrm does). */
      case 'drm': { const d = drmSanitise(msg); if (d) this.callbacks.onDrm?.(d); return true; }
      /* ★ V12 the AM family's state (carrier lock + offset, the applied AUTO BW edges, AM stereo's pilot) — once a second
       *  on a change, only while the listener is in AM, SAM, ECSS or AM stereo (shim sendAmSync). */
      case 'amsync': { const a = parseAmSync(msg); if (a) this.callbacks.onAmSync?.(a); return true; }
      /* ★★ V12 what AUTO has done to this listener's passband — the sliders show it while it cuts (bwAuto.ts). */
      case 'bwauto': { const b = parseBwAuto(msg); if (b) this.callbacks.onBwAuto?.(b); return true; }
      /* ★ V12 AIS: a report about once a second — ITS ARRIVAL MEANS AIS IS ON (a second listener joining a radio somebody
       *  else put there); `ais_off` ended it (the restored config follows); `ais_error` is the refusal, with its reason. */
      case 'ais': {
        const now = Date.now();
        if (!this.aisExit.accept(now)) return true;   // ★ a ghost after our own off
        const r = aisSanitise(msg);
        if (!r) return true;
        if (!this.aisHeld || now - this.aisHealHeldAt > 3000) {
          this.aisHealHeldAt = now;
          holdNativeHealing(VibeServerClient.AIS_HOLD_MS, this.aisHeld ? 'AIS running' : 'server reports AIS', this.aisHeld);
        }
        this.aisHeld = true;
        this.wholeLastAt.ais = now;
        this.callbacks.onAis?.(r);
        return true;
      }
      case 'ais_off':
        this.aisExit.offConfirmed(Date.now());
        if (this.aisHeld) holdNativeHealing(VibeServerClient.AIS_HOLD_MS, 'server reports AIS off');
        this.aisHeld = false;
        this.callbacks.onAis?.(null);
        return true;
      case 'ais_error':
        this.aisHeld = false;
        this.callbacks.onAis?.(null, dabSafeText(msg.why, 200) || 'AIS is not available on this receiver');
        return true;
      /* ★★ The answer to modeResume: ok = the server is in the mode (or going back in — its reports follow); otherwise
       *  `why`, in words, to this listener only. */
      case 'mode_resume': {
        const k = String(msg.mode);
        if (k !== 'adsb' && k !== 'ndb' && k !== 'ism' && k !== 'ais') return true;
        this.callbacks.onModeResume?.(k, Number(msg.ok) === 1, typeof msg.why === 'string' ? dabSafeText(msg.why, 200) : undefined);
        return true;
      }
      case 'ais_trail': {
        const t = aisTrailSanitise(msg);
        if (t) this.callbacks.onAisTrail?.(t.mmsi, t.pts);
        return true;
      }
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
      case 'dab_scan': {
        const st = parseDabScan(msg);
        if (st) this.callbacks.onDabScan?.(st);
        return true;
      }
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
