// main.swift — VibeSDR Jr's certificate pins (GitHub #52), checked on the Mac against REAL self-signed servers.
// Compiled by tools/test-jr-tls-pin.sh together with the app's own WristSDR/TlsPin.swift and TlsTrust.swift —
// Security, Network and URLSession are the same code on watchOS and macOS.
//
// What it proves, with each of Jr's transports wired exactly as the app wires them:
//   • TlsPin: key, fingerprint lines, the DER reader on the real certificates, decide() for every case
//   • no pin → refused (URLSession AND NWConnection WebSocket), and the server never saw the request/upgrade
//   • the failure each transport reports is recognised as a certificate failure (what sends Jr to the probe)
//   • the probe classifies untrusted / expired / unreachable, with openssl's fingerprint
//   • pinned → GET, URLSessionWebSocketTask and NWConnection WebSocket all work, for THAT host:port only
//   • a different certificate → refused ("changed"); a pinned certificate past its date → refused
//   • the pins survive a reload; forget → refused again
import Foundation
import Network

var bad = 0
func check(_ ok: Bool, _ what: String) { print(ok ? "  ok   \(what)" : "  FAIL \(what)"); if !ok { bad += 1 } }

let a = CommandLine.arguments
guard a.count >= 11 else {
  print("usage: test <pSelf> <fpSelf> <pLh> <fpLh> <pEx> <fpEx> <log> <derSelf> <derLh> <derEx>"); exit(2)
}
let pSelf = Int(a[1])!, pLh = Int(a[3])!, pEx = Int(a[5])!
let fpSelf = a[2], fpLh = a[4], fpEx = a[6], logPath = a[7]
let derSelf = try! Data(contentsOf: URL(fileURLWithPath: a[8]))
let derLh = try! Data(contentsOf: URL(fileURLWithPath: a[9]))
let derEx = try! Data(contentsOf: URL(fileURLWithPath: a[10]))
func serverLog() -> String { (try? String(contentsOfFile: logPath, encoding: .utf8)) ?? "" }

/// Run an async closure to completion from top-level code.
func wait<T>(_ f: @escaping @Sendable () async -> T) -> T {
  let sem = DispatchSemaphore(value: 0)
  nonisolated(unsafe) var out: T?
  Task { out = await f(); sem.signal() }
  sem.wait()
  return out!
}

// ── TlsPin (pure) ──────────────────────────────────────────────────────────────────────────────
print("keys and fingerprints")
check(TlsPin.key(host: "SDR.Example.org.", port: 8073) == "sdr.example.org:8073", "host lower-cased, trailing dot dropped")
check(TlsPin.key(host: "[fe80::1]", port: 443) == "fe80::1:443", "IPv6 brackets dropped")
check(TlsPin.normalise("AB:" + String(repeating: "0", count: 62)) == "ab" + String(repeating: "0", count: 62), "normalise strips colons, lower-cases")
check(TlsPin.normalise("abc") == "", "a short fingerprint is nothing")
let ls = TlsPin.lines(fpSelf)
check(ls.count == 4 && ls.allSatisfy { $0.count == 23 }, "fingerprint = four 23-character lines for the wrist")
check(ls.joined(separator: ":").replacingOccurrences(of: ":", with: "").lowercased() == fpSelf, "lines join back to the fingerprint")
check(TlsPin.target("https://Host.example:8073/ws/")! == ("Host.example", 8073), "target: https host:port")
check(TlsPin.target("wss://h.example/x")! == ("h.example", 443), "target: wss default 443")
check(TlsPin.target("wss://[fe80::1]:9000/")! == ("fe80::1", 9000), "target: IPv6")
check(TlsPin.target("http://h.example:8073") == nil && TlsPin.target("ws://h:1") == nil, "target: plain http/ws is not TLS")
check(TlsPin.target("https://h:99999") == nil, "target: bad port")

