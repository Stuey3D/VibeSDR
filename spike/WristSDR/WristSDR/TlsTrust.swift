import Foundation
import Security
import Network
import CryptoKit

/// ★★★ PER-SERVER CERTIFICATE PINS — ENFORCEMENT, ON THE WATCH (GitHub #52).
///
/// A Swift port of the phone's modules/vibe-tls-trust/VibeTlsCore.m, with the same rules:
///
/// ★★★ NEVER WEAKER THAN THE PLATFORM. Every chain the system trusts is accepted exactly as before, and that
///     decision is the system's own evaluation with the host name. Our pin is consulted ONLY after the
///     system has refused, ONLY for the host:port it was made for, and ONLY for the exact leaf the person was
///     shown — and only while that leaf is in date. Nothing here trusts all, skips the host-name check for
///     anyone else, or touches the system trust store.
///
/// Jr's three ways to reach a server, all covered here:
///  - NWConnection (AudioSocket: OWRX, Kiwi SND + W/F, FM-DX text/chat/audio, UberSDR/VibeServer audio +
///    spectrum) → `parameters(for:)`: the stock `.tls` unless a pin exists, then a verify block that asks
///    the system first.
///  - URLSessionWebSocketTask (AudioSocket's LAN fallback, OwrxSocket) and every HTTP request to a server
///    (UberSDR /connection, VibeServer /vibeserver/auth, the front door, detection, FM-DX /static_data)
///    → `TlsSessionDelegate` / `TlsTrust.session`.
///  - the PROBE (`probe`) — a handshake cancelled at the certificate, for the prompt only.
/// ★ PURE Foundation/Security/Network: tools/test-jr-tls-pin.sh compiles this file on the Mac and runs it
///   against real self-signed servers (scripts/tls/tls-test-server.mjs).
final class TlsTrust: @unchecked Sendable {

  static let shared = TlsTrust(defaults: .standard)

  enum Verdict { case system, pinned, rejected }

  /// The watch's own pins — Jr's UserDefaults, the phone's key name. Jr is a separate app with its own
  /// container, so the two never share (or clobber) a store; each person trusts on each device.
  static let pinsKey = "VibeTlsPins"

  private let defaults: UserDefaults
  private let lock = NSLock()
  private var pins: [String: [String: Any]] = [:]

  /// Tests pass their own suite so the app's pins are never touched.
  init(defaults: UserDefaults) {
    self.defaults = defaults
    // ★ Only well-formed entries survive a load — a pin is 64 hex characters or it is nothing.
    for (k, v) in defaults.dictionary(forKey: Self.pinsKey) ?? [:] {
      if let e = v as? [String: Any], let fp = e["sha256"] as? String, TlsPin.normalise(fp) == fp { pins[k] = e }
    }
  }

  // ── pins ──────────────────────────────────────────────────────────────────────────────────────

  func pin(host: String, port: Int) -> String? {
    let k = TlsPin.key(host: host, port: port)
    lock.lock(); defer { lock.unlock() }
    return pins[k]?["sha256"] as? String
  }

  func allPins() -> [String: [String: Any]] {
    lock.lock(); defer { lock.unlock() }
    return pins
  }

  func pin(host: String, port: Int, sha256: String, subject: String) {
    let fp = TlsPin.normalise(sha256)
    guard !fp.isEmpty else { return }
    let k = TlsPin.key(host: host, port: port)
    lock.lock(); defer { lock.unlock() }
    // ★ "at" as an epoch-ms Double, NOT Int: an Int of epoch-ms traps on arm64_32 (arm64_32_int_trap).
    pins[k] = ["sha256": fp, "subject": subject, "at": (Date().timeIntervalSince1970 * 1000).rounded()]
    defaults.set(pins, forKey: Self.pinsKey)
  }

  @discardableResult
  func forget(host: String, port: Int) -> Bool {
    let k = TlsPin.key(host: host, port: port)
    lock.lock(); defer { lock.unlock() }
    guard pins.removeValue(forKey: k) != nil else { return false }
    defaults.set(pins, forKey: Self.pinsKey)
    return true
  }

  // ── fingerprints and checks ───────────────────────────────────────────────────────────────────

  /// SHA-256 of the certificate's DER, lower-case hex, no separators.
  static func sha256(_ cert: SecCertificate) -> String {
    let der = SecCertificateCopyData(cert) as Data
    return SHA256.hash(data: der).map { String(format: "%02x", $0) }.joined()
  }

  /// The chain as the server sent it (leaf first).
  private static func chain(_ trust: SecTrust) -> [SecCertificate] {
    (SecTrustCopyCertificateChain(trust) as? [SecCertificate]) ?? []
  }

  /// ★ THE PLATFORM'S DECISION — the system roots, the dates and the host name, as every connection already
  ///   got. Evaluated on the trust the connection handed us, so stapled OCSP and TLS-delivered SCTs count.
  private static func systemTrusts(_ trust: SecTrust, host: String) -> Bool {
    guard !host.isEmpty else { return false }
    SecTrustSetPolicies(trust, SecPolicyCreateSSL(true, host as CFString))
    return SecTrustEvaluateWithError(trust, nil)
  }

