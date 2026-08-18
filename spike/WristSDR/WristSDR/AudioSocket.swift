import Foundation
import Network

/// THE AUDIO WEBSOCKET, ON NETWORK FRAMEWORK — not on URLSession.
///
/// Not a stylistic choice. **Two concurrent `URLSessionWebSocketTask`s do not work on
/// watchOS**: the second one to open never connects ("Socket is not connected") while the
/// first runs perfectly beside it. Separate `URLSession`s made no difference. Whichever
/// socket won was a coin toss — you got a waterfall or you got audio, never both.
///
/// The phone never discovered this because it never had two: it runs audio through native
/// `NWConnection` (VibePowerModule.openAudioWsNW) and only the spectrum through a
/// WebSocket. One socket per network stack, entirely by accident.
///
/// So JR does the same on purpose. This is a straight port of the phone's pattern.
final class AudioSocket {

  /// WHICH SOCKET THIS IS. Both the audio and the spectrum socket are instances of this
  /// class, and every state message it emitted said "audio ws …" — so a SPECTRUM failure
  /// was reported on screen as an audio failure, while audio was visibly running at 50/s.
  /// A diagnostic that lies about its own subject costs more than no diagnostic.
  private let name: String
  init(name: String) { self.name = name }

  private var conn: NWConnection?
  private let queue = DispatchQueue(label: "wristsdr.audiows")
  private var gen = 0
  /// ★★★ A CONNECTION THAT NEVER CONNECTS HAD NO WAY OUT.
  ///
  ///  `.waiting` means "not satisfiable YET", and the framework retries toward `.ready` on its
  ///  own — so the rule below is right in general and fatal as an absolute. When the path is
  ///  never going to be satisfied (wrong port, an address this device cannot route, a plain
  ///  `ws://` the stack refuses) NWConnection sits in `.waiting` for ever, perfectly silently.
  ///
  ///  ★★ AND EVERY ESCAPE WAS DOWNSTREAM OF `.ready`. UberClient arms its spectrum watchdog
  ///     inside `onReady`, and retries only on a state containing "failed" — so a socket that
  ///     never became ready armed nothing and failed nothing. Jr sat on "waiting for signal"
  ///     indefinitely with no error anywhere (Stuart, 2026-08-18, UberSDR by LAN address).
  ///     THE SAME SHAPE AS THE PHONE'S: an open socket is not a working socket, and a socket
  ///     that is still opening is not a socket that will open.
  ///  ★★★ AND IT MUST TELL "STUCK" FROM "SLOW", WHICH THE FIRST CUT DID NOT. A flat 8s deadline
  ///      cancelled connections that were WORKING: a tunnel with TLS, reached over the watch's
  ///      Bluetooth relay, legitimately takes longer than that to hand shake. Each cancellation
  ///      threw away real progress, retried, and fell into the caller's 2s→10s ladder — so a
  ///      slow-but-fine server took a FULL MINUTE to show a waterfall where before it just worked
  ///      (Stuart, 2026-08-18, UberSDR over the tunnel).
  ///  ★★★ THE STATES ALREADY SAY WHICH IS WHICH. `.preparing` is a handshake in progress;
  ///      `.waiting` is "this path cannot be satisfied", which is the one that never ends. So the
  ///      deadline only acts on a connection sitting in `.waiting` — a slow handshake is left
  ///      alone however long it takes, and a stuck one still gets its way out.
  ///  ★ 12s of continuous `.waiting`. Nothing recovers from that state by itself here; the margin
  ///    is for a path that flickers on a watch waking its radio.
  private var connectDeadline: DispatchWorkItem?
  private var isWaiting = false
  private static let connectTimeout: TimeInterval = 12
  /// ★★ ONE SILENT ESCALATION BEFORE GIVING UP. The OWRX client reaches a LAN address happily and
  ///    the UberSDR one hangs, and the ONLY difference between the two open() calls is
  ///    `forceIPv4` — OWRX pins v4 because a dynamic-DNS host can hand back an AAAA the watch
  ///    cannot route. So when a connection never arrives, try that once before reporting: it costs
  ///    one timeout, and it is the difference that is already known to work on this device.
  /// ★ Remembered so the retry does not loop: v4-forced is the LAST attempt, never the first.
  private var retriedV4 = false
  /// ★★★ NWConnection CANNOT REACH A PLAIN ws:// ON A PRIVATE ADDRESS — proved on the watch, and
  ///     on the phone before it. The crumb log from build 36:
  ///        UBER preflight POST http://192.168.86.11:8080/connection → HTTP 200 allowed=true
  ///        audio ws waiting: POSIXErrorCode(53): Software caused connection abort
  ///        spec  ws waiting: POSIXErrorCode(53): Software caused connection abort
  ///     An HTTP request to that exact host and port SUCCEEDS in the same second, so it is the
  ///     transport, not the network. URLSession reaches it; Network.framework does not.
  /// ★★ SO FALL BACK, PER HOST, AND REMEMBER. Keyed by host:port because it is a property of the
  ///    address, not of this socket — and shared statically so the spectrum socket does not have
  ///    to rediscover what the audio socket just learned, 12 seconds at a time.
  /// ★ THE KNOWN RISK, WRITTEN DOWN: this file exists because two concurrent
  ///   URLSessionWebSocketTasks did not work on watchOS — the second never connected. If that is
  ///   still true, a LAN server will get one working socket and one dead one. The transport is
  ///   crumbed on every open precisely so that shows up as a FACT instead of a theory.
  private static var forceURLSession = Set<String>()
  private var task: URLSessionWebSocketTask?
  private var urlSession: URLSession?
  private var usingURLSession = false
  private var lastOpen: (url: URL, headers: [(name: String, value: String)], autoReplyPing: Bool, avoidRelay: Bool)?