print("the DER reader, on the real certificates")
if let c = TlsPin.parse(der: derSelf) {
  check(c.subjectCN == "openwebrx" && c.issuerCN == "openwebrx" && c.selfSigned, "OWRX+-style: CN openwebrx, self-signed")
  check(c.names.isEmpty, "no SAN")
  check(!TlsPin.namesHost(c, host: "sdr.example.org"), "does not name the address (the prompt says so)")
  check((c.notAfter ?? .distantPast) > Date().addingTimeInterval(3000 * 86400), "expires ~10 years out")
} else { check(false, "parse the OWRX+-style certificate") }
if let c = TlsPin.parse(der: derLh) {
  check(c.names == ["localhost", "127.0.0.1"], "SANs: DNS + IP in order")
  check(TlsPin.namesHost(c, host: "LOCALHOST") && TlsPin.namesHost(c, host: "127.0.0.1"), "names localhost and 127.0.0.1")
} else { check(false, "parse the localhost certificate") }
if let c = TlsPin.parse(der: derEx) {
  var cal = Calendar(identifier: .gregorian); cal.timeZone = TimeZone(identifier: "UTC")!
  check(c.notAfter == cal.date(from: DateComponents(year: 2021, month: 1, day: 1)), "expired: notAfter 2021-01-01")
} else { check(false, "parse the expired certificate") }
check(TlsPin.parse(der: Data([0x30, 0x84, 0xff, 0xff, 0xff, 0xff])) == nil, "a hostile length is refused, not trapped")
check(TlsPin.parse(der: Data()) == nil, "empty → nil")
var cw = TlsPin.Cert(subjectCN: "", issuerCN: "", names: ["*.example.org"], notBefore: nil, notAfter: nil, selfSigned: false)
check(TlsPin.namesHost(cw, host: "a.example.org") && !TlsPin.namesHost(cw, host: "a.b.example.org"), "wildcard covers one label only")
cw.names = []; cw.subjectCN = "sdr.example.org"
check(TlsPin.namesHost(cw, host: "sdr.example.org."), "CN used when there is no SAN")

print("decide()")
let fpA = String(repeating: "a", count: 64), fpB = String(repeating: "b", count: 64)
func ins(_ reason: String, sys: Bool = false, fp: String = fpA, pin: String? = nil) -> TlsPin.Inspection {
  .init(systemTrusted: sys, reason: reason, sha256: fp, pinned: pin)
}
check(TlsPin.decide(nil) == .none, "nothing → none")
check(TlsPin.decide(ins("ok", sys: true)) == .none, "system-trusted → none")
check(TlsPin.decide(ins("ok", sys: true, pin: fpB)) == .none, "system-trusted with a stale pin → none (the pin is never needed)")
check(TlsPin.decide(ins("untrusted")) == .offer(sha256: fpA), "untrusted, no pin → offer")
check(TlsPin.decide(ins("untrusted", pin: fpA)) == .none, "untrusted, the pin → none")
check(TlsPin.decide(ins("untrusted", pin: fpB)) == .changed(sha256: fpA, previous: fpB), "untrusted, another pin → changed")
check(TlsPin.decide(ins("untrusted"), pinned: fpB) == .changed(sha256: fpA, previous: fpB), "explicit pin argument wins")
check(TlsPin.decide(ins("expired")) == .refuse(why: .expired, sha256: fpA), "expired → refuse")
check(TlsPin.decide(ins("expired", pin: fpA)) == .refuse(why: .expired, sha256: fpA), "expired even when pinned → refuse")
check(TlsPin.decide(ins("hostname")) == .refuse(why: .hostname, sha256: fpA), "public cert for another name → refuse")
check(TlsPin.decide(ins("unreachable", fp: "")) == .none, "unreachable → none")
check(TlsPin.decide(ins("untrusted", fp: "xyz")) == .none, "no usable fingerprint → none")

// ── TlsTrust against the servers ───────────────────────────────────────────────────────────────
let suite = "net.vibesdr.jr-tls-pin-test"
UserDefaults().removePersistentDomain(forName: suite)
let core = TlsTrust(defaults: UserDefaults(suiteName: suite)!)

final class Box: @unchecked Sendable { var v: String?; var err: Error? }

