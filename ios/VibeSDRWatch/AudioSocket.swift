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
        self.onState?("\(name) ws ready [\(Self.pathName(c.currentPath))]")
        self.receive(c, g)
      case .waiting(let e):
        // Path not satisfiable YET. NWConnection retries toward .ready on its own — do not
        // tear it down here or you fight the framework and lose.
        self.onState?("\(name) ws waiting: \(e)")
      case .failed(let e):
        self.onState?("\(name) ws failed: \(e)")
      case .cancelled:
        self.onState?("\(name) ws cancelled")
      default:
        break
      }
    }
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
    conn?.cancel()
    conn = nil
  }
}
