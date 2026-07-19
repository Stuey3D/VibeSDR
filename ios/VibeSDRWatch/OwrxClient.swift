import Foundation
import Combine

/// A grouped profile entry for the picker (SDR device → its profiles). `sdrName` is the device the
/// profile belongs to; `active` marks the one we're currently on.
struct SDRProfile: Identifiable, Hashable {
  let id: String        // OWRX profile id, "sdrId|profileId"
  let name: String      // profile display name (SDR prefix stripped)
  let sdrName: String   // owning SDR device name
  var active: Bool = false
}

/// A DAB service (programme) within the tuned ensemble — id is OWRX's service id, name the label.
struct DabProgramme: Identifiable, Equatable {
  let id: Int
  let name: String
}

/// One line in the server's shared chat. `mine` is our own message echoed back (OWRX broadcasts every
/// message to all clients including the sender), drawn aligned right so the conversation reads naturally.
struct ChatLine: Identifiable, Equatable {
  let id = UUID()
  let name: String
  let text: String
  var mine: Bool = false
}

/// One decoded aircraft from an ADS-B `secondary_demod` ADSB-LIST. Position is what the plane sends;
/// distance/bearing are computed client-side from the receiver location.
struct Aircraft: Identifiable, Equatable {
  let icao: String
  var flight: String?      // callsign
  var country: String?
  var ccode: String?       // ISO country of registry
  var altitude: Double?    // ft
  var speed: Double?       // kt
  var vspeed: Double?      // ft/min
  var course: Double?      // deg
  var squawk: String?
  var rssi: Double?
  var msgs: Int?
  var lat: Double?
  var lon: Double?
  var distKm: Double?
  var bearing: Double?
  var id: String { icao }
}

/// A DIRECT OpenWebRX / OpenWebRX+ client on the watch — a Swift port of `src/services/OwrxAdapter.ts`.
///
/// SINGLE multiplexed WebSocket (unlike Kiwi's two): JSON text control + binary FFT (type 1) + binary
/// audio (type 2 = 12k, type 4 = 48k HD). Handshake: send `SERVER DE CLIENT …` → server `CLIENT DE
/// SERVER …` ack → `connectionproperties` + `dspcontrol start`. FFT spans the WHOLE profile
/// (center ± samp_rate/2); we slice a view window client-side. Profile switching is EXPLICIT-ONLY
/// (retunes the shared SDR for every listener — see the etiquette rule).
@MainActor
final class OwrxClient: ObservableObject, SDRClient {

  // spike mode → OWRX wire modulation + default passband (Hz offsets from carrier)
  private static let modeMap: [String: (mod: String, lo: Double, hi: Double)] = [
    "usb": ("usb", 0, 2700),   "lsb": ("lsb", -2700, 0),
    "am":  ("am", -4500, 4500), "sam": ("am", -4500, 4500),
    "fm":  ("nfm", -6250, 6250), "nfm": ("nfm", -6250, 6250),
    "cwu": ("cw", 200, 500),   "cwl": ("cw", -500, -200),
    "wfm": ("wfm", -80000, 80000),
    // DAB: OWRX's digital DAB demod. The wire passband is server-managed (sendDemod nulls low/high_cut),
    // but these offsets drive the VFO display — a DAB block is ~1.536 MHz, so show ±768 kHz rather than
    // a zero-width VFO. The entry MUST exist or sendDemod falls back to AM and the decoder never engages.
    "dab": ("dab", -768_000, 768_000),
    // ADS-B: raw-IF digimode. mod=adsb, passband nulled by sendDemod (raw 2.4 MHz IF). Offsets unused.
    "adsb": ("adsb", 0, 0),
  ]
  private static let ua = "Mozilla/5.0 (iPhone; CPU iPhone OS 18_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1"

  // ── Published surface the UI mirrors ──
  @Published var frequency: Double = 0
  @Published var mode = "am"
  @Published var bwLow: Double = -4500
  @Published var bwHigh: Double = 4500
  @Published var signalLevel: Double = 0
  @Published var signalDb: Double = 0
  @Published var framesPerSec: Double = 0
  @Published var status = "starting"
  @Published var lastError: String? = nil
  @Published var profiles: [SDRProfile] = []
  @Published var clients: Int = 0
  @Published var stationName = ""          // RDS programme-service name (WFM) → band pill
  @Published var chatLog: [ChatLine] = []  // shared server chat (capped to the last 40 lines)
  @Published var chatActivity = 0          // bumps on each INBOUND message → breathes the chat glyph
  var supportsChat: Bool { true }          // OWRX has a shared text chat; other backends default off
  var rowsPushed = 0
  var displaySpanHz: Double { viewBw }

  // ── Endpoint ──
  private let hostPart: String          // host:port
  private var secure: Bool              // wss vs ws (auto-falls-back to ws on a TLS error)
  private var triedInsecureFallback = false
  private var wsURL: URL { URL(string: "\(secure ? "wss" : "ws")://\(hostPart)/ws/")! }

  // ── Server / profile state ──
  private var centerFreq = 0.0
  private var sampRate = 0.0
  private var fftCompression = "none"
  private var audioCompression = "none"
  // Snapshots read by the off-main decode paths (benign single-writer-on-config races).
  nonisolated(unsafe) private var audioCompressionSnapshot = "none"
  nonisolated(unsafe) private var activeProfileId = ""   // read off-main in buildProfiles
  private var started = false
  private var zeroSecs = 0                                // consecutive no-data seconds (watchdog)
  private var handshaked = false

  // ── View (client-side slice of the whole-profile FFT) ──
  private var viewCenter = 0.0
  private var viewBw = 48_000.0
  nonisolated(unsafe) private var lastRow: [Float] = []

  // ── DAB speed fix (Stuart) — under-state the PCM rate so the resampler stretches DAB back to
  // correct speed/pitch. 1.0 = off. Only applied on the DAB mode. ──
  nonisolated(unsafe) var dabRateScale = 1.0
  // ── DAB services within the tuned ensemble. OWRX plays NO audio until one is selected, so we adopt
  // the first when the list lands, and expose the list for the programme picker. ──
  @Published var dabProgrammes: [DabProgramme] = []
  // ── ADS-B: aircraft list + the secondary-demod plumbing to engage it ──
  @Published var aircraft: [Aircraft] = []
  private var serverModes: [String: (type: String, underlying: [String], ifRate: Double)] = [:]  // from `modes`
  private var secondaryDecoder: String? = nil     // e.g. "adsb" — a digimode running on the raw IF
  private var pendingStartMod: String? = nil       // digimode start_mod waiting for the `modes` list
  private var rxLat: Double? = nil                 // receiver position (for aircraft distance/bearing)
  private var rxLon: Double? = nil
  var receiverLat: Double? { rxLat }               // SDRClient — receiver site for the ADS-B map centre
  var receiverLon: Double? { rxLon }
  var dabScale: Double { dabRateScale }   // SDRClient — current speed-fix factor for the menu highlight
  private var audioServiceId = -1
  private var dabDrySecs = 0        // seconds on a DAB profile with no ensemble yet (re-lock watchdog)
  private var dabRetries = 0
  var dabActiveId: Int { audioServiceId }          // SDRClient — the playing service
  var dabEnsembleName: String { dabEnsemble }      // SDRClient — multiplex label (or profile-name fallback)
  private var dabEnsemble = ""