/// GET https://localhost:<port>/ through a session wired like TlsTrust.session (the pin delegate).
func fetch(_ core: TlsTrust, _ port: Int) -> (body: String?, error: Error?) {
  let cfg = URLSessionConfiguration.ephemeral
  cfg.timeoutIntervalForRequest = 5
  let s = URLSession(configuration: cfg, delegate: TlsSessionDelegate(core: core), delegateQueue: nil)
  let b = Box(); let sem = DispatchSemaphore(value: 0)
  s.dataTask(with: URL(string: "https://localhost:\(port)/")!) { d, _, e in
    if let d, e == nil { b.v = String(data: d, encoding: .utf8) }
    b.err = e; sem.signal()
  }.resume()
  _ = sem.wait(timeout: .now() + 8)
  s.invalidateAndCancel()
  return (b.v, b.err)
}

/// URLSessionWebSocketTask with the pin delegate (AudioSocket's LAN transport, OwrxSocket). First message or nil.
func urlWs(_ core: TlsTrust, _ port: Int) -> (msg: String?, error: Error?) {
  let s = URLSession(configuration: .ephemeral, delegate: TlsSessionDelegate(core: core), delegateQueue: nil)
  let t = s.webSocketTask(with: URL(string: "wss://localhost:\(port)/ws/")!)
  let b = Box(); let sem = DispatchSemaphore(value: 0)
  t.resume()
  t.receive { r in
    switch r { case .success(.string(let m)): b.v = m; case .failure(let e): b.err = e; default: break }
    sem.signal()
  }
  _ = sem.wait(timeout: .now() + 8)
  t.cancel(); s.invalidateAndCancel()
  return (b.v, b.err)
}

/// NWConnection WebSocket with TlsTrust.parameters(for:) — exactly how AudioSocket builds every socket.
func nwWs(_ core: TlsTrust, _ port: Int) -> (msg: String?, error: NWError?) {
  let url = URL(string: "wss://localhost:\(port)/ws/")!
  let params = core.parameters(for: url)
  let ws = NWProtocolWebSocket.Options()
  params.defaultProtocolStack.applicationProtocols.insert(ws, at: 0)
  let c = NWConnection(to: .url(url), using: params)
  final class NB: @unchecked Sendable { var v: String?; var err: NWError?; var done = false }
  let b = NB(); let sem = DispatchSemaphore(value: 0)
  let q = DispatchQueue(label: "nw")
  c.stateUpdateHandler = { st in
    switch st {
    case .ready:
      c.receiveMessage { d, _, _, e in
        if let d { b.v = String(data: d, encoding: .utf8) }
        if !b.done { b.done = true; sem.signal() }
      }
    case .failed(let e), .waiting(let e):
      b.err = e
      if !b.done { b.done = true; sem.signal() }
    default: break
    }
  }
  c.start(queue: q)
  _ = sem.wait(timeout: .now() + 8)
  c.cancel()
  return (b.v, b.err)
}

print("self-signed CN=openwebrx, no pin")
var r = wait { await core.probe(host: "localhost", port: pSelf, timeout: 5) }
check(r.reason == "untrusted" && !r.systemTrusted, "probe says untrusted")
check(r.sha256 == fpSelf, "probe fingerprint = openssl's")
check(r.pinned == nil, "probe: no pin")
check(r.der == derSelf, "probe carries the leaf's DER for the prompt")
check(TlsPin.decide(r) == .offer(sha256: fpSelf), "→ offer")
let f0 = fetch(core, pSelf)
check(f0.body == nil, "GET refused without a pin")
check(f0.error.map(TlsTrust.isTrustFailure) == true, "…and recognised as a certificate failure (\((f0.error as NSError?)?.code ?? 0))")
let u0 = urlWs(core, pSelf)
check(u0.msg == nil, "URLSession wss refused without a pin")
check(u0.error.map(TlsTrust.isTrustFailure) == true, "…recognised (\((u0.error as NSError?)?.code ?? 0))")
let n0 = nwWs(core, pSelf)
check(n0.msg == nil, "NWConnection wss refused without a pin")
check(n0.error.map { TlsTrust.isTrustFailure($0) } == true, "…recognised (\(n0.error.map { "\($0)" } ?? "no error"))")
check(!serverLog().contains("REQ \(pSelf)") && !serverLog().contains("UPGRADE \(pSelf)"),
      "the server never saw a request or an upgrade (probe and refusals)")

