/**
 * Apple Watch remote-view provider.
 *
 * The watch is a thin client. This module owns the whole phone->watch view path:
 * raw dBFS bins in, a VFO-centred 256-byte row out over WCSession.
 *
 * IT RUNS OFF THE RAW SPECTRUM, NOT OFF THE WATERFALL COMPONENT. That is
 * deliberate and load-bearing: the PRIMARY use case is the phone locked in a
 * pocket, and on lock the app unmounts the Skia canvases and cancels every
 * animation driver, so anything hanging off WaterfallView is dead exactly when
 * the watch matters most. We therefore keep our own SignalProcessor, fed the same
 * settings the phone's renderer uses, so the wrist looks identical without
 * depending on a component that is deliberately torn down.
 *
 * Everything here must stay in the "safe to run while backgrounded" class: plain
 * JS plus one native bridge call. No Skia, no Reanimated/worklets, no per-frame
 * React state — those are what starved the audio DSP in the v6 regression.
 *
 * The palette travels as data: the phone's own 256-entry RGBA LUT, rather than
 * reimplementing 26 colour maps in Swift.
 *
 * Direction of truth: phone owns frequency/mode/step; the watch sends DELTAS and
 * mirrors whatever the phone echoes back.
 */
import { NativeModules, NativeEventEmitter, Platform } from 'react-native';
import { getColorLUT } from '../assets/colormapUtils';
import { SignalProcessor, type SignalProcessorSettings } from '../assets/signalProcessor';
import { getBandsAtRegion, bandHex } from '../constants/bandPlan';

const Native = NativeModules.VibeWatchModule as
  | {
      isReachable(): Promise<boolean>;
      sendRow(rowB64: string, freq: number, span: number, snr: number, level: number,
              lo: number, hi: number, meter: string): void;
      sendState(freq: number, mode: string, step: number, meter: string,
                level: number, why: string, link: number,
                band: string, bandCol: string,
                bandLo: number, bandHi: number): void;
      sendVolume(vol: number, muted: boolean): void;
      sendFmdx(json: string): void;
      sendStations(json: string): void;
      sendDab(json: string): void;
      sendAircraft(json: string): void;
      sendFavourites(json: string): void;
      sendPhone(status: string): void;
      sendLogo(b64: string): void;
      sendSettings(lutB64: string, smoothing: number, needle: string,
                   needleIntensity: number, sharpness: number, peakHold: boolean): void;
    }
  | undefined;

/** Watch waterfall width. MUST MATCH WaterfallBuffer.width on the watch — the
 *  watch drops any row of the wrong length, so a mismatch is a blank waterfall.
 *
 *  256, not 128: the Ultra's screen is ~205pt wide, so 128 columns were being
 *  UPSCALED 1.6x before they even reached your eye — a self-inflicted blur that
 *  no amount of sharpening can undo. Above native width the image is downscaled
 *  instead, which is sharp. It costs 256 bytes a row (~1.3KB/s at 5fps). */
// 128, not 256: the row is the biggest thing on the WCSession link (16/sec), and halving
// its width halves that traffic at zero CPU cost. The wrist is only ~205pt wide, so 128
// upscales slightly rather than downscaling — the sharpness filter + the watch's own
// brightness/contrast recover the look. MUST match WaterfallBuffer.width (watch) and
// WatchSpectrumForwarder.watchBins (native forwarder) — the three define the wire width.
const WATCH_BINS = 128;

/** Row cadence — 10fps, and rows are BATCHED TWO TO A MESSAGE on the native side.
 *
 *  It was 60ms (~16/sec). The DATA was never the problem — a row is 317 bytes, so even
 *  16/sec is ~5 KB/s and Bluetooth doesn't notice. THE MESSAGE RATE was the problem:
 *  WCSession.sendMessage is an INTERACTIVE channel (individually framed and queued,
 *  meant for occasional request/response), and we were making sixteen calls a second at
 *  it, forever. Far outside its design envelope — and that is what kept wedging it,
 *  backing it up, and leaving it silently ONE-WAY after a transport hop.
 *
 *  THE MIDDLE POSITION. 60ms + 2 rows per message = EIGHT messages a second instead of
 *  sixteen: the channel is still driven half as hard (which is what was wedging it),
 *  but full temporal resolution is restored and a pair now completes in 60ms rather
 *  than 100ms.
 *
 *  100ms + batching (5 msgs/sec) was tried and the LAG WAS VISIBLE — a voice would
 *  start and the trace would lift a moment later. The audio never crosses the watch
 *  link, so it always arrives first; every millisecond we add to the picture widens
 *  that gap. Halving the message rate buys most of the reliability; the second halving
 *  cost more than it was worth.
 *
 *  Original note follows, still true:
 *  Row cadence: ~10fps of REAL data.
 *
 *  It was 5fps, on the theory that interpolation hides a low frame rate the way
 *  VibeServer's web client does. That conflated two different things:
 *  interpolation hides a low FRAME RATE, it cannot recover MISSING DATA. The
 *  synthesised in-between rows are linear blends of two real rows — they smooth
 *  the scroll but carry no information. At 5fps half the spectrum frames were
 *  simply discarded, and on SSB speech the energy genuinely changes on a ~100ms
 *  timescale: a syllable peeking up between two sampled frames was gone for good.
 *  The result looked fluid and read as mush.
 *
 *  (VibeServer gets away with 5fps because you're watching a broadcast signal
 *  that's essentially stationary. Hunting SSB conversations is the opposite case,
 *  and that is what the watch is FOR.)
 *
 *  MUST SIT WELL CLEAR OF BOTH SOURCE INTERVALS, not just under one.
 *
 *  It was 90ms — just under the ~100ms of the LOCKED feed (half-rate FFT). Any
 *  jitter (a frame landing 85ms after the last) failed the gate, so that row was
 *  dropped and the next one didn't arrive for 200ms. Locked, that gave an
 *  irregular ~8fps full of 200ms holes, which the jitter buffer and the trace's
 *  EMA then smoothed over — and it read exactly as "the averaging has been
 *  cranked right up". Awake (20fps source) a frame was always ready the instant
 *  the gate opened, so it was a rock-steady 10fps. Same gate, two behaviours.
 *
 *  At 60ms: the locked 100ms feed passes EVERY frame with room for jitter, and
 *  the awake 50ms feed still halves cleanly to ~10fps. */