  /// Raw WebSocket binary frames — one Opus packet each, with UberSDR's 21-byte header.
  var onData: ((Data) -> Void)?
  /// Text frames. The audio socket never sends any; the spectrum socket does.
  var onText: ((String) -> Void)?
  var onState: ((String) -> Void)?
  /// Fires when the handshake actually COMPLETES. The thing to chain the next socket on —
  /// an event, not a guessed sleep.
  var onReady: (() -> Void)?

  /// `headers` = extra WebSocket handshake request headers. KiwiSDR needs a browser User-Agent or
  /// it classifies us as an `ext_api` client and DROPS the connection after a few seconds.
  func open(url: URL, headers: [(name: String, value: String)] = [], forceIPv4: Bool = false, autoReplyPing: Bool = true, avoidRelay: Bool = false) {
    gen &+= 1
    let g = gen
    cancel()
    if !forceIPv4 { retriedV4 = false }          // a fresh caller-driven open starts the ladder again
    lastOpen = (url, headers, autoReplyPing, avoidRelay)
    // ★★★ A PLAIN ws:// ON A PRIVATE ADDRESS: DO NOT EVEN TRY NWConnection. We know how that
    //     ends — POSIX 53 at connect, on this watch and on the phone — so attempting it costs a
    //     12-second deadline and buys nothing. Stuart, on build 40: "local connection still takes
    //     an eternity to connect." An eternity is exactly what a deadline for a KNOWN failure is.
    // ★★ Scoped as narrowly as the evidence: only when the scheme is plain `ws` AND the host is a
    //    private/link-local address or a .local name. Everything else — public hosts, wss, the
    //    tunnel — keeps Network.framework, which is there for the iOS 27 URLSession regression
    //    and is the better transport where it works.
    if !secure && Self.isPrivateHost(url) {
      onState?("\(name) ws: private address on plain ws — using URLSession directly")
      openURLSession(url: url, headers: headers, g: g)
      return
    }
    // ★ Already learned this address needs the other transport — go straight there rather than
    //   spending another 12s deadline discovering it again.
    if Self.forceURLSession.contains(Self.hostKey(url)) {
      openURLSession(url: url, headers: headers, g: g)
      return
    }

    let secure = (url.scheme == "wss")
    let params: NWParameters = secure ? .tls : .tcp
    // avoidRelay: refuse the iPhone Bluetooth relay (it presents as the `.other` interface) so a
    // bandwidth-heavy feed goes over the watch's OWN wifi/cellular instead. watchOS often powers the
    // watch's wifi down while the phone is near, so a caller MUST have a fallback: if no data arrives,
    // reopen with avoidRelay=false or the connection just waits forever.
    if avoidRelay {
      params.prohibitedInterfaceTypes = [.other]
    }
    // Force IPv4 when asked: a dynamic-DNS host (freemyip etc.) can hand back an AAAA the watch
    // can't route home ("no route to host") while IPv4 works fine — pin to v4 to dodge it.
    if forceIPv4, let ip = params.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options {
      ip.version = .v4
    }
    let ws = NWProtocolWebSocket.Options()
    // autoReplyPing=false surfaces .ping to receive() so we PONG manually — belt-and-braces where the
    // native auto-reply seems not to fire (OWRX stalls the stream after a few seconds otherwise).
    ws.autoReplyPing = autoReplyPing
    // Default max WS message is 64 KB. An OWRX ADS-B `secondary_demod` frame (a full aircraft table —
    // 100+ planes) blows past that, and NWConnection silently DROPS the oversized message → no aircraft
    // ever reach the client (sd=0) even though the demod is correct. Raise the ceiling.
    ws.maximumMessageSize = 16 * 1024 * 1024
    if !headers.isEmpty { ws.setAdditionalHeaders(headers) }
    params.defaultProtocolStack.applicationProtocols.insert(ws, at: 0)

    let c = NWConnection(to: .url(url), using: params)
    conn = c
    c.stateUpdateHandler = { [weak self] state in
      guard let self, self.gen == g else { return }
      switch state {
      case .ready:
        // Tag the interface actually in use so the UI can show wifi vs the phone relay (.other).
        self.isWaiting = false
        self.connectDeadline?.cancel(); self.connectDeadline = nil
        self.onState?("\(name) ws ready [\(Self.pathName(c.currentPath))]")
        self.receive(c, g)
      case .waiting(let e):
        // Path not satisfiable YET. NWConnection retries toward .ready on its own — do not
        // tear it down here or you fight the framework and lose.
        // ★ Reported, but NOT acted on: the deadline armed in open() is what ends this state if
        //   it turns out to be permanent. Naming the error matters — POSIX 53 (aborted), 61
        //   (refused) and 65 (no route) send you to three different places.
        self.onState?("\(name) ws waiting: \(e)")
        // ★ The deadline is ARMED HERE, by the state that means stuck — not by open(), which
        //   cannot know yet. Re-entering `.waiting` restarts it; leaving it disarms it.
        self.isWaiting = true
        self.armConnectDeadline(c, g, url)
      case .failed(let e):
        self.isWaiting = false
        self.connectDeadline?.cancel(); self.connectDeadline = nil
        self.onState?("\(name) ws failed: \(e)")
      case .cancelled:
        self.isWaiting = false
        self.connectDeadline?.cancel(); self.connectDeadline = nil
        self.onState?("\(name) ws cancelled")
      default:
        break
      }
    }
    c.start(queue: queue)
  }

