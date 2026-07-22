import Foundation
import SwiftUI
import WatchKit
import Combine
import Network

/// How the watch is reaching the internet. `.iphone` = the paired-iPhone Bluetooth relay, which
/// on watchOS surfaces as `NWInterface.InterfaceType.other` (Apple TN3135 — a well-supported
/// heuristic, not a documented contract; the mapping lives in ONE place, UberClient.transportFor).
enum Transport { case iphone, wifi, cellular, none }

/// A `WatchLink`-shaped adapter for the STANDALONE spike.
///
/// The ported companion views (ContentView / ControlMenu / NumpadView) were written against
/// the phone-fed `WatchLink` — an `@EnvironmentObject` exposing frequency, span, the
/// waterfall buffer, the VFO colour, band-plan fields and so on. This class presents the SAME
/// published surface, but backs it with the spike's own direct-to-UberSDR `UberClient`
/// instead of a WCSession pipe to a phone.
///
/// Everything the phone used to compute and mirror (band plan, S-meter text, the two-hop
/// link diagnostics) either derives from the spike's own socket health or is stubbed blank
/// with a NOTE — there is no phone in this chain, and there is no band plan in the spike yet.
@MainActor
final class SpikeLink: ObservableObject {

  /// The direct backend client — sockets, DSP, audio. Built when the picker chooses a server
  /// (UberClient or KiwiClient), so it's nil until then. Draws into the buffer WE own.
  var client: (any SDRClient)?

  /// The waterfall buffer, OWNED here and injected into the client so its processed 0-255
  /// rows land in the exact buffer the ported views draw from.
  ///
  /// `nonisolated(unsafe)` for the same reason `UberClient.waterfall` was: SwiftUI's `Canvas`
  /// draw closure is not main-actor-isolated, and the ported `ContentView` reads
  /// `link.waterfall` from inside it. The buffer itself is built for cross-thread use — rows
  /// in from the data path, pixels out on the render clock.
  nonisolated(unsafe) let waterfall = WaterfallBuffer()

  // ── Mirrored / derived state the ported views consume ──────────────────────
  // Band label + colour follow the tuned frequency automatically — several update paths set
  // `frequency` and only some remembered to call updateBand(), so the ticker wash could stay
  // stuck on the boot band's colour (looked "always blue"). A didSet can't miss a path.
  @Published var frequency = 0.0 { didSet { if frequency != oldValue { updateBand() } } }
  @Published var span = 0.0
  /// Bookmark gating (all Hz), mirrored from the client each tick. `tuneMin/Max` is the window
  /// reachable RIGHT NOW (OWRX = current profile only); `coverMin/Max` is the server's broad
  /// coverage, used to filter which bookmarks are even shown. See BookmarkStore / BookmarksView.
  @Published var tuneMin = 0.0
  @Published var tuneMax = 0.0
  @Published var coverMin = 0.0
  @Published var coverMax = 0.0
  /// True when `hz` can be tuned on the current server/profile without a profile change.
  func canTune(_ hz: Double) -> Bool { tuneMax > tuneMin && hz >= tuneMin && hz <= tuneMax }
  /// True when a bookmark at `hz` belongs on the current server at all (broad coverage).
  func inCoverage(_ hz: Double) -> Bool { coverMax > coverMin && hz >= coverMin && hz <= coverMax }
  /// How the watch is reaching the server — mirrored from the client's NWPathMonitor. Drives
  /// the connection-method glyph. Republished (not driverTick-mirrored) so it updates even
  /// before the first row lands, e.g. while still connecting on wifi.
  @Published var transport: Transport = .none
  @Published var snr = 0.0
  /// The spike has no server-supplied meter string (that was a phone/OWRX/FM-DX concept).
  /// STUB: blank → the readout shows "—". NOTE for later: could derive an S-meter from the
  /// DSP's own level.
  @Published var meter = ""
  /// Which UNIT the meter reads in. Set from the Display menu; three ways of saying the same
  /// signal, and which one is "right" depends entirely on the operator — a DXer wants S-units,
  /// someone setting squelch wants SNR, someone chasing overload wants dBFS.
  @AppStorage("meterUnit") var meterUnit = "snr" { didSet { meter = "" } }

  /// Format the meter readout in the chosen unit.
  ///
  /// dBFS and S-units need an ABSOLUTE level, which only radiod's per-packet baseband power gives
  /// us; until the first audio packet lands (or on a backend that doesn't carry it) `dbfs` is NaN
  /// and there is no honest absolute number, so both fall back to the SNR reading rather than
  /// inventing one from a ratio.
  static func meterText(unit: String, snrDb: Double, dbfs: Double) -> String {
    let snrTxt = "\(Int(snrDb.rounded()))dB"
    guard unit != "snr", !dbfs.isNaN else { return snrTxt }
    if unit == "dbfs" { return "\(Int(dbfs.rounded()))dBFS" }
    return sUnit(dbfs)
  }