  // ── Sockets / audio / DSP ──
  // NWConnection (AudioSocket). A URLSessionWebSocketTask swap was tried and made the stall WORSE on
  // watchOS (~15 s cycles vs ~60 s) — reverted. The stall is NOT the transport: OWRX force-feeds a
  // full-profile 8192-bin FFT (~64-80 KB/s, ~16-20 fps) that it will NOT let the client throttle
  // (fps/fft_fps are ignored — measured), and the watch's link/CPU can't sustain it where a desktop can.
  nonisolated(unsafe) private let sock = AudioSocket(name: "owrx")
  nonisolated(unsafe) private let decodeQueue = DispatchQueue(label: "owrx.decode")  // FFT decode
  nonisolated(unsafe) private let audioQueue  = DispatchQueue(label: "owrx.audio")   // audio decode — SEPARATE so a slow FFT can't gap audio
  // FFT coalescing: keep only the LATEST frame so the decode queue can never build a backlog.
  private let fftLock = NSLock()
  nonisolated(unsafe) private var latestFft: [UInt8]? = nil
  nonisolated(unsafe) private var fftScheduled = false
  nonisolated(unsafe) let waterfall: WaterfallBuffer
  private let proc = SignalProcessor()
  nonisolated(unsafe) private let audio = WatchAudio()
  nonisolated(unsafe) private let audioDec = OwrxAudioDecoder()      // 12k
  nonisolated(unsafe) private let hdAudioDec = OwrxAudioDecoder()    // 48k HD/WFM
  private var rateTimer: Timer?
  nonisolated(unsafe) private var frameCount = 0        // bumped off-main; rateTimer reads/resets
  nonisolated(unsafe) private var fftSuppressed = false // true on ADS-B: no waterfall shown, skip the heavy IF-FFT decode
  nonisolated(unsafe) private var bytesThisSec = 0      // all inbound WS bytes this second → live KB/s
  nonisolated(unsafe) private var fftThisSec = 0        // FFT frames this second → live FFT fps
  private var kbPerSec = 0                               // read by the status pill + advisory
  var inboundKbPerSec: Int { kbPerSec }                 // SDRClient — drives the heavy-server advisory
  private var fftFps = 0
  nonisolated(unsafe) private var sawFirstFrame = false
  private var everFrame = false
  // OFF by default. We PROVED (2026-07-18) watchOS will not hand the watch a direct wifi route while the
  // phone is reachable — prohibiting the `.other` relay just fails and falls back, costing a ~6 s connect
  // delay for nothing. OWRX's heavy FFT firehose only sustains on the watch's OWN wifi/cellular (phone
  // off/away), and that's the OS's call, not ours. Kept as a lever but disabled; instrumentation stays.
  private var avoidRelayActive = false
  private var preFrameSecs = 0
  private var linkIface = "?"       // which interface the socket actually came up on (wifi/cell/relay)
  nonisolated(unsafe) private var goingIdle = false

  init(url: String, waterfall: WaterfallBuffer) {
    self.waterfall = waterfall
    // http(s)://host:port[/path] → ws(s)://host:port/ws/  (bare /ws 404s — the route is exactly "/ws/")
    var u = url
    self.secure = u.hasPrefix("https") || u.hasPrefix("wss")
    for p in ["https://", "http://", "wss://", "ws://"] { if u.hasPrefix(p) { u.removeFirst(p.count) } }
    if let slash = u.firstIndex(of: "/") { u = String(u[..<slash]) }
    self.hostPart = u
    proc.autoContrast = 5
  }

  // ── Lifecycle ──
  func start() {
    started = true
    let t = Timer(timeInterval: 1, repeats: true) { [weak self] _ in
      Task { @MainActor in guard let self else { return }
        self.framesPerSec = Double(self.frameCount)
        self.kbPerSec = self.bytesThisSec / 1024; self.bytesThisSec = 0
        self.fftFps = self.fftThisSec; self.fftThisSec = 0
        if self.frameCount > 0 {
          self.retries = 0; self.zeroSecs = 0
        } else if !self.everFrame, self.avoidRelayActive, !self.retrying {
          // Still waiting for the FIRST frame on a wifi/cellular-only socket. If it never comes, the
          // watch has no non-relay route right now (phone near, wifi asleep) → drop the restriction and
          // reopen so it connects over the relay. Out-of-house this is the normal path.
          self.preFrameSecs += 1
          if self.preFrameSecs >= 6 {
            self.avoidRelayActive = false
            self.status = "no wifi — using phone"
            self.reopen()
          }
        } else if self.everFrame, !self.retrying {
          self.zeroSecs += 1
          // Recover a SILENT stall (data stops with no socket error → the spike shows 'reconnecting'
          // but nothing reconnects → stuck). 15s is long enough to tolerate OWRX's normal multi-second
          // audio stutters (FFT frames count too, so it only fires when EVERYTHING stops). No WS ping —
          // that caused the server-visible connect/disconnect churn.
          // ADS-B updates are bursty (aircraft come and go) — be far more lenient there so a quiet sky
          // doesn't tear down a healthy decoder. Everything else keeps the tight 15s.
          let stallLimit = (self.mode == "adsb") ? 45 : 15
          if self.zeroSecs >= stallLimit { self.retry(reason: "no data \(stallLimit)s") }
        }
        // ADS-B SELF-HEAL: if we're on adsb but the secondary decoder isn't engaged (auto-connect or a
        // reconnect where the config beat the modes list), engage it now that modes are loaded. Stops as
        // soon as secondaryDecoder is set, so it's not a loop. This is the fix for "connected on the wrong
        // secondary mode" / needing a manual DEMOD pick.
        if self.mode == "adsb", self.secondaryDecoder == nil, self.serverModes["adsb"]?.type == "digimode" {
          self.applyStartMod("adsb")
          self.sendDemod(); self.send(["type": "dspcontrol", "action": "start"])
        }
        if self.mode == "adsb" {
          // ADS-B diagnostic (no pill on that screen): decoder engaged? modes loaded? aircraft-msgs seen?
          self.status = "sec=\(self.secondaryDecoder ?? "nil") adsb=\(self.serverModes["adsb"]?.type ?? "MISSING") m=\(self.serverModes.count) sd=\(self.sdCount) cpu:\(Int(CpuMeter.processCpuPercent()))"
        } else if self.everFrame, !self.retrying {
          self.status = "\(self.linkIface) \(self.kbPerSec)KB/s \(self.fftFps)fft cpu:\(Int(CpuMeter.processCpuPercent()))"
        }
        // DAB re-lock safety net: a DAB profile that came up but produced NO ensemble after a few
        // seconds usually means the re-asserted `dspcontrol start` raced the retune and the dablin
        // chain wasn't (re)built. Re-send the demod + start a couple of times before giving up — this
        // is what made switching between DAB profiles reliable instead of "works once".
        if self.everFrame, self.mode == "dab", self.dabProgrammes.isEmpty, !self.retrying {
          self.dabDrySecs += 1
          if self.dabDrySecs >= 3, self.dabRetries < 3 {
            self.dabRetries += 1; self.dabDrySecs = 0
            self.sendDemod(); self.send(["type": "dspcontrol", "action": "start"])
          }
        } else {
          self.dabDrySecs = 0
          if !self.dabProgrammes.isEmpty { self.dabRetries = 0 }
        }
        self.frameCount = 0 }
    }
    RunLoop.main.add(t, forMode: .common); rateTimer = t
    audio.start { _, _ in }
    openSocket()
  }