  /// ★★ Armed by `.waiting`, cleared by anything else. A connection that is preparing, ready or
  ///    finished has no deadline at all — only one that says it cannot get a path.
  /// RFC1918 / link-local / loopback / .local — the addresses Network.framework will not reach
  /// over a plain ws:// on iOS 27 and watchOS 27.
  private static func isPrivateHost(_ u: URL) -> Bool {
    guard let h = u.host?.lowercased() else { return false }
    if h == "localhost" || h.hasSuffix(".local") { return true }
    let p = h.split(separator: ".").compactMap { Int($0) }
    guard p.count == 4 else { return false }          // not a v4 literal — treat as public
    switch (p[0], p[1]) {
    case (10, _), (127, _), (192, 168): return true
    case (172, 16...31):                return true
    case (169, 254):                    return true   // link-local
    default:                            return false
    }
  }

  private static func hostKey(_ u: URL) -> String {
    "\(u.host ?? "?"):\(u.port.map(String.init) ?? "-")"
  }

  private func armConnectDeadline(_ c: NWConnection, _ g: Int, _ url: URL) {
    connectDeadline?.cancel()
    let dl = DispatchWorkItem { [weak self] in
      guard let self, self.gen == g, self.conn === c, self.isWaiting else { return }
      self.connectDeadline = nil
      c.cancel()
      if !self.retriedV4, let again = self.lastOpen {
        self.retriedV4 = true
        self.onState?("\(self.name) ws waiting: \(Int(Self.connectTimeout))s with no path — retrying pinned to IPv4")
        self.open(url: again.url, headers: again.headers, forceIPv4: true,
                  autoReplyPing: again.autoReplyPing, avoidRelay: again.avoidRelay)
        return
      }
      // ★★★ NW COULD NOT GET A PATH. Before declaring failure, try the transport that reaches
      //     these addresses — see forceURLSession. Only then is it a failure worth reporting.
      let key = Self.hostKey(url)
      if !Self.forceURLSession.contains(key), let again = self.lastOpen {
        Self.forceURLSession.insert(key)
        self.onState?("\(self.name) ws: no path via NWConnection — switching to URLSession for \(key)")
        self.openURLSession(url: again.url, headers: again.headers, g: g)
        return
      }
      // ★ "failed" is the word every caller's retry looks for — and the address is named, so the
      //   person reading it knows WHICH server and port never answered.
      self.onState?("\(self.name) ws failed: no path after \(Int(Self.connectTimeout))s — \(url.host ?? "?"):\(url.port.map(String.init) ?? "default")")
    }
    connectDeadline = dl
    queue.asyncAfter(deadline: .now() + Self.connectTimeout, execute: dl)
  }


