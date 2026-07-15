import Foundation
import Combine

/// A DIRECT UberSDR client, running on the WATCH. No phone anywhere in the chain.
///
/// This is the whole question JR asks: today the phone holds these sockets, does the DSP,
/// decodes the audio, and hands the watch a finished picture. Here the watch does all of
/// it. The protocol is UberSDR's own, ported from `src/services/UberSDRClient.ts` and
/// `VibePowerModule.swift` — same endpoints, same frame formats, same session uuid shared
/// between the two sockets.
///
/// Hard-coded to one server on purpose. A spike that also has to be an app is a spike that
/// never gets finished.
@MainActor
final class UberClient: ObservableObject {

  static let host = "stuey3d.tunnel.ubersdr.org"

  // ── Published state (the UI mirrors this and nothing else) ────────────────
  @Published var status = "starting"
  @Published var frequency: Double = 648_000        // Radio Caroline
  @Published var mode = "am"
  @Published var binCount = 0
  @Published var binBandwidth: Double = 0
  @Published var centerHz: Double = 0
  @Published var audioRoute = "—"
  @Published var audioLive = false
  @Published var framesPerSec: Double = 0
  @Published var audioPerSec: Double = 0

  /// NONISOLATED, deliberately.
  ///
  /// This class is `@MainActor`, but SwiftUI's `Canvas` draw closure does NOT necessarily
  /// run on the main actor — so reading `client.waterfall` from inside it is an isolation
  /// violation, and it traps the instant frames start arriving. "It crashes when it tries
  /// to render" was exactly that, and it was in the code all along.
  ///
  /// The shipped watch app never hits it because `WatchLink` is a plain ObservableObject
  /// with no actor isolation. The buffer itself is built for this: rows go in from the data
  /// path, pixels come out on the render clock, and it has been doing that on the wrist for
  /// weeks.
  nonisolated(unsafe) let waterfall = WaterfallBuffer()

  /// A STABLE session id, persisted across launches.
  ///
  /// It was a fresh UUID every launch, and that is poison on a watch: watchOS suspends the
  /// app WITHOUT WARNING, so the sockets never close cleanly and the server keeps the
  /// session open. A new uuid next launch means a new session — and the old one is still
  /// sitting there. Six test runs later the server rejects you for being connected six
  /// times, and it is quite right to.
  ///
  /// Reusing one id means a relaunch RE-ATTACHES to the session it left behind instead of
  /// stacking another on top of it. JR needs this for exactly the same reason: a watch app
  /// does not get to run its teardown, so it must not depend on having done so.
  private var uuid: String = {
    let key = "wristsdr.session.uuid"
    if let s = UserDefaults.standard.string(forKey: key), !s.isEmpty { return s }
    let s = UUID().uuidString.lowercased()
    UserDefaults.standard.set(s, forKey: key)
    return s
  }()

  /// Once per launch, and only after a rejection.
  private var rotated = false
  private func rotateSession() -> Bool {
    guard !rotated else { return false }
    rotated = true
    uuid = UUID().uuidString.lowercased()
    UserDefaults.standard.set(uuid, forKey: "wristsdr.session.uuid")
    return true
  }
  private let proc = SignalProcessor()
  private let opus = OpusDecoder()
  private let audio = WatchAudio()

  /// BOTH sockets are Network framework. NEITHER is URLSession.
  ///
  /// Moving only the audio across was not enough: audio then ran perfectly at 50 packets/sec
  /// and the SPECTRUM — the one still on `URLSessionWebSocketTask` — failed with "Socket is
  /// not connected". The loser is not "the second one", it is "the URLSession one". Whatever
  /// watchOS does to a WebSocket task once another stream is live, it does it reliably, and
  /// no arrangement of separate URLSessions avoids it. NWConnection is simply the API that
  /// works here — which is, in the end, the same conclusion the PHONE reached in v5.1.2 when
  /// URLSessionWebSocketTask stalled its audio and we replaced it with NWConnection.
  private let specSock  = AudioSocket(name: "spec")
  private let audioSock = AudioSocket(name: "audio")

