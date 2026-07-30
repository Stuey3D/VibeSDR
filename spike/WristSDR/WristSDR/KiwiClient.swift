import Foundation
import Combine

/// The surface SpikeLink drives — implemented by both UberClient and KiwiClient so the wrist UI
/// is backend-agnostic. Everything here is read/called on the main actor.
@MainActor
protocol SDRClient: AnyObject {
  var frequency: Double { get }
  var mode: String { get }
  var displaySpanHz: Double { get }
  var bwLow: Double { get }
  var bwHigh: Double { get }
  var signalLevel: Double { get }
  var signalDb: Double { get }
  /// Absolute level in dBFS, for the dBFS/S-unit meter units. NaN means "this backend can't say" —
  /// only radiod (UberSDR) carries a true per-packet baseband power, so everyone else defaults to
  /// NaN below and the meter falls back to the SNR reading.
  var signalDbfs: Double { get }
  var rowsPushed: Int { get }
  var framesPerSec: Double { get }
  var kbps: Double { get }   // DEBUG incoming KB/s (spectrum+audio)
  var status: String { get }
  /// How far Link Management has had to throttle the waterfall (1 = full rate). Drives the link
  /// glyph. A backend with no rate lever (OWRX) keeps the default 1 via the extension below.
  var adaptiveRung: Int { get }
  /// Still working out what this link will carry — the UI draws an INDETERMINATE indicator while
  /// true. Default false: a backend with no rate lever has nothing to settle on.
  var linkSettling: Bool { get }
  /// A plain-English refusal/timeout reason to show the user (nil = fine). Kiwi sets this on
  /// badp/too_busy/handshake-block/connect-timeout so nobody waits forever for a dead connection.
  var lastError: String? { get }

  func start()
  func drainSpectrum(now: Double)
  func tune(delta: Int, step: Double)
  func tuneTo(_ hz: Double)
  func zoom(delta: Int)
  func setVolume(_ v: Double)
  func setMode(_ m: String)
  func setBandwidth(_ low: Double, _ high: Double)
  func resumeSpectrum()
  func reconnectIfNeeded()
  func suspend()
  /// Foreground recovery. UberClient overrides with a targeted, backoff-resetting version; the
  /// default keeps the other backends' previous behaviour (resume the spectrum, then a guarded full
  /// reconnect) so this change is Uber-only for now. See UberClient.wake / the "eternity" fix.
  func wake()
  func goIdle()
  // OWRX profile surface — REQUIREMENTS (not just extension members) so dynamic dispatch reaches
  // OwrxClient's real list through `any SDRClient`. UberSDR/Kiwi get the default-empty extension.
  var profiles: [SDRProfile] { get }
  var clients: Int { get }
  func selectProfile(_ id: String)
  /// Live inbound link load (KB/s of all WS bytes). Drives the "heavy server" advisory when on the
  /// phone relay. REQUIREMENT (not just extension) so `any SDRClient` reaches the real number. 0 = the
  /// backend doesn't measure it (fine — no advisory).
  var inboundKbPerSec: Int { get }
  /// Live station name (RDS ps / DAB service) to show in place of the band label. "" = none.
  var stationName: String { get }
  /// DAB services in the tuned ensemble (empty unless on a DAB profile), the current speed-fix factor,
  /// and the controls to change them. OWRX-only; defaults make the other backends inert.
  var dabProgrammes: [DabProgramme] { get }
  var dabScale: Double { get }
  var dabActiveId: Int { get }
  var dabEnsembleName: String { get }
  func selectDabService(_ id: Int)
  func setDabScale(_ scale: Double)
  /// ADS-B decoded aircraft (empty unless on a 1090 MHz ADS-B profile). OWRX-only; default inert.
  var aircraft: [Aircraft] { get }
  /// The receiver's own location (SDR site), for the ADS-B map centre + aircraft distances. nil = unknown.
  var receiverLat: Double? { get }
  var receiverLon: Double? { get }
  /// Shared server chat (OWRX today; FM-DX later). `supportsChat` gates the whole UI; `chatActivity`
  /// bumps per inbound line to breathe the glyph. Default inert so non-chat backends need nothing.
  var supportsChat: Bool { get }
  var chatLog: [ChatLine] { get }
  var chatActivity: Int { get }
  func sendChat(_ text: String)
  /// FM-DX tuner state (nil unless this is an FM-DX server). Default inert.
  var fmdxInfo: FmdxInfo? { get }
  /// The frequency window tunable RIGHT NOW (Hz) on the current server — for OWRX this is the
  /// CURRENT profile only (centre ± sampRate/2), since bookmarks must never switch profile. A
  /// bookmark outside this window is refused with a warning rather than retuned.
  var tuneMinHz: Double { get }
  var tuneMaxHz: Double { get }
  /// Auto-contrast (0–20) — the DSP's dynamic-range squeeze (`SignalProcessor.autoContrast`). The
  /// primary visibility control; Brightness/Contrast are post-normalisation tweaks on top. Default
  /// no-op for a backend with no waterfall DSP (FM-DX).
  func setAutoContrast(_ v: Double)
  /// ★ MANUAL waterfall range. `on == false` returns to auto-contrast. Floor/ceiling are dBFS,
  /// the same units as the phone's dbMin/dbMax so the two can be compared side by side.
  func setManualRange(_ on: Bool, floor: Double, ceil: Double)
  /// SNR squelch (audio gate): mute below `minSnr` dB. ≤ -999 = OFF/open. Default no-op for backends
  /// with no squelch (FM-DX). UberSDR/VibeServer send it to radiod's audio gate.
  func setSquelch(_ minSnr: Double)
}

extension SDRClient {
  func wake() { resumeSpectrum(); reconnectIfNeeded() }
  var kbps: Double { 0 }   // default for backends without a byte tally
}