  /// dBFS → S-unit, 6 dB per unit — the same ladder the phone and web client use, so one signal
  /// reads the same number on all three surfaces.
  static func sUnit(_ dbfs: Double) -> String {
    if dbfs >= -73 { return "S9+\(min(60, Int((dbfs + 73) / 6 + 0.5) * 6))" }
    let ladder = [-79.0, -85, -91, -97, -103, -109, -115]
    for (i, t) in ladder.enumerated() where dbfs >= t { return "S\(8 - i)" }
    return "S1"
  }
  /// Smoothed 0..1 meter fill behind the frequency pill. STUB: the spike's DSP does not
  /// surface a normalised level yet, so this stays 0 (empty bar). NOTE for later.
  @Published var level = 0.0
  @Published var mode = ""
  @Published var step = 9_000.0

  /// Always true — we are DIRECT, there is no phone hop to lose.
  @Published var reachable = true
  @Published var everGotRow = false
  @Published var lastRowAt: Date? = nil
  @Published var lastStateAt: Date? = nil
  /// WHY there are no rows. Direct link, so effectively always "live"; the row-gap logic in
  /// ContentView still surfaces a "spectrum stalled" hint if the socket goes quiet.
  @Published var why = "live"
  /// Derived from OUR OWN socket health: 3 while spectrum frames are flowing, 1 when they
  /// have stalled. There is no far (server↔phone) hop to score independently.
  @Published var serverLink = 3
  /// Waterfall rate rung Link Management has settled on: 1 = full, 2 = half, 3 = the emergency
  /// floor. Feeds the link glyph so a compensated-but-poor link never reads as green.
  @Published var throttleRung = 1
  /// ★ Link Management is still working out what this connection will carry (the first few seconds
  /// of a session). ContentView draws the link glyph as INDETERMINATE — cycling and breathing —
  /// rather than showing a bar count it has not earned yet.
  @Published var linkSettling = false
  /// No phone → no boot handshake. Always "ready" so the placeholder shows "Waiting for
  /// signal" on a cold start rather than a phone-setup message.
  @Published var phoneStatus = "ready"

  /// LOCAL stand-ins. The watch has no in-app system-volume control (see the note in the
  /// spike's ContentView / ControlMenu), so these are cosmetic local state the crown/menu
  /// can nudge. NOTE: they do not change actual output loudness — that is Control Centre.
  @Published var volume = 1.0
  @Published var muted = false

  @Published var battery: Double = -1
  /// A plain-English refusal/timeout from the backend (Kiwi full / password / blocked / no
  /// connection). nil = fine. ContentView shows a card so nobody waits on a dead connection.
  @Published var connectError: String? = nil
  /// The backend client's own status string (Kiwi: 'live' / 'registering' / 'reconnecting N: <reason>').
  /// Surfaced small on screen so a mid-session drop's REASON is visible (debug).
  @Published var backendStatus = ""
  /// Non-fatal "heavy server for the phone link" advisory — set only when on the iPhone relay and the
  /// inbound load sits above what that relay reliably carries. nil = nothing to say.
  @Published var bandwidthWarning: String? = nil
  private var relayHeavySince: Double? = nil
  /// OWRX profiles (grouped SDR→profiles) + connected-listener count, mirrored from the client.
  @Published var profiles: [SDRProfile] = []
  @Published var clients = 0

  // ── Shared server chat (OWRX; FM-DX later) ──
  @Published var chatLog: [ChatLine] = []
  @Published var chatActivity = 0
  var supportsChat: Bool { client?.supportsChat ?? false }
  func sendChat(_ text: String) { client?.sendChat(text) }
  /// EXPLICIT profile switch from the profile menu — never automatic (etiquette).
  func selectProfile(_ id: String) { client?.selectProfile(id) }

  // ── Band plan: NONE yet in the spike. Left blank; the label/edges simply don't draw. ──
  @Published var bandName = ""
  /// Live station name (RDS ps on FM) — shown in place of the band label when present.
  @Published var stationName = ""
  /// DAB services in the tuned ensemble + the current speed-fix factor (mirrored from the client).
  @Published var dabProgrammes: [DabProgramme] = []
  @Published var dabScale: Double = 1.0
  @Published var dabActiveId: Int = -1
  @Published var dabEnsembleName: String = ""
  /// ADS-B decoded aircraft (mirrored from the client).
  @Published var aircraft: [Aircraft] = []
  @Published var receiverLat: Double? = nil       // SDR site → ADS-B map centre + home marker
  @Published var receiverLon: Double? = nil

  /// FM-DX tuner state (mirrored from the client). nil = not on an FM-DX server.
  @Published var fmdx: FmdxInfo? = nil
  var isFmDx: Bool { client is FmDxClient }
  /// The FM-DX client, when we're on one — the server-settings menu drives it directly (antenna,
  /// cEQ, iMS) rather than widening SDRClient for controls only one backend has.
  var fmdxClient: FmDxClient? { client as? FmDxClient }

  /// The VibeServer radio, if we're on one — the client that OWNS the dongle and accepts hardware controls
  /// (gain / bias-T / AGC / PPM / sample rate). The hold-menu's "Radio" sub-sheet observes it directly, so
  /// the hardware controls don't have to be mirrored through the whole SDRClient protocol.
  var vibe: UberClient? { if let u = client as? UberClient, u.isVibe { return u }; return nil }
  var hasHardwareControls: Bool { vibe != nil }