  /// ONE URLSession PER SOCKET.
  ///
  /// They shared a session, and the SECOND WebSocket to come up simply never connected —
  /// "Socket is not connected", with the first one running perfectly beside it. Audio at 26
  /// packets/sec and a spectrum socket that had never opened at all.
  ///
  /// The phone never hits this because it runs its audio through native NWConnection and
  /// only the spectrum through a WebSocket: one socket per stack, by accident. Two
  /// concurrent WebSocket tasks on one URLSession is the case nobody tests, and on watchOS
  /// it does not work. Worth knowing for JR, which needs exactly two.
  private lazy var audioSession = Self.makeSession()
  private lazy var httpSession  = Self.makeSession()

  private static func makeSession() -> URLSession {
    let c = URLSessionConfiguration.default
    c.waitsForConnectivity = true
    // The watch's own Wi-Fi/cellular — nothing is proxied through the phone.
    return URLSession(configuration: c)
  }

  /// The predicted view, exactly as the phone does it: gestures must move the picture NOW,
  /// not one round-trip later.
  private var viewCenterHz: Double = 0
  private var viewBinBw: Double = 0

  private var frameCount = 0
  private var audioCount = 0
  private var rateTimer: Timer?

  // ── Lifecycle ─────────────────────────────────────────────────────────────

  func start() {
    waterfall.setLUT(Viridis.lut)
    waterfall.contrast = 0          // the DSP does the contrast; the buffer just paints
    waterfall.brightness = 0
    proc.autoContrast = 5           // 10 (UberSDR's own) crushes the noise floor to black

    // .common mode. `Timer.scheduledTimer` installs in DEFAULT mode, and while you are
    // turning the crown the run loop is in TRACKING mode — where default-mode timers do not
    // fire at all. The counters would freeze exactly when you were interacting, which is
    // exactly when you want to read them.
    let t = Timer(timeInterval: 1, repeats: true) { [weak self] _ in
      Task { @MainActor in
        guard let self else { return }
        self.framesPerSec = Double(self.frameCount)
        self.audioPerSec  = Double(self.audioCount)
        self.frameCount = 0
        self.audioCount = 0
      }
    }
    RunLoop.main.add(t, forMode: .common)
    rateTimer = t

    Task { await connect() }
  }

