import Foundation
import Combine

/// A learned FM station for the dial — PS name at a 100 kHz channel. Persisted in UserDefaults so the
/// spike's dial fills in over time (the standalone equivalent of the phone's RDS memory).
struct LearnedStation: Identifiable, Equatable, Codable {
  var id: Double { freqHz }
  let freqHz: Double
  var name: String

  private static let key = "vibe.fmdx.stations"
  static func load() -> [LearnedStation] {
    guard let d = UserDefaults.standard.data(forKey: key),
          let s = try? JSONDecoder().decode([LearnedStation].self, from: d) else { return [] }
    return s
  }
  static func save(_ s: [LearnedStation]) {
    // Cap so a long DX session can't grow it without bound (nearest-tuned wins on the dial anyway).
    let capped = Array(s.suffix(400))
    if let d = try? JSONEncoder().encode(capped) { UserDefaults.standard.set(d, forKey: key) }
  }
}

/// A snapshot of the FM-DX Webserver's tuner state — one whole-state JSON frame off the `/text` socket,
/// flattened into what the tuner screen reads. Mirrors the companion's `WatchLink.FmdxState`.
struct FmdxInfo: Equatable {
  var freq: Double = 0        // Hz (server sends MHz)
  var users: Int = 0
  var level: Double = 0       // 0…1 bar fill, derived from dBf
  var meter: String = ""      // "12.3 dBf"
  var ps: String = ""         // programme service (station) name
  var rt: String = ""         // current RadioText bank
  var pi: String = ""
  var pty: String = ""
  var stereo = false
  var rds = false
  var tx: String = ""         // transmitter/station name
  var city: String = ""
  var dist: Double = 0        // km from the receiver
  var rx: String = ""         // the receiver's own name (origin of `dist`)
  var flag: String = ""       // country flag emoji
  // ── Server-side controls (FM-DX Webserver). Mirrors the phone's FmdxAdapter. ──
  var eq = false              // cEQ filter
  var ims = false             // iMS (multipath suppression)
  var antenna = 0             // currently selected antenna (0-based, matches the `Z` command)
  /// Antennas this server advertises. EMPTY = no switch (single antenna, or the owner disabled it),
  /// in which case the control must not be shown at all — the same rule as OWRX's lockedRate.
  var antennas: [FmdxAntenna] = []
}

/// One selectable antenna on an FM-DX server. Keys arrive as `antN` (1-based) but the `Z` command
/// and the `ant` state field are 0-based, so `id` is N-1.
struct FmdxAntenna: Identifiable, Equatable {
  let id: Int
  let name: String
}

/// FM-DX Webserver client. Unlike the SDR backends there is NO spectrum — the server does all demod +
/// RDS and streams a JSON tuner state plus a 3LAS MP3 audio feed. So this client pushes no waterfall
/// rows; its "content" is `info` (station/RDS/signal), surfaced on the dedicated FmdxView.
///
/// Three sockets: `/text` (control + whole-state JSON), `/chat`, `/audio` (raw MP3 frames). STEREO: the
/// MP3 decoder emits interleaved L/R and we hand `channels: 2` straight to WatchAudio — the first stereo
/// path on the wrist, and the one VibeServer's full WFM stereo will reuse.
final class FmDxClient: SDRClient {
  // ── SDRClient surface ──
  @Published var frequency: Double = 0
  @Published var mode = "WFM"           // FM-DX is WFM broadcast only
  @Published var signalLevel: Double = 0
  @Published var signalDb: Double = 0
  @Published var status = "starting"
  @Published var lastError: String? = nil
  @Published var clients: Int = 0
  @Published var stationName = ""
  @Published var chatLog: [ChatLine] = []
  @Published var chatActivity = 0
  var supportsChat: Bool { true }

  // ── FM-DX specific ──
  @Published var info = FmdxInfo()
  var fmdxInfo: FmdxInfo? { info }

  // No spectrum on FM-DX.
  var rowsPushed = 0
  var framesPerSec: Double = 0
  var displaySpanHz: Double { 0 }
  var bwLow: Double { 0 }
  var bwHigh: Double { 0 }

  // ── Sockets ──
  nonisolated(unsafe) private let textSock = AudioSocket(name: "fmdx-text")
  nonisolated(unsafe) private let chatSock = AudioSocket(name: "fmdx-chat")
  nonisolated(unsafe) private let audioSock = AudioSocket(name: "fmdx-audio")
  nonisolated(unsafe) private let audio = WatchAudio()
  nonisolated(unsafe) private let audioQueue = DispatchQueue(label: "fmdx.audio")
  nonisolated(unsafe) private let decoder = FmdxMp3Decoder()