  /// The chain is publicly trusted, but not for this name. Asked only to EXPLAIN a refusal, never to accept one.
  private static func trustedIgnoringName(_ trust: SecTrust) -> Bool {
    SecTrustSetPolicies(trust, SecPolicyCreateSSL(true, nil))
    return SecTrustEvaluateWithError(trust, nil)
  }

  /// ★★ IS THE LEAF ITSELF IN DATE? A pinned certificate is still refused once it has expired (or before it
  ///    starts). Evaluated with the leaf as its own anchor and the BASIC X.509 policy — VibeTlsCore measured
  ///    it on 2026-10-20: the SSL policy refuses a typical self-signed server certificate for reasons that
  ///    have nothing to do with trust (10-year validity, CA:TRUE from `openssl req -x509`), while the basic
  ///    policy accepts it and still reports errSecCertificateExpired for one out of date.
  private static func leafInDate(_ leaf: SecCertificate) -> Bool {
    var t: SecTrust?
    guard SecTrustCreateWithCertificates(leaf, SecPolicyCreateBasicX509(), &t) == errSecSuccess, let t else { return false }
    SecTrustSetAnchorCertificates(t, [leaf] as CFArray)
    SecTrustSetAnchorCertificatesOnly(t, true)
    return SecTrustEvaluateWithError(t, nil)
  }

  // ── decisions ─────────────────────────────────────────────────────────────────────────────────

  /// ★★★ ENFORCEMENT. The platform's own evaluation first, with the host name — accepted exactly as before.
  ///     Only a chain the platform REFUSES is compared with the pin for this host:port, and only an exact
  ///     leaf match whose dates are valid is let through. No pin, or a different certificate → refused.
  func evaluate(_ trust: SecTrust, host: String, port: Int) -> Verdict {
    if Self.systemTrusts(trust, host: host) { return .system }
    guard let pin = pin(host: host, port: port) else { return .rejected }   // ★ no pin: the refusal stands
    guard let leaf = Self.chain(trust).first else { return .rejected }
    guard Self.sha256(leaf) == pin else { return .rejected }                // changed
    guard Self.leafInDate(leaf) else { return .rejected }                   // the trusted one, out of date
    return .pinned
  }

  /// ★★ THE URLSession ANSWER. A server-trust challenge the platform passes → default handling, i.e.
  ///    untouched; a pinned match → use the trust; anything else → default handling, so the platform refuses
  ///    it exactly as it always did. Every other kind of challenge → default handling.
  func disposition(for challenge: URLAuthenticationChallenge) -> (URLSession.AuthChallengeDisposition, URLCredential?) {
    let ps = challenge.protectionSpace
    guard ps.authenticationMethod == NSURLAuthenticationMethodServerTrust, let trust = ps.serverTrust else {
      return (.performDefaultHandling, nil)
    }
    guard evaluate(trust, host: ps.host, port: ps.port) == .pinned else { return (.performDefaultHandling, nil) }
    return (.useCredential, URLCredential(trust: trust))
  }

  /// ★ INSPECTION, for the prompt only — never used to decide a live connection.
  func inspect(_ trust: SecTrust, host: String, port: Int) -> TlsPin.Inspection {
    guard let leaf = Self.chain(trust).first else {
      return .init(systemTrusted: false, reason: "error", sha256: "")
    }
    let fp = Self.sha256(leaf)
    let sys = Self.systemTrusts(trust, host: host)
    let reason = sys ? "ok"
      : !Self.leafInDate(leaf) ? "expired"
      : Self.trustedIgnoringName(trust) ? "hostname"
      : "untrusted"
    return .init(systemTrusted: sys, reason: reason, sha256: fp,
                 der: SecCertificateCopyData(leaf) as Data, pinned: pin(host: host, port: port))
  }

  /// ★ THE PROBE behind the prompt: a TLS handshake to https://host:port/ on a throwaway ephemeral session
  ///   that is CANCELLED at the certificate — no request is ever sent, whatever the certificate. Returns
  ///   inspect()'s answer, or reason "unreachable" when no certificate arrived.
  func probe(host: String, port: Int, timeout: TimeInterval = 8) async -> TlsPin.Inspection {
    let h = host.contains(":") && !host.hasPrefix("[") ? "[\(host)]" : host
    guard let url = URL(string: "https://\(h):\(port)/") else {
      return .init(systemTrusted: false, reason: "error", sha256: "", message: "bad address")
    }
    return await withCheckedContinuation { (cont: CheckedContinuation<TlsPin.Inspection, Never>) in
      let p = Probe(core: self, host: host, port: port) { cont.resume(returning: $0) }
      let cfg = URLSessionConfiguration.ephemeral
      cfg.timeoutIntervalForRequest = timeout
      cfg.timeoutIntervalForResource = timeout
      cfg.urlCache = nil
      cfg.httpCookieStorage = nil                              // ★ nothing of ours rides a probe
      let s = URLSession(configuration: cfg, delegate: p, delegateQueue: nil)
      s.dataTask(with: url).resume()
    }
  }