  private func openSocket() {
    sock.onText = { [weak self] s in self?.onText(s) }   // parse OFF main (OWRX+ floods JSON)
    // Decode on a DEDICATED queue, NOT the socket's receive queue. AudioSocket only reads the next
    // frame AFTER onData returns — so decoding inline gates reads, OWRX's send buffer fills, and it
    // STALLS after a few seconds. Copy fast + hand off, so the receive loop never waits on decode.
    sock.onData = { [weak self] d in
      guard let self else { return }
      let bytes = [UInt8](d)
      let type = bytes.first ?? 0
      self.bytesThisSec &+= d.count                        // live link load (all inbound bytes)
      // ADS-B shows NO waterfall and has NO audio — but OWRX still streams a 2.4 MHz-IF FFT (type 1) AND
      // a secondary-FFT (type 3). Routing type-3 to the AUDIO decoder pegged the watch at ~100%. On ADS-B
      // drop ALL binary; the aircraft arrive as text (secondary_demod).
      if self.fftSuppressed { return }
      if type == 1 {
        self.fftThisSec &+= 1
        // FFT: COALESCE to the latest frame only. Never build a backlog — if decode falls a little
        // behind over time the queue would grow unbounded (memory creep → the ~1-min freeze). The
        // waterfall only needs the newest frame; older ones are stale anyway.
        self.fftLock.lock()
        self.latestFft = bytes
        let alreadyScheduled = self.fftScheduled
        self.fftScheduled = true
        self.fftLock.unlock()
        if !alreadyScheduled { self.decodeQueue.async { self.drainFft() } }
      } else if type == 2 || type == 4 {
        // Audio (12k / 48k HD) ONLY — never feed a secondary-FFT (type 3) or other stream to the decoder.
        self.audioQueue.async { self.onBinary(bytes) }
      }
    }
    sock.onState = { [weak self] st in
      Task { @MainActor in
        guard let self else { return }
        // wss → ws auto-fallback: a plain-HTTP OWRX box (most self-hosted ones) refuses TLS with
        // "-9836 bad protocol version". If we tried wss and hit that, retry once as ws.
        let lower = st.lowercased()
        if self.secure, !self.triedInsecureFallback,
           lower.contains("9836") || lower.contains("protocol version") || lower.contains("tls") || lower.contains("secure") {
          self.triedInsecureFallback = true
          self.secure = false
          self.handshaked = false
          self.status = "retrying (ws)…"
          self.sock.cancel()
          self.sock.open(url: self.wsURL, headers: [("User-Agent", Self.ua)], forceIPv4: true, autoReplyPing: false, avoidRelay: self.avoidRelayActive)
          return
        }
        if !self.everFrame, self.status != "live" { self.status = st }
        if st.contains("ready"), !self.handshaked {
          // AudioSocket reports the live interface as "owrx ws ready [wifi]" — surface it so the pill
          // shows whether OWRX is on real wifi/cellular or the phone relay.
          if let a = st.firstIndex(of: "["), let b = st.firstIndex(of: "]"), a < b {
            self.linkIface = String(st[st.index(after: a)..<b])
          }
          self.handshaked = true
          self.status = "registering"
          self.sock.send(text: "SERVER DE CLIENT client=vibesdr type=receiver")
        }
        if (st.contains("failed") || st.contains("recv:")), !self.goingIdle {
          self.retry(reason: st)
        }
      }
    }
    sock.open(url: wsURL, headers: [("User-Agent", Self.ua)], forceIPv4: true, autoReplyPing: false, avoidRelay: avoidRelayActive)
  }

  // Reopen the socket in place (same handshake path) — used to drop the wifi-only restriction and fall
  // back to the relay when no non-relay route came up.
  private func reopen() {
    preFrameSecs = 0
    handshaked = false
    sock.cancel()
    sock.open(url: wsURL, headers: [("User-Agent", Self.ua)], forceIPv4: true, autoReplyPing: false, avoidRelay: avoidRelayActive)
  }