  private let base: String              // normalised host[:port], no scheme, no trailing slash
  private let secure: Bool
  private var goingIdle = false
  private var lastKhz = 0               // for delta tuning off the current freq
  private var rxName = ""               // receiver name from /static_data (kept across state frames)

  init(url: String) {
    // Accept http(s)://host[:port][/...] or bare host[:port]; strip scheme + trailing slashes.
    var s = url.trimmingCharacters(in: .whitespaces)
    var sec = true
    if s.hasPrefix("https://") { s.removeFirst(8); sec = true }
    else if s.hasPrefix("http://") { s.removeFirst(7); sec = false }
    else if s.hasPrefix("wss://") { s.removeFirst(6); sec = true }
    else if s.hasPrefix("ws://") { s.removeFirst(5); sec = false }
    if let slash = s.firstIndex(of: "/") { s = String(s[..<slash]) }
    self.base = s
    self.secure = sec
  }

  private func wsURL(_ path: String) -> URL {
    URL(string: "\(secure ? "wss" : "ws")://\(base)\(path)")!
  }

  func start() {
    status = "connecting"
    audio.start { _, _ in }

    // The MP3 decoder emits interleaved LE Int16 PCM with the stream's OWN rate + channel count.
    // Hand it straight to WatchAudio — channels:2 for a stereo broadcast is passed through untouched
    // and the audio path converts to the output route (mono speaker / stereo AirPods).
    decoder.onPcm = { [weak self] data, channels, rate in
      guard let self else { return }
      let pcm = data.withUnsafeBytes { raw -> [Int16] in
        Array(raw.bindMemory(to: Int16.self))
      }
      self.audio.play(pcm: pcm, rate: Int32(rate), channels: Int32(channels))
    }

    openText()
    openChat()
    openAudio()
    fetchStaticData()
  }

