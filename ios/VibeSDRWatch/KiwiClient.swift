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
  var rowsPushed: Int { get }
  var framesPerSec: Double { get }
  var status: String { get }
  /// How far Link Management has had to throttle the waterfall (1 = full rate). Drives the link
  /// glyph. A backend with no rate lever (OWRX) keeps the default 1 via the extension below.
  var adaptiveRung: Int { get }
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
}

// Default-empty so UberSDR/Kiwi don't have to implement the profile surface; OWRX overrides.
extension SDRClient {
  /// OpenWebRX has no waterfall-rate lever at all (fps/fft_fps/fft_size are ignored), so it never
  /// throttles and is never blamed for one.
  var adaptiveRung: Int { 1 }
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
  private static let maxZoom = 14
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
  @Published var framesPerSec: Double = 0
  @Published var status = "starting"
  @Published var lastError: String? = nil
  var rowsPushed = 0
  var displaySpanHz: Double { viewInit ? viewBw : rxBw }

  private var everFrame = false
  private var errorShown = false
  private var connectTimer: Timer?
  private var sockState = "connecting"

  // ── Endpoint ──
  private let wsBase: String     // ws(s)://host:port
  private let secure: Bool
  private let ts = Int(Date().timeIntervalSince1970 * 1000)

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

  // ── Sockets / audio / DSP ──
  // nonisolated so the BACKGROUND keepalive timer can send on them without hopping to main — a
  // main-actor hop would miss whenever the waterfall DSP stalls main, and a missed keepalive is
  // exactly what makes Kiwi drop us. AudioSocket.send is thread-safe (NWConnection.send).
  nonisolated(unsafe) private let sndSock = AudioSocket(name: "kiwi-snd")
  nonisolated(unsafe) private let wfSock  = AudioSocket(name: "kiwi-wf")
  nonisolated(unsafe) let waterfall: WaterfallBuffer
  private let proc = SignalProcessor()
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
    let t = Timer(timeInterval: 1, repeats: true) { [weak self] _ in
      Task { @MainActor in guard let self else { return }
        self.framesPerSec = Double(self.frameCount)
        if self.frameCount > 0, self.retries > 0 { self.retries = 0 }   // stable again → reset backoff
        self.frameCount = 0
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
        self.fail("No data from this KiwiSDR after 12s.\n[state: \(self.sockState)]")
      }
    }
    RunLoop.main.add(ct, forMode: .common); connectTimer = ct

    audio.start { _, _ in }
    openSnd()
  }

  /// Surface a refusal reason ONCE (badp/too_busy/handshake/timeout all funnel here).
  private func fail(_ msg: String) {
    guard !errorShown, !everFrame else { return }
    errorShown = true
    lastError = msg
    status = "refused"
    goIdle()
  }

  private func markFrame() {
    if !everFrame { everFrame = true; connectTimer?.invalidate(); connectTimer = nil }
  }

  // ── Reconnect on a mid-session drop (UberClient's proven pattern) ──
  private var retries = 0
  private var retrying = false
  @Published var dropReason = ""     // shown so we can see WHY Kiwi keeps dropping (failed vs recv)
  private func retrySnd(reason: String = "") {
    guard !retrying, !goingIdle else { return }
    retrying = true
    retries += 1
    if !reason.isEmpty { dropReason = reason }
    status = "reconnect \(retries) ka=\(kaCount): \(dropReason)"
    sndSock.cancel(); wfSock.cancel()
    sndAuthed = false; wfAuthed = false; wfOpened = false
    let wait = UInt64(min(retries, 5)) * 1_500_000_000   // 1.5s → 7.5s, then hold
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
            self.fail("This KiwiSDR wouldn’t open a connection.\n[\(st)]")
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
        self.wfSend("SET wf_speed=4")
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
    audio.play(pcm: pcm, rate: rate, channels: 1)
  }

  // ── Waterfall (W/F binary) ──
  private var out256 = [UInt8]()
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
    let row = proc.process(floats, centerHz: viewCenter, bwHz: viewBw)
    signalLevel = proc.level
    let dec = decimate(row, to: WaterfallBuffer.width)
    if dec.count == WaterfallBuffer.width {
      rowsPushed += 1
      specQueue.append((ProcessInfo.processInfo.systemUptime, dec))
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

  private func decimate(_ row: [UInt8], to width: Int) -> [UInt8] {
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
    self?.wfSend("SET wf_speed=\(5 - rung)")     // rung 1→4, 2→3, 3→2
    self?.waterfall.setExpectedRowRate(fps)
  }
  var adaptiveRung: Int { linkMgr.adaptiveRung }

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
  func resumeSpectrum() { wfSend("SET wf_speed=4") }
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
