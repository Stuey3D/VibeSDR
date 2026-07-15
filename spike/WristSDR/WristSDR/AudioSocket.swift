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

  func open(url: URL) {
    gen &+= 1
    let g = gen
    cancel()

    let secure = (url.scheme == "wss")
    let params: NWParameters = secure ? .tls : .tcp
    let ws = NWProtocolWebSocket.Options()
    ws.autoReplyPing = true                       // answer the server's pings natively
    params.defaultProtocolStack.applicationProtocols.insert(ws, at: 0)

    let c = NWConnection(to: .url(url), using: params)
    conn = c
    c.stateUpdateHandler = { [weak self] state in
      guard let self, self.gen == g else { return }
      switch state {
      case .ready:
        self.onState?("\(name) ws ready")
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
      if let data, !data.isEmpty {
        if op == .text {
          if let t = String(data: data, encoding: .utf8) { self.onText?(t) }
        } else if op == .binary {
          self.onData?(data)
        }
      }
      self.receive(c, g)
    }
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