  /// LEARNED station memory for the FM-DX dial. On the phone this was built as you tuned; the watch
  /// COMPANION piggybacked off the phone over WCSession. The standalone spike has no phone, so it learns
  /// its OWN — PS name keyed by 100 kHz channel — and persists it so the dial fills in over time.
  @Published var stations: [LearnedStation] = LearnedStation.load()
  private func learnStation(_ i: FmdxInfo) {
    let name = i.ps.trimmingCharacters(in: .whitespaces)
    guard i.freq > 0, name.count >= 2 else { return }              // need a real PS to learn
    let ch = (i.freq / 100_000).rounded() * 100_000                 // snap to the FM raster
    if let idx = stations.firstIndex(where: { $0.freqHz == ch }) {
      if stations[idx].name != name { stations[idx].name = name; LearnedStation.save(stations) }
    } else {
      stations.append(LearnedStation(freqHz: ch, name: name))
      LearnedStation.save(stations)
    }
  }

  /// Which top-level screen to show. DAB, ADS-B and FM-DX are their own full screens (no waterfall),
  /// exactly as the companion routes it.
  enum Screen { case sdr, dab, adsb, fmdx }
  var screen: Screen {
    if client is FmDxClient { return .fmdx }
    // Route on the ACTUAL demod — the source of truth. Using `!aircraft.isEmpty` too meant a reconnect
    // that landed back on FM still showed the ADS-B screen (stale list) over an FM demod.
    if mode == "dab", !dabProgrammes.isEmpty { return .dab }
    if mode == "adsb" { return .adsb }
    return .sdr
  }
  @Published var bandColor: Color? = nil
  @Published var bandLo = 0.0
  @Published var bandHi = 0.0

  // ── VFO + filter, from the phone's picker originally; here, sensible defaults. ──
  /// Filter passband edges as Hz offsets from the carrier. The spike's DSP does not report
  /// them, so the dashed passband edges are suppressed (lo == hi). NOTE for later.
  @Published var filtLo = 0.0
  @Published var filtHi = 0.0
  /// VFO needle colour — ORANGE by default (#FF8C00). The user will fine-tune the exact hex
  /// on device.
  @Published var needle = Color(hex: "#FF8C00") ?? .orange
  /// Needle intensity 1…10; 7 is a slightly-brighter-than-stock starting point.
  @Published var needleI = 7.0
  @Published var peakHold = true

  /// Input-aware display unit, exactly as the companion.
  @Published var displayUnit: DisplayUnit = .auto {
    didSet { UserDefaults.standard.set(displayUnit.rawValue, forKey: "vibe.displayUnit") }
  }
  enum DisplayUnit: String { case auto, hz, khz, mhz }

  private var lastRowsPushed = 0
  private var batteryTimer: Timer?
  private var stateTick = 0

  init() {
    // Sonar Green by default, baked into the buffer before the client draws anything.
    waterfall.setLUT(Self.sonarGreenLUT)
    waterfall.peakHold = true
    if let raw = UserDefaults.standard.string(forKey: "vibe.displayUnit"),
       let u = DisplayUnit(rawValue: raw) {
      displayUnit = u
    }
    updateBand()
  }

  // ── Transport glyph — one path monitor here (client-agnostic; the network interface the watch
  // is using is the same whichever backend we talk to). Was on UberClient; lifted so KiwiClient
  // doesn't have to duplicate it. ──
  private let pathMonitor = NWPathMonitor()
  private let pathQueue = DispatchQueue(label: "spikelink.path")
  private func startPathMonitor() {
    pathMonitor.pathUpdateHandler = { [weak self] path in
      // Order matters. The watch↔phone companion bridge advertises BOTH `.other` AND `.wifi`, so a
      // naive `.wifi`-first check shows WiFi whenever the phone is near even though every byte is
      // actually egressing through the relay. `.other` present = the phone link is up and IS the route,
      // so it wins; WiFi/cellular only mean direct egress once the relay is truly gone (phone off/away).
      let tr: Transport
      if path.status != .satisfied { tr = .none }
      else if path.usesInterfaceType(.other) { tr = .iphone }
      else if path.usesInterfaceType(.wifi) { tr = .wifi }
      else if path.usesInterfaceType(.cellular) { tr = .cellular }
      else { tr = .none }
      Task { @MainActor in if self?.transport != tr { self?.transport = tr } }
    }
    pathMonitor.start(queue: pathQueue)
  }

  /// Band label + boundary edges for the ticker, from the tuned frequency (Region 1 HF plan).
  private func updateBand() {
    let b = BandPlan.band(for: frequency)
    bandName = b?.name ?? ""
    bandColor = b?.color
    bandLo = b?.lo ?? 0
    bandHi = b?.hi ?? 0
  }

  /// A sensible tuning step for a demod + frequency, adopted on a mode/profile change (mirrors the
  /// phone's mode+step conventions). Must be a member of the STEP menu's options.
  static func defaultStep(mode: String, hz: Double) -> Double {
    switch mode {
    case "wfm":            return 100_000                          // FM broadcast
    case "fm":             return hz > 30_000_000 ? 12_500 : 1_000 // VHF/UHF NFM channels vs HF
    case "am", "sam":
      if hz > 108_000_000, hz < 137_000_000 { return 25_000 }     // airband
      if hz < 1_705_000                       { return 9_000 }     // MW broadcast (Region 1)
      return 1_000                                                 // SW broadcast
    case "usb", "lsb":     return 1_000
    case "cwu", "cwl":     return 100
    case "dab":            return 1_000
    default:               return 1_000
    }
  }