  // ── Transport B: URLSession, for addresses Network.framework cannot reach ────────────────
  //
  /// Same callbacks, same semantics, different plumbing. Kept deliberately small: this exists for
  /// LAN servers on plain ws://, not as a second full implementation to maintain.
  /// ★ `maximumMessageSize` matches the NW path — an OWRX ADS-B table blows past the default and
  ///   the framework silently drops the oversized message.
  private func openURLSession(url: URL, headers: [(name: String, value: String)], g: Int) {
    connectDeadline?.cancel(); connectDeadline = nil
    usingURLSession = true
    var req = URLRequest(url: url)
    for h in headers { req.setValue(h.value, forHTTPHeaderField: h.name) }
    let cfg = URLSessionConfiguration.default
    cfg.waitsForConnectivity = true
    let sess = URLSession(configuration: cfg)
    let t = sess.webSocketTask(with: req)
    t.maximumMessageSize = 16 * 1024 * 1024
    urlSession = sess
    task = t
    onState?("\(name) ws opening [URLSession]")
    t.resume()
    // URLSessionWebSocketTask has no "ready" callback — the first successful receive IS the
    // handshake having completed, so report ready there rather than guessing from resume().
    receiveURLSession(t, g, first: true)
  }

  private func receiveURLSession(_ t: URLSessionWebSocketTask, _ g: Int, first: Bool = false) {
    t.receive { [weak self] result in
      guard let self, self.gen == g, self.task === t else { return }
      switch result {
      case .failure(let e):
        self.onState?("\(self.name) ws failed: \(e.localizedDescription)")
      case .success(let msg):
        if first {
          self.onState?("\(self.name) ws ready [URLSession]")
          self.onReady?()
        }
        switch msg {
        case .data(let d):   if !d.isEmpty { self.onData?(d) }
        case .string(let str): self.onText?(str)
        @unknown default: break
        }
        self.receiveURLSession(t, g)
      }
    }
  }

