// socket/main.swift — Jr's REAL AudioSocket.swift (every NWConnection socket Jr opens: OWRX, Kiwi, FM-DX,
// UberSDR/VibeServer audio + spectrum) against the self-signed test server, with a scripted stand-in for the
// prompt. Proves the wiring around the decision, not just the decision:
//   • a refused certificate is NOT reported to the caller as a failure while the question is open — the
//     caller sees "checking the server's certificate", then
//   • "Trust" → the socket pins nothing itself, reopens exactly as it was opened, and receives the server's hello;
//   • "Cancel" → ONE "… ws failed: certificate not trusted — host:port", and the socket does not ask again;
//   • a plain failure (nothing listening) never reaches the question at all.
// Compiled by tools/test-jr-tls-pin.sh with AudioSocket.swift + TlsPin.swift + TlsTrust.swift and THIS
// file's TlsTrustCenter stand-in (the real one is SwiftUI + Jr's diagnostics, which a Mac CLI cannot link).
import Foundation

/// The stand-in: same signature as Jr's TlsTrustCenter.checkAfterFailure. Probes for real, decides with the
/// real TlsPin.decide, and answers as scripted — pinning through the real TlsTrust.shared on "trust".
@MainActor final class TlsTrustCenter {
  enum Outcome: Sendable { case trusted, declined, refused, none }
  static let shared = TlsTrustCenter()
  var script: Outcome = .declined
  var asked = 0
  func checkAfterFailure(host: String, port: Int) async -> Outcome {
    asked += 1
    let r = await TlsTrust.shared.probe(host: host, port: port, timeout: 5)
    guard case .offer(let sha) = TlsPin.decide(r) else { return .none }
    if script == .trusted { TlsTrust.shared.pin(host: host, port: port, sha256: sha, subject: "") }
    return script
  }
}

var bad = 0
func check(_ ok: Bool, _ what: String) { print(ok ? "  ok   \(what)" : "  FAIL \(what)"); if !ok { bad += 1 } }
let port = Int(CommandLine.arguments[1])!
_ = TlsTrust.shared.forget(host: "localhost", port: port)

final class Log: @unchecked Sendable {
  private let l = NSLock(); private var s: [String] = []
  func add(_ x: String) { l.lock(); s.append(x); l.unlock() }
  var all: [String] { l.lock(); defer { l.unlock() }; return s }
}

/// Open one AudioSocket to wss://localhost:<port>/ws/ and wait until `until` holds (or 12 s).
func run(_ until: @escaping ([String]) -> Bool) -> (states: [String], text: String?) {
  let sock = AudioSocket(name: "test")
  let log = Log(); let got = Log()
  sock.onState = { log.add($0) }
  sock.onText = { got.add($0) }
  sock.open(url: URL(string: "wss://localhost:\(port)/ws/")!)
  let end = Date().addingTimeInterval(12)
  while Date() < end, !until(log.all + got.all.map { "TEXT " + $0 }) {
    RunLoop.main.run(until: Date().addingTimeInterval(0.05))
  }
  RunLoop.main.run(until: Date().addingTimeInterval(1.0))   // anything that would follow (it must not)
  sock.cancel()
  return (log.all, got.all.first)
}

print("Cancel")
MainActor.assumeIsolated { TlsTrustCenter.shared.script = .declined }
var r = run { $0.contains { $0.contains("certificate not trusted") } }
check(r.states.contains { $0.contains("checking the server's certificate") }, "caller told it is checking the certificate")
check(!r.states.contains { $0.contains("-9808") || $0.contains("cancelled") }, "the raw TLS failure and the retire never reach the caller")
check(r.states.filter { $0.contains("failed") } == ["test ws failed: certificate not trusted — localhost:\(port)"],
      "exactly one failure, saying the certificate is not trusted")
check(MainActor.assumeIsolated { TlsTrustCenter.shared.asked } == 1, "asked once")
check(TlsTrust.shared.pin(host: "localhost", port: port) == nil, "nothing pinned")

print("Trust")
MainActor.assumeIsolated { TlsTrustCenter.shared.script = .trusted; TlsTrustCenter.shared.asked = 0 }
r = run { $0.contains { $0.hasPrefix("TEXT ") } }
check(r.text?.hasPrefix("CLIENT DE SERVER") == true, "reopened with the pin and received the server's hello")
check(r.states.contains { $0.contains("ws ready") }, "ready")
check(!r.states.contains { $0.contains("failed") }, "no failure reported")
check(MainActor.assumeIsolated { TlsTrustCenter.shared.asked } == 1, "asked once")

print("Pinned — no question")
MainActor.assumeIsolated { TlsTrustCenter.shared.asked = 0 }
r = run { $0.contains { $0.hasPrefix("TEXT ") } }
check(r.text?.hasPrefix("CLIENT DE SERVER") == true && MainActor.assumeIsolated { TlsTrustCenter.shared.asked } == 0,
      "a trusted server connects straight away, nothing asked")

print("Nothing listening — never the question")
_ = TlsTrust.shared.forget(host: "localhost", port: port)
MainActor.assumeIsolated { TlsTrustCenter.shared.asked = 0 }
let s2 = AudioSocket(name: "dead"); let l2 = Log()
s2.onState = { l2.add($0) }
s2.open(url: URL(string: "wss://localhost:1/ws/")!)
let end = Date().addingTimeInterval(4)
while Date() < end, !l2.all.contains(where: { $0.contains("failed") || $0.contains("waiting") }) {
  RunLoop.main.run(until: Date().addingTimeInterval(0.05))
}
s2.cancel()
check(MainActor.assumeIsolated { TlsTrustCenter.shared.asked } == 0, "a refused port is not a certificate question")

_ = TlsTrust.shared.forget(host: "localhost", port: port)
print(bad > 0 ? "\n\(bad) FAILED" : "\nall passed")
exit(bad > 0 ? 1 : 0)