  // ── Lifecycle ──────────────────────────────────────────────────────────────

  /// The server the picker chose, shown in the UI. Empty = show the picker.
  @Published var serverName = ""
  private var booted = false

  /// Start (or switch to) a chosen server from the picker. Builds the backend client for the
  /// server's type (UberSDR or KiwiSDR), tearing down any previous one. One-time boot (battery +
  /// path monitor) runs on the first pick.
  func start(url: String, host: String, type: ServerType, name: String, pin: String = "") {
    serverName = name
    client?.goIdle()
    everGotRow = false
    lastRowsPushed = 0

    let c: any SDRClient
    switch type {
    case .kiwi:
      let k = KiwiClient(url: url, waterfall: waterfall)
      // Set BEFORE start(): the first wf_speed goes out during the handshake.
      k.onRelay = (transport == .iphone)
      c = k
    case .owrx:
      c = OwrxClient(url: url, waterfall: waterfall)
    case .fmdx:
      c = FmDxClient(url: url)
    case .vibeserver:
      // VibeServer = the shim's UberSDR-style server → the UberClient in VibeServer mode (LAN ws://, PIN,
      // ADPCM audio). `host` is host:port; scheme from the url (https/wss → secure).
      let u = UberClient(waterfall: waterfall)
      u.isVibe = true
      u.secure = url.hasPrefix("https") || url.hasPrefix("wss")
      u.host = host
      u.vibePin = pin
      c = u
    default:  // .ubersdr
      let u = UberClient(waterfall: waterfall)
      u.host = host
      c = u
    }
    client = c
    // Clear cross-backend state so nothing from the previous server lingers (e.g. OWRX's profiles menu
    // showing over an FM-DX session). driverTick re-mirrors from the new client on the next tick anyway.
    profiles = []; clients = 0; chatLog = []; chatActivity = 0
    dabProgrammes = []; aircraft = []; fmdx = nil; stationName = ""
    connectError = nil
    frequency = c.frequency
    mode = c.mode
    updateBand()

    if !booted { booted = true; startBatteryMonitor(); startPathMonitor() }
    c.start()
  }

  /// Back out to the instance picker (the menu's SERVERS tile). Drops the sockets/audio.
  func backToPicker() {
    client?.goIdle()
    serverName = ""
  }