// 5 rows/sec. The wrist waterfall does NOT scroll at this rate — Jr's WaterfallBuffer
// runs tickScroll (pixels, render-rate, smooth) on a separate clock from tickTrace (data
// intake), so 5fps of fresh data still scrolls smoothly, each row spanning a few steps.
// What this actually buys is HEADROOM on the WCSession interactive queue: rows share that
// one Bluetooth-limited queue with state, and at 16.7/s (batched 2 = ~8.3 msg/s) a weak
// link backs up — the waterfall freezes while tune commands (other direction) still flow,
// and phoneHopHealthy starves into a false "Reconnecting to iPhone" over a live session.
// At 5fps that's ~2.5 msg/s, 3.3x the room. The old "1fps" revert was choppy because the
// buffer then had ONE clock; the split-clock buffer is what makes this safe now. (Stuart
// 2026-07-20, off the saturating-link screenshot.)
const MIN_ROW_MS = 200;

/** Frequency echoes: ≤1 per this, trailing edge always delivered. 4/sec keeps the
 *  wrist tracking a phone-side tune without ever building a WCSession backlog. */
const STATE_MS = 250;

/** FM-DX state echoes. RDS text changes constantly; 4/sec reads as live and stays
 *  well clear of the WCSession backlog the row feed taught us about. */
const FMDX_MS = 250;

/** ADS-B tables churn — a couple of dozen aircraft, re-sent every few seconds. One
 *  per second is plenty for a wrist and can never build a WCSession backlog. */
const AIR_MS = 1000;

/** Rows quiet for this long, with the phone still claiming a live session, and the
 *  watch escalates: it asks the phone to rebuild the spectrum socket. Sits clear of
 *  the client's own detectors (12s pong timeout, ~10s frame staleness) so it only
 *  ever fires as a backstop for a gap they somehow both missed. */
const IDLE_ESCALATE_MS = 15_000;

/** Span = demod bandwidth x this. Lands a signal on ~25 of the 256 bins: wide
 *  enough to read as a blob rather than a 2-bin hairline, tight enough to leave
 *  room for its neighbours. */
const SPAN_MULT = 10;

/** Fallback span when a backend reports no usable filter width. */
const DEFAULT_SPAN_HZ = 125_000;

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