// Default-empty so UberSDR/Kiwi don't have to implement the profile surface; OWRX overrides.
extension SDRClient {
  /// OpenWebRX has no waterfall-rate lever at all (fps/fft_fps/fft_size are ignored), so it never
  /// throttles and is never blamed for one.
  var adaptiveRung: Int { 1 }
  var linkSettling: Bool { false }
  var signalDbfs: Double { .nan }
  var profiles: [SDRProfile] { [] }
  var clients: Int { 0 }
  func selectProfile(_ id: String) {}
  var inboundKbPerSec: Int { 0 }
  var stationName: String { "" }
  var dabProgrammes: [DabProgramme] { [] }
  var dabScale: Double { 1.0 }
  var dabActiveId: Int { -1 }
  var dabEnsembleName: String { "" }
  func selectDabService(_ id: Int) {}
  func setDabScale(_ scale: Double) {}
  var aircraft: [Aircraft] { [] }
  var receiverLat: Double? { nil }
  var receiverLon: Double? { nil }
  var supportsChat: Bool { false }
  var chatLog: [ChatLine] { [] }
  var chatActivity: Int { 0 }
  func sendChat(_ text: String) {}
  var fmdxInfo: FmdxInfo? { nil }
  /// The server's BROAD coverage (Hz) — used to FILTER the bookmark list so a bookmark saved on a
  /// wideband server doesn't clutter one that can't reach it (e.g. a +30 MHz OWRX bookmark on a
  /// 0–30 MHz Kiwi). Defaults to the live tune window; OWRX widens it because its tune window is
  /// only the current profile while the server itself covers far more.
  var coverMinHz: Double { tuneMinHz }
  var coverMaxHz: Double { tuneMaxHz }
  func setAutoContrast(_ v: Double) {}
  func setManualRange(_ on: Bool, floor: Double, ceil: Double) {}
  func setSquelch(_ minSnr: Double) {}
}

extension UberClient: SDRClient {
  /// UberSDR surfaces its own refusals via its status/cards; no separate channel here.
  var lastError: String? { nil }
}

/// A DIRECT KiwiSDR client on the watch — a Swift port of `src/services/KiwiAdapter.ts`.
///
/// Two Network-framework WebSockets (SND audio + W/F waterfall) to /ws/kiwi/<ts>/{SND,W/F}.
/// Control plane = `SET key=val` text; SND/W/F/MSG frames are all BINARY with a 3-char tag.
/// Audio = IMA-ADPCM ('kiwi') → PCM → WatchAudio; waterfall = 1024 u8 bins → the shared buffer.
@MainActor
final class KiwiClient: ObservableObject, SDRClient {

  // Mode → Kiwi wire mode + default passband (Hz).
  private static let modeMap: [String: (mod: String, lo: Double, hi: Double)] = [
    "usb": ("usb", 300, 2700),  "lsb": ("lsb", -2700, -300),
    "am":  ("am", -4900, 4900), "sam": ("sam", -4900, 4900),
    "fm":  ("nbfm", -6000, 6000), "nfm": ("nbfm", -6000, 6000),
    "cwu": ("cw", 300, 700),    "cwl": ("cw", -700, -300),
    "wfm": ("nbfm", -6000, 6000),
  ]
  private static let fullBW = 30_000_000.0
  // 12, not the server's 14. 30 MHz / 2^12 = 7.3 kHz min span; 2^14 = 1.8 kHz, NARROWER than
  // an SSB channel, so one contact overran the screen and auto-contrast flooded it with pure
  // bright pixels. Matches the 6 kHz floor UberSDRClient enforces for the same reason.
  private static let maxZoom = 12
  private static let ua = "Mozilla/5.0 (iPhone; CPU iPhone OS 18_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1"
  private static let OPEN_WF = true

  /// Browser-identity handshake headers. Kiwi classifies connections as `ext_api` (and time-limits/
  /// DROPS them after a few seconds) unless they look like the web client. The phone gets this for
  /// free — React Native's WebSocket sends Origin + browser headers automatically; a raw NWConnection
  /// sends neither, so we add them explicitly. `Origin` = the Kiwi's own http(s) origin.
  private var browserHeaders: [(name: String, value: String)] {
    let origin = wsBase.replacingOccurrences(of: "wss://", with: "https://")
                       .replacingOccurrences(of: "ws://", with: "http://")
    return [("User-Agent", Self.ua), ("Origin", origin)]
  }

  // ── Published surface the UI mirrors ──
  @Published var frequency: Double = 9_600_000
  @Published var mode = "am"
  @Published var bwLow: Double = -4900
  @Published var bwHigh: Double = 4900
  @Published var signalLevel: Double = 0
  @Published var signalDb: Double = 0
  /// Kiwi squelch is a CLIENT-SIDE gate on the S-meter dBm the SND header already carries — the
  /// same choice the phone made, because Kiwi's server-side squelch is SNR-based and unreliable.
  /// −130 = off.
  private var sqDbm: Double = -130
  /// Release tail. Speech has natural gaps and the S-meter dips into them; without a short hold the
  /// gate chatters and chops the front off every syllable.
  private var sqOpenUntil: Double = 0
  func setSquelch(_ dbm: Double) { sqDbm = dbm <= -999 ? -130 : dbm }
  /// Should audio pass right now? Cheap enough to call per packet.
  private var sqOpen: Bool {
    if sqDbm <= -130 { return true }
    let now = ProcessInfo.processInfo.systemUptime
    if signalDb >= sqDbm { sqOpenUntil = now + 0.35; return true }
    return now < sqOpenUntil
  }
  @Published var framesPerSec: Double = 0
  @Published var status = "starting"
  @Published var lastError: String? = nil
  var rowsPushed = 0
  var displaySpanHz: Double { viewInit ? viewBw : rxBw }
  var tuneMinHz: Double { 0 }
  var tuneMaxHz: Double { rxBw }   // server-reported coverage top (30 MHz, or narrower/converted)
  func setAutoContrast(_ v: Double) { proc.autoContrast = v }
  func setManualRange(_ on: Bool, floor: Double, ceil: Double) {
    proc.manualRange = on; proc.manualFloorDb = floor; proc.manualCeilDb = ceil
  }