  /// Called from the ported ContentView's 20fps driver tick. Drains the audio-synced
  /// spectrum delay queue (the spike's own row cadence — MUST run on the main actor) and
  /// mirrors the client's state onto our published surface.
  func driverTick(now: Double) {
    guard let client else { return }
    client.drainSpectrum(now: now)
    if connectError != client.lastError { connectError = client.lastError }
    if backendStatus != client.status { backendStatus = client.status }
    // ★ NEVER let a 0 blank the count. While we are CONNECTED, 0 is not a truthful listener
    // count — we are ourselves a client — so it only ever means "the server has not told us
    // yet". OWRX sends `clients` on CHANGE, so after a reconnect or a profile switch there may
    // be no message until somebody joins or leaves, and the glyph would sit blank for minutes
    // while the chat sheet (opened later, after an update arrived) showed the real number.
    // Hold the last known value instead; goIdle() resets it to 0 on a real disconnect.
    if client.clients > 0, clients != client.clients { clients = client.clients }
    if profiles != client.profiles { profiles = client.profiles }
    if stationName != client.stationName { stationName = client.stationName }
    if dabProgrammes != client.dabProgrammes { dabProgrammes = client.dabProgrammes }
    if dabScale != client.dabScale { dabScale = client.dabScale }
    if dabActiveId != client.dabActiveId { dabActiveId = client.dabActiveId }
    if dabEnsembleName != client.dabEnsembleName { dabEnsembleName = client.dabEnsembleName }
    if aircraft != client.aircraft { aircraft = client.aircraft }
    if receiverLat != client.receiverLat { receiverLat = client.receiverLat }
    if receiverLon != client.receiverLon { receiverLon = client.receiverLon }
    if chatActivity != client.chatActivity { chatActivity = client.chatActivity }
    if chatLog != client.chatLog { chatLog = client.chatLog }
    if fmdx != client.fmdxInfo { fmdx = client.fmdxInfo; if let i = client.fmdxInfo { learnStation(i) } }

    if frequency != client.frequency { frequency = client.frequency; updateBand() }
    if mode != client.mode {
      mode = client.mode
      // Adopt a mode/band-appropriate tuning step on a demod change (profile switch or manual mode).
      // Matches the phone's mode+step pairing; the user can still override via the STEP menu after.
      step = Self.defaultStep(mode: mode, hz: client.frequency)
    }
    let sp = client.displaySpanHz   // the on-screen width, held across a reconnect (no snap)
    if span != sp { span = sp }
    // Bookmark gating ranges (see canTune/inCoverage). Cheap to mirror; changes rarely.
    if tuneMin  != client.tuneMinHz  { tuneMin  = client.tuneMinHz  }
    if tuneMax  != client.tuneMaxHz  { tuneMax  = client.tuneMaxHz  }
    if coverMin != client.coverMinHz { coverMin = client.coverMinHz }
    if coverMax != client.coverMaxHz { coverMax = client.coverMaxHz }
    // Passband edges → the VFO's dashed LSB/USB lines (drawVFO), and the bandwidth UI.
    if filtLo != client.bwLow { filtLo = client.bwLow }
    if filtHi != client.bwHigh { filtHi = client.bwHigh }
    // Signal meter — bar fill + SNR text, computed for free by the spectrum DSP. Round the
    // text so it doesn't invalidate the view on sub-dB jitter.
    if abs(level - client.signalLevel) > 0.005 { level = client.signalLevel }
    let mt = Self.meterText(unit: meterUnit, snrDb: client.signalDb, dbfs: client.signalDbfs)
    if meter != mt { meter = mt }
    let sn = min(1, max(0, client.signalDb / 50))   // signal on the needle's 0..1 scale (pos*50 dB)
    if abs(sqlSignal - sn) > 0.004 { sqlSignal = sn }

    // A new row was drawn → the spectrum is alive.
    if client.rowsPushed != lastRowsPushed {
      lastRowsPushed = client.rowsPushed
      lastRowAt = Date()
      if !everGotRow { everGotRow = true }
    }

    // Our own socket-health score, standing in for the phone's link meter.
    let sl = client.framesPerSec > 0 ? 3 : (everGotRow ? 1 : 3)
    if serverLink != sl { serverLink = sl }

    // THROTTLE RUNG — how far Link Management has had to back off (1 = full rate).
    //
    // The glyph must not go green just because we successfully compensated. Once the ladder
    // steps down, frames arrive punctually again and every gap-based signal reads HEALTHY —
    // while the user is looking at a jerkier waterfall than they asked for. That is the link
    // being bad, not good, and the indicator has to say so or it is lying by omission.
    // ADAPTIVE rung, not the wire rate: a rate the USER pinned (Low Data mode, for a metered
    // plan) is a preference, not a symptom, and must never show a permanent red link.
    let rung = client.adaptiveRung
    if throttleRung != rung { throttleRung = rung }
    let settling = client.linkSettling
    if linkSettling != settling { linkSettling = settling }

    // Heavy-server advisory — only meaningful on the iPhone relay (own wifi/cellular has headroom).
    //
    // ★★ THE OLD 55 KB/s THRESHOLD COULD NEVER FIRE, and that is why Stuart only ever saw the
    //    disconnect. `inboundKbPerSec` measures what ARRIVED, and the relay's real-world ceiling is
    //    ~25-62 KB/s (200-500 kbps). A server demanding 70 KB/s does not deliver 70 — it delivers
    //    whatever the link manages, ~30, and then drops. So the meter reads BELOW the threshold
    //    precisely when the link is failing. We were watching DEMAND through a window that only
    //    shows DELIVERY, and delivery is capped by the very thing we wanted to detect.
    //
    //    ★ 25 KB/s, and this is now MEASURED rather than guessed (Stuart's OWRX, 2026-07-20):
    //
    //        WFM/DAB audio alone   23.8 KB/s   (195 kbps — and it CANNOT be lowered, see OwrxClient)
    //        FFT / waterfall       18-53 KB/s  (also cannot be lowered)
    //        together              42-77 KB/s  against a Bluetooth ceiling of ~25-62 KB/s
    //
    //      So on this class of profile the audio ON ITS OWN reaches the bottom of what the relay
    //      can carry. 25 is the point where "you are at the limit of Bluetooth" stops being a
    //      prediction and becomes a description — and it is comfortably reachable by the meter,
    //      which the old 55 never was (delivery is capped by the very thing we want to detect).
    //
    //      Deliberately BELOW the earlier 40: the warning has to arrive while there is still
    //      something the user can do about it, and there is no lever left inside the app.
    let kb = client.inboundKbPerSec
    let relayHeavy = transport == .iphone && kb > 25

    // ★ AND THE RECONNECT ITSELF IS EVIDENCE. A reconnect while on the relay is the symptom the
    //   user actually experiences, and it needs no threshold to be believed — if we are dropping
    //   and re-establishing over Bluetooth, the link is the problem whatever the meter says. This
    //   is the case a throughput test structurally cannot catch, because a dropped socket delivers
    //   0 KB/s.
    let relayDropping = transport == .iphone && client.status.hasPrefix("reconnect")

    if relayHeavy || relayDropping {
      if relayHeavySince == nil { relayHeavySince = now }
      // Dropping needs no dwell — it has already happened. Heaviness waits ~4s so a spike cannot nag.
      if let st = relayHeavySince, relayDropping || now - st >= 4, bandwidthWarning == nil {
        bandwidthWarning = relayDropping
          ? "Too heavy for the phone\u{2019}s Bluetooth link — it keeps dropping. Switch to the watch\u{2019}s own Wi-Fi or cellular."
          : "At the limit of the phone\u{2019}s Bluetooth link (~\(kb) KB/s). Use the watch\u{2019}s own Wi-Fi or cellular for this server."
      }
    } else {
      relayHeavySince = nil
      // Hysteresis, so a server sitting right on the line cannot flicker the pill on and off.
      if bandwidthWarning != nil, transport != .iphone || kb <= 18 { bandwidthWarning = nil }
    }

    // Surface a RECONNECT so the UI shows the "Reconnecting" pill, not the hard "link lost"
    // overlay (a phone-companion concept — there's no watch↔phone link to lose here). We're
    // recovering whenever frames have stopped after having flowed, and we're NOT intentionally
    // backgrounded (wrist-down keeps the audio and drops the waterfall on purpose).
    let recovering = everGotRow && client.framesPerSec == 0 && !isBackground
    let newWhy = recovering ? "reconnecting" : "live"
    if why != newWhy { why = newWhy }

    // The "state" channel is always fresh on a direct link — touch it about once a second
    // so ContentView's hint debouncer has a clock even when rows have stopped.
    stateTick += 1
    if stateTick >= 20 { stateTick = 0; lastStateAt = Date() }
  }