  // Reconnect on a mid-session drop (flaky server / receive-loop stop), with backoff.
  private var retries = 0
  private var retrying = false
  private func retry(reason: String) {
    guard !retrying, !goingIdle else { return }
    retrying = true; retries += 1
    status = "reconnect \(retries): \(reason)"
    handshaked = false
    // A reconnect lands on the server's DEFAULT profile — drop ADS-B state so the ADS-B screen doesn't
    // linger over an FM demod (stale aircraft kept routing to .adsb).
    aircraft = []; secondaryDecoder = nil; fftSuppressed = false
    sock.cancel()
    // ★ FIRST RETRY IS IMMEDIATE, then back off. Backoff exists to stop us hammering a server that
    // is genuinely down — it has no business delaying the first attempt after a HEALTHY session,
    // and here it cost 1.5s minimum because `retries` is incremented above.
    //
    // That delay is not merely slow, it is DESTRUCTIVE on OWRX: the profile is server state, and a
    // window with zero clients lets the server fall back to its default profile. Worse, someone
    // else can arrive in that window and pick their own — at which point reconnecting and grabbing
    // it back would hijack the radio from them. Every second of gap makes that likelier, so the
    // gap is the thing to attack.
    //
    // `everFrame` is the guard: only a connection that WORKED gets the instant retry. A server that
    // never gave us a frame (down, full, refusing) still gets the full backoff.
    let steps = everFrame ? max(0, retries - 1) : retries
    let wait = UInt64(min(steps, 5)) * 1_500_000_000
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: wait)
      self.retrying = false
      guard !self.goingIdle else { return }
      self.sock.open(url: self.wsURL, headers: [("User-Agent", Self.ua)], forceIPv4: true, autoReplyPing: false, avoidRelay: self.avoidRelayActive)
    }
  }

  nonisolated private func send(_ obj: [String: Any]) {
    guard let d = try? JSONSerialization.data(withJSONObject: obj),
          let s = String(data: d, encoding: .utf8) else { return }
    sock.send(text: s)
  }

  // ── Shared chat ─────────────────────────────────────────────────────────────
  /// Send a chat line under the user's shared callsign/chat name. The server echoes it back to
  /// every client (us included) as a `chat_message`, so we DON'T optimistically append here —
  /// `appendChat` marks the echo `mine` by matching our name. Same wire shape as the phone
  /// (`OwrxAdapter.sendChat`): `{type:"sendmessage", text, name}`.
  func sendChat(_ text: String) {
    let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !t.isEmpty else { return }
    send(["type": "sendmessage", "text": t, "name": ChatIdentity.name])
  }

  /// Append an inbound line, dedup our own echo as `mine`, cap the log, and breathe the glyph
  /// only for OTHER people's messages (our own send shouldn't pulse our own icon).
  @MainActor private func appendChat(name: String, text: String) {
    let mine = name == ChatIdentity.name
    chatLog.append(ChatLine(name: name, text: text, mine: mine))
    if chatLog.count > 40 { chatLog.removeFirst(chatLog.count - 40) }
    if !mine { chatActivity &+= 1 }
  }

  // ── inbound text / JSON — runs OFF the main actor; only the handled types hop to main, so the
  //    OWRX+ metadata/dial/secondary flood is parsed and dropped without ever touching the UI thread.
  nonisolated private func onText(_ data: String) {
    if data.hasPrefix("CLIENT DE SERVER") {
      // 12 kHz for the narrow demods (output_rate) but keep the full 48 kHz HD channel (hd_output_rate)
      // for WFM — broadcast FM needs the wide audio for hi-fi/stereo, and that's the whole point of the
      // BC FM listening experience. The stall fix rides on the URLSession transport, not on starving the
      // audio. If the watch still can't keep up under load, 24000 here halves the WFM decode as a fallback.
      send(["type": "connectionproperties", "params": ["output_rate": 12000, "hd_output_rate": 48000]])
      send(["type": "dspcontrol", "action": "start"])
      Task { @MainActor in self.status = "connecting" }
      return
    }
    guard let d = data.data(using: .utf8),
          let json = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
          let type = json["type"] as? String else { return }
    switch type {
    case "config":   let v = json["value"] as? [String: Any] ?? [:]; Task { @MainActor in self.onConfig(v) }
    case "profiles": let ps = buildProfiles(json["value"] as? [Any] ?? []); Task { @MainActor in self.profiles = ps }
    case "clients":  if let n = (json["value"] as? NSNumber)?.intValue { Task { @MainActor in self.clients = n } }
    case "chat_message":
      let nm = (json["name"] as? String) ?? "?"
      let tx = (json["text"] as? String) ?? ""
      if !tx.isEmpty { Task { @MainActor in self.appendChat(name: nm, text: tx) } }
    case "smeter":   if let v = (json["value"] as? NSNumber)?.doubleValue, v > 0 { let db = 10 * log10(v); Task { @MainActor in self.signalDb = db } }
    case "sdr_error", "demodulator_error":
      let msg = String(describing: json["value"] ?? "OpenWebRX error"); Task { @MainActor in self.lastError = msg }
    case "metadata":
      if let v = json["value"] as? [String: Any] { onMetadata(v) }
    case "modes":
      onModes(json["value"] as? [Any] ?? [])
    case "receiver_details":
      if let v = json["value"] as? [String: Any] { onReceiverDetails(v) }
    case "secondary_demod":
      onSecondaryDemod(json["value"])
    default: break   // parsed off-main, ignored — never reaches the UI thread
    }
  }

  /// The server's mode table. We only need which modes are DIGIMODES (run as a secondary decoder on top
  /// of an underlying carrier) and their raw-IF hints — so a start_mod like `adsb` engages as a secondary
  /// decoder instead of a primary demod (which decodes nothing).
  nonisolated private func onModes(_ list: [Any]) {
    var out: [String: (type: String, underlying: [String], ifRate: Double)] = [:]
    for el in list {
      guard let m = el as? [String: Any], let id = m["modulation"] as? String else { continue }
      let type = (m["type"] as? String) ?? "analog"
      let under = (m["underlying"] as? [String]) ?? []
      let ifr = (m["ifRate"] as? NSNumber)?.doubleValue ?? 0
      out[id] = (type, under, ifr)
    }
    Task { @MainActor in
      self.serverModes = out
      // A digimode start_mod (e.g. adsb) had to WAIT for this list to know it's a secondary decoder.
      // Re-engage + re-send the demod + re-assert start so the secondary decoder actually starts.
      if let pend = self.pendingStartMod {
        self.pendingStartMod = nil
        self.applyStartMod(pend)
        if self.secondaryDecoder != nil { self.sendDemod(); self.send(["type": "dspcontrol", "action": "start"]) }
      }
    }
  }

  /// Receiver location — the reference point for aircraft distance/bearing on the ADS-B screen.
  nonisolated private func onReceiverDetails(_ v: [String: Any]) {
    // OWRX puts it in `receiver_gps: {lat, lon}` (measured on the server), with `gps`/top-level as fallbacks.
    var lat: Double?; var lon: Double?
    if let g = (v["receiver_gps"] as? [String: Any]) ?? (v["gps"] as? [String: Any]) {
      lat = (g["lat"] as? NSNumber)?.doubleValue
      lon = (g["lon"] as? NSNumber)?.doubleValue
    }
    lat = lat ?? (v["lat"] as? NSNumber)?.doubleValue
    lon = lon ?? (v["lon"] as? NSNumber)?.doubleValue
    if let lat, let lon { Task { @MainActor in self.rxLat = lat; self.rxLon = lon } }
  }

  /// ADS-B aircraft table (`secondary_demod` → ADSB-LIST). It's a SNAPSHOT — replace the whole list.
  nonisolated(unsafe) private var sdCount = 0   // secondary_demod messages seen (ADS-B diagnostic)
  nonisolated private func onSecondaryDemod(_ value: Any?) {
    // Count as DATA for the stall watchdog — ADS-B has no audio/FFT, so without this the "no data 15s"
    // watchdog tears the stream down and reconnects to the default profile. Even an empty list counts.
    frameCount &+= 1; sdCount &+= 1
    if !sawFirstFrame { sawFirstFrame = true; Task { @MainActor in self.everFrame = true; if self.status != "live" { self.status = "live" } } }
    guard let v = value as? [String: Any], let list = v["aircraft"] as? [[String: Any]] else { return }
    var parsed: [Aircraft] = []
    for a in list {
      guard let icao = a["icao"] as? String, !icao.isEmpty else { continue }
      parsed.append(Aircraft(
        icao: icao,
        flight: (a["flight"] as? String)?.trimmingCharacters(in: .whitespaces),
        country: a["country"] as? String,
        ccode: a["ccode"] as? String,
        altitude: (a["altitude"] as? NSNumber)?.doubleValue,
        speed: (a["speed"] as? NSNumber)?.doubleValue,
        vspeed: (a["vspeed"] as? NSNumber)?.doubleValue,
        course: (a["course"] as? NSNumber)?.doubleValue,
        squawk: a["squawk"] as? String,
        rssi: (a["rssi"] as? NSNumber)?.doubleValue,
        msgs: (a["msgs"] as? NSNumber)?.intValue,
        lat: (a["lat"] as? NSNumber)?.doubleValue,
        lon: (a["lon"] as? NSNumber)?.doubleValue,
        distKm: nil, bearing: nil))
    }
    Task { @MainActor in self.applyAircraft(parsed) }
  }

  @MainActor private func applyAircraft(_ list: [Aircraft]) {
    // DEDUPE by icao — a snapshot can carry the same aircraft twice, and SwiftUI's ForEach CRASHES on a
    // duplicate id ("ID occurs multiple times"). Keep the last seen for each icao.
    var byId: [String: Aircraft] = [:]
    for a in list { byId[a.icao] = a }
    var out = Array(byId.values)
    if let rlat = rxLat, let rlon = rxLon {
      for i in out.indices where out[i].lat != nil && out[i].lon != nil {
        out[i].distKm = Self.haversineKm(rlat, rlon, out[i].lat!, out[i].lon!)
        out[i].bearing = Self.bearingDeg(rlat, rlon, out[i].lat!, out[i].lon!)
      }
    }
    // NOT a staleness filter — measured against Stuart's own OWRX (2026-07-19, 58 snapshots):
    // OWRX reports ~203 aircraft of which ~170 carry a POSITION, and a dedicated FlightAware
    // receiver on the same sky reported 170. So the discrepancy is not stale contacts being kept,
    // it is aircraft heard WITHOUT a position fix. A dump1090-style 60s TTL on message activity
    // would have cut the list to ~106 — further from FlightAware, not closer.
    aircraft = out
  }

  /// Adopt a profile's start_mod. A DIGIMODE (adsb, ft8, packet…) must run as a SECONDARY decoder on the
  /// raw IF or the server decodes nothing — and that needs the `modes` list, which lands AFTER config on
  /// connect. So if modes aren't here yet, remember it and replay from onModes.
  @MainActor private func applyStartMod(_ id: String) {
    guard let sm = serverModes[id.lowercased()] else {
      pendingStartMod = id
      mode = Self.wireToSpike[id.lowercased()] ?? id.lowercased()
      fftSuppressed = (mode == "adsb")   // MUST set here too — the deferred path (modes not loaded yet)
      return                             // otherwise ADS-B's 2.4 MHz IF-FFT decodes at full tilt = ~100% CPU
    }
    pendingStartMod = nil
    if sm.type == "digimode" {
      secondaryDecoder = id.lowercased()
      let real = sm.underlying.filter { $0 != "empty" }
      let carrier = real.first ?? id.lowercased()          // adsb → 'adsb' (empty underlying = raw IF)
      mode = Self.wireToSpike[carrier] ?? carrier
    } else {
      secondaryDecoder = nil
      mode = Self.wireToSpike[id.lowercased()] ?? id.lowercased()
    }
    if let p = Self.modeMap[mode] { bwLow = p.lo; bwHigh = p.hi }
    fftSuppressed = (mode == "adsb")   // ADS-B has no waterfall → skip the heavy IF-FFT decode
    if mode != "adsb" { aircraft = [] }   // leaving ADS-B → drop the aircraft list (screen follows mode)
    modeSnapshot = mode
  }

  private static func haversineKm(_ lat1: Double, _ lon1: Double, _ lat2: Double, _ lon2: Double) -> Double {
    let r = 6371.0, dLat = (lat2-lat1) * .pi/180, dLon = (lon2-lon1) * .pi/180
    let a = sin(dLat/2)*sin(dLat/2) + cos(lat1 * .pi/180)*cos(lat2 * .pi/180)*sin(dLon/2)*sin(dLon/2)
    return r * 2 * atan2(sqrt(a), sqrt(1-a))
  }
  private static func bearingDeg(_ lat1: Double, _ lon1: Double, _ lat2: Double, _ lon2: Double) -> Double {
    let dLon = (lon2-lon1) * .pi/180, y = sin(dLon)*cos(lat2 * .pi/180)
    let x = cos(lat1 * .pi/180)*sin(lat2 * .pi/180) - sin(lat1 * .pi/180)*cos(lat2 * .pi/180)*cos(dLon)
    return (atan2(y, x) * 180 / .pi + 360).truncatingRemainder(dividingBy: 360)
  }

  /// RDS (broadcast FM) station name from the `metadata` message. Keyed protocol:'WFM'; `ps` is the
  /// 8-char programme-service name. Incremental (ps and radiotext arrive separately) so a radiotext-only
  /// update must NOT blank a known ps — only a non-empty ps updates the name. DAB/digital-voice
  /// metadata is handled on the DAB/ADS-B screens, not here.
  nonisolated private func onMetadata(_ v: [String: Any]) {
    if (v["mode"] as? String) == "DAB" { onDabMetadata(v); return }
    // RDS (WFM). ps = programme-service name (8 chars). Incremental (ps/radiotext arrive separately),
    // so only a NON-EMPTY ps updates the name — a radiotext-only frame must not blank a known station.
    let proto = v["protocol"] as? String
    guard proto == "WFM" || v["ps"] != nil || v["radiotext"] != nil else { return }
    if let ps = (v["ps"] as? String)?.trimmingCharacters(in: .whitespaces), !ps.isEmpty {
      Task { @MainActor in if self.stationName != ps { self.stationName = ps } }
    }
  }

  /// DAB metadata: programmes (id→name) + ensemble label, resent ~1×/s. OWRX plays NO audio until a
  /// service is selected, so adopt the first when the list lands and re-send the demod. Station name =
  /// selected programme (else ensemble).
  nonisolated private func onDabMetadata(_ v: [String: Any]) {
    var progs: [DabProgramme] = []
    if let p = v["programmes"] as? [String: Any] {
      for (k, name) in p { if let id = Int(k) { progs.append(DabProgramme(id: id, name: String(describing: name))) } }
      progs.sort { $0.id < $1.id }
    }
    // This OWRX+ build sends `ensemble_id` (a number), NOT `ensemble_label` — so there's often no label
    // on the wire. Fall back to the active profile's name (which carries the multiplex name, e.g.
    // "DAB 7A: NNDAB Northampton") in applyDabMeta when no label arrives.
    let ensemble = (v["ensemble_label"] as? String)?.trimmingCharacters(in: .whitespaces)
    Task { @MainActor in self.applyDabMeta(progs, ensemble) }
  }

  private var dabServiceMap: [Int: String] = [:]   // accumulated id→name (metadata is incremental)
  // Per-station speed-fix recall (like the phone). Keyed "<ensemble>|<programme>" so a station you fixed
  // once re-applies automatically when you come back — persisted across launches in UserDefaults.
  private var dabSpeedMap: [String: Double] = (UserDefaults.standard.dictionary(forKey: "owrx_dab_speed_map") as? [String: Double]) ?? [:]
  private var dabKey = ""
  @MainActor private func applyDabMeta(_ progs: [DabProgramme], _ ensemble: String?) {
    // ACCUMULATE — the server dribbles services out over several messages and some batches are partial
    // (a big mux like SDL National showed only the latest 2 when we REPLACED). Merge into a map that only
    // grows until the profile changes, so the full ensemble builds up and never shrinks.
    for p in progs { dabServiceMap[p.id] = p.name }
    let merged = dabServiceMap.map { DabProgramme(id: $0.key, name: $0.value) }
      .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    if dabProgrammes != merged { dabProgrammes = merged }
    if let e = ensemble, !e.isEmpty { dabEnsemble = e }
    if dabEnsemble.isEmpty, let pn = profiles.first(where: { $0.active })?.name { dabEnsemble = pn }
    // Adopt a service ONLY when nothing has been chosen yet (audioServiceId < 0). Never re-adopt after
    // that: a flaky mux whose service list cycles (grows → resets → regrows, as SDL does) must NOT be
    // allowed to silently switch the station or its name out from under the user. Once you're on a
    // service, you stay on it until YOU pick another.
    if audioServiceId < 0, let first = dabProgrammes.first {
      audioServiceId = first.id
      audioDec.reset(); hdAudioDec.reset()
      sendDemod()
    }
    let sel = dabProgrammes.first(where: { $0.id == audioServiceId })?.name
    let name = sel ?? dabEnsemble
    if !name.isEmpty, stationName != name { stationName = name }
    // Recall the saved speed fix when the tuned SERVICE changes (not every metadata tick, so a value the
    // user just set isn't stomped). Key on ensemble+programme, matching the phone.
    if let sel {
      let key = dabEnsemble + "|" + sel
      if key != dabKey {
        dabKey = key
        let saved = dabSpeedMap[key] ?? 1.0
        if dabRateScale != saved { dabRateScale = saved }
      }
    }
  }

  /// Switch DAB service within the ensemble (from the programme picker). Re-sends the demod with the
  /// new audio_service_id; OWRX swaps the decoded programme on the shared stream.
  func selectDabService(_ id: Int) {
    guard id != audioServiceId else { return }
    audioServiceId = id
    audioDec.reset(); hdAudioDec.reset()
    sendDemod()
    if let n = dabProgrammes.first(where: { $0.id == id })?.name {
      stationName = n
      let key = dabEnsemble + "|" + n     // recall this station's saved speed fix immediately
      dabKey = key
      dabRateScale = dabSpeedMap[key] ?? 1.0
    }
  }

  /// DAB speed-correction factor (1 = off). Applied on the next audio frame (per-frame play rate) and
  /// REMEMBERED for the current station (ensemble|programme) so it auto-re-applies on return.
  func setDabScale(_ scale: Double) {
    dabRateScale = scale > 0 ? scale : 1.0
    if !dabKey.isEmpty {
      dabSpeedMap[dabKey] = dabRateScale
      UserDefaults.standard.set(dabSpeedMap, forKey: "owrx_dab_speed_map")
    }
  }

  private var lastConfigCenter = 0.0
  private var pendingProfileSwitch = false   // set by selectProfile; forces demod adoption on next config
  private func onConfig(_ c: [String: Any]) {
    let prevCenter = centerFreq
    if let cf = (c["center_freq"] as? NSNumber)?.doubleValue { centerFreq = cf }
    if let sr = (c["samp_rate"] as? NSNumber)?.doubleValue { sampRate = sr }
    if let fc = c["fft_compression"] as? String { fftCompression = fc; fftCompressionSnapshot = fc }
    if let ac = c["audio_compression"] as? String { audioCompression = ac; audioCompressionSnapshot = ac }
    modeSnapshot = mode
    // A RECONNECT re-sends config for the SAME profile — preserve the VFO, zoom and demod so a blip
    // doesn't yank the view/mode back. Only reset those on a genuinely NEW profile (centre changed) OR
    // on an EXPLICIT profile switch (pendingProfileSwitch) — a switch that happens to land on a same-
    // centre profile must still adopt its demod, or the "auto demod on switch" is silently lost.
    let sameProfile = (centerFreq == prevCenter && prevCenter != 0)
    let newCentre = !sameProfile
    // A new profile is arriving if the centre changed OR an explicit switch is pending. Keep the
    // "awaiting demod" state (pendingProfileSwitch) alive through it — OWRX dribbles SEVERAL config
    // messages per switch and `start_mod` often isn't in the first one. Clearing on the first config
    // (as before) meant a DAB profile whose start_mod landed a message later never adopted → stuck on
    // the previous demod (NFM). So we only clear once start_mod is actually adopted, below.
    let adopt = newCentre || pendingProfileSwitch
    if adopt { pendingProfileSwitch = true }
    if newCentre {
      if frequency == 0 || abs(frequency - centerFreq) > sampRate / 2, centerFreq != 0 {
        frequency = centerFreq
        if let off = (c["start_offset_freq"] as? NSNumber)?.doubleValue { frequency = centerFreq + off }
      }
      viewCenter = frequency
      // Show a GENEROUS chunk of the band by default (OWRX profiles are often multi-MHz — a 12 kHz
      // window looked "extremely zoomed"). The crown zoom narrows/widens from here.
      viewBw = min(sampRate > 0 ? sampRate : 192_000, 250_000)
      stationName = ""   // new profile/station — drop any stale RDS name until fresh metadata lands
      dabProgrammes = []; audioServiceId = -1; dabEnsemble = ""   // new ensemble on a DAB profile switch
      dabServiceMap = [:]                                         // drop the old mux's accumulated services
      dabDrySecs = 0; dabRetries = 0                              // re-arm the DAB re-lock watchdog
      aircraft = []; secondaryDecoder = nil                      // drop the old profile's ADS-B decoder/list
    }
    // ADOPT THE PROFILE'S DEFAULT DEMODULATOR (start_mod) whenever it appears while a switch is pending —
    // this config OR a later one. Routed through applyStartMod so a DIGIMODE (adsb) engages as a secondary
    // decoder, not a dead primary demod. Only clearing the pending flag once we've got it fixes DAB→NFM.
    if let sm = c["start_mod"] as? String {
      // ALWAYS adopt the profile's start_mod when a config carries it — like the phone. The old gated
      // version skipped it whenever it wasn't an explicit switch (initial connect / reconnect to a DAB or
      // ADS-B profile), so the demod didn't auto-select and you had to pick it by hand. A digimode that
      // NEWLY engaged also needs the demod re-asserted so its raw-IF decoder chain builds.
      let before = secondaryDecoder
      applyStartMod(sm)
      pendingProfileSwitch = false
      if secondaryDecoder != nil, secondaryDecoder != before {
        sendDemod(); send(["type": "dspcontrol", "action": "start"])
      }
    }
    lastConfigCenter = centerFreq
    audioDec.reset(); hdAudioDec.reset()
    sendDemod()
    // RE-ASSERT dspcontrol start AFTER the demod on a profile switch. The single start at connect fires
    // before the profile/mod exist, so the DSP chain isn't (re)built around the new demod — FATAL for
    // DAB (and ADS-B): the dablin/decoder chain must be assembled with the digital mod set, or no
    // metadata/decode ever flows. Proven against the server: without this, 0 metadata; with it, 75.
    if adopt { send(["type": "dspcontrol", "action": "start"]) }
  }

  private static let wireToSpike: [String: String] = [
    "nfm": "fm", "fm": "fm", "wfm": "wfm", "am": "am", "sam": "sam",
    "usb": "usb", "lsb": "lsb", "cw": "cwu", "dab": "dab",
  ]

  // Decode the LATEST FFT frame (coalesced — see onData). Runs on decodeQueue.
  nonisolated private func drainFft() {
    fftLock.lock()
    let b = latestFft; latestFft = nil; fftScheduled = false
    fftLock.unlock()
    if let b { onBinary(b) }
  }

  // Profile entries can be objects {id,name} OR bare strings — handle both (like the phone). Runs
  // OFF main (a large profiles list must not be grouped on the UI thread — that's the stall).
  nonisolated private func buildProfiles(_ list: [Any]) -> [SDRProfile] {
    let raw: [(id: String, name: String)] = list.compactMap { el in
      if let d = el as? [String: Any] {
        let id = (d["id"] as? String) ?? String(describing: d["id"] ?? "")
        return id.isEmpty ? nil : (id, (d["name"] as? String) ?? id)
      } else if let s = el as? String { return (s, s) }
      return nil
    }
    // Group by SDR (id prefix before "|"); SDR name = common prefix of the group's profile names.
    var groups: [String: [(id: String, name: String)]] = [:]
    for r in raw { groups[String(r.id.split(separator: "|").first ?? ""), default: []].append(r) }
    var out: [SDRProfile] = []
    for (_, items) in groups.sorted(by: { $0.key < $1.key }) {
      let sdrName = commonPrefix(items.map { $0.name }).trimmingCharacters(in: .whitespaces)
      for it in items {
        var pname = it.name
        if !sdrName.isEmpty, pname.hasPrefix(sdrName) { pname = String(pname.dropFirst(sdrName.count)).trimmingCharacters(in: .whitespaces) }
        out.append(SDRProfile(id: it.id, name: pname.isEmpty ? it.name : pname,
                              sdrName: sdrName.isEmpty ? "SDR" : sdrName, active: it.id == activeProfileId))
      }
    }
    return out
  }

  nonisolated private func commonPrefix(_ strs: [String]) -> String {
    guard var pre = strs.first else { return "" }
    for s in strs.dropFirst() {
      var k = pre.startIndex, j = s.startIndex
      while k < pre.endIndex, j < s.endIndex, pre[k] == s[j] { k = pre.index(after: k); j = s.index(after: j) }
      pre = String(pre[..<k]); if pre.isEmpty { break }
    }
    return pre
  }

  // ── inbound binary (FFT type 1 / audio type 2,4) ──
  nonisolated private func onBinary(_ buf: [UInt8]) {
    guard buf.count > 1 else { return }
    let type = buf[0]
    let payload = buf[1...]
    switch type {
    case 1: onFft(payload)
    case 2: onAudio(payload, 12_000, audioDec)
    case 4: onAudio(payload, 48_000, hdAudioDec)
    default: break
    }
  }

  // Audio decoded + played on the socket queue (OFF main), like UberClient/KiwiClient.
  nonisolated private func onAudio(_ payload: ArraySlice<UInt8>, _ rate: Int, _ dec: OwrxAudioDecoder) {
    var pcm: [Int16]
    if audioCompressionSnapshot == "adpcm" {
      pcm = dec.decode(payload)
    } else {
      let bytes = Array(payload); let n = bytes.count >> 1
      pcm = [Int16](repeating: 0, count: n)
      for i in 0..<n { pcm[i] = Int16(bitPattern: UInt16(bytes[i*2]) | (UInt16(bytes[i*2+1]) << 8)) }  // LE
    }
    guard !pcm.isEmpty else { return }
    // DAB speed fix: under-state the rate so the resampler stretches DAB back to correct speed.
    let playRate = (modeSnapshot == "dab") ? Int(Double(rate) * dabRateScale) : rate
    audio.play(pcm: pcm, rate: Int32(playRate), channels: 1)
    frameCount &+= 1                                   // off-main; no per-frame main hop
    if !sawFirstFrame { sawFirstFrame = true; Task { @MainActor in self.everFrame = true; if self.status != "live" { self.status = "live" } } }
  }
  nonisolated(unsafe) private var modeSnapshot = "am"   // read off-main in onAudio; benign

  // FFT decoded off main; slice the view window; hop to main to enqueue the row.
  nonisolated(unsafe) private var lastFftAt = 0.0
  nonisolated private func onFft(_ payload: ArraySlice<UInt8>) {
    // THROTTLE to ~10 fps. OWRX pushes FFT fast and each frame is a whole-profile ADPCM decode +
    // DSP — at full rate that pegged a watch core (>100%). The waterfall doesn't need more than
    // ~10 fps; SKIP the decode entirely on dropped frames (the decode is the expensive part).
    let now = ProcessInfo.processInfo.systemUptime
    if now - lastFftAt < 0.16 { return }        // ~6 fps — the watch can't chew OWRX's full FFT rate
    lastFftAt = now
    var row: [Float]
    if fftCompressionSnapshot == "adpcm" {
      row = decodeOwrxFftFrame(payload)
    } else {
      let bytes = Array(payload); let n = bytes.count / 4
      row = [Float](repeating: 0, count: n)
      bytes.withUnsafeBytes { rb in
        for i in 0..<n { row[i] = Float(bitPattern: rb.loadUnaligned(fromByteOffset: i*4, as: UInt32.self)) }
      }
    }
    guard row.count > 8 else { return }
    frameCount &+= 1        // FFT counts as data too — the watchdog must not reconnect while the
                            // waterfall is flowing just because audio momentarily gapped.
    if !sawFirstFrame { sawFirstFrame = true; Task { @MainActor in self.everFrame = true; if self.status != "live" { self.status = "live" } } }
    lastRow = row
    Task { @MainActor in self.pushSlice(row) }
  }
  nonisolated(unsafe) private var fftCompressionSnapshot = "none"

  private func defaultSpanForMode() -> Double {
    switch mode { case "wfm": return 200_000; case "fm", "nfm": return 30_000; default: return 12_000 }
  }

  // ── View slice: the FFT spans center±sampRate/2; extract [viewCenter±viewBw/2], decimate to width.
  private var specQueue: [(t: Double, row: [UInt8])] = []
  private let spectrumDelay = 0.15
  private var out256 = [UInt8]()

  private func pushSlice(_ row: [Float]) {
    guard sampRate > 0, !row.isEmpty else { return }
    let n = row.count
    let lo = centerFreq - sampRate / 2
    let hzPerBin = sampRate / Double(n)
    let vlo = viewCenter - viewBw / 2, vhi = viewCenter + viewBw / 2
    var i0 = Int((vlo - lo) / hzPerBin), i1 = Int((vhi - lo) / hzPerBin)
    i0 = max(0, min(n - 1, i0)); i1 = max(i0 + 1, min(n, i1))
    let slice = Array(row[i0..<i1])
    let processed = proc.process(slice, centerHz: viewCenter, bwHz: viewBw)
    signalLevel = proc.level
    let dec = decimate(processed, to: WaterfallBuffer.width)
    if dec.count == WaterfallBuffer.width { rowsPushed += 1; specQueue.append((ProcessInfo.processInfo.systemUptime, dec)) }
  }

  func drainSpectrum(now: Double) {
    while let first = specQueue.first, now - first.t >= spectrumDelay {
      waterfall.push(row: first.row); specQueue.removeFirst()
    }
  }

  private func decimate(_ r: [UInt8], to width: Int) -> [UInt8] {
    let n = r.count
    if n == width { return r }
    guard n > 0 else { return [] }
    if out256.count != width { out256 = [UInt8](repeating: 0, count: width) }
    let ratio = Double(n) / Double(width)
    for i in 0..<width {
      let a = Int(Double(i) * ratio), b = min(n, max(a + 1, Int(Double(i + 1) * ratio)))
      var m: UInt8 = 0; for j in a..<b { if r[j] > m { m = r[j] } }
      out256[i] = m
    }
    return out256
  }

  // ── Control ──
  private func sendDemod() {
    modeSnapshot = mode; fftCompressionSnapshot = fftCompression
    let m = Self.modeMap[mode] ?? ("am", -4500, 4500)
    // Raw-IF decoders (DAB/DRM/ADS-B and any digimode with an 'empty' underlying or an ifRate) must get
    // NULL cuts — a numeric bandpass inserts a resampler that STARVES the fixed-rate decoder (no decode).
    let decDef = secondaryDecoder.flatMap { serverModes[$0] }
    let rawIf = mode == "dab" || mode == "drm" || mode == "adsb"
      || (decDef.map { $0.underlying.contains("empty") || $0.ifRate > 0 } ?? false)
    // ADS-B's profile config carries NO center_freq (it's the raw 1090 MHz IF), so don't gate on it and
    // don't compute an offset — the decoder works on the whole IF. Other modes still need a valid centre.
    guard started, centerFreq != 0 || rawIf else { return }
    let offset = (centerFreq != 0 && !rawIf) ? Int((frequency - centerFreq).rounded()) : 0
    // A secondary decoder (adsb, ft8, packet…) rides on top of the carrier via secondary_mod (else false).
    var params: [String: Any] = [
      "offset_freq": offset,
      "mod": m.mod, "squelch_level": -150,
      "secondary_mod": (secondaryDecoder as Any?) ?? false,
    ]
    if secondaryDecoder != nil { params["secondary_offset_freq"] = 1000 }
    if rawIf { params["low_cut"] = NSNull(); params["high_cut"] = NSNull() }
    else { params["low_cut"] = Int(bwLow.rounded()); params["high_cut"] = Int(bwHigh.rounded()) }
    // DAB: without a service id the server outputs NO audio. Send the adopted/selected programme.
    if mode == "dab", audioServiceId >= 0 { params["audio_service_id"] = audioServiceId }
    send(["type": "dspcontrol", "params": params])
  }

  func tune(delta: Int, step: Double) {
    guard delta != 0 else { return }
    let base = delta > 0 ? (frequency / step).rounded(.down) : (frequency / step).rounded(.up)
    var f = (base + Double(delta)) * step
    // CLAMP to the profile window — NEVER auto-switch (etiquette). Edge = a hard wall.
    let half = sampRate / 2
    if half > 0 { f = min(max(f, centerFreq - half), centerFreq + half) }
    guard f != frequency else { return }
    frequency = f; viewCenter = f
    stationName = ""   // left the old station — drop its RDS name until the new one's ps arrives
    sendDemod()
  }
  func tuneTo(_ hz: Double) {
    var f = hz; let half = sampRate / 2
    if half > 0 { f = min(max(f, centerFreq - half), centerFreq + half) }
    guard f != frequency else { return }
    frequency = f; viewCenter = f
    stationName = ""   // retune clears the stale RDS name (a no-RDS station must not inherit it)
    sendDemod()
  }
  func zoom(delta: Int) {
    let factor = pow(2.0, Double(-delta))
    let maxSpan = sampRate > 0 ? sampRate : 200_000
    viewBw = min(maxSpan, max(2_000, viewBw * factor))
  }
  func setVolume(_ v: Double) { audio.setVolume(Float(v)) }
  func setMode(_ m: String) {
    let target = m.lowercased()
    if m != "wfm" { stationName = "" }   // leaving WFM → the RDS name no longer applies
    audioDec.reset(); hdAudioDec.reset()
    // A DIGIMODE picked by hand (adsb, ft8, packet…) must engage as a SECONDARY decoder, not a primary
    // demod — same as a profile's start_mod. Route it through applyStartMod so manual selection works too.
    if let sm = serverModes[target], sm.type == "digimode" {
      applyStartMod(target)
      sendDemod(); send(["type": "dspcontrol", "action": "start"])   // re-assert so the decoder chain builds
      return
    }
    guard m != mode else { return }
    secondaryDecoder = nil               // leaving a digimode for a plain analog demod
    fftSuppressed = false                // analog demod shows a waterfall again
    aircraft = []                        // drop the ADS-B list when switching to an analog demod
    mode = m; modeSnapshot = m
    if let p = Self.modeMap[m] { bwLow = p.lo; bwHigh = p.hi }
    sendDemod()
  }
  func setBandwidth(_ low: Double, _ high: Double) { bwLow = low; bwHigh = high; sendDemod() }
  // OWRX never dropped its socket on suspend (the FFT keeps flowing), so resume just clears the
  // background status — otherwise it stays "background · audio only" forever on return, and isBackground
  // stays stuck true (the debug pill showed this even with the waterfall live).
  func resumeSpectrum() { if status.hasPrefix("background") { status = "live" } }
  func suspend() { specQueue.removeAll(); status = "background · audio only" }
  func reconnectIfNeeded() {}
  func goIdle() {
    goingIdle = true
    rateTimer?.invalidate(); rateTimer = nil
    sock.cancel(); audio.stop()
    if status != "dropped" { status = "idle" }
  }

  /// EXPLICIT profile switch (from the profile menu only — never automatic). Retunes the shared SDR.
  func selectProfile(_ id: String) {
    activeProfileId = id
    pendingProfileSwitch = true   // force the incoming config to adopt this profile's start_mod
    profiles = profiles.map { var p = $0; p.active = (p.id == id); return p }
    audioDec.reset(); hdAudioDec.reset()
    send(["type": "selectprofile", "params": ["profile": id]])
    status = "switching profile…"
  }
}
