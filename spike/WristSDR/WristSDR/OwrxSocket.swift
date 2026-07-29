import Foundation

/// A URLSessionWebSocketTask-based socket for OWRX ONLY.
///
/// OWRX pushes the WHOLE profile's FFT (e.g. a 10 MHz-wide spectrum), which overwhelmed the
/// NWConnection WebSocket (AudioSocket) — the stream silently stalled after seconds while a desktop
/// browser handled it fine. URLSessionWebSocketTask has larger internal buffering and is the same
/// transport browser-like clients use. The old "two concurrent URLSession sockets fail on watchOS"
/// caveat does NOT apply here: OWRX is a SINGLE socket. Interface mirrors AudioSocket so OwrxClient
/// swaps in unchanged.
final class OwrxSocket: NSObject {
  private lazy var session: URLSession = {
    let c = URLSessionConfiguration.default
    c.waitsForConnectivity = true
    return URLSession(configuration: c, delegate: self, delegateQueue: nil)
  }()
  private var task: URLSessionWebSocketTask?
  private var gen = 0

  var onData:  ((Data) -> Void)?
  var onText:  ((String) -> Void)?
  var onState: ((String) -> Void)?

  /// Signature matches AudioSocket.open; forceIPv4/autoReplyPing are no-ops here (URLSession does
  /// happy-eyeballs and answers pings itself).
  func open(url: URL, headers: [(name: String, value: String)] = [], forceIPv4: Bool = false, autoReplyPing: Bool = true) {
    gen &+= 1; let g = gen
    cancel()
    var req = URLRequest(url: url)
    for h in headers { req.setValue(h.value, forHTTPHeaderField: h.name) }
    let t = session.webSocketTask(with: req)
    t.maximumMessageSize = 8 * 1024 * 1024   // OWRX FFT frames can be large; default 1 MB is tight
    task = t
    t.resume()
    receive(t, g)
  }

  private func receive(_ t: URLSessionWebSocketTask, _ g: Int) {
    t.receive { [weak self] result in
      guard let self, self.gen == g else { return }
      switch result {
      case .failure(let e):
        self.onState?("owrx ws recv: \(e)")
      case .success(let msg):
        switch msg {
        case .string(let s): self.onText?(s)
        case .data(let d):   self.onData?(d)
        @unknown default:    break
        }
        self.receive(t, g)   // keep reading
      }
    }
  }

  func send(text: String) {
    task?.send(.string(text)) { [weak self] err in if let err { self?.onState?("owrx ws send: \(err)") } }
  }
  func send(json: [String: Any]) {
    guard let d = try? JSONSerialization.data(withJSONObject: json), let s = String(data: d, encoding: .utf8) else { return }
    send(text: s)
  }
  func sendPing() { task?.sendPing { _ in } }

  func cancel() {
    task?.cancel(with: .goingAway, reason: nil)
    task = nil
  }
}

extension OwrxSocket: URLSessionWebSocketDelegate {
  func urlSession(_ s: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol proto: String?) {
    onState?("owrx ws ready")
  }
  func urlSession(_ s: URLSession, webSocketTask: URLSessionWebSocketTask, didCloseWith code: URLSessionWebSocketTask.CloseCode, reason: Data?) {
    onState?("owrx ws failed: closed \(code.rawValue)")
  }
  func urlSession(_ s: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
    if let error { onState?("owrx ws failed: \(error)") }
  }
}