/** RN has no dependable global btoa; a 256-byte row makes this trivial anyway. */
function toBase64(bytes: Uint8Array): string {
  let out = '';
  const n = bytes.length;
  for (let i = 0; i < n; i += 3) {
    const b0 = bytes[i];
    const b1 = i + 1 < n ? bytes[i + 1] : 0;
    const b2 = i + 2 < n ? bytes[i + 2] : 0;
    out += B64[b0 >> 2];
    out += B64[((b0 & 3) << 4) | (b1 >> 4)];
    out += i + 1 < n ? B64[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    out += i + 2 < n ? B64[b2 & 63] : '=';
  }
  return out;
}

export interface WatchFrameCtx {
  centerHz:   number;
  bwHz:       number;
  tuneHz:     number;
  filterLow?: number;
  filterHigh?: number;
}

export interface WatchCommandHandlers {
  /** `armed` is only ever true from the watch's FM-DX screen. A SHARED tuner must
   *  REQUIRE it — the gate cannot live in the watch's UI alone, or any other watch
   *  screen can tune the receiver out from under every listener on the server. */
  onTuneDelta(delta: number, armed: boolean): void;
  /** Absolute tune from the watch numpad — the one non-delta command. */
  onTuneHz(hz: number): void;
  onMode(mode: string): void;
  onStep(hz: number): void;
  /** Crown in zoom mode. Drives the REAL server zoom, so the watch gets finer
   *  bins rather than a magnified crop — the only thing that beats the
   *  bin-resolution ceiling. */
  onZoomDelta(delta: number): void;
  /** Crown in volume mode. DELTAS, never absolutes — same direction-of-truth rule as
   *  tuning: the phone owns the value, the watch nudges it and adopts what comes back.
   *  One detent = one 1/16 step, because that is iOS's own volume quantisation. */
  onVolumeDelta(delta: number): void;
  /** Mute toggle from the wrist. A mute is NOT "volume to zero" — that would lose the
   *  level you were listening at, so unmuting could not restore it. */
  onMute(muted: boolean): void;
  /** Watch app opened/closed. Used to keep the spectrum WS alive while the phone
   *  is locked but the watch is actually looking at it. */
  onReachableChange(reachable: boolean): void;
  /** The watch said hello (it pings on appear, on wake, and every 4s). Push the
   *  current state straight back, so its menu already knows the mode and step
   *  before the user opens it — state messages otherwise only fire when something
   *  CHANGES, which left the pickers blank for a couple of seconds. */
  onHello(): void;
  /** DAB: pick an audio service out of the multiplex. Not a tune — DAB is a list. */
  onDabSelect?(id: number): void;
}

class WatchProvider {
  private available = Platform.OS === 'ios' && !!Native;
  private reachable = false;
  private lastRowAt = 0;
  private lastStateAt = 0;
  private pendingState: { freq: number; mode: string; step: number } | null = null;
  /** Last state we were asked to send — so a meter update can reuse it rather than
   *  opening a stream of its own. */
  private lastState: { freq: number; mode: string; step: number } | null = null;
  private stateTimer: ReturnType<typeof setTimeout> | null = null;
  private lastPalette = '';
  private snr = 0;
  /** The meter string the PHONE is drawing right now (e.g. "S9+10", "-72dB",
   *  "18db"). Mirrored verbatim — see setSignal. */
  private meter = '';
  private level = 0;
  private lastFmdxAt = 0;
  private pendingFmdx: string | null = null;
  private fmdxTimer: ReturnType<typeof setTimeout> | null = null;
  private lastAir = '';
  private lastAirAt = 0;
  private airTimer: ReturnType<typeof setTimeout> | null = null;
  private lastDab = '';
  private sentDab = '\u0000';
  private lastStations = '';
  private sentStations = '\u0000';
  private lastPingAt = 0;
  /** What the PHONE is doing, told to the watch rather than left to be guessed.
   *
   *  'starting'  — cold-launched (by the watch) and connecting. Not a fault; a boot.
   *  'ready'     — a session is live.
   *  'pick'      — no default instance, but there ARE favourites: the wrist chooses.
   *  'setup'     — no default AND no favourites. Nothing the watch can do; say so. */
  private phoneStatus = 'ready';
  /** Set by SDRScreen when it pauses the spectrum socket for power saving. */
  private specPaused = false;
  private lastFavs = '';
  private sentFavs = '\u0000';
  /** Handled OUTSIDE attach(), because it must work when NO SDR screen is mounted —
   *  which is the whole point: the watch launches the phone, the phone lands on the
   *  picker with no default instance, and the wrist is looking at nothing. */
  private instanceHandler: ((url: string) => void) | null = null;
  private instanceSub: { remove(): void } | null = null;
  private lastLogo = '';   // what we WANT the watch to have
  private sentLogo = '\u0000';  // what it actually has (sentinel: never sent)
  private out = new Uint8Array(WATCH_BINS);
  /** Does the watch actually want the waterfall? False while it is in Standalone mode, running its
   *  own receiver. Defaults TRUE: a watch that has said nothing is a companion, which is what every
   *  build before this one was. */
  private rowsWanted = true;
  private emitter: NativeEventEmitter | null = null;
  /** LINK subs (reachability) — owned by the app, live for its whole life. */
  private subs: { remove(): void }[] = [];
  /** COMMAND subs — owned by the current SCREEN, torn down when it unmounts. */
  private cmdSubs: { remove(): void }[] = [];
  private pollTimer: ReturnType<typeof setInterval> | null = null;
  private proc = new SignalProcessor();
  private colormap = 'gqrx';
  private onReachable: ((r: boolean) => void) | null = null;

  /** WHICH SCREEN OWNS THE WATCH — and a token proving it.
   *
   *  Two screens can be alive at once. React Navigation's `navigate()` PUSHES and
   *  leaves the old screen MOUNTED underneath, still connected and still streaming;
   *  even a stack reset unmounts it asynchronously, so there is always a window where
   *  the outgoing SDR screen is pushing spectrum ROWS while the incoming FM-DX screen
   *  is pushing STATION blobs. The watch routes on what it receives, so it flips
   *  between the waterfall and the station screen many times a second — and the
   *  waterfall "wins" simply because rows arrive more often.
   *
   *  Fixing that in the navigator is whack-a-mole. Fix it at the SOURCE: the last
   *  screen to claim the watch owns it, and anything sent by a screen that no longer
   *  owns it is DROPPED. A stale screen cannot talk to the wrist, whatever the
   *  navigator is doing.
   *
   *  The token matters for the same reason: the outgoing screen's cleanup runs AFTER
   *  the incoming screen's setup, so a naive detach() would tear down the new owner's
   *  claim. Only the current owner can release it. */
  private owner = 0;
  private ownerScreen: 'sdr' | 'fmdx' = 'sdr';

  /** Claim the watch for this screen. Returns the token to release it with. */
  claim(screen: 'sdr' | 'fmdx'): number {
    this.owner += 1;
    this.ownerScreen = screen;
    return this.owner;
  }

  /** Release ONLY if still the owner — a late cleanup must not evict its successor. */
  release(token: number) {
    if (token === this.owner) this.owner = 0;
  }

  private owns(screen: 'sdr' | 'fmdx') {
    return this.owner !== 0 && this.ownerScreen === screen;
  }

  /** When the phone's renderer last handed us a row.
   *
   *  This REPLACES a "phoneRendering" flag driven by AppState. A flag can desync
   *  — one resume path forgot to set it back to true, so the provider kept doing
   *  its own DSP while the phone was rendering, and both fought over the JS
   *  thread: the phone's waterfall went slow and jerky after every screen wake.
   *
   *  Recency can't desync. A row arriving from the renderer IS the proof that the
   *  renderer is alive; if none has arrived lately, the phone is asleep and we do
   *  our own DSP. Self-healing, no state machine. */
  private lastBorrowAt = 0;
  private get borrowing() { return Date.now() - this.lastBorrowAt < 1000; }

  /** The phone's acrylic-VFO settings, mirrored so the wrist needle is the same
   *  one the user configured — a hairline is invisible over a bright palette. */
  private needle = '#ffffff';
  private needleIntensity = 5;
  setNeedle(color: string, intensity: number) {
    if (color === this.needle && intensity === this.needleIntensity) return;
    this.needle = color;
    this.needleIntensity = intensity;
    this.lastPalette = '';   // force a settings resend
  }

  /** The phone applies sharpness in its SHADER, not in SignalProcessor — so the
   *  row we hand the watch is unsharpened and it must do its own. Mirror the
   *  setting so the two agree. */
  private sharpness = 0;
  private peakHold = true;
  setSharpness(v: number) {
    if (v === this.sharpness) return;
    this.sharpness = v;
    this.lastPalette = '';   // force a settings resend
  }

  /** Peak hold, mirrored from the phone — the wrist must not decide this for itself.
   *  Rides the (rare) settings message; toggling it forces a resend. */
  setPeakHold(on: boolean) {
    if (on === this.peakHold) return;
    this.peakHold = on;
    this.lastPalette = '';   // force a settings resend
  }

  /** True only when a watch app is actually in the foreground with a live link.
   *  Everything here no-ops otherwise, so a user with no watch pays nothing. */
  get isActive() { return this.available && this.reachable; }

  /** Mirror the phone renderer's own settings so the wrist looks the same. */
  setProcessorSettings(patch: Partial<SignalProcessorSettings>) {
    this.proc.applySettings(patch);
  }

  setColormap(name: string) { this.colormap = name; }

  /** Wire watch commands into the screen's existing tune/mode/step handlers. */
  /** THE LINK, independent of any screen.
   *
   *  Reachability used to be established inside attach() — which only runs when an SDR
   *  or tuner screen MOUNTS. So on a cold boot with NO DEFAULT INSTANCE, no screen ever
   *  mounts, `reachable` stayed false forever, and everything gated on it (the
   *  favourites list, the phone status) was never sent: the wrist asked you to choose a
   *  server and then showed you an empty list. Having a default "fixed" it only because
   *  a default mounts a screen, which started the link as a side effect.
   *
   *  The link is a property of the APP, not of a screen. Idempotent; called from
   *  App.tsx at startup and from attach(). */
  startLink() {
    if (!this.available || this.pollTimer) return;
    this.emitter ??= new NativeEventEmitter(NativeModules.VibeWatchModule);
    this.subs.push(
      this.emitter.addListener('VibeWatchState', (e: { reachable: boolean }) => {
        this.setReachable(!!e.reachable);
      }),
    );
    // Reachability flips when the watch app foregrounds/backgrounds, and the delegate
    // callback can be missed across an app relaunch — poll as a floor. A plain timer,
    // no React/Skia work, so it is safe while the phone is locked (which is exactly
    // when we need it: the watch app is usually opened AFTER the phone is pocketed).
    void Native!.isReachable().then((r) => this.setReachable(r)).catch(() => {});
    this.pollTimer = setInterval(() => {
      Native!.isReachable().then((r) => this.setReachable(r)).catch(() => {});
    }, 2000);
  }

  attach(handlers: WatchCommandHandlers) {
    if (!this.available) return;
    this.detach();
    this.startLink();

    this.emitter ??= new NativeEventEmitter(NativeModules.VibeWatchModule);
    this.cmdSubs.push(
      this.emitter.addListener('VibeWatchCommand', (e: { cmd: string; delta?: number; val?: unknown; armed?: boolean }) => {
        switch (e.cmd) {
          case 'tune': handlers.onTuneDelta(Number(e.delta ?? 0), e.armed === true); break;
          case 'freq': handlers.onTuneHz(Number(e.val ?? 0)); break;
          case 'mode': handlers.onMode(String(e.val ?? '')); break;
          case 'step': handlers.onStep(Number(e.val ?? 0)); break;
          case 'zoom': handlers.onZoomDelta(Number(e.delta ?? 0)); break;
          case 'vol':  handlers.onVolumeDelta(Number(e.delta ?? 0)); break;
          case 'mute': handlers.onMute(e.val === true); break;
          case 'ping':
            // The watch pings on appear, on wake and every 4s. Treat the FIRST one
            // after a gap as "it has nothing" — cheap, and it heals a watch that was
            // force-quit and relaunched while the phone kept running.
            handlers.onHello();
            if (Date.now() - this.lastPingAt > 8000) this.flushAll();
            this.lastPingAt = Date.now();
            break;
          case 'dab':  handlers.onDabSelect?.(Number(e.val ?? 0)); break;
          // The watch is telling us it's missing something we only send ON CHANGE
          // (the palette LUT, the logo, the station memory). It knows; we don't.
          // Forget what we think it has.
          // ★ The watch has gone STANDALONE — it is running its own receiver and is not reading our
          //   waterfall. Reachability cannot tell us this: the watch app is still in the
          //   foreground, so by every flag we had it looked like an attached companion. Without
          //   being told, we stream a waterfall nobody reads — phone battery spent, and WCSession
          //   traffic on the radio the watch needs for its OWN sockets.
          //
          //   Rows only. State, favourites and the rest are cheap, occasional, and still wanted:
          //   the watch keeps showing the phone's link health, and its mode toggle only appears
          //   while we are alive. Silence would look like a dead phone.
          case 'stop': this.rowsWanted = false; break;
          // 'need' is the watch saying "I have nothing" — which is exactly what entering Companion
          // mode sends. That is the resume, and it is deliberately NOT 'ping': the watch pings
          // while merely SHOWING the servers list, and that must not restart a stream it isn't
          // going to draw.
          //
          // ★ onHello() FIRST — it is what wakes the spectrum. When the phone is locked and playing
          //   in the background we close the spectrum socket for power, and only 'ping' used to
          //   reopen it. So a watch entering Companion sent 'need', got nothing (there was nothing
          //   flowing to send), and concluded the phone had no server — when it was sitting on a
          //   live one with the screen off. "I have nothing, send me everything" has to include
          //   waking the source, or it is a request that cannot be satisfied.
          case 'need':
            handlers.onHello();
            this.rowsWanted = true;
            this.flushAll();
            break;
        }
      }),
    );
    this.onReachable = handlers.onReachableChange;
  }

  private setReachable(r: boolean) {
    if (r === this.reachable) return;
    this.reachable = r;
    if (r) this.lastPalette = '';   // re-send palette on (re)connect
    this.onReachable?.(r);
    if (r) this.flushAll();   // the watch arrived (or came back) with nothing
  }

  /** A SCREEN is going away — not the app.
   *
   *  This used to tear down the reachability listener and the poll timer as well, i.e.
   *  the LINK ITSELF. That was invisible while a screen always existed, but it means an
   *  app with no screen mounted (a cold boot with no default instance) has no link at
   *  all: `reachable` never becomes true and nothing is ever sent to the wrist. The
   *  link belongs to the app; only the command handlers belong to the screen. */
  detach() {
    this.cmdSubs.forEach((s) => s.remove());
    this.cmdSubs = [];
    this.onReachable = null;
  }

  /** Latest signal, mirrored from the meter bus. `level` is the already-smoothed,
   *  already-compressed 0..1 fill the phone's own meter draws — send it rather than
   *  the raw dB so the wrist bar and the phone bar move identically. */
  // ── FM-DX ────────────────────────────────────────────────────────────────
  //
  // A DIFFERENT SCREEN, not a variant of the waterfall: FM-DX has no spectrum at
  // all, so on this backend the STATION is the content. The watch routes on the
  // message it last received, so nothing here needs to know about screens.
  //
  // Throttled + trailing, like the frequency and the meter. RDS RadioText changes
  // constantly and WCSession queues rather than drops.

  /** The whole FM-DX state as JSON. A string, not 12 bridge args: this shape will
   *  keep growing (PTY, TA, AF...), and a JSON blob absorbs that without touching
   *  the native bridge or the .m signature every time. It's ~200 bytes at 4/sec. */
  sendFmdx(state: {
    freq: number; ps: string; rt: string; pi: string; sig: number;
    users: number; stereo: boolean; tx: string; meter: string; level: number;
    pty: string; city: string; dist: number; flag: string; rx: string;
  }) {
    if (!this.isActive || !this.owns('fmdx')) return;
    this.pendingFmdx = JSON.stringify(state);
    const wait = this.lastFmdxAt + FMDX_MS - Date.now();
    if (wait <= 0) { this.flushFmdx(); return; }
    if (!this.fmdxTimer) {
      this.fmdxTimer = setTimeout(() => { this.fmdxTimer = null; this.flushFmdx(); }, wait);
    }
  }

  private flushFmdx() {
    const j = this.pendingFmdx;
    if (!j || !this.isActive) return;
    this.pendingFmdx = null;
    this.lastFmdxAt = Date.now();
    Native!.sendFmdx(j);
  }

  /** OWRX ADS-B: the live aircraft table.
   *
   *  Like DAB, this is a LIST, not a continuum — the profile IS the content and there
   *  is nothing to tune. Unlike DAB it CHURNS (a 20-aircraft table, re-sent every few
   *  seconds), so it's throttled like the meter rather than sent on change: WCSession
   *  queues rather than drops, and a wedged downlink is the price of forgetting that. */
  sendAircraft(list: unknown[]) {
    if (!this.isActive) return;
    this.lastAir = JSON.stringify(list);
    const wait = this.lastAirAt + AIR_MS - Date.now();
    if (wait <= 0) { this.flushAir(); return; }
    if (!this.airTimer) {
      this.airTimer = setTimeout(() => { this.airTimer = null; this.flushAir(); }, wait);
    }
  }

  private flushAir() {
    if (!this.isActive || !this.lastAir) return;
    this.lastAirAt = Date.now();
    Native!.sendAircraft(this.lastAir);
  }

  /** The DAB multiplex: the ensemble, its services, and which one is playing.
   *
   *  DAB is a LIST, not a continuum — the services arrive as an id->name map from
   *  the ensemble, and you switch with setAudioServiceId(), never by tuning. So the
   *  watch gets a list and a selection, and the crown becomes a SELECTOR rather than
   *  a tuning control. Sent on change only (the list changes when the mux does). */
  sendDab(state: { ensemble: string; active: number;
                   list: { id: number; name: string }[] }) {
    if (!this.available) return;
    this.lastDab = JSON.stringify(state);
    this.flushDab();
  }

  private flushDab() {
    if (!this.isActive || this.lastDab === this.sentDab) return;
    this.sentDab = this.lastDab;
    Native!.sendDab(this.lastDab);
  }

  /** The dial's station memory — the SAME list the phone's dial draws, so the wrist
   *  is a mirror rather than a second implementation. Tiny and rarely changes (a new
   *  entry when RDS names a station you tuned), so it goes on change only, with the
   *  same want-vs-have tracking as the logo: the watch usually arrives AFTER the
   *  list was built, and a list marked sent before the watch was there is a list
   *  that never arrives. */
  sendStations(list: { freqHz: number; name: string }[]) {
    if (!this.available) return;
    this.lastStations = JSON.stringify(list);
    this.flushStations();
  }

  private flushStations() {
    if (!this.isActive || this.lastStations === this.sentStations) return;
    this.sentStations = this.lastStations;
    Native!.sendStations(this.lastStations);
  }

  /** The station logo, as bytes. The phone has ALREADY resolved it to a local file
   *  (stationLogoCache) and drawn it — so we ship the same image rather than making
   *  the watch fetch a URL it has no network path to. Only on change: it's tens of
   *  KB, and the station changes about as often as you tune. */
  sendLogo(b64: string) {
    if (!this.available) return;
    this.lastLogo = b64;
    this.flushLogo();
  }

  /** Send the logo only once the watch is actually THERE.
   *
   *  It used to mark the logo as sent before checking reachability — so a station
   *  that resolved before the watch connected (the normal case: the phone has been
   *  tuned for a while, you then raise your wrist) was recorded as delivered and
   *  never actually went out. The wrist showed a permanently empty background.
   *  Track what we WANT the watch to have separately from what it HAS, and flush
   *  whenever the link comes up. */
  /** The watch just (re)appeared and has NOTHING — re-send everything that is sent
   *  "on change".
   *
   *  Send-on-change + a peer that restarted = NEVER SENDS. The palette LUT is only
   *  pushed when the colormap changes, so after the watch app was force-quit the
   *  phone stayed silent and the wrist sat in its greyscale fallback: a black-and-
   *  white waterfall with no way back. Same trap the logo hit. Forget what we think
   *  the watch has; it has nothing. */
  /** The user's FAVOURITE instances — a curated handful, not the full directory.
   *
   *  Bringing 2,000 receivers to a watch would be silly; bringing the five you
   *  actually use is exactly right. They already carry serverType, so the phone can
   *  go straight to the right screen with no detection round-trip. */
  setSpecPaused(p: boolean) { this.specPaused = p; }

  /** The phone is rebuilding its server link right now. A fact the watch cannot
   *  infer: from the wrist, a recovery in progress and a dead phone are the same
   *  black screen. Rides the existing throttled state echo as a `why` value. */
  setReconnecting(b: boolean) {
    if (b === this.reconnecting) return;
    this.reconnecting = b;
    this.nudgeState();
  }
  private reconnecting = false;

  /** The PHONE↔SERVER hop's quality (0=down … 3=good), teed from the client's own
   *  link meter. There are TWO links in series — phone↔server and watch↔phone —
   *  and they fail independently, so without this the watch can only say "something
   *  is rough" and not which. */
  setLinkQuality(q: 0 | 1 | 2 | 3) {
    if (q === this.linkQuality) return;
    this.linkQuality = q;
    this.nudgeState();
  }
  private linkQuality: 0 | 1 | 2 | 3 = 3;

  /** Re-send the state echo because a value INSIDE it changed (why / link quality),
   *  rather than because the frequency did.
   *
   *  Without this the watch would not hear about a reconnect until its next 4s
   *  heartbeat — and a "reconnecting" pill that shows up four seconds into a
   *  fifteen-second recovery has missed most of the point.
   *
   *  This is NOT a new stream, and must never become one. It fires on a TRANSITION
   *  only (both callers early-return when the value is unchanged), and it rides the
   *  existing 250ms trailing-edge throttle — so a flapping link coalesces instead of
   *  queueing. The channel budget is the one thing that must not move; see the
   *  downlink-wedge notes on setSignal. */
  private nudgeState() {
    const s = this.lastState;
    if (!s || !this.isActive) return;
    this.sendFreq(s.freq, s.mode, s.step);
  }

  /** Phase 4 escalation. The watch already knows when rows have stopped; that
   *  knowledge was merely DISPLAYED. Make it actionable: if the phone believes it
   *  is connected but has sent nothing for long enough, have it prove that by
   *  rebuilding the socket. Rate-limiting lives in the client. */
  setStaleHandler(fn: (() => void) | null) { this.staleHandler = fn; }
  private staleHandler: (() => void) | null = null;
  private idleSince = 0;

  setPhoneStatus(st: string) {
    if (st === this.phoneStatus) return;
    this.phoneStatus = st;
    this.flushPhone();
  }

  private flushPhone() {
    // NOT gated on `reachable`: at cold boot that flag hasn't settled yet, so gating
    // on it meant the watch never heard "starting" and sat on a stale "ready" — i.e.
    // it reported a normal boot as a fault. The native side already refuses to send on
    // a dead link, and flushAll() re-sends the moment the watch appears.
    if (!this.available) return;
    Native!.sendPhone(this.phoneStatus);
  }

  /** WHY the wrist has no waterfall — the phone knows, so it should SAY so.
   *
   *  "No spectrum from iPhone" is a symptom, not a diagnosis, and it sent us round
   *  in circles: a paused socket, a stalled renderer and a dead link all look
   *  identical from the watch. The state channel still works whenever this matters
   *  (that's why the frequency kept updating), so it can carry the reason. */
  private whyNoRows(): string {
    if (this.specPaused) return 'paused';                      // socket closed for power
    // A recovery IN PROGRESS is not a fault, and must not be drawn as one — the
    // watch shows a "reconnecting" pill over the last frames rather than a black
    // overlay. Ranks above 'idle': rows have of course stopped, that is what a
    // reconnect IS.
    if (this.reconnecting) return 'reconnecting';
    if (Date.now() - this.lastRowAt > 2000) {                  // nothing to send us rows
      this.escalateIfStuck();
      return 'idle';
    }
    this.idleSince = 0;
    return 'live';
  }

  /** Rows have stopped for long enough that the phone's own watchdogs should have
   *  caught it. If they haven't, the watch's staleness knowledge is the last line
   *  of defence — so use it instead of merely drawing it. */
  private escalateIfStuck() {
    const now = Date.now();
    if (this.idleSince === 0) { this.idleSince = now; return; }
    if (now - this.idleSince < IDLE_ESCALATE_MS) return;
    this.idleSince = now;   // re-arm; the client rate-limits the real work
    this.staleHandler?.();
  }

  sendFavourites(list: { name: string; url: string; type?: string }[]) {
    if (!this.available) return;
    this.lastFavs = JSON.stringify(list);
    this.flushFavs();
  }

  private flushFavs() {
    if (!this.available || !this.reachable || this.lastFavs === this.sentFavs) return;
    this.sentFavs = this.lastFavs;
    Native!.sendFavourites(this.lastFavs);
  }

  /** Register the "switch to this instance" handler. Lives OUTSIDE attach/detach so
   *  it survives screen changes — the command has to work from the picker, where no
   *  SDR screen exists to have attached anything. */
  setInstanceHandler(fn: (url: string) => void) {
    if (!this.available) return;
    this.instanceHandler = fn;
    if (this.instanceSub) return;
    this.emitter ??= new NativeEventEmitter(NativeModules.VibeWatchModule);
    this.instanceSub = this.emitter.addListener(
      'VibeWatchCommand',
      (e: { cmd: string; val?: unknown }) => {
        if (e.cmd === 'inst') this.instanceHandler?.(String(e.val ?? ''));
      },
    );
  }

  private flushAll() {
    this.lastPalette = '';    // forces the settings/LUT resend on the next row
    this.sentLogo = '\u0000';
    this.sentStations = '\u0000';
    this.sentDab = '\u0000';
    this.sentFavs = '\u0000';
    this.flushPhone();
    this.flushLogo();
    this.flushStations();
    this.flushDab();
    this.flushFavs();
  }

  private flushLogo() {
    if (!this.isActive || this.lastLogo === this.sentLogo) return;
    this.sentLogo = this.lastLogo;
    Native!.sendLogo(this.lastLogo);
  }

  /**
   * MIRROR THE PHONE'S METER — don't pick a metric on the watch.
   *
   * The wrist used to render SNR specifically, and OWRX / Kiwi / FM-DX have no SNR
   * to give (they send an absolute S-meter or dBf, with no noise reference), so
   * SDRScreen sent a hardcoded 0 and the watch showed a permanent "—" — while the
   * bar underneath moved perfectly well, which is what made it look like a display
   * bug rather than a missing metric. So the phone sends the STRING IT IS ALREADY
   * DRAWING, and the watch prints it.
   *
   * It rides the SAME throttled state message as the frequency. It had its own, and
   * that was a mistake: two dict streams at 4/sec each, on top of the 16/sec rows,
   * flooded WCSession — which QUEUES rather than drops — and the DOWNLINK WEDGED.
   * (The uplink still worked, because a message from the watch always wakes the
   * phone: the wrist could tune but had gone deaf.) ONE channel, one throttle.
   */
  setSignal(snr: number, level: number, meter: string) {
    this.snr = snr;
    this.level = level;
    this.meter = meter;
    // NO SEND HERE. The meter rides the ROW (see sendRow) — it must never get a
    // message of its own.
    //
    // It did, briefly, and it broke the one thing the watch exists for. The meter
    // text changes every frame, so scheduling a state message on each change added a
    // continuous ~4/sec dictionary stream on top of the 16/sec rows. Awake, that was
    // survivable. LOCKED — which is the primary use case, phone in a pocket — iOS
    // throttles the JS thread, WCSession QUEUES rather than drops, and the downlink
    // WEDGED: "No spectrum from iPhone" the moment the screen went off. The row is
    // already going out; put the meter in it and the extra stream disappears.
  }

  sendState(freq: number, mode: string, step: number) {
    // SDR SCREEN ONLY — this is an INVARIANT, not a nicety.
    //
    // The watch routes on what it RECEIVES: a `state` message means "the SDR screen is
    // up", so it sets `isFmdx = false` and shows the waterfall. That was safe while only
    // SDRScreen ever called this. It stopped being safe the moment anything else could
    // trigger a state send — and a volume echo (which fires on EVERY screen) did exactly
    // that, throwing the wrist off FM-DX and onto the waterfall mid-turn.
    //
    // The guard belongs HERE, not at the call sites: there is no context in which a
    // non-SDR screen wants to assert "the SDR screen is up".
    if (!this.isActive || !this.owns('sdr')) return;
    this.lastStateAt = Date.now();
    // `link` = the PHONE↔SERVER hop. The watch can measure its OWN hop (rows stop
    // arriving) but is blind to the far one, which is why its warning pill could
    // only ever say "something is rough". Riding the existing 250ms state echo, so
    // this costs no extra WCSession traffic — the one budget that must not move.
    // The BAND — name, colour and EDGES — from the phone's own ITU band plan. The wrist
    // must not hold a second opinion about what band it is on, any more than it holds one
    // about the palette: the phone computes, the watch mirrors.
    const b = this.primaryBand(freq);
    Native!.sendState(freq, mode, step, this.meter, this.level, this.whyNoRows(),
                      this.linkQuality,
                      b?.name ?? '', b ? bandHex(b) : '',
                      b?.lo ?? 0, b?.hi ?? 0);
  }

  /** The ITU region the RECEIVER is in (1/2/3; 0 = unknown). Set by SDRScreen, which
   *  derives it from the receiver's longitude. */
  setItuRegion(r: number) { this.ituRegion = r; }
  private ituRegion = 0;

  /** REGION-AWARE, and it has to be.
   *
   *  The band plan carries per-region variants and they genuinely differ: 40m ham runs to
   *  7200 kHz in Region 1 (Europe) and to 7300 in Region 2, with 41m broadcast starting
   *  where it stops. `getPrimaryBandAt` ignores regions entirely, so on a UK receiver it
   *  matched the REGION 2 entry and told the wrist the 40m/41m border was at 7300 — the
   *  American band plan, on a British radio.
   *
   *  Same lookup and same precedence (ham > broadcast > utility) the phone's own band
   *  crossing uses, so the two can't disagree. */
  private primaryBand(hz: number) {
    const order: Record<string, number> = { ham: 0, broadcast: 1, utility: 2 };
    const bands = getBandsAtRegion(hz, this.ituRegion)
      .sort((a, b) => (order[a.type] ?? 9) - (order[b.type] ?? 9));
    return bands[0] ?? null;
  }

  /** The iPhone's SYSTEM volume (0…1), straight from the KVO observer — so it carries
   *  changes the watch did NOT make (hardware buttons, Control Centre, a headset's own
   *  rocker) as well as the ones it did.
   *
   *  The wrist MIRRORS this rather than tracking a knob of its own. Getting that wrong
   *  is what broke the last attempt: it drove an app-level gain, delivered loudness was
   *  `appGain × systemVolume`, and the watch could only see one of the two — so with the
   *  phone at 50% the meter read full while delivering half. */
  setVolume(v: number) {
    const clamped = Math.max(0, Math.min(1, v));
    if (clamped === this.volume) return;
    this.volume = clamped;
    this.flushVolume();
  }

  /** The watch toggled mute. Mirror it back so the wrist's own glyph is the truth
   *  rather than an optimistic guess. */
  setMuted(m: boolean) {
    if (m === this.muted) return;
    this.muted = m;
    this.flushVolume();
  }

  /** Volume has its OWN message — it must NOT ride the `state` echo.
   *
   *  It did, and it broke FM-DX badly: the watch treats a `state` message as PROOF the
   *  phone is on the SDR screen (it sets `isFmdx = false`). But system volume changes on
   *  every screen, so a volume echo from the FM-DX screen made the watch throw away the
   *  FM-DX view and jump to the waterfall — mid-turn, while the crown was moving.
   *
   *  Volume is a fact about the DEVICE, not about the session. It asserts nothing about
   *  which screen is up, so it travels on its own and works on both. Transition-only, so
   *  it can never become a stream. */
  private flushVolume() {
    if (!this.isActive) return;
    Native!.sendVolume(this.volume, this.muted);
  }
  private volume = 1;
  private muted = false;

  /**
   * The FREQUENCY echo — throttled, trailing-edge, exactly like
   * UberSDRClient._sendView.
   *
   * The watch used to read the frequency off the ROWS, on the reasoning that every
   * row already carries it, so a separate echo would be redundant traffic. That is
   * true right up until the link is busy — and then it is badly wrong. Rows are
   * fire-and-forget at ~16/sec, and WCSession QUEUES rather than drops: spin the
   * crown and rows pile up behind the tune commands. The watch then reads its
   * frequency out of a row that is SECONDS old, so the readout lurches backwards
   * and then walks forward again as the backlog drains. (Backpressuring the rows to
   * stop that was tried and reverted — it made the waterfall 1fps. The rows are
   * right to be lossy; they are pixels. The frequency is not pixels.)
   *
   * So the frequency gets its own channel: a small state message, sent at most
   * once per STATE_MS, with the final value ALWAYS delivered on the trailing edge.
   * Because it is throttled it can never build a backlog, and because it is
   * trailing-edge the last thing the watch hears is always the truth.
   */
  sendFreq(freq: number, mode: string, step: number) {
    if (!this.isActive) return;
    this.lastState = { freq, mode, step };
    this.pendingState = { freq, mode, step };
    const wait = this.lastStateAt + STATE_MS - Date.now();
    if (wait <= 0) { this.flushState(); return; }
    if (!this.stateTimer) {
      this.stateTimer = setTimeout(() => { this.stateTimer = null; this.flushState(); }, wait);
    }
  }

  private flushState() {
    const p = this.pendingState;
    if (!p) return;
    this.pendingState = null;
    this.sendState(p.freq, p.mode, p.step);   // carries the meter too
  }

  /**
   * A row the phone's renderer ALREADY computed. Free to forward — no DSP — and
   * pixel-identical to what's on the phone screen. Used whenever the phone is
   * drawing (see `borrowing`).
   */
  pushProcessedRow(row: Uint8Array, ctx: WatchFrameCtx) {
    // Stamp BEFORE the isActive gate: this is how we know the renderer is alive,
    // and that must hold whether or not a watch is currently listening.
    this.lastBorrowAt = Date.now();
    if (!this.isActive) return;
    this.sendRow(row, ctx);
  }

  /**
   * One raw dBFS spectrum frame, straight off the client. This is the LOCKED-PHONE
   * path: the renderer is torn down, so there's no row to borrow and we must do
   * our own DSP. Safe to call while backgrounded — no Skia, worklet or React work.
   */
  onSpectrum(bins: Float32Array, ctx: WatchFrameCtx) {
    if (!this.isActive || this.borrowing) return;   // renderer is feeding us
    if (!bins || bins.length < 2 || !ctx.bwHz) return;

    // Cheap gate FIRST: don't pay for a 4096-bin pass on a frame we'd only drop.
    if (Date.now() - this.lastRowAt < MIN_ROW_MS) return;

    this.sendRow(this.proc.process(bins, ctx.centerHz, ctx.bwHz).row, ctx);
  }

  private sendRow(row: Uint8Array, ctx: WatchFrameCtx) {
    // The watch told us it is running its own receiver and isn't reading this (cmd:stop).
    if (!this.rowsWanted) return;
    // A screen that no longer owns the watch must not talk to it. The outgoing SDR
    // screen stays mounted (and streaming) for a beat after the incoming one takes
    // over, and its rows would drag the wrist back to the waterfall.
    if (!this.owns('sdr')) return;
    const now = Date.now();
    if (now - this.lastRowAt < MIN_ROW_MS) return; // coalesce: newest wins
    this.lastRowAt = now;

    if (this.colormap !== this.lastPalette) {
      this.lastPalette = this.colormap;
      Native!.sendSettings(toBase64(getColorLUT(this.colormap)), 0.35,
                           this.needle, this.needleIntensity, this.sharpness,
                           this.peakHold);
    }

    const n = row.length;
    if (n < 2 || !ctx.bwHz) return;

    const binHz = ctx.bwHz / n;

    // Span follows the demod bandwidth so the signal is always a readable blob.
    const bw = Math.abs((ctx.filterHigh ?? 0) - (ctx.filterLow ?? 0));
    const wanted = (bw > 0 ? bw : DEFAULT_SPAN_HZ / SPAN_MULT) * SPAN_MULT;

    // ...but NEVER crop below the source resolution. The watch can't be sharper
    // than the phone's bins: with the phone zoomed out each bin covers a lot of
    // Hz, so a narrow window may hold only ~20 real bins, and stretching those
    // across 256 columns just invents pixels — which is the blur, and no amount
    // of sharpening recovers detail that was never sampled. Better to show a
    // WIDER span that is genuinely sharp than a narrow one that is mush.
    const floorSpan = WATCH_BINS * binHz;
    const span = Math.min(ctx.bwHz, Math.max(wanted, floorSpan));

    const centreBin = (ctx.tuneHz - ctx.centerHz) / binHz + n / 2;
    const halfBins  = span / binHz / 2;
    const start     = centreBin - halfBins;
    const step      = (halfBins * 2) / WATCH_BINS;

    // Peak-preserving decimation: a narrow carrier must survive the squeeze to
    // 256 columns. Averaging would bury a CW tone in its own noise floor.
    for (let x = 0; x < WATCH_BINS; x++) {
      const s0 = start + x * step;
      const s1 = s0 + step;
      let i0 = Math.floor(s0);
      const i1 = Math.max(i0 + 1, Math.ceil(s1));
      let peak = 0;
      for (; i0 < i1; i0++) {
        // Clamp to the edges rather than wrapping — off-span reads as floor,
        // not as a phantom signal folded in from the far side of the band.
        const v = row[i0 < 0 ? 0 : i0 >= n ? n - 1 : i0];
        if (v > peak) peak = v;
      }
      this.out[x] = peak;
    }

    // Send the filter EDGES, not a width. The passband is only symmetric about
    // the carrier on AM/FM — LSB sits entirely below it, USB entirely above, CW
    // is offset — so a single bandwidth number would draw every mode as AM.
    Native!.sendRow(toBase64(this.out), ctx.tuneHz, span, this.snr, this.level,
                    ctx.filterLow ?? 0, ctx.filterHigh ?? 0, this.meter);
  }
}

export const watchProvider = new WatchProvider();