  func ping() { /* no phone to announce ourselves to */ }

  /// True while the spectrum has been intentionally dropped for wrist-down (audio keeps
  /// playing). Used by the scene handler to tell a real suspend from a quick glance-away.
  var isBackground: Bool { client?.status.hasPrefix("background") ?? false }

  // ── Scene lifecycle passthroughs (the spike's socket watchdog) ──────────────
  func resume() { client?.resumeSpectrum() }
  func reconnectIfNeeded() { client?.reconnectIfNeeded() }
  func suspend() { client?.suspend() }

  // ── Controls the ported views call ──────────────────────────────────────────
  // Tuning COALESCES crown detents over 100ms rather than firing the client per detent.
  // Stuart prefers the heavier, deliberate feel: the readout advances in step with the tune
  // that actually happens, instead of predicting instantly per detent and then visibly
  // JUMPING when the server echo corrects a guess. 100ms matches the server tune rate
  // (UberClient sendTuneThrottled) and Buddy's crown coalesce, so the two apps feel the same.
  private var pendingTune = 0
  private var tuneFlushScheduled = false

  func tune(delta: Int) {
    pendingTune += delta
    guard !tuneFlushScheduled else { return }
    tuneFlushScheduled = true
    // asyncAfter, NOT Timer: while the crown turns the run loop is in TRACKING mode and a
    // default-mode timer would never fire (see WatchLink.scheduleFlush for the full story).
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
      guard let self else { return }
      self.tuneFlushScheduled = false
      guard self.pendingTune != 0 else { return }
      let d = self.pendingTune; self.pendingTune = 0
      self.client?.tune(delta: d, step: self.step)
      self.frequency = self.client?.frequency ?? self.frequency
    }
  }

  func zoom(delta: Int) { client?.zoom(delta: delta) }

  /// LOCAL volume nudge — cosmetic (see `volume`). One detent = one 1/16 step, matching the
  /// companion's quantisation so the meter feels the same.
  func volume(delta: Int) {
    volume = min(1, max(0, volume + Double(delta) / 16))
    if !muted { client?.setVolume(volume) }   // drives the engine's real output gain
  }

  func setMuted(_ m: Bool) {
    muted = m
    client?.setVolume(m ? 0 : volume)          // real mute/unmute, not just a glyph
  }

  func setMode(_ m: String) {
    client?.setMode(m)
    mode = client?.mode ?? mode
  }

  var isOwrx: Bool { client is OwrxClient }   // for the OWRX-specific tutorial line

  func setStep(_ hz: Double) { step = hz }
  func setDabScale(_ s: Double) { client?.setDabScale(s); dabScale = s }
  func selectDabService(_ id: Int) { client?.selectDabService(id) }

  /// Passband edges (Hz offsets from carrier). Pushed to the server + mirrored to filtLo/filtHi
  /// (which drive the VFO's dashed sideband lines).
  func setBandwidth(_ low: Double, _ high: Double) {
    client?.setBandwidth(low, high)
    filtLo = low; filtHi = high
  }

  /// Crown step per demod: fine for voice (0.1 kHz), coarse for wide FM. Hz.
  func bwStep() -> Double {
    switch mode {
    case "wfm":               return 5_000
    case "fm", "nfm":         return 500
    default:                  return 100     // am/sam/usb/lsb/cw — 0.1 kHz
    }
  }

  /// Symmetric-sideband modes default to SYNC ON (adjusting one edge mirrors the other);
  /// SSB is asymmetric so it defaults OFF. The user can override either way.
  var symmetricMode: Bool { !(mode == "usb" || mode == "lsb") }

  /// Absolute tune, from the numpad.
  func tune(toHz hz: Double) {
    client?.tuneTo(hz)
    frequency = client?.frequency ?? frequency
  }

  // ── Bookmark recall + transient notice pill ──────────────────────────────────
  /// A short-lived notice shown as a pill over the waterfall (bookmark refusals etc). nil = none.
  @Published var notice: String? = nil
  private var noticeTask: Task<Void, Never>? = nil
  func notify(_ text: String, for seconds: Double = 2.5) {
    notice = text
    noticeTask?.cancel()
    noticeTask = Task { @MainActor [weak self] in
      try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
      if !Task.isCancelled { self?.notice = nil }
    }
  }

  /// Recall a bookmark: retune + set demod on the CURRENT server. NEVER switches an OWRX profile —
  /// if the frequency is outside the window tunable right now, refuse and warn rather than clamp.
  func recall(_ b: Bookmark) {
    guard canTune(b.frequency) else { notify("Frequency Range Not Available"); return }
    if !b.mode.isEmpty, b.mode != mode { setMode(b.mode) }
    tune(toHz: b.frequency)
  }

  // ── Battery ─────────────────────────────────────────────────────────────────
  private func startBatteryMonitor() {
    let dev = WKInterfaceDevice.current()
    dev.isBatteryMonitoringEnabled = true
    let read = { [weak self] in
      let lvl = Double(WKInterfaceDevice.current().batteryLevel)
      Task { @MainActor in self?.battery = lvl }
    }
    read()
    let t = Timer(timeInterval: 60, repeats: true) { _ in read() }
    RunLoop.main.add(t, forMode: .common)
    batteryTimer = t
  }

  // ── Waterfall palettes ───────────────────────────────────────────────────────
  /// Expand an evenly-spaced RGB stop list to a 256-entry RGBA LUT by linear interpolation.
  /// (The waterfall buffer maps intensity 0…255 → LUT index → colour.)
  nonisolated static func buildLUT(_ stops: [(Double, Double, Double)]) -> [UInt8] {
    var lut = [UInt8](repeating: 0, count: 256 * 4)
    let segs = Double(stops.count - 1)
    for i in 0..<256 {
      let p = Double(i) / 255.0 * segs
      let s = min(Int(p), stops.count - 2)
      let f = p - Double(s)
      let a = stops[s], b = stops[s + 1]
      lut[i * 4 + 0] = UInt8(max(0, min(255, (a.0 + (b.0 - a.0) * f).rounded())))
      lut[i * 4 + 1] = UInt8(max(0, min(255, (a.1 + (b.1 - a.1) * f).rounded())))
      lut[i * 4 + 2] = UInt8(max(0, min(255, (a.2 + (b.2 - a.2) * f).rounded())))
      lut[i * 4 + 3] = 255
    }
    return lut
  }

  /// The 12-stop Sonar Green gradient from `src/assets/colormaps.ts`.
  static let sonarGreenLUT: [UInt8] = buildLUT(WFPalette.sonar.stops)

  /// The selectable palettes (plus the SYNC sentinel first — see WFPalette.sync). LUTs are built
  /// lazily on first use and cached by the struct.
  static let palettes: [WFPalette] = [.sync, .sonar, .sonarOrange, .nightVision, .classic, .jet, .viridis, .grey, .blackHot]

  /// The selectable VFO colours (SYNC first — falls back to the orange default until iCloud lands).
  static let vfoColours: [VfoColour] = [
    .init(id: "sync",    name: "Sync",    hex: "#FF8C00"),   // shows the fallback colour until iCloud
    .init(id: "orange",  name: "Orange",  hex: "#FF8C00"),
    .init(id: "red",     name: "Red",     hex: "#FF3B30"),
    .init(id: "yellow",  name: "Yellow",  hex: "#FFD60A"),
    .init(id: "green",   name: "Green",   hex: "#34C759"),
    .init(id: "cyan",    name: "Cyan",    hex: "#32ADE6"),
    .init(id: "blue",    name: "Blue",    hex: "#0A84FF"),
    .init(id: "magenta", name: "Magenta", hex: "#FF375F"),
    .init(id: "white",   name: "White",   hex: "#FFFFFF"),
  ]

  /// Apply a saved palette choice to the waterfall. `sync` resolves to the phone's palette once the
  /// iCloud layer exists; until then it falls back to Sonar Green (the historical default).
  func applyPalette(_ id: String) {
    let chosen = id == "sync" ? "sonar" : id            // TODO(iCloud): map sync → phone's palette
    let p = SpikeLink.palettes.first { $0.id == chosen } ?? .sonar
    waterfall.setLUT(p.lut)
  }

  /// Apply a saved VFO-colour choice. `sync` → phone's colour later; orange fallback for now.
  func applyVfo(_ id: String) {
    let v = SpikeLink.vfoColours.first { $0.id == id } ?? SpikeLink.vfoColours[1]
    needle = Color(hex: v.hex) ?? .orange
  }

  /// Push the DSP auto-contrast (0–20) to the active backend. Re-asserted on connect/reconnect
  /// because each client seeds its own default (5) at start — see ContentView.applyTone.
  func setAutoContrast(_ v: Double) { client?.setAutoContrast(v) }

  // ── Squelch ─────────────────────────────────────────────────────────────────
  // The GATE is SERVER-SIDE (client.setSquelch → radiod's audio gate) — proven to mute correctly, same
  // as the phone/web. The bar+needle is just how you SET it visually: the blank bar is driven off Jr's
  // live signal reading, the red needle off the squelch value. `sql` (0..1) is the needle position; the
  // indicator's CLOSED state is derived (level < needle), like the phone.
  @Published var sql = -1.0
  /// The signal on the SAME 0..1 scale as the needle (signalDb / 50) — so the squelch bar and needle
  /// line up (the meter's own `level` is a compressed fill pinned near full, useless for this).
  @Published var sqlSignal = 0.0

  /// `pos`: 0..1 needle position on the signal bar; < 0 = off. Maps to a 0..50 dB SNR for the server.
  func setSquelch(_ pos: Double) {
    sql = pos < 0 ? -1 : min(1, pos)
    client?.setSquelch(sql < 0 ? -999 : sql * 50)
  }

  /// Keep the signal bar live while the squelch SHEET covers the waterfall (its 20fps render driver
  /// pauses when hidden, freezing `level`). The editor ticks this ~15fps so the bar bounces.
  func pollSignal() {
    client?.drainSpectrum(now: ProcessInfo.processInfo.systemUptime)
    if let l = client?.signalLevel, abs(level - l) > 0.003 { level = l }
    if let db = client?.signalDb {
      let sn = min(1, max(0, db / 50))
      if abs(sqlSignal - sn) > 0.004 { sqlSignal = sn }
    }
  }
}