  private func receive(_ c: NWConnection, _ g: Int) {
    c.receiveMessage { [weak self] data, context, _, error in
      guard let self, self.gen == g, self.conn === c else { return }
      if let error {
        self.onState?("\(name) ws recv: \(error)")
        return
      }
      let op = (context?.protocolMetadata(definition: NWProtocolWebSocket.definition)
                as? NWProtocolWebSocket.Metadata)?.opcode
      if op == .ping {
        // MANUAL PONG. `autoReplyPing` is set, but if it silently doesn't fire on watchOS the
        // server (KiwiSDR) sees a missed pong and RSTs us after a few seconds (recv ENOTCONN).
        // Answer every ping ourselves — harmless if the framework already did.
        let meta = NWProtocolWebSocket.Metadata(opcode: .pong)
        let ctx = NWConnection.ContentContext(identifier: "pong", metadata: [meta])
        c.send(content: data ?? Data(), contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in })
      } else if let data, !data.isEmpty {
        if op == .text {
          if let t = String(data: data, encoding: .utf8) { self.onText?(t) }
        } else if op == .binary {
          self.onData?(data)
        }
      }
      self.receive(c, g)
    }
  }

  /// Send a WebSocket PING. Browsers/RN send these periodically to keep the connection alive; a raw
  /// NWConnection does not, so an OWRX stream with no outbound traffic gets reaped by NAT/idle timeout
  /// after a few minutes. A periodic client ping keeps the mapping (and the server session) alive.
  func sendPing() {
    if usingURLSession { task?.sendPing { _ in }; return }
    guard let c = conn else { return }
    let meta = NWProtocolWebSocket.Metadata(opcode: .ping)
    let ctx = NWConnection.ContentContext(identifier: "ping", metadata: [meta])
    c.send(content: Data(), contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in })
  }

  /// Text control frame — KiwiSDR's `SET …` command plane (UberSDR uses JSON below).
  func send(text: String) {
    // ★ Whichever transport is LIVE. On the phone, three places asked a compile-time flag instead
    //   and a fallen-back socket would have sent no tune at all — "audio works, tuning does
    //   nothing". The same trap is one line away here.
    if usingURLSession { task?.send(.string(text)) { _ in }; return }
    guard let c = conn, let d = text.data(using: .utf8) else { return }
    let meta = NWProtocolWebSocket.Metadata(opcode: .text)
    let ctx = NWConnection.ContentContext(identifier: "text", metadata: [meta])
    c.send(content: d, contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in })
  }

  /// JSON control (the tune). Text frame, same as the phone.
  func send(json: [String: Any]) {
    if usingURLSession {
      if let d = try? JSONSerialization.data(withJSONObject: json),
         let str = String(data: d, encoding: .utf8) { task?.send(.string(str)) { _ in } }
      return
    }
    guard let c = conn,
          let d = try? JSONSerialization.data(withJSONObject: json) else { return }
    let meta = NWProtocolWebSocket.Metadata(opcode: .text)
    let ctx = NWConnection.ContentContext(identifier: "text", metadata: [meta])
    c.send(content: d, contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in })
  }

  private static func pathName(_ p: NWPath?) -> String {
    guard let p else { return "no path" }
    if p.usesInterfaceType(.wifi)          { return "wifi" }
    if p.usesInterfaceType(.cellular)      { return "cell" }
    if p.usesInterfaceType(.wiredEthernet) { return "eth" }
    // `.other` is what the iPhone Bluetooth relay reports as. If one socket says "wifi" and
    // the other says "other", that IS the bug.
    if p.usesInterfaceType(.other)         { return "OTHER(relay?)" }
    if p.usesInterfaceType(.loopback)      { return "loopback" }
    return "unknown"
  }

  func cancel() {
    connectDeadline?.cancel(); connectDeadline = nil
    isWaiting = false
    conn?.cancel()
    conn = nil
    task?.cancel(with: .goingAway, reason: nil)
    task = nil
    urlSession = nil
    usingURLSession = false
  }
}