print("pin it")
core.pin(host: "localhost", port: pSelf, sha256: fpSelf, subject: "openwebrx")
check(fetch(core, pSelf).body?.contains("openwebrx") == true, "GET accepted with the pin")
check(urlWs(core, pSelf).msg?.hasPrefix("CLIENT DE SERVER") == true, "URLSession wss accepted, server hello received")
check(nwWs(core, pSelf).msg?.hasPrefix("CLIENT DE SERVER") == true, "NWConnection wss accepted (verify block), server hello received")
r = wait { await core.probe(host: "localhost", port: pSelf, timeout: 5) }
check(r.pinned == fpSelf && TlsPin.decide(r) == .none, "probe: pin matches → nothing to ask")

print("the same pin does not cover another port")
check(fetch(core, pLh).body == nil, "other port refused (a pin is host:port, not host)")
check(core.parameters(for: URL(string: "wss://localhost:\(pLh)/")!).defaultProtocolStack.applicationProtocols.contains { $0 is NWProtocolTLS.Options },
      "an unpinned wss still gets TLS")

print("certificate changed")
core.pin(host: "localhost", port: pLh, sha256: fpSelf, subject: "openwebrx")     // the OLD cert, for that port
check(fetch(core, pLh).body == nil, "GET: a different certificate is refused")
check(nwWs(core, pLh).msg == nil, "NWConnection: a different certificate is refused")
check(urlWs(core, pLh).msg == nil, "URLSession wss: a different certificate is refused")
r = wait { await core.probe(host: "localhost", port: pLh, timeout: 5) }
check(TlsPin.decide(r) == .changed(sha256: fpLh, previous: fpSelf), "→ changed, both fingerprints")

print("expired, even when pinned")
core.pin(host: "localhost", port: pEx, sha256: fpEx, subject: "localhost")
check(fetch(core, pEx).body == nil, "GET: an expired pinned certificate is refused")
check(nwWs(core, pEx).msg == nil, "NWConnection: an expired pinned certificate is refused")
r = wait { await core.probe(host: "localhost", port: pEx, timeout: 5) }
check(r.reason == "expired" && TlsPin.decide(r) == .refuse(why: .expired, sha256: fpEx), "probe says expired → refuse (OK only)")

print("restart")
let again = TlsTrust(defaults: UserDefaults(suiteName: suite)!)
check(again.pin(host: "LOCALHOST", port: pSelf) == fpSelf, "pin survives a reload")
check(fetch(again, pSelf).body != nil, "and still connects")

print("forget")
check(core.forget(host: "localhost", port: pSelf), "forget returns true")
check(!core.forget(host: "localhost", port: pSelf), "a second forget returns false")
check(fetch(core, pSelf).body == nil, "refused again after forget")
check(nwWs(core, pSelf).msg == nil, "NWConnection refused again after forget")

print("plain ws stays plain")
check(core.parameters(for: URL(string: "ws://localhost:1/")!).defaultProtocolStack.applicationProtocols.allSatisfy { !($0 is NWProtocolTLS.Options) }, "ws → .tcp")

print("nothing listening")
r = wait { await core.probe(host: "localhost", port: 1, timeout: 5) }
check(r.reason == "unreachable" && TlsPin.decide(r) == .none, "probe: unreachable → nothing to ask")

// ★ Optional, needs the internet: a publicly trusted server is the system's; a wrong name is "hostname".
r = wait { await core.probe(host: "vibesdr.net", port: 443, timeout: 6) }
if r.reason == "unreachable" { print("  (offline — public-CA cases not run)") }
else {
  print("public CA (network)")
  check(r.systemTrusted && r.reason == "ok" && TlsPin.decide(r) == .none, "vibesdr.net: system-trusted → nothing to ask")
  r = wait { await core.probe(host: "wrong.host.badssl.com", port: 443, timeout: 6) }
  if r.reason != "unreachable" {
    check(r.reason == "hostname" && TlsPin.decide(r) == .refuse(why: .hostname, sha256: r.sha256),
          "wrong.host.badssl.com: hostname mismatch → refuse, never offered")
  }
}

UserDefaults().removePersistentDomain(forName: suite)
print(bad > 0 ? "\n\(bad) FAILED" : "\nall passed")
exit(bad > 0 ? 1 : 0)