/// A selectable waterfall palette: an id, a display name, and the gradient stops (used both to build
/// the 256-entry LUT and to draw the little preview stripes on the Display button).
struct WFPalette: Identifiable {
  let id: String
  let name: String
  let stops: [(Double, Double, Double)]
  var lut: [UInt8] { SpikeLink.buildLUT(stops) }

  /// Build from "#rrggbb" stops copied straight from the phone's `src/assets/colormaps.ts`, so the
  /// watch palettes are the SAME as the phone's (and "Sync" can map by name later).
  init(id: String, name: String, hex: [String]) {
    self.id = id; self.name = name
    self.stops = hex.map { h in
      var s = h; if s.hasPrefix("#") { s.removeFirst() }
      let v = UInt32(s, radix: 16) ?? 0
      return (Double((v >> 16) & 0xff), Double((v >> 8) & 0xff), Double(v & 0xff))
    }
  }

  /// A handful of representative swatches sampled across the gradient, for the button/preview stripes.
  var swatches: [Color] {
    guard stops.count > 1 else { return stops.map { Color(red: $0.0/255, green: $0.1/255, blue: $0.2/255) } }
    let n = 6
    return (0..<n).map { k -> Color in
      let p = Double(k) / Double(n - 1) * Double(stops.count - 1)
      let s = min(Int(p), stops.count - 2); let f = p - Double(s)
      let a = stops[s], b = stops[s + 1]
      return Color(red: (a.0 + (b.0 - a.0) * f)/255,
                   green: (a.1 + (b.1 - a.1) * f)/255,
                   blue: (a.2 + (b.2 - a.2) * f)/255)
    }
  }