  /// One probe's delegate: captures the server trust, classifies it, cancels. Owned by its session until
  /// the session is invalidated.
  private final class Probe: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    let core: TlsTrust, host: String, port: Int
    private var done: ((TlsPin.Inspection) -> Void)?
    private let lock = NSLock()
    init(core: TlsTrust, host: String, port: Int, done: @escaping (TlsPin.Inspection) -> Void) {
      self.core = core; self.host = host; self.port = port; self.done = done
    }
    private func finish(_ r: TlsPin.Inspection) {
      lock.lock(); let d = done; done = nil; lock.unlock()
      d?(r)
    }
    func urlSession(_ session: URLSession, task: URLSessionTask, didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
      let ps = challenge.protectionSpace
      if ps.authenticationMethod == NSURLAuthenticationMethodServerTrust, let trust = ps.serverTrust {
        finish(core.inspect(trust, host: host, port: port))
      }
      // ★ Cancelled whatever it was: this session exists only to see the certificate, never to talk.
      completionHandler(.cancelAuthenticationChallenge, nil)
    }
    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
      finish(.init(systemTrusted: false, reason: "unreachable", sha256: "",
                   message: error?.localizedDescription ?? "no certificate received"))
      session.finishTasksAndInvalidate()
    }
  }

  // ── the hooks ─────────────────────────────────────────────────────────────────────────────────

  /// ★★ THE PARAMETERS FOR AN NWConnection TO `url` (VibePowerModule.socketParams, the phone's twin).
  ///    Plain ws → `.tcp`. wss to a host:port with NO pin → the stock `.tls`, byte-for-byte what every
  ///    socket here used before. Only a pinned host:port gets a verify block — and that block asks the
  ///    system first (with the host name), then accepts only the exact pinned, in-date certificate.
  /// ★ Always a FRESH instance: callers go on to add the WebSocket protocol and interface limits to it.
  func parameters(for url: URL) -> NWParameters {
    guard url.scheme?.lowercased() == "wss" else { return .tcp }
    guard let t = TlsPin.target(url), pin(host: t.host, port: t.port) != nil else { return .tls }
    let tls = NWProtocolTLS.Options()
    sec_protocol_options_set_verify_block(tls.securityProtocolOptions, { [self] _, secTrust, complete in
      let trust = sec_trust_copy_ref(secTrust).takeRetainedValue()
      complete(self.evaluate(trust, host: t.host, port: t.port) != .rejected)
    }, DispatchQueue.global(qos: .userInitiated))
    return NWParameters(tls: tls)
  }

  /// The URLSession every HTTP request to a SERVER goes through (in place of URLSession.shared): the shared
  /// session's configuration plus the pin delegate. Public directories may use it too — a certificate the
  /// system trusts is untouched either way.
  static let session = URLSession(configuration: .default, delegate: TlsSessionDelegate.shared, delegateQueue: nil)

  // ── was it the certificate? ───────────────────────────────────────────────────────────────────
  // ★ Only these failures send us to the probe. The probe is what decides — these just avoid spending a
  //   handshake on a refused port, a timeout or a plain-HTTP server answered with TLS (OWRX's "-9836 bad
  //   protocol version", which OwrxClient answers by falling back to ws).

  /// The local certificate-refusal statuses Secure Transport / boringssl report: errSSLXCertChainInvalid,
  /// errSSLBadCert, errSSLUnknownRootCert, errSSLNoRootCert, errSSLCertExpired, errSSLCertNotYetValid,
  /// errSSLHostNameMismatch.
  static let certStatuses: Set<OSStatus> = [-9807, -9808, -9812, -9813, -9814, -9815, -9843]

  static func isTrustFailure(_ e: NWError) -> Bool {
    if case .tls(let s) = e { return certStatuses.contains(s) }
    return false
  }

  /// URLSession: the serverCertificate* codes, or a secure-connection failure that carries the refused trust.
  static func isTrustFailure(_ error: Error) -> Bool {
    if let nw = error as? NWError { return isTrustFailure(nw) }
    let ns = error as NSError
    guard ns.domain == NSURLErrorDomain else { return false }
    if (-1204 ... -1201).contains(ns.code) { return true }
    return ns.code == NSURLErrorSecureConnectionFailed && ns.userInfo[NSURLErrorFailingURLPeerTrustErrorKey] != nil
  }
}

/// ★ A URLSession delegate that answers server-trust challenges with TlsTrust.disposition and nothing else.
///   URLSession keeps a strong reference to its delegate, so one shared instance serves every session.
final class TlsSessionDelegate: NSObject, URLSessionDelegate, @unchecked Sendable {
  static let shared = TlsSessionDelegate(core: .shared)
  let core: TlsTrust
  /// Tests hand in their own core (and so their own pins).
  init(core: TlsTrust) { self.core = core }
  func urlSession(_ session: URLSession, didReceive challenge: URLAuthenticationChallenge,
                  completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
    let (d, c) = core.disposition(for: challenge)
    completionHandler(d, c)
  }
}