  // The receiver's own name/location (origin of every txInfo.dist) comes from the HTTP /static_data
  // endpoint, NOT the state frames — so fetch it once and keep it. Without this the "to <receiver>"
  // location line has nothing to show.
  private func fetchStaticData() {
    guard let url = URL(string: "\(secure ? "https" : "http")://\(base)/static_data") else { return }
    URLSession.shared.dataTask(with: url) { [weak self] data, _, _ in
      guard let self, let data,
            let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return }
      let name = (j["tunerName"] as? String) ?? ""
      // ANTENNAS. `ant` is { enabled, ant1: { enabled, name }, … }. Expose the switch ONLY when
      // ant.enabled, and only the individual antennas marked enabled — a server with one antenna
      // must show no control at all (same rule as OWRX's lockedRate: never offer a control whose
      // every use is a no-op). Keys are 1-based; the `Z` command and the `ant` state are 0-based.
      var ants: [FmdxAntenna] = []
      if let ant = j["ant"] as? [String: Any], (ant["enabled"] as? Bool) == true {
        for (k, v) in ant where k != "enabled" {
          guard let d = v as? [String: Any], (d["enabled"] as? Bool) == true else { continue }
          let n = Int(k.filter(\.isNumber)) ?? (ants.count + 1)
          ants.append(FmdxAntenna(id: n - 1, name: (d["name"] as? String) ?? k))
        }
      }
      let sorted = ants.sorted { $0.id < $1.id }
      Task { @MainActor in
        if !name.isEmpty { self.rxName = name; self.info.rx = name }
        self.antennaList = sorted
        self.info.antennas = sorted
      }
    }.resume()
  }

  // ── /text: control + whole-state JSON ──
  private func openText() {
    guard !goingIdle else { return }
    textSock.onText = { [weak self] t in self?.onTextFrame(t) }
    textSock.onState = { [weak self] st in
      Task { @MainActor in
        guard let self else { return }
        if st.contains("ready") { self.status = "live" }
        if st.contains("failed") || st.contains("cancelled") { self.status = "reconnecting"; self.retry(self.openText) }
      }
    }
    status = "connecting"
    textSock.open(url: wsURL("/text"))
  }

  // FM-DX pushes a whole-state JSON snapshot per frame; non-JSON lines are keepalive.
  nonisolated private func onTextFrame(_ t: String) {
    guard let d = t.data(using: .utf8),
          let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { return }
    // A valid state frame must carry a numeric freq (MHz). Skip plugin/other frames.
    let freqMhz: Double? = (j["freq"] as? NSNumber)?.doubleValue
      ?? Double((j["freq"] as? String) ?? "")
    guard let mhz = freqMhz, mhz.isFinite, mhz > 0 else { return }

    var i = FmdxInfo()
    i.freq = (mhz * 1_000_000).rounded()
    // cEQ / iMS / antenna — server sends 0|1 (or numeric) for the filters and an index for `ant`.
    i.eq  = ((j["eq"]  as? NSNumber)?.intValue ?? Int((j["eq"]  as? String) ?? "") ?? 0) == 1
    i.ims = ((j["ims"] as? NSNumber)?.intValue ?? Int((j["ims"] as? String) ?? "") ?? 0) == 1
    i.antenna = (j["ant"] as? NSNumber)?.intValue ?? Int((j["ant"] as? String) ?? "") ?? 0
    i.users = (j["users"] as? NSNumber)?.intValue ?? Int((j["users"] as? String) ?? "") ?? 0
    i.stereo = truthy(j["st"])
    i.rds = truthy(j["rds"])
    i.pi = (j["pi"] as? String ?? "").replacingOccurrences(of: "?", with: "")
    i.ps = (j["ps"] as? String ?? "").trimmingCharacters(in: .whitespaces)
    let rtFlag = "\(j["rt_flag"] ?? "0")"
    i.rt = ((rtFlag == "1" ? j["rt1"] : j["rt0"]) as? String ?? "").trimmingCharacters(in: .whitespaces)

    let dBf = (j["sig"] as? NSNumber)?.doubleValue ?? Double((j["sig"] as? String) ?? "") ?? 0
    i.meter = String(format: "%.1f dBf", dBf)
    // Map dBf onto a 0…1 bar. FM-DX useful range ≈ 0 (noise) … 90 (very strong).
    i.level = min(1, max(0, dBf / 90.0))

    if let pty = j["pty"] { i.pty = Self.ptyName(pty) }

    if let tx = j["txInfo"] as? [String: Any] {
      i.tx = (tx["tx"] as? String ?? "").trimmingCharacters(in: .whitespaces)
      i.city = (tx["city"] as? String ?? "").trimmingCharacters(in: .whitespaces)
      i.dist = (tx["dist"] as? NSNumber)?.doubleValue ?? Double((tx["dist"] as? String) ?? "") ?? 0
    }
    if let cc = j["country_iso"] as? String { i.flag = Self.flag(cc) }

    Task { @MainActor in self.adopt(i) }
  }

  /// The antenna list, held OUTSIDE the per-frame struct — same reason as `rxName`.
  @MainActor private var antennaList: [FmdxAntenna] = []

  @MainActor private func adopt(_ i: FmdxInfo) {
    var i = i
    i.rx = rxName         // tunerName arrives via /static_data (kept across frames); preserve it
    // ★ AND THE ANTENNAS, for exactly the same reason. Every state frame builds a FRESH FmdxInfo
    // (`var i = FmdxInfo()`), so a field that arrives once over HTTP is wiped by the next frame —
    // which arrives constantly. The antenna switch appeared for a fraction of a second and then
    // vanished, looking exactly like a server that advertises no antennas.
    i.antennas = antennaList
    if info != i { info = i }
    if frequency != i.freq { frequency = i.freq }
    lastKhz = Int((i.freq / 1000).rounded())
    if clients != i.users { clients = i.users }
    if stationName != i.ps { stationName = i.ps }
    let db = Double(i.meter.split(separator: " ").first.flatMap { Double($0) } ?? 0)
    if signalDb != db { signalDb = db }
    if signalLevel != i.level { signalLevel = i.level }
    if status != "live" { status = "live" }
  }

  // ── /chat ──
  private func openChat() {
    guard !goingIdle else { return }
    chatSock.onText = { [weak self] t in self?.onChatFrame(t) }
    chatSock.onState = { [weak self] st in
      Task { @MainActor in guard let self else { return }
        if st.contains("failed") || st.contains("cancelled") { self.retry(self.openChat) } }
    }
    chatSock.open(url: wsURL("/chat"))
  }

  nonisolated private func onChatFrame(_ t: String) {
    guard let d = t.data(using: .utf8),
          let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { return }
    if (j["type"] as? String) == "clientIp" { return }   // the server telling us our own IP
    guard let msg = j["message"] as? String, !msg.isEmpty else { return }
    let nm = (j["nickname"] as? String) ?? "?"
    Task { @MainActor in self.appendChat(name: nm, text: msg) }
  }

  func sendChat(_ text: String) {
    let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !t.isEmpty else { return }
    // FM-DX caps: nickname ≤32, message ≤255. name rides every message (no join).
    let name = String(ChatIdentity.name.prefix(32))
    let body = String(t.prefix(255))
    if let d = try? JSONSerialization.data(withJSONObject: ["nickname": name, "message": body]),
       let s = String(data: d, encoding: .utf8) { chatSock.send(text: s) }
  }

  @MainActor private func appendChat(name: String, text: String) {
    let mine = name == ChatIdentity.name
    chatLog.append(ChatLine(name: name, text: text, mine: mine))
    if chatLog.count > 40 { chatLog.removeFirst(chatLog.count - 40) }
    if !mine { chatActivity &+= 1 }
  }

  // ── /audio: 3LAS raw MP3 ──
  private func openAudio() {
    guard !goingIdle else { return }
    audioSock.onData = { [weak self] d in
      guard let self else { return }
      self.audioQueue.async { self.decoder.feed(d) }
    }
    // NB: AudioSocket.onReady is never actually fired by AudioSocket — the READY signal comes through
    // onState ("… ws ready …"). So the 3LAS handshake MUST be sent from onState, not onReady.
    audioSock.onState = { [weak self] st in
      Task { @MainActor in
        guard let self else { return }
        if st.contains("ready") {
          self.audioSock.send(text: "{\"type\":\"fallback\",\"data\":\"mp3\"}")   // request MP3 fallback
        }
        if st.contains("failed") || st.contains("cancelled") { self.retry(self.openAudio) }
      }
    }
    audioSock.open(url: wsURL("/audio"))
  }

  private func retry(_ reopen: @escaping () -> Void) {
    guard !goingIdle else { return }
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: 2_000_000_000)
      guard !self.goingIdle else { return }
      reopen()
    }
  }

  // ── Tuning (shared receiver — the VIEW gates this behind an arm latch) ──
  func tuneTo(_ hz: Double) {
    let khz = Int((hz / 1000).rounded())
    lastKhz = khz
    textSock.send(text: "T\(khz)")
  }

  // ── Server-side controls. Wire format taken from the phone's FmdxAdapter so the two agree. ──

  /// cEQ and iMS ride ONE command — `G<eq><ims>` — so both bits must be sent together or the
  /// unmentioned one is cleared. Always send the current value of the other.
  func setEq(_ on: Bool) {
    var i = info; i.eq = on; info = i
    textSock.send(text: "G\(on ? 1 : 0)\(i.ims ? 1 : 0)")
  }
  func setIms(_ on: Bool) {
    var i = info; i.ims = on; info = i
    textSock.send(text: "G\(i.eq ? 1 : 0)\(on ? 1 : 0)")
  }
  /// Antenna select — `Z<n>`, 0-based. Only meaningful when the server advertised more than one.
  func setAntenna(_ id: Int) {
    var i = info; i.antenna = id; info = i
    textSock.send(text: "Z\(id)")
  }