  private var everFrame = false
  private var errorShown = false
  private var connectTimer: Timer?
  private var sockState = "connecting"

  // ── Endpoint ──
  private let wsBase: String     // ws(s)://host:port
  private let secure: Bool
  // ★★★ Int64, NOT Int. On arm64_32 — every watch below watchOS 27, e.g. a Series 6 —
  // Swift's `Int` is THIRTY-TWO BITS. Unix ms is ~1.78e12, some 800× past Int32.max, and
  // converting an out-of-range Double to Int TRAPS. So this line crashed the app the
  // instant a Kiwi client was constructed, on every older watch, while being completely
  // fine on the Ultra 3 (watchOS 27, arm64, 64-bit Int). Symptom: "Kiwi crashed as it
  // tried to connect", 2026-07-29.
  // ★ It is only a cache-buster in the socket path, so the exact value never mattered.
  private let ts = Int64(Date().timeIntervalSince1970 * 1000)

  // ── Kiwi state ──
  private var rxBw = KiwiClient.fullBW
  /// Read from the audio decode on the SND socket queue (off main) — a benign Double race; the
  /// value only changes once at stream start (MSG sample_rate).
  nonisolated(unsafe) private var trueAudioRate = 12000.0
  private var viewCenter = KiwiClient.fullBW / 2
  private var viewBw = KiwiClient.fullBW
  private var viewInit = false
  private var wfReady = false
  private let ident: String
  /// Reaching the server through the paired iPhone's Bluetooth relay? Set by SpikeLink before
  /// connecting. Caps the waterfall rate — see `topWfSpeed`.
  ///
  /// `nonisolated(unsafe)`: written once at construction, then only read (including from the
  /// LinkManager callback, which runs off the main actor).
  nonisolated(unsafe) var onRelay = false

  /// The FASTEST waterfall rate we will ask this server for.
  ///
  /// ★★ MEASURED, 2026-07-20. Kiwi honours `wf_speed` properly — unlike OWRX, where every rate
  ///    request either does nothing or kills the stream. On a fast server (85.183.11.108):
  ///
  ///        wf_speed 4 (fast)  18.9 KB/s   18.7 fps
  ///        wf_speed 3 (med)   13.0 KB/s   12.9 fps
  ///        wf_speed 2 (slow)   5.1 KB/s    5.1 fps
  ///
  ///    And rates vary ~6x BETWEEN servers at the same setting (3.4 - 18.9 KB/s measured across
  ///    six), so "Kiwi is light" is only true of some Kiwis.
  ///
  /// ★ WHY A CAP AND NOT JUST THE LADDER. LinkManager already steps 4->3->2, but it is REACTIVE:
  ///   it needs frames to arrive late before it backs off. Over Bluetooth the failure is not
  ///   slowness, it is the socket being dropped — by which point there is nothing to adapt. On a
  ///   fast server 18.9 KB/s of waterfall plus ~7 of audio is ~26 KB/s, at the very floor of the
  ///   relay's ~25-62 KB/s range. Capping at 3 lands ~20 KB/s and keeps a usable waterfall; the
  ///   ladder still steps DOWN from there if the link is worse than that.
  nonisolated var topWfSpeed: Int { onRelay ? 3 : 4 }

  /// The rate to ASK FOR RIGHT NOW: the controller's current rung, never above the relay cap.
  /// At connect the controller starts mid-ladder, so this opens at 13 fps rather than 23.
  ///
  /// `nonisolated(unsafe)` and mirrored rather than read from `linkMgr` directly: the handshake and
  /// the keepalive both run OFF the main actor, and hopping actors mid-handshake to ask a question
  /// we already know the answer to is how a missed wf_speed becomes a stuck rate.
  nonisolated(unsafe) private var wfSpeedMirror = 3
  nonisolated var wfSpeedNow: Int { min(wfSpeedMirror, topWfSpeed) }

  // ── Sockets / audio / DSP ──
  // nonisolated so the BACKGROUND keepalive timer can send on them without hopping to main — a
  // main-actor hop would miss whenever the waterfall DSP stalls main, and a missed keepalive is
  // exactly what makes Kiwi drop us. AudioSocket.send is thread-safe (NWConnection.send).
  nonisolated(unsafe) private let sndSock = AudioSocket(name: "kiwi-snd")
  nonisolated(unsafe) private let wfSock  = AudioSocket(name: "kiwi-wf")
  nonisolated(unsafe) let waterfall: WaterfallBuffer
  // ★★ wfDecodeQueue ONLY (see onWf) — never main, so no lock is needed.
  nonisolated(unsafe) private let proc = SignalProcessor()
  nonisolated(unsafe) private let wfDecodeQueue = DispatchQueue(label: "kiwi.wf")
  // audio + the persistent ADPCM decoder run on the SND socket's serial queue (OFF the main actor,
  // like UberClient) so the audio path never fights the waterfall for the main thread — a saturated
  // main actor freezes the UI AND stalls the keepalive, which is what made Kiwi drop us.
  nonisolated(unsafe) private let audio = WatchAudio()
  nonisolated(unsafe) private let audioDec = ImaAdpcmDecoder(clampLo: -32768, clampHi: 32767)
  private var audioStarted = false
  nonisolated(unsafe) private var keepaliveSource: DispatchSourceTimer?
  nonisolated(unsafe) private var kaCount = 0     // keepalives actually sent (debug)
  private let keepaliveQueue = DispatchQueue(label: "kiwi.keepalive")
  private var rateTimer: Timer?
  private var frameCount = 0