  private func connect() async {
    status = "registering"
    if !(await postConnection()) {
      // DO NOT overwrite the reason. postConnection() already put the SERVER'S OWN words in
      // `status`, and clobbering them with "REJECTED by server" threw away the only piece of
      // evidence there was.
      //
      // One retry, on a FRESH session id. The stable id is right — it stops a suspended app
      // stacking a new session on top of the one it abandoned — but it has a failure mode:
      // if the server is still holding that id open, we are locked out of our own session
      // forever, and no relaunch can fix it. Rotating once turns a permanent lockout into a
      // single bad connect.
      let old = status
      guard rotateSession() else { return }
      status = "retrying (fresh session) · was: \(old)"
      if !(await postConnection()) { return }
    }

    // AUDIO FIRST, THEN SPECTRUM WHEN THE AUDIO SOCKET SAYS IT IS UP.
    //
    // This used to be two guessed sleeps — 1.5s for the session to register, 3s for the rate
    // limiter — and guessed sleeps are why the connection was a lottery. Both numbers were
    // invented, neither was observable, and when either was wrong the socket died.
    //
    // The socket already knows. It reports `.ready` the moment the handshake completes, so
    // the spectrum socket now waits for THAT, not for a clock. The remaining 1s is the one
    // delay that is real and documented: UberSDR rate-limits new WebSockets to 2/sec per IP.
    //
    // Order matters and is not arbitrary: the audio socket is the DURABLE one — it survives
    // lock and background, and it is the only one the user can hear die. The spectrum socket
    // is disposable by design; we already drop and reopen it on every pause/resume. So audio
    // gets the clean slot, and the spectrum goes second because it is the one we know how to
    // lose cheaply.
    // RESET THE GATE. `specOpened` is a one-shot guard, and connect() runs more than once —
    // the retry path calls it, the wrist-up reconnect calls it. Leaving it latched from a
    // previous attempt meant the callback returned early and openSpectrum() was NEVER CALLED
    // AT ALL on any connect after the first. Not "failed": never attempted. That is what an
    // empty `S: —` on screen was telling us, and it is the whole reason the waterfall was
    // missing while the audio was fine.
    specOpened = false

    audioSock.onReady = { [weak self] in
      Task { @MainActor in
        guard let self, !self.specOpened else { return }
        self.specOpened = true
        try? await Task.sleep(nanoseconds: 1_000_000_000)   // the server's 2/sec limit
        self.openSpectrum()
      }
    }
    openAudio()

    // AND NEVER LET THE SPECTRUM BE HOSTAGE TO THE AUDIO. Chaining the waterfall off the
    // audio socket's handshake is right when the audio comes up — but if it never does, the
    // waterfall must not be punished for it. Open it anyway.
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: 8_000_000_000)
      guard !self.specOpened else { return }
      self.specOpened = true
      self.specWsState = "spec: audio never readied — opening anyway"
      self.openSpectrum()
    }

    audio.start { [weak self] ok, info in
      Task { @MainActor in
        self?.audioLive = ok
        self?.audioRoute = ok ? info : "FAILED: \(info)"
      }
    }
    status = "live"
  }

  private var specOpened = false

  private func postConnection() async -> Bool {
    var req = URLRequest(url: URL(string: "https://\(Self.host)/connection")!)
    req.httpMethod = "POST"
    req.setValue("application/json", forHTTPHeaderField: "Content-Type")
    // The same headers the phone sends. It is not obvious that the server cares — but a
    // rejection with no reason and an unfamiliar User-Agent is not the moment to be
    // different for the sake of it.
    req.setValue("VibeSDR/2.0 (watchOS; WristSDR spike)", forHTTPHeaderField: "User-Agent")
    req.setValue("VibeSDR", forHTTPHeaderField: "X-Requested-With")
    req.httpBody = try? JSONSerialization.data(withJSONObject: ["user_session_id": uuid])
    do {
      let (data, resp) = try await httpSession.data(for: req)
      guard let http = resp as? HTTPURLResponse else {
        status = "no HTTP response"
        return false
      }
      let obj = try? JSONSerialization.jsonObject(with: data)
      let j = obj as? [String: Any]
      let allowed = (j?["allowed"] as? Bool) ?? false
      if !allowed {
        // THE SERVER SAID WHY. Throwing that away and printing "REJECTED" was the single
        // most useless thing this app could have done — the answer was in the response
        // body the whole time.
        let reason = (j?["reason"] as? String)
          ?? String(data: data.prefix(80), encoding: .utf8)
          ?? "no reason given"
        status = "HTTP \(http.statusCode): \(reason)"
      }
      return allowed
    } catch {
      status = "connection failed: \(error.localizedDescription)"
      return false
    }
  }

  // ── Spectrum ──────────────────────────────────────────────────────────────

  private func openSpectrum() {
    let url = URL(string: "wss://\(Self.host)/ws/user-spectrum?user_session_id=\(uuid)&mode=binary8")!

    specSock.onData = { [weak self] d in
      Task { @MainActor in self?.onSpectrumBinary(d) }
    }
    specSock.onText = { [weak self] t in
      Task { @MainActor in self?.onSpectrumJSON(Data(t.utf8)) }
    }
    specSock.onState = { [weak self] st in
      Task { @MainActor in
        guard let self else { return }
        self.specWsState = st
        if st.contains("failed") || st.contains("recv:") { self.retrySpectrum() }
      }
    }
    // SUBSCRIBE ONLY ONCE THE SOCKET IS ACTUALLY UP.
    //
    // This used to fire the instant `open()` was called — before the handshake had finished.
    // `NWConnection.send()` on a connection that is not yet `.ready` does not reliably queue;
    // the subscribe simply evaporated. The socket then opened beautifully, the server never
    // learned which band we wanted, and it sent nothing. Forever.
    //
    // And because the socket had not FAILED, the retry never fired — a silent socket looked
    // exactly like a healthy one. That is the same shape as the frozen-waterfall bug v9 just
    // fixed on the phone, and it is worth saying plainly: an open socket is not a working
    // socket, and only FRAMES prove it.
    specSock.onReady = { [weak self] in
      Task { @MainActor in
        guard let self else { return }
        self.sendView(self.frequency, self.viewBinBw > 0 ? self.viewBinBw : 100)
        self.armSpectrumWatchdog()
      }
    }
    specSock.open(url: url)
  }

  /// FRAMES OR IT DIDN'T HAPPEN. Ready and silent is a real state, and it needs its own way
  /// out — first re-ask for the band, then tear the socket down and start again.
  private func armSpectrumWatchdog() {
    let n = frameCount
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: 5_000_000_000)
      guard self.frameCount == n else { return }             // frames arrived — all well
      self.specWsState = "spec ready but SILENT · re-subscribing"
      self.sendView(self.frequency, self.viewBinBw > 0 ? self.viewBinBw : 100)

      try? await Task.sleep(nanoseconds: 5_000_000_000)
      guard self.frameCount == n else { return }
      self.specWsState = "spec SILENT · reopening"
      self.specSock.cancel()
      self.retrySpectrum()
    }
  }

  /// What the sockets are actually doing. On screen, because a spike that cannot tell you
  /// why it is empty is not a spike, it is a mystery.
  @Published var wsDiag = ""
  @Published var specWsState = ""

  /// WRIST DOWN KILLS THE SOCKETS, and nothing was watching.
  ///
  /// watchOS suspends the app when the screen sleeps; the WebSockets die with it, and on
  /// wake there is nothing to bring them back — so the waterfall stopped and stayed
  /// stopped. This is the SAME class of bug V9 just fixed on the phone (a spectrum socket
  /// with no watchdog), and finding it here within minutes says something: it is not an
  /// UberSDR quirk, it is what happens to any long-lived socket on a device that sleeps.
  ///
  /// JR would need the same starvation watchdog the phone now has. Noting it as a real
  /// cost, not papering over it — the spike just needs to survive long enough to measure.
  /// Going away — say so, rather than being killed and leaving the server holding a socket
  /// it thinks is alive. Best effort: watchOS may suspend us before this ever runs, which
  /// is precisely why the session uuid above is stable and not fresh each launch.
  func suspend() {
    // DROP THE SPECTRUM. KEEP THE AUDIO. This is the whole point of background audio, and we
    // were defeating it ourselves: suspend() used to cancel BOTH sockets, so the moment the
    // wrist dropped the app shut the audio off — and then we went looking for the watchOS
    // setting that would keep it alive. No entitlement can save audio the app itself kills.
    //
    // The asymmetry is the same one the phone already lives by: nobody is looking at the
    // waterfall with the screen off, so the spectrum socket is pure cost and goes. The audio
    // is the reason the app is still running at all, so it stays — that is what
    // WKBackgroundModes=[audio] and .longFormAudio are FOR.
    specSock.cancel()
    specOpened = false
    status = "background · audio only"
  }

  /// The app is really going away (not just wrist-down) — let the server go too, rather than
  /// leaving it holding a socket it believes is alive.
  func teardown() {
    specSock.cancel()
    audioSock.cancel()
    audio.stop()
  }

  /// Wrist back up: the audio never stopped, so only the waterfall needs bringing home.
  func resumeSpectrum() {
    guard status.hasPrefix("background") else { return }
    status = "live"
    specRetries = 0
    specOpened = true
    openSpectrum()
  }

  func reconnectIfNeeded() {
    // ONLY IF IT WAS EVER WORKING, and only if it has been dead for a while.
    //
    // Without those two guards this kills the connection it exists to protect: scenePhase
    // fires `.active` the moment the view appears, when fps is legitimately 0 because
    // nothing has arrived YET — and it cancelled the sockets it had just opened. A
    // watchdog that cannot tell "not started" from "died" is worse than no watchdog.
    guard everHadFrames, status == "live" else { return }
    guard Date().timeIntervalSince(lastFrameAt) > 3 else { return }
    status = "reconnecting"
    specSock.cancel()
    audioSock.cancel()
    Task { await connect() }
  }
  private var everHadFrames = false
  private var lastFrameAt = Date.distantPast

  /// UberSDR sends its JSON config as a GZIPPED BINARY frame, not a text frame — the magic
  /// bytes are the only way to tell it from a spectrum frame. (The web client sniffs for
  /// exactly this before reaching for DecompressionStream.)
  private func onSpectrumBinary(_ d: Data) {
    if d.count >= 2, d[0] == 0x1f, d[1] == 0x8b {
      if let un = Gzip.inflate(d) { onSpectrumJSON(un) }
      return
    }
    guard d.count >= 22 else { return }

    let magic: UInt32 = d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt32.self) }
    guard magic == 0x4345_5053 else { return }   // "SPEC" little-endian

    // COUNT IT HERE. It used to be counted after the `flags` switch — which `return`s early
    // on frame types we don't decode — so a working feed could report 0 fps and the whole
    // measurement was of nothing at all. Count the frame when the frame ARRIVES; whether we
    // like its format is a separate question.
    frameCount += 1
    everHadFrames = true
    lastFrameAt = Date()

    let flags = d[5]
    let lo: UInt32 = d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 14, as: UInt32.self) }
    let hi: UInt32 = d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 18, as: UInt32.self) }
    let freq = Double(lo) + Double(hi) * 4_294_967_296

    let body = d.subdata(in: 22..<d.count)

    // binary8: uint8 = clamp(dBFS,-256,0)+256  →  dBFS = uint8 - 256
    switch flags {
    case 0x03:                                     // full uint8
      if bins.count != body.count { bins = [Float](repeating: -120, count: body.count) }
      body.withUnsafeBytes { raw in
        let p = raw.bindMemory(to: UInt8.self)
        for i in 0..<body.count { bins[i] = Float(Int(p[i]) - 256) }
      }
    case 0x04:                                     // delta uint8
      guard body.count >= 2 else { return }
      let changes: UInt16 = body.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt16.self) }
      var off = 2
      for _ in 0..<Int(changes) {
        guard off + 3 <= body.count else { break }
        let idx: UInt16 = body.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: off, as: UInt16.self) }
        let val = body[body.startIndex + off + 2]
        off += 3
        if Int(idx) < bins.count { bins[Int(idx)] = Float(Int(val) - 256) }
      }
    case 0x01:                                     // full float32
      let n = body.count / 4
      if bins.count != n { bins = [Float](repeating: -120, count: n) }
      body.withUnsafeBytes { raw in
        for i in 0..<n { bins[i] = raw.loadUnaligned(fromByteOffset: i * 4, as: Float32.self) }
      }
    case 0x02:                                     // delta float32
      guard body.count >= 2 else { return }
      let changes: UInt16 = body.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt16.self) }
      var off = 2
      for _ in 0..<Int(changes) {
        guard off + 6 <= body.count else { break }
        let idx: UInt16 = body.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: off, as: UInt16.self) }
        let v: Float32 = body.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: off + 2, as: Float32.self) }
        off += 6
        if Int(idx) < bins.count { bins[Int(idx)] = v }
      }
    default:
      // Say so, loudly. An unhandled frame type used to be a silent `return` — the frame
      // counter ticked up and the waterfall stayed black, which looks like a render bug and
      // is a protocol bug.
      unknownFlags = flags
      return
    }

    centerHz = freq

    // ── THE COST JR PAYS. Unwrap, then the full DSP, then the paint. Every frame.
    let n = bins.count
    guard n > 1 else { return }
    if unwrapped.count != n { unwrapped = [Float](repeating: 0, count: n) }
    let half = n / 2
    // radiod sends [DC→+Nyquist, −Nyquist→DC]; the display wants [negative, positive].
    // Without this every signal is drawn half a span from where it actually is.
    for i in 0..<half { unwrapped[i] = bins[half + i] }
    for i in 0..<half { unwrapped[half + i] = bins[i] }

    let row = proc.process(unwrapped, centerHz: freq, bwHz: binBandwidth * Double(n))
    let dec = decimate(row, to: WaterfallBuffer.width)
    // WaterfallBuffer DROPS rows that aren't exactly its width, silently. A blank waterfall
    // with a healthy frame count is exactly what that looks like.
    if dec.count == WaterfallBuffer.width { rowsPushed += 1 }
    waterfall.push(row: dec)
  }

  /// UberSDR sends 1024 bins; the watch draws 256. So three quarters of every frame is
  /// received, DSP'd, and then thrown away — which is the concrete argument for making
  /// `binCount` requestable in the VibeServer protocol rather than inherited.
  ///
  /// PEAK, not mean. Averaging four bins together buries a narrow carrier in the noise
  /// beside it — the signal you are hunting is exactly the one a mean would erase.
  private func decimate(_ row: [UInt8], to width: Int) -> [UInt8] {
    let n = row.count
    if n == width { return row }
    guard n > 0 else { return [] }
    if out.count != width { out = [UInt8](repeating: 0, count: width) }
    let ratio = Double(n) / Double(width)
    for i in 0..<width {
      let lo = Int(Double(i) * ratio)
      let hi = min(n, max(lo + 1, Int(Double(i + 1) * ratio)))
      var m: UInt8 = 0
      for k in lo..<hi where row[k] > m { m = row[k] }
      out[i] = m
    }
    return out
  }

  private var bins: [Float] = []
  private var unwrapped: [Float] = []
  private var out: [UInt8] = []
  /// Non-zero = the server is sending a frame format we do not decode, and the waterfall is
  /// black for a reason that has nothing to do with the waterfall.
  @Published var unknownFlags: UInt8 = 0
  /// Rows actually handed to the renderer. `fps` counts frames RECEIVED — if these two
  /// disagree, the data is arriving and being thrown away, which is a completely different
  /// bug from the data not arriving.
  @Published var rowsPushed = 0

  private func onSpectrumJSON(_ d: Data) {
    guard let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { return }
    guard (j["type"] as? String) == "config" else { return }
    if let bc = j["binCount"] as? Int { binCount = bc }
    if let bb = j["binBandwidth"] as? Double { binBandwidth = bb }
    if let cf = j["centerFreq"] as? Double { centerHz = cf }
    if viewBinBw == 0, binBandwidth > 0 {
      viewBinBw = binBandwidth
      viewCenterHz = centerHz
    }

    // RE-ASSERT THE RATE. A binBandwidth change means the session may have MIGRATED between
    // the shared default channel and a private one — and `set_rate` works only on a private
    // session (the shared SSRC is hardcoded to every 2nd tick, and ignores us). So a zoom
    // can silently take away the rate we asked for, or hand us one we didn't. The phone
    // client re-sends on exactly this signal for exactly this reason.
    if binBandwidth != lastRateBinBw {
      lastRateBinBw = binBandwidth
      if rateDivisor > 1 { sendRate() }
    }
  }
  private var lastRateBinBw: Double = 0

  private func sendView(_ freq: Double, _ binBw: Double) {
    viewCenterHz = freq
    viewBinBw = binBw
    let msg: [String: Any] = ["type": "zoom",
                              "frequency": Int(freq.rounded()),
                              "binBandwidth": binBw]
    specSock.send(json: msg)
  }

  // ── Audio ─────────────────────────────────────────────────────────────────

  private func openAudio() {
    // `/ws`, NOT `/ws/audio` — and the tune rides the QUERY STRING, not a message. Taken
    // verbatim from VibePowerModule.audioWsURL.
    guard let url = URL(string:
      "wss://\(Self.host)/ws?user_session_id=\(uuid)" +
      "&frequency=\(Int(frequency))&mode=\(mode)&format=opus&version=2") else { return }

    audioSock.onData = { [weak self] d in
      guard let self else { return }
      // Decode + play OFF the main actor: ~50 packets/sec, and it must never fight the
      // waterfall for the main thread.
      if let out = self.opus.decode(d) {
        self.audio.play(pcm: out.pcm, rate: out.rate, channels: out.channels)
        Task { @MainActor in self.audioCount += 1 }
      }
    }
    audioSock.onState = { [weak self] s in
      Task { @MainActor in
        guard let self else { return }
        self.audioWsState = s
        // A rate-limited socket is not a broken socket — it is an EARLY socket. Back off and
        // come back, rather than sitting there dead until the app is relaunched.
        if s.contains("failed") || s.contains("recv:") { self.retryAudio() }
      }
    }
    audioSock.open(url: url)
  }

  /// What the audio socket is doing, in its own words.
  @Published var audioWsState = ""

  private var specRetries = 0
  private func retrySpectrum() {
    // NEVER STOP TRYING. It gave up after four attempts, which on a wrist — where the app is
    // suspended, the radio sleeps and the path changes under you — means "dead until you
    // relaunch". A receiver that quits on you is not a receiver.
    specRetries += 1
    let wait = UInt64(min(specRetries, 5)) * 2_000_000_000   // 2s → 10s, then hold
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: wait)
      guard self.framesPerSec == 0 else { return }   // it recovered on its own
      self.specWsState = "spec retry \(self.specRetries)…"
      self.openSpectrum()
    }
  }

  private var audioRetries = 0
  private func retryAudio() {
    audioRetries += 1
    let wait = UInt64(min(audioRetries, 5)) * 2_000_000_000
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: wait)
      guard self.audioPerSec == 0 else { return }   // it recovered on its own
      self.audioWsState = "audio retry \(self.audioRetries)…"
      self.openAudio()
    }
  }

  private func sendTune() {
    let msg: [String: Any] = ["type": "tune", "frequency": Int(frequency), "mode": mode]
    audioSock.send(json: msg)
  }

  // ── Controls ──────────────────────────────────────────────────────────────

  /// Crown tuning. The audio socket carries the tune; the spectrum view follows it.
  func tune(delta: Int, step: Double) {
    guard delta != 0 else { return }
    let base = delta > 0 ? (frequency / step).rounded(.down) : (frequency / step).rounded(.up)
    let f = max(10_000, min(30_000_000, (base + Double(delta)) * step))
    guard f != frequency else { return }
    frequency = f
    sendTune()
    sendView(f, viewBinBw > 0 ? viewBinBw : binBandwidth)
  }

  func setMode(_ m: String) { mode = m; sendTune() }

  /// FRAME RATE, in our back pocket.
  ///
  /// UberSDR polls radiod every 100ms — 10 Hz — and `set_rate` divides that server-side, so
  /// halving the frame rate costs us NOTHING to try: no re-render, no re-decode, no rebuild.
  /// The server simply sends half as many frames, and every per-frame cost we are here to
  /// measure (the receive, the unwrap, the DSP, the decimate, the paint) halves with it.
  ///
  /// If 10fps proves too expensive on the wrist, 5fps is the answer — and the waterfall
  /// already interpolates to a 20fps render clock, so the SCROLL stays smooth either way.
  /// What you lose is time resolution, not fluidity. Being able to A/B it on the wrist is
  /// the whole reason it is a toggle and not a constant.
  @Published var rateDivisor = 1 {
    didSet { sendRate() }
  }

  private func sendRate() {
    let msg: [String: Any] = ["type": "set_rate", "divisor": max(1, min(8, rateDivisor))]
    specSock.send(json: msg)
  }

  /// Crown zoom. The REAL server zoom — finer bins, not a magnified crop. That is the only
  /// thing that beats the bin-resolution ceiling, and it is why the watch feels sharp.
  func zoom(delta: Int) {
    guard delta != 0, viewBinBw > 0 else { return }
    let factor = pow(2.0, -Double(delta) / 6.0)
    let n = Double(max(binCount, 256))
    let bb = max(6_000 / n, min(viewBinBw * factor, 30_000_000 / n))
    sendView(viewCenterHz != 0 ? viewCenterHz : frequency, bb)
  }

  var spanHz: Double { binBandwidth * Double(max(binCount, 1)) }
}
