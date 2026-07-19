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
  /// How the watch is reaching the server — mirrored from the client's NWPathMonitor. Drives
  /// the connection-method glyph. Republished (not driverTick-mirrored) so it updates even
  /// before the first row lands, e.g. while still connecting on wifi.
  @Published var transport: Transport = .none
  @Published var snr = 0.0
  /// The spike has no server-supplied meter string (that was a phone/OWRX/FM-DX concept).
  /// STUB: blank → the readout shows "—". NOTE for later: could derive an S-meter from the
  /// DSP's own level.
  @Published var meter = ""
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
      c = KiwiClient(url: url, waterfall: waterfall)
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
    // Passband edges → the VFO's dashed LSB/USB lines (drawVFO), and the bandwidth UI.
    if filtLo != client.bwLow { filtLo = client.bwLow }
    if filtHi != client.bwHigh { filtHi = client.bwHigh }
    // Signal meter — bar fill + SNR text, computed for free by the spectrum DSP. Round the
    // text so it doesn't invalidate the view on sub-dB jitter.
    if abs(level - client.signalLevel) > 0.005 { level = client.signalLevel }
    let mt = "\(Int(client.signalDb.rounded()))dB"
    if meter != mt { meter = mt }

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

    // Heavy-server advisory. Only meaningful on the iPhone relay (own wifi/cellular has the headroom).
    // Most servers stream ~18-40 KB/s and never trip this; only a deliberately-cranked FFT (like an
    // 8192@20 box) sits above ~55. Require it SUSTAINED ~4s so a momentary spike doesn't nag; clear
    // when the load drops or the watch moves off the relay.
    let kb = client.inboundKbPerSec
    if transport == .iphone, kb > 55 {
      if relayHeavySince == nil { relayHeavySince = now }
      if let s = relayHeavySince, now - s >= 4, bandwidthWarning == nil {
        bandwidthWarning = "Heavy server (~\(kb) KB/s) — may stutter over the phone. Best on the watch’s own Wi-Fi."
      }
    } else {
      relayHeavySince = nil
      if bandwidthWarning != nil, transport != .iphone || kb <= 45 { bandwidthWarning = nil }
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
  func tune(delta: Int) {
    client?.tune(delta: delta, step: step)
    frequency = client?.frequency ?? frequency
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

  // ── Sonar Green LUT ──────────────────────────────────────────────────────────
  /// The 12-stop Sonar Green gradient from `src/assets/colormaps.ts`, expanded to a 256-entry
  /// RGBA LUT by linear interpolation across the evenly-spaced stops.
  static let sonarGreenLUT: [UInt8] = {
    let stops: [(Double, Double, Double)] = [
      (0x00, 0x00, 0x00), (0x00, 0x08, 0x00), (0x00, 0x1a, 0x00), (0x00, 0x33, 0x00),
      (0x00, 0x50, 0x00), (0x00, 0x78, 0x00), (0x00, 0xaa, 0x00), (0x00, 0xcc, 0x00),
      (0x00, 0xff, 0x00), (0x80, 0xff, 0x80), (0xcc, 0xff, 0xcc), (0xef, 0xff, 0xff),
    ]
    var lut = [UInt8](repeating: 0, count: 256 * 4)
    let segs = Double(stops.count - 1)
    for i in 0..<256 {
      let p = Double(i) / 255.0 * segs
      let s = min(Int(p), stops.count - 2)
      let f = p - Double(s)
      let a = stops[s], b = stops[s + 1]
      let r = a.0 + (b.0 - a.0) * f
      let g = a.1 + (b.1 - a.1) * f
      let bl = a.2 + (b.2 - a.2) * f
      lut[i * 4 + 0] = UInt8(max(0, min(255, r.rounded())))
      lut[i * 4 + 1] = UInt8(max(0, min(255, g.rounded())))
      lut[i * 4 + 2] = UInt8(max(0, min(255, bl.rounded())))
      lut[i * 4 + 3] = 255
    }
    return lut
  }()
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