  init(url: String, waterfall: WaterfallBuffer) {
    self.waterfall = waterfall
    // http(s)/ws(s)://host:port[/…] → ws(s)://host:port
    var u = url
    for p in ["https://", "http://", "wss://", "ws://"] { if u.hasPrefix(p) { u.removeFirst(p.count) } }
    if let slash = u.firstIndex(of: "/") { u = String(u[..<slash]) }
    let wantSecure = url.hasPrefix("https") || url.hasPrefix("wss")
    self.secure = wantSecure
    self.wsBase = "\(wantSecure ? "wss" : "ws")://\(u)"
    self.ident = (UserDefaults.standard.string(forKey: "vibe.kiwi.ident") ?? "VibeSDR")
    proc.autoContrast = 5
  }

  private func wsURL(_ stream: String) -> URL { URL(string: "\(wsBase)/ws/kiwi/\(ts)/\(stream)")! }

  // ── Lifecycle ──
  func start() {
    // ★ On the decode queue, because that is the only place `proc` is otherwise touched.
    wfDecodeQueue.async { self.proc.reset() }
    // ★ Sync the off-actor mirror to whatever rung the controller OPENED on, before the handshake
    //   sends the first wf_speed. Without this the mirror's own default wins and a user who pinned
    //   Full Rate (or Low Data) would silently get the adaptive mid-ladder rate instead — the
    //   override would be honoured by every later step and by none of the first ones.
    wfSpeedMirror = 5 - linkMgr.rung

    let t = Timer(timeInterval: 1, repeats: true) { [weak self] _ in
      Task { @MainActor in guard let self else { return }
        self.framesPerSec = Double(self.frameCount)
        // Reset the backoff only once a session has PROVEN ITSELF (see shortSessionLimit). Resetting
        // on any frame at all meant a server that kicks after a few seconds got an endless 1.5s
        // reconnect loop: frames arrive → backoff cleared → kicked → retry fast → repeat, forever.
        if self.frameCount > 0, self.retries > 0,
           ProcessInfo.processInfo.systemUptime - self.sessionStart >= Self.shortSession {
          self.retries = 0; self.shortSessions = 0
        }
        self.frameCount = 0

        // SILENT-AUDIO WATCHDOG (Stuart, 2026-07-21). frameCount above is the SND *audio* rate. If SND
        // dies silently while the W/F waterfall keeps advancing, framesPerSec falls to 0 but nothing
        // reconnects (reconnectIfNeeded is a no-op; the connect timer only fires before the first frame).
        // So: audio was flowing (everFrame), status is live, the W/F socket is STILL pushing rows this
        // second (link + server are fine), yet no SND frame for >5s ⇒ reconnect. retrySnd's own `retrying`
        // guard + backoff stop this from stacking.
        let wfAdvancing = self.rowsPushed > self.lastRowsSnapshot
        self.lastRowsSnapshot = self.rowsPushed
        if self.everFrame, self.status == "live", wfAdvancing, !self.retrying,
           ProcessInfo.processInfo.systemUptime - self.lastSndAt > 5 {
          self.retrySnd(reason: "audio silent")
        }

        self.linkMgr.tick(fps: self.framesPerSec,
                          live: !self.goingIdle && self.rowsPushed > 0,
                          settled: ProcessInfo.processInfo.systemUptime - self.lastWfChangeAt > 3) }
    }
    RunLoop.main.add(t, forMode: .common); rateTimer = t

    // Connect watchdog — if no audio/waterfall frame has arrived in 12s the connection is never
    // coming (blocked, full, callsign-only, or just unreachable). Say so instead of hanging.
    let ct = Timer(timeInterval: 12, repeats: false) { [weak self] _ in
      Task { @MainActor in
        guard let self, !self.everFrame else { return }
        // Include the last socket state (debug): "ready" ⇒ socket opened but Kiwi sent no frames
        // (handshake/UA/protocol), "waiting"/"preparing" ⇒ the connection never completed.
        if let advice = self.ownerBlockAdvice(self.sockState) {
          self.fail("\(advice)\n[state: \(self.sockState)]")
        } else {
          self.fail("No data from this KiwiSDR after 12s.\n[state: \(self.sockState)]")
        }
      }
    }
    RunLoop.main.add(ct, forMode: .common); connectTimer = ct

    audio.start { _, _ in }
    openSnd()
  }

  /// Surface a refusal reason ONCE (badp/too_busy/handshake/timeout all funnel here).
  /// `midSession: true` for refusals that can only arrive AFTER audio has been flowing — the daily
  /// per-IP time limit above all. The plain `guard !everFrame` was written for handshake-time
  /// refusals, and would have silently swallowed exactly the message that explains a mid-session
  /// kick, leaving the reconnect loop to guess again.
  /// ★★★ SAY WHAT THE OWNER DID, NOT WHAT THE SOCKET SAID. A KiwiSDR that only accepts its own web
  /// page does not send a refusal — it lets the WebSocket open and then aborts it, so the user was
  /// shown `POSIXErrorCode(rawValue: 53): Software caused connection abort`, which reads as "Jr is
  /// broken" when it is the receiver's owner exercising a choice we should respect and explain.
  /// (ECONNABORTED at handshake time is the signature; it is not something OUR end can do anything
  /// about, so the message must not invite the user to retry.)
  ///
  /// Deliberately hedged — a blocked client is the usual cause but not the only one a Kiwi can abort
  /// for. See [[third_party_receiver_etiquette]]: we identify ourselves honestly and honour a no.
  private func ownerBlockAdvice(_ state: String) -> String? {
    let s = state.lowercased()
    guard s.contains("connection abort") || s.contains("rawvalue: 53") else { return nil }
    return "This KiwiSDR closed the connection straight away.\n"
         + "Its owner has most likely restricted it to their own web page, which blocks apps like "
         + "Jr. Nothing you can change at this end — try another server."
  }