func tune(delta: Int, step: Double) {
    // FM broadcast is a fixed 100 kHz raster — IGNORE the shared UI's step (which is set for SDR bands).
    let target = (frequency > 0 ? frequency : Double(lastKhz) * 1000) + Double(delta) * 100_000
    tuneTo(max(87_500_000, min(108_000_000, target)))
  }

  // ── No-ops / stubs for the SDR surface FM-DX doesn't have ──
  func drainSpectrum(now: Double) {}
  func zoom(delta: Int) {}
  func setBandwidth(_ low: Double, _ high: Double) {}
  func setMode(_ m: String) {}                       // WFM only
  func resumeSpectrum() {}
  func reconnectIfNeeded() {}
  func suspend() {}

  func setVolume(_ v: Double) { audio.setVolume(Float(v)) }

  func goIdle() {
    goingIdle = true
    textSock.cancel(); chatSock.cancel(); audioSock.cancel()
    audio.stop()
  }

  // ── Helpers ──
  private nonisolated func truthy(_ v: Any?) -> Bool {
    if let b = v as? Bool { return b }
    if let n = v as? NSNumber { return n.intValue != 0 }
    if let s = v as? String { return s == "1" || s.lowercased() == "true" }
    return false
  }

  /// ISO-2 country code → flag emoji (regional-indicator letters), like the other list screens.
  nonisolated static func flag(_ code: String) -> String {
    let c = code.uppercased()
    guard c.count == 2, c.unicodeScalars.allSatisfy({ $0.value >= 65 && $0.value <= 90 }) else { return "" }
    return String(c.unicodeScalars.compactMap { Unicode.Scalar(0x1F1E6 + $0.value - 65).map(Character.init) })
  }

  /// European RDS programme-type table (0–31). Only the label is shown.
  nonisolated static func ptyName(_ raw: Any) -> String {
    let n = (raw as? NSNumber)?.intValue ?? Int("\(raw)") ?? 0
    let t = ["", "News", "Current Affairs", "Information", "Sport", "Education", "Drama", "Culture",
             "Science", "Varied", "Pop Music", "Rock Music", "Easy Listening", "Light Classical",
             "Serious Classical", "Other Music", "Weather", "Finance", "Children's", "Social Affairs",
             "Religion", "Phone In", "Travel", "Leisure", "Jazz", "Country", "National Music",
             "Oldies", "Folk Music", "Documentary", "Alarm Test", "Alarm"]
    return (n >= 0 && n < t.count) ? t[n] : ""
  }
}
