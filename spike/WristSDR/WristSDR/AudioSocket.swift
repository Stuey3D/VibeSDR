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
  ///  ★ 8s: long enough for a slow DDNS + TLS handshake over the watch's relay, short enough
  ///    that a person has not yet decided the app is broken.
  private var connectDeadline: DispatchWorkItem?
  private static let connectTimeout: TimeInterval = 8
  /// ★★ ONE SILENT ESCALATION BEFORE GIVING UP. The OWRX client reaches a LAN address happily and
  ///    the UberSDR one hangs, and the ONLY difference between the two open() calls is
  ///    `forceIPv4` — OWRX pins v4 because a dynamic-DNS host can hand back an AAAA the watch
  ///    cannot route. So when a connection never arrives, try that once before reporting: it costs
  ///    one timeout, and it is the difference that is already known to work on this device.
  /// ★ Remembered so the retry does not loop: v4-forced is the LAST attempt, never the first.
  private var retriedV4 = false
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
      case .failed(let e):
        self.connectDeadline?.cancel(); self.connectDeadline = nil
        self.onState?("\(name) ws failed: \(e)")
      case .cancelled:
        self.onState?("\(name) ws cancelled")
      default:
        break
      }
    }
    // ★★★ THE DEADLINE. Fires only if the socket has not reached `.ready`, and reports itself as
    //     a FAILURE — which is the word every caller's retry already looks for. A hang becomes a
    //     retry, and an invisible fault becomes a line on the screen naming the address.
    let dl = DispatchWorkItem { [weak self] in
      guard let self, self.gen == g, self.conn === c else { return }
      self.connectDeadline = nil
      c.cancel()
      if !self.retriedV4, let again = self.lastOpen {
        self.retriedV4 = true
        self.onState?("\(self.name) ws waiting: no connection after \(Int(Self.connectTimeout))s — retrying pinned to IPv4")
        self.open(url: again.url, headers: again.headers, forceIPv4: true,
                  autoReplyPing: again.autoReplyPing, avoidRelay: again.avoidRelay)
        return
      }
      // ★ "failed" is the word every caller's retry looks for — and the address is named, so the
      //   person reading it knows WHICH server and port never answered.
      self.onState?("\(self.name) ws failed: no connection after \(Int(Self.connectTimeout))s — \(url.host ?? "?"):\(url.port.map(String.init) ?? "default")")
    }
    connectDeadline?.cancel()
    connectDeadline = dl
    queue.asyncAfter(deadline: .now() + Self.connectTimeout, execute: dl)
    c.start(queue: queue)
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
    guard let c = conn else { return }
    let meta = NWProtocolWebSocket.Metadata(opcode: .ping)
    let ctx = NWConnection.ContentContext(identifier: "ping", metadata: [meta])
    c.send(content: Data(), contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in })
  }

  /// Text control frame — KiwiSDR's `SET …` command plane (UberSDR uses JSON below).
  func send(text: String) {
    guard let c = conn, let d = text.data(using: .utf8) else { return }
    let meta = NWProtocolWebSocket.Metadata(opcode: .text)
    let ctx = NWConnection.ContentContext(identifier: "text", metadata: [meta])
    c.send(content: d, contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in })
  }

  /// JSON control (the tune). Text frame, same as the phone.
  func send(json: [String: Any]) {
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
    conn?.cancel()
    conn = nil
  }
}