  private func fail(_ msg: String, midSession: Bool = false) {
    guard !errorShown, midSession || !everFrame else { return }
    gaveUp = true          // a stated rule is final — never keep knocking after one
    errorShown = true
    lastError = msg
    status = "refused"
    goIdle()
  }

  private func markFrame() {
    // A session STARTS at its first audio frame — that's when the server has actually accepted us,
    // and it's what the short-session test above measures from.
    if !everFrame { everFrame = true; connectTimer?.invalidate(); connectTimer = nil }
    if sessionStart <= sessionStartUnset { sessionStart = ProcessInfo.processInfo.systemUptime }
    lastSndAt = ProcessInfo.processInfo.systemUptime   // audio-liveness stamp for the silent-SND watchdog
  }
  // Audio-liveness watchdog state (see the rateTimer). Kiwi runs SND (audio) and W/F (waterfall) as
  // two separate sockets; SND can die silently while W/F keeps painting, leaving the waterfall alive
  // and the audio gone for good. lastSndAt = the last SND audio frame; lastRowsSnapshot lets the
  // watchdog confirm the W/F socket is STILL advancing (link is fine) before blaming the audio.
  private var lastSndAt: Double = 0
  private var lastRowsSnapshot = 0

  // ── Reconnect on a mid-session drop (UberClient's proven pattern) ──
  private var retries = 0
  private var retrying = false
  /// A session shorter than this is not a session — it's a server showing us the door (listening-time
  /// limit, slot limit, one-connection-per-IP rule). 20s is well past a slow handshake but well
  /// inside any of those limits.
  private static let shortSession = 20.0
  /// How many short sessions before we STOP. ★ This is an etiquette limit as much as a UX one: an
  /// unbounded reconnect loop against someone's KiwiSDR is abusive, and ours had one — the backoff
  /// reset on any frame, so a server that kicked us after a few seconds got hammered every 1.5s
  /// indefinitely. Three strikes, then we leave them alone and say why. (Stuart, 2026-07-22, on a
  /// server that kicks in seconds: "may be garbage for the server owner".)
  private static let shortSessionLimit = 3
  private var shortSessions = 0
  private static let sessionStartUnset = -1.0
  private let sessionStartUnset = KiwiClient.sessionStartUnset
  private var sessionStart = KiwiClient.sessionStartUnset
  /// Set once we've decided to stop retrying this server. Nothing reopens a socket after this.
  private var gaveUp = false
  @Published var dropReason = ""     // shown so we can see WHY Kiwi keeps dropping (failed vs recv)
  private func retrySnd(reason: String = "") {
    guard !retrying, !goingIdle, !gaveUp else { return }

    // STOP RECONNECTING to a server that keeps ending the session in seconds. We cannot tell a
    // deliberate kick from a hostile link, but the right response is the same either way: stop
    // knocking. Reconnecting forever would be rude to the owner and useless to the user.
    if ProcessInfo.processInfo.systemUptime - sessionStart < Self.shortSession {
      shortSessions += 1
      if shortSessions >= Self.shortSessionLimit {
        gaveUp = true
        sndSock.cancel(); wfSock.cancel()
        fail("This KiwiSDR keeps ending the session after a few seconds — usually a listening-time "
           + "limit, a full slot, or one-connection-per-listener. We\u{2019}ve stopped retrying so we "
           + "don\u{2019}t hammer the owner\u{2019}s receiver. Try another KiwiSDR.")
        return
      }
    } else {
      shortSessions = 0
    }

    retrying = true
    retries += 1
    sessionStart = Self.sessionStartUnset   // the next session is only 'real' once frames flow again
    if !reason.isEmpty { dropReason = reason }
    status = "reconnect \(retries) ka=\(kaCount): \(dropReason)"
    sndSock.cancel(); wfSock.cancel()
    sndAuthed = false; wfAuthed = false; wfOpened = false
    // Backoff grows with BOTH the retry count and how many short sessions we've had — a server
    // ending sessions in seconds gets progressively longer gaps, not a fixed 1.5s knock.
    let steps = min(retries + shortSessions * 2, 8)
    let wait = UInt64(steps) * 1_500_000_000   // 1.5s → 12s, then hold
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: wait)
      self.retrying = false
      guard !self.goingIdle else { return }
      self.openSnd()          // W/F reopens after SND's first MSG, as on a cold connect
    }
  }

  private func openSnd() {
    // Audio (SND frames) decoded + played HERE, on the socket's serial queue — off the main actor.
    // Only UI state hops to main. MSG/other frames go to the main dispatcher (rare).
    sndSock.onData = { [weak self] d in
      guard let self else { return }
      let buf = [UInt8](d)
      guard buf.count >= 10, buf[0] == 0x53, buf[1] == 0x4e, buf[2] == 0x44 else {
        Task { @MainActor in self.onBinary(d, "SND") }   // MSG / W-F-tagged / control
        return
      }
      let flags = Int(buf[3])
      let smeter = (Int(buf[8]) << 8) | Int(buf[9])
      let offset = (flags & 0x0008) != 0 ? 20 : 10       // SND_STEREO
      guard buf.count > offset else { return }
      let payload = buf[offset...]
      var pcm: [Int16]
      if (flags & 0x0010) != 0 {                          // SND_COMPRESSED → IMA-ADPCM
        pcm = self.audioDec.decode(payload)
      } else {
        let little = (flags & 0x0080) != 0
        let bytes = Array(payload); let n = bytes.count >> 1
        pcm = [Int16](repeating: 0, count: n)
        for i in 0..<n {
          let b0 = Int16(bytes[i*2]), b1 = Int16(bytes[i*2+1])
          pcm[i] = little ? (b1 << 8) | b0 : (b0 << 8) | b1
        }
      }
      guard !pcm.isEmpty else { return }
      guard self.sqOpen else { return }   // squelch closed — see setSquelch
      self.audio.play(pcm: pcm, rate: Int32(self.trueAudioRate.rounded()), channels: 1)
      Task { @MainActor in
        self.signalDb = Double(smeter) / 10 - 127
        self.frameCount += 1
        self.markFrame()
        if self.status != "live" { self.status = "live" }
      }
    }
    sndSock.onText = { [weak self] s in Task { @MainActor in self?.onText(s, "SND") } }
    // AudioSocket.onReady is DEAD (never invoked) — drive the handshake off the .ready STATE instead.
    sndSock.onState = { [weak self] st in
      Task { @MainActor in
        guard let self else { return }
        self.sockState = st
        if !self.everFrame, self.status != "live" { self.status = st }
        if st.contains("ready"), !self.sndAuthed {
          self.sndAuthed = true
          self.status = "registering"
          self.sndSend("SET auth t=kiwi p=")
          self.sndSend("SET ident_user=\(self.ident)")
          self.sndSend("SERVER DE CLIENT openwebrx.js SND")
          // Do NOT open W/F here — Kiwi DROPS SND (ENOTCONN) if the second socket opens before SND's
          // auth is processed. W/F opens from onMsg (first SND MSG = auth done).
          self.startKeepalive()
        }
        if (st.contains("failed") || st.contains("recv:")), !self.goingIdle {
          if self.everFrame {
            self.retrySnd(reason: st)
          } else {
            self.fail("\(self.ownerBlockAdvice(st) ?? "This KiwiSDR wouldn’t open a connection.")\n[\(st)]")
          }
        }
      }
    }
    sndSock.open(url: wsURL("SND"), headers: browserHeaders)
  }
  private var sndAuthed = false

  private var wfAuthed = false
  private func openWf() {
    wfSock.onData = { [weak self] d in Task { @MainActor in self?.onBinary(d, "W/F") } }
    wfSock.onText = { [weak self] s in Task { @MainActor in self?.onText(s, "W/F") } }
    wfSock.onState = { [weak self] st in
      Task { @MainActor in
        guard let self, st.contains("ready"), !self.wfAuthed else { return }
        self.wfAuthed = true
        self.wfSend("SET auth t=kiwi p=")
        self.wfSend("SERVER DE CLIENT openwebrx.js W/F")
        self.wfSend("SET send_dB=1")
        self.wfSend("SET wf_comp=1")
        self.wfSend("SET wf_speed=\(self.wfSpeedNow)")
        self.wfSend("SET maxdb=-10 mindb=-110")
        self.sendZoom()
      }
    }
    wfSock.open(url: wsURL("W/F"), headers: browserHeaders)
  }

  /// BACKGROUND keepalive — a DispatchSourceTimer on its own queue, sending directly on the sockets
  /// (no main-actor hop, no main run loop). Kiwi kicks a client that misses keepalives, and the old
  /// main-actor Timer stopped firing the moment the waterfall DSP stalled main → the drop.
  private func startKeepalive() {
    keepaliveSource?.cancel()
    let t = DispatchSource.makeTimerSource(queue: keepaliveQueue)
    t.schedule(deadline: .now() + 0.5, repeating: 1.0)
    t.setEventHandler { [weak self] in
      guard let self else { return }
      self.kaCount &+= 1
      self.sndSock.send(text: "SET keepalive")
      self.wfSock.send(text: "SET keepalive")
    }
    t.resume()
    keepaliveSource = t
  }

  // ── Binary dispatch (MSG/SND/W/F all arrive as binary with a 3-char tag) ──
  private func onBinary(_ data: Data, _ stream: String) {
    guard data.count >= 3 else { return }
    let tag = String(bytes: data.prefix(3), encoding: .ascii) ?? ""
    switch tag {
    case "MSG": if let s = String(bytes: data, encoding: .isoLatin1) { onText(s, stream) }
    case "SND": onSnd([UInt8](data))
    case "W/F": onWf([UInt8](data))
    default: break
    }
  }

  private var wfOpened = false
  private func onText(_ data: String, _ stream: String) {
    guard data.hasPrefix("MSG") else { return }
    // First SND MSG ⇒ auth processed ⇒ now it's safe to open the W/F socket (see onReady note).
    // DIAGNOSTIC: OPEN_WF gates the waterfall socket. If audio streams indefinitely with it OFF,
    // the two-concurrent-socket interaction is what Kiwi drops (POSIXErrorCode 57 on SND).
    if Self.OPEN_WF, stream == "SND", !wfOpened { wfOpened = true; openWf() }
    let body = String(data.dropFirst(4))
    for tok in body.split(separator: " ") {
      guard let eq = tok.firstIndex(of: "=") else { continue }
      onMsg(String(tok[..<eq]), String(tok[tok.index(after: eq)...]), stream)
    }
  }

  private func onMsg(_ key: String, _ val: String, _ stream: String) {
    switch key {
    case "audio_rate":
      let r = Int(val) ?? 12000
      sndSend("SET AR OK in=\(r) out=44100")
      if stream == "SND" { sendRxParams() }
    case "sample_rate":
      if let f = Double(val), f > 1000 { trueAudioRate = f }
    case "bandwidth":
      if let bw = Double(val), bw > 1000 {
        rxBw = bw
        if !viewInit { viewCenter = bw / 2; viewBw = bw }
      }
    case "wf_setup":
      if !wfReady { wfReady = true; sendZoom() }
    case "audio_adpcm_state":
      let parts = val.split(separator: ",").compactMap { Int($0) }
      if parts.count == 2 { audioDec.setState(index: parts[0], predictor: parts[1]) }
    case "too_busy":
      // too_busy=0 is a NORMAL "you are not too busy" broadcast — only non-zero means full.
      if val != "0" && val != "" {
        fail("This KiwiSDR is full — every listening slot is in use. Try another KiwiSDR, or use UberSDR or OpenWebRX.")
      }
    case "ip_limit":
      // ★ THE DAILY PER-IP TIME LIMIT — the real reason behind "it lets us in and then kicks us".
      // Many owners set `ip_limit_mins` (25 on Bedford, for example): once your IP has used its
      // allowance for the day, the server still ACCEPTS the connection and then ends it seconds
      // later. Identical on the wire to a flaky link, which is why we used to blame Bluetooth and
      // reconnect forever. It is a rule, not a fault — say so and stop.
      fail("You\u{2019}ve used this KiwiSDR\u{2019}s daily time allowance for your connection"
         + " — the owner limits how long each listener gets per day. It\u{2019}ll let you back in"
         + " tomorrow. Try another KiwiSDR in the meantime.", midSession: true)
    case "badp":
      // Non-zero = the sign-in was rejected: a private listen PASSWORD we don't have, or the owner
      // only allows their own web page. Owner setting, not an app fault.
      if val != "0" {
        fail("This KiwiSDR is password-protected — the owner requires a listen password, which VibeSDR doesn’t have. Try another KiwiSDR, or use UberSDR or OpenWebRX.")
      }
    default: break
    }
  }

  // ── Audio (SND binary) ──
  private func onSnd(_ buf: [UInt8]) {
    guard buf.count >= 10, buf[0] == 0x53, buf[1] == 0x4e, buf[2] == 0x44 else { return }
    let flags = Int(buf[3])
    let smeter = (Int(buf[8]) << 8) | Int(buf[9])
    signalDb = Double(smeter) / 10 - 127            // dBm from header
    frameCount += 1
    markFrame()
    if status != "live" { status = "live" }

    let offset = (flags & 0x0008) != 0 ? 20 : 10    // SND_STEREO
    guard buf.count > offset else { return }
    let payload = buf[offset...]
    var pcm: [Int16]
    if (flags & 0x0010) != 0 {                       // SND_COMPRESSED → IMA-ADPCM
      pcm = audioDec.decode(payload)
    } else {
      let little = (flags & 0x0080) != 0
      let bytes = Array(payload)
      let n = bytes.count >> 1
      pcm = [Int16](repeating: 0, count: n)
      for i in 0..<n {
        let b0 = Int16(bytes[i*2]), b1 = Int16(bytes[i*2+1])
        pcm[i] = little ? (b1 << 8) | b0 : (b0 << 8) | b1
      }
    }
    guard !pcm.isEmpty else { return }
    let rate = Int32(trueAudioRate.rounded())
    guard sqOpen else { return }   // squelch closed — drop the packet rather than play silence
    audio.play(pcm: pcm, rate: rate, channels: 1)
  }

  // ── Waterfall (W/F binary) ──
  nonisolated(unsafe) private var out256 = [UInt8]()   // wfDecodeQueue only, with proc
  private func onWf(_ buf: [UInt8]) {
    guard buf.count >= 16 else { return }
    let zoomFlags = UInt32(buf[8]) | (UInt32(buf[9]) << 8) | (UInt32(buf[10]) << 16) | (UInt32(buf[11]) << 24)
    let wfFlags = (zoomFlags >> 16) & 0xffff
    var bins = ArraySlice(buf[16...])
    if wfFlags & 1 != 0 { bins = ArraySlice(decodeKiwiWaterfallFrame(bins)) }   // WF_COMPRESSED
    guard bins.count >= 8 else { return }
    // u8 → dBm-ish (bin − 255); the auto-contrast in SignalProcessor ranges it.
    let floats = bins.map { Float($0) - 255 }
    markFrame()
    // ★★★ THE DSP RUNS OFF MAIN — same reasoning as UberClient and OwrxClient. On a watch,
    // per-frame DSP on the main actor competes with UI rendering and keeps a core awake that
    // could otherwise idle, so it costs responsiveness AND battery.
    let centre = viewCenter, span = viewBw
    wfDecodeQueue.async { [weak self] in
      guard let self else { return }
      let row = self.proc.process(floats, centerHz: centre, bwHz: span)
      let dec = self.decimate(row, to: WaterfallBuffer.width)
      let lvl = self.proc.level
      Task { @MainActor in
        self.signalLevel = lvl
        if dec.count == WaterfallBuffer.width {
          self.rowsPushed += 1
          self.specQueue.append((ProcessInfo.processInfo.systemUptime, dec))
        }
      }
    }
    viewInit = true
  }

  // ── Spectrum delay (audio-sync), mirrored from UberClient ──
  private var specQueue: [(t: Double, row: [UInt8])] = []
  private let spectrumDelay = 0.15
  func drainSpectrum(now: Double) {
    while let first = specQueue.first, now - first.t >= spectrumDelay {
      waterfall.push(row: first.row)
      specQueue.removeFirst()
    }
  }

  nonisolated private func decimate(_ row: [UInt8], to width: Int) -> [UInt8] {
    let n = row.count
    if n == width { return row }
    guard n > 0 else { return [] }
    if out256.count != width { out256 = [UInt8](repeating: 0, count: width) }
    let ratio = Double(n) / Double(width)
    for i in 0..<width {
      let lo = Int(Double(i) * ratio)
      let hi = min(n, max(lo + 1, Int(Double(i + 1) * ratio)))
      var m: UInt8 = 0
      for j in lo..<hi { if row[j] > m { m = row[j] } }   // PEAK — a mean buries narrow carriers
      out256[i] = m
    }
    return out256
  }

  // ── Control: demod + zoom, throttled (Kiwi kicks clients that spam SET) ──
  private static let minMs = 110.0
  private var lastDemodAt = 0.0, demodPending = false, demodScheduled = false
  private func sendDemod() {
    let now = Date().timeIntervalSince1970 * 1000
    if now - lastDemodAt >= Self.minMs { lastDemodAt = now; sendDemodNow() }
    else {
      demodPending = true
      if !demodScheduled {
        demodScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + (Self.minMs - (now - lastDemodAt)) / 1000) { [weak self] in
          guard let self else { return }
          self.demodScheduled = false
          if self.demodPending { self.demodPending = false; self.lastDemodAt = Date().timeIntervalSince1970 * 1000; self.sendDemodNow() }
        }
      }
    }
  }
  private func sendDemodNow() {
    let m = Self.modeMap[mode] ?? Self.modeMap["am"]!
    sndSend("SET mod=\(m.mod) low_cut=\(Int(bwLow.rounded())) high_cut=\(Int(bwHigh.rounded())) freq=\(String(format: "%.3f", frequency / 1000))")
  }

  private var lastZoomAt = 0.0, zoomPending = false, zoomScheduled = false
  private func zoomLevel() -> Int {
    let z = Int((log2(Self.fullBW / max(1, viewBw))).rounded())
    return min(max(z, 0), Self.maxZoom)
  }
  private func sendZoom() {
    viewBw = Self.fullBW / pow(2, Double(zoomLevel()))
    let now = Date().timeIntervalSince1970 * 1000
    if now - lastZoomAt >= Self.minMs { lastZoomAt = now; sendZoomNow() }
    else {
      zoomPending = true
      if !zoomScheduled {
        zoomScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + (Self.minMs - (now - lastZoomAt)) / 1000) { [weak self] in
          guard let self else { return }
          self.zoomScheduled = false
          if self.zoomPending { self.zoomPending = false; self.lastZoomAt = Date().timeIntervalSince1970 * 1000; self.sendZoomNow() }
        }
      }
    }
  }
  private func sendZoomNow() {
    lastWfChangeAt = ProcessInfo.processInfo.systemUptime   // frames pause over a re-subscribe
    wfSend("SET zoom=\(zoomLevel()) cf=\(String(format: "%.3f", viewCenter / 1000))")
  }
  private var lastWfChangeAt: Double = 0

  /// Adaptive waterfall rate. Kiwi's ladder is the widest of any backend: `wf_speed` 4/3/2 =
  /// 23/13/5 fps (upstream constants WF_SPEED_FAST/MED/SLOW in rx_waterfall.h). We ask every Kiwi
  /// for 23 fps today, so there is a lot to give back.
  ///
  /// Rung 4 (`wf_speed=1`, 1 fps) is DELIBERATELY NOT IN THE LADDER — Stuart: "no amount of
  /// interpolation will rescue that". 5 fps is the floor for both adaptation and Low Data.
  lazy var linkMgr = LinkManager(ladder: [23, 13, 5], lowDataRung: 3) { [weak self] rung, fps in
    // Never ABOVE the cap: on the relay rung 1 means 3 (med), not 4 (fast).
    self?.wfSpeedMirror = 5 - rung
    self?.wfSend("SET wf_speed=\(self?.wfSpeedNow ?? 3)")
    self?.waterfall.setExpectedRowRate(fps)
  }
  var adaptiveRung: Int { linkMgr.adaptiveRung }
  var linkSettling: Bool { linkMgr.settling }

  private func sendRxParams() {
    sendDemod()
    sndSend("SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50")
    sndSend("SET compression=1")
  }

  // ── SDRClient controls ──
  func tune(delta: Int, step: Double) {
    guard delta != 0 else { return }
    let base = delta > 0 ? (frequency / step).rounded(.down) : (frequency / step).rounded(.up)
    let f = min(max((base + Double(delta)) * step, 0), rxBw)
    guard f != frequency else { return }
    frequency = f
    sendDemod()
    viewCenter = f
    if viewInit { sendZoom() }
  }
  func tuneTo(_ hz: Double) {
    let f = min(max(hz, 0), rxBw)
    guard f != frequency else { return }
    frequency = f
    sendDemod()
    viewCenter = f
    if viewInit { sendZoom() }
  }
  func zoom(delta: Int) {
    // delta>0 = zoom IN (narrower span). One detent = one Kiwi zoom step.
    let factor = pow(2.0, Double(-delta))
    viewBw = min(Self.fullBW, max(Self.fullBW / pow(2, Double(Self.maxZoom)), viewBw * factor))
    sendZoom()
  }
  func setVolume(_ v: Double) { audio.setVolume(Float(v)) }
  func setMode(_ m: String) {
    guard m != mode else { return }
    mode = m
    if let p = Self.modeMap[m] { bwLow = p.lo; bwHigh = p.hi }
    sendDemod()
  }
  func setBandwidth(_ low: Double, _ high: Double) { bwLow = low; bwHigh = high; sendDemod() }
  func resumeSpectrum() { wfSend("SET wf_speed=\(wfSpeedNow)") }
  func suspend() { wfSend("SET wf_speed=0"); specQueue.removeAll(); status = "background · audio only" }
  func reconnectIfNeeded() { /* Kiwi keepalive + socket retry handle this for now */ }
  private var goingIdle = false
  func goIdle() {
    goingIdle = true
    keepaliveSource?.cancel(); keepaliveSource = nil
    rateTimer?.invalidate(); rateTimer = nil
    connectTimer?.invalidate(); connectTimer = nil
    sndSock.cancel(); wfSock.cancel(); audio.stop()
    if status != "refused" { status = "idle" }
  }

  private func sndSend(_ s: String) { sndSock.send(text: s) }
  private func wfSend(_ s: String) { wfSock.send(text: s) }
}