  // Stops copied verbatim from the phone's COLORMAPS_STOPS (src/assets/colormaps.ts) so the wrist
  // matches the phone. "Sync" tracks the phone's own choice once iCloud lands; preview = sonar for now.
  static let sync = WFPalette(id: "sync", name: "Sync",
    hex: ["#000000","#000800","#001a00","#003300","#005000","#007800","#00aa00","#00cc00","#00ff00","#80ff80","#ccffcc","#efffff"])
  static let sonar = WFPalette(id: "sonar", name: "Sonar Green",
    hex: ["#000000","#000800","#001a00","#003300","#005000","#007800","#00aa00","#00cc00","#00ff00","#80ff80","#ccffcc","#efffff"])
  static let sonarOrange = WFPalette(id: "sonarorange", name: "Sonar Orange",
    hex: ["#000000","#0d0400","#1a0800","#2e1000","#4a1a00","#6e2800","#9e3c00","#c85000","#e86800","#ff8c20","#ffb860","#fff0cc"])
  static let nightVision = WFPalette(id: "nightvision", name: "Night Vision",
    hex: ["#000000","#0c0000","#1a0000","#2e0000","#4a0000","#700000","#a00000","#cc0000","#ee1010","#ff4040","#ff8080","#ffcccc"])
  static let classic = WFPalette(id: "classic", name: "Classic",
    hex: ["#000020","#000030","#000050","#000091","#1E90FF","#FFFFFF","#FFFF00","#FE6D16","#FE6D16","#FF0000","#FF0000","#C60000","#9F0000","#750000","#4A0000"])
  static let jet = WFPalette(id: "jet", name: "Jet",
    hex: ["#00008f","#0000ff","#00ffff","#ffff00","#ff0000","#800000"])
  // The canonical 10-stop viridis (matplotlib) — perceptually uniform, dark-purple → teal → yellow.
  static let viridis = WFPalette(id: "viridis", name: "Viridis",
    hex: ["#440154","#482878","#3e4a89","#31688e","#26828e","#1f9e89","#35b779","#6ece58","#b5de2b","#fde725"])
  static let grey = WFPalette(id: "grey", name: "Greyscale", hex: ["#000000","#FFFFFF"])
  static let blackHot = WFPalette(id: "blackhot", name: "Black Hot", hex: ["#FFFFFF","#000000"])
}

/// A selectable VFO (tuned-carrier line) colour.
struct VfoColour: Identifiable {
  let id: String
  let name: String
  let hex: String
  var color: Color { Color(hex: hex) ?? .orange }
}

/// "#rrggbb" → Color. Shared by the adapter and the ported views (the companion carried this
/// on WatchLink; here it lives with the adapter).
extension Color {
  init?(hex: String) {
    var s = hex.trimmingCharacters(in: .whitespaces)
    if s.hasPrefix("#") { s.removeFirst() }
    guard s.count == 6, let v = UInt32(s, radix: 16) else { return nil }
    self.init(
      red:   Double((v >> 16) & 0xff) / 255,
      green: Double((v >>  8) & 0xff) / 255,
      blue:  Double( v        & 0xff) / 255
    )
  }
}
