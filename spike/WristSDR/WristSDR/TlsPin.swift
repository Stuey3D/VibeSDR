import Foundation

/// ★★★ PER-SERVER CERTIFICATE PINS — THE DECISIONS, ON THE WATCH (GitHub #52).
///
/// casm's OpenWebRX+ answered https on 8073 with the self-signed pair it was set up with, and nothing of
/// ours could connect: the platform rightly refuses a certificate no recognised authority signed. Like a
/// browser or SSH, Jr now shows THAT certificate's SHA-256 fingerprint and lets the person trust it for
/// THAT host:port only.
///
/// ★★ A SWIFT PORT OF THE PHONE'S src/utils/tlsPin.ts, rule for rule — Jr is a separate app with its own
///    sockets (watch_split_jr_buddy), so it cannot borrow the phone's code, but the two must never
///    disagree: the same key ("host:port", lower-case, no IPv6 brackets, no trailing dot), the same
///    fingerprint (SHA-256 of the leaf's DER, lower-case hex) and the same decideTls().
/// ★ PURE Foundation: spike/WristSDR/tools/test-jr-tls-pin.sh compiles this file on the Mac and checks
///   it against real certificates. The Security half is TlsTrust.swift; the prompt is TlsTrustPrompt.swift.
/// ★ arm64_32: Swift `Int` is 32-bit below watchOS 27 (arm64_32_int_trap). Nothing here multiplies or
///   converts a Double; DER lengths are bounded by the buffer before they can grow.
enum TlsPin {

  /// "host:port" — host lower-cased, IPv6 brackets and a trailing dot removed. tlsPin.ts pinKey() and
  /// VibeTlsCore.keyForHost are the same.
  static func key(host: String, port: Int) -> String {
    var h = host.trimmingCharacters(in: .whitespaces).lowercased()
    if h.hasPrefix("["), h.hasSuffix("]"), h.count >= 2 { h = String(h.dropFirst().dropLast()) }
    while h.hasSuffix(".") { h.removeLast() }
    return "\(h):\(port)"
  }

  /// A SHA-256 fingerprint, normalised: 64 lower-case hex characters, or "" when it is not one.
  static func normalise(_ fp: String?) -> String {
    let h = (fp ?? "").lowercased().filter { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    return h.count == 64 ? h : ""
  }

  /// The way browsers and `openssl -fingerprint` show it: upper-case byte pairs joined by colons.
  static func format(_ fp: String?) -> String {
    pairs(fp).joined(separator: ":")
  }

  /// ★ For the wrist: `perLine` bytes a line, so 32 bytes at 8 a line is four short lines that never
  ///   wrap mid-pair ("AB:CD:EF:01:23:45:67:89" — 23 characters, which fits a 41 mm screen in mono).
  static func lines(_ fp: String?, perLine: Int = 8) -> [String] {
    let p = pairs(fp)
    let n = max(1, perLine)
    return stride(from: 0, to: p.count, by: n).map { p[$0..<min($0 + n, p.count)].joined(separator: ":") }
  }

  private static func pairs(_ fp: String?) -> [String] {
    let h = Array(normalise(fp).uppercased())
    return stride(from: 0, to: h.count, by: 2).map { String(h[$0..<$0 + 2]) }
  }

  // ── what the certificate says about itself (for the prompt's WORDS only — never a trust decision) ──

  struct Cert: Equatable {
    var subjectCN: String
    var issuerCN: String
    /// subjectAltName dNSName and iPAddress entries, in order.
    var names: [String]
    var notBefore: Date?
    var notAfter: Date?
    /// Issuer and subject are the same name — what a self-signed certificate looks like.
    var selfSigned: Bool
  }

  private struct Tlv { let tag: UInt8; let start: Int; let end: Int }
  private struct Bad: Error {}

  /// One DER element at `p`. Throws on anything malformed — including a length past the buffer, checked
  /// at every byte so the accumulator can never outgrow a 32-bit Int.
  private static func tlv(_ d: [UInt8], _ p: Int) throws -> Tlv {
    guard p >= 0, p + 2 <= d.count else { throw Bad() }
    let tag = d[p]
    var len = Int(d[p + 1])
    var hdr = 2
    if len & 0x80 != 0 {
      let n = len & 0x7f
      guard n >= 1, n <= 4, p + 2 + n <= d.count else { throw Bad() }
      len = 0
      for i in 0..<n {
        len = len * 256 + Int(d[p + 2 + i])
        guard len <= d.count else { throw Bad() }
      }
      hdr = 2 + n
    }
    let start = p + hdr
    let end = start + len
    guard end <= d.count else { throw Bad() }
    return Tlv(tag: tag, start: start, end: end)
  }

  private static func children(_ d: [UInt8], _ t: Tlv) throws -> [Tlv] {
    var out: [Tlv] = []
    var p = t.start
    while p < t.end { let c = try tlv(d, p); out.append(c); p = c.end }
    return out
  }

  private static func str(_ d: [UInt8], _ t: Tlv) -> String {
    let b = Array(d[t.start..<t.end])
    return String(bytes: b, encoding: .utf8) ?? String(bytes: b, encoding: .isoLatin1) ?? ""
  }

  /// CN out of a Name (SEQUENCE OF SET OF SEQUENCE { OID, value }). OID 2.5.4.3 = 55 04 03.
  private static func commonName(_ d: [UInt8], _ name: Tlv) throws -> String {
    for rdn in try children(d, name) {
      for atv in try children(d, rdn) {
        let parts = try children(d, atv)
        guard parts.count >= 2 else { continue }
        let oid = parts[0]
        if oid.end - oid.start == 3, d[oid.start] == 0x55, d[oid.start + 1] == 0x04, d[oid.start + 2] == 0x03 {
          return str(d, parts[1])
        }
      }
    }
    return ""
  }

  private static func time(_ d: [UInt8], _ t: Tlv) -> Date? {
    let s = str(d, t)
    let digits = Array(s.prefix(while: { $0.isNumber }))
    guard s.hasSuffix("Z") else { return nil }
    func num(_ a: Int, _ n: Int) -> Int? {
      guard a + n <= digits.count else { return nil }
      return Int(String(digits[a..<a + n]))
    }
    var c = DateComponents()
    c.timeZone = TimeZone(identifier: "UTC")
    var i: Int
    if t.tag == 0x17 {                                      // UTCTime YYMMDDHHMM[SS]Z
      guard let yy = num(0, 2) else { return nil }
      c.year = yy < 50 ? 2000 + yy : 1900 + yy             // RFC 5280 §4.1.2.5.1
      i = 2
    } else {                                                // GeneralizedTime YYYYMMDDHHMM[SS]Z
      guard let y = num(0, 4) else { return nil }
      c.year = y
      i = 4
    }
    guard let mo = num(i, 2), let dd = num(i + 2, 2), let hh = num(i + 4, 2), let mi = num(i + 6, 2) else { return nil }
    c.month = mo; c.day = dd; c.hour = hh; c.minute = mi
    c.second = num(i + 8, 2) ?? 0
    var cal = Calendar(identifier: .gregorian)
    cal.timeZone = TimeZone(identifier: "UTC")!
    return cal.date(from: c)
  }

  private static func ipText(_ d: [UInt8], _ t: Tlv) -> String {
    let b = Array(d[t.start..<t.end])
    if b.count == 4 { return b.map(String.init).joined(separator: ".") }
    if b.count == 16 {
      var g: [String] = []
      for i in stride(from: 0, to: 16, by: 2) { g.append(String((Int(b[i]) << 8) | Int(b[i + 1]), radix: 16)) }
      var s = g.joined(separator: ":")
      if let r = s.range(of: "(^|:)0(:0)+(:|$)", options: .regularExpression) { s.replaceSubrange(r, with: "::") }
      return s
    }
    return ""
  }

  /// Read the leaf's DER. nil when it cannot be read — the prompt then shows only the fingerprint, which
  /// is the part that matters.
  static func parse(der data: Data) -> Cert? {
    let d = [UInt8](data)
    do {
      let cert = try tlv(d, 0)
      guard let tbs = try children(d, cert).first else { return nil }
      let f = try children(d, tbs)
      var i = 0
      if let first = f.first, first.tag == 0xa0 { i += 1 }      // [0] version
      guard f.count > i + 4 else { return nil }
      let issuer = f[i + 2], validity = f[i + 3], subject = f[i + 4]
      let v = try children(d, validity)
      var names: [String] = []
      if f.count > i + 6, let ext = f[(i + 6)...].first(where: { $0.tag == 0xa3 }),   // [3] extensions
         let list = try children(d, ext).first {
        for e in try children(d, list) {
          let parts = try children(d, e)
          guard let oid = parts.first, let oct = parts.last else { continue }
          // 2.5.29.17 subjectAltName = 55 1D 11; the value is the last element (an OCTET STRING)
          if oid.end - oid.start == 3, d[oid.start] == 0x55, d[oid.start + 1] == 0x1d, d[oid.start + 2] == 0x11 {
            let seq = try tlv(d, oct.start)
            for gn in try children(d, seq) {
              if gn.tag == 0x82 { names.append(str(d, gn)) }
              else if gn.tag == 0x87 { let ip = ipText(d, gn); if !ip.isEmpty { names.append(ip) } }
            }
          }
        }
      }
      let selfSigned = issuer.end - issuer.start == subject.end - subject.start
        && d[issuer.start..<issuer.end].elementsEqual(d[subject.start..<subject.end])
      return Cert(subjectCN: try commonName(d, subject), issuerCN: try commonName(d, issuer), names: names,
                  notBefore: v.count > 0 ? time(d, v[0]) : nil, notAfter: v.count > 1 ? time(d, v[1]) : nil,
                  selfSigned: selfSigned)
    } catch { return nil }
  }

  /// Does the certificate name this host? SANs when it has any, else the CN — the usual rules, one
  /// wildcard label. ★ For the prompt's WORDS only ("issued to openwebrx — not this address"); the pin
  /// never depends on it.
  static func namesHost(_ c: Cert, host: String) -> Bool {
    var h = key(host: host, port: 0)
    h.removeLast(2)                                           // ":0"
    let list = !c.names.isEmpty ? c.names : (c.subjectCN.isEmpty ? [] : [c.subjectCN])
    return list.contains { n in
      var name = n.lowercased()
      while name.hasSuffix(".") { name.removeLast() }
      if name == h { return true }
      if name.hasPrefix("*.") {
        let rest = String(name.dropFirst())                   // ".example.org"
        guard h.hasSuffix(rest), let dot = h.firstIndex(of: ".") else { return false }
        return h.distance(from: h.startIndex, to: dot) == h.count - rest.count
      }
      return false
    }
  }

  // ── which prompt ───────────────────────────────────────────────────────────────────────────

  /// What the probe (TlsTrust.probe) reports about host:port.
  struct Inspection {
    var systemTrusted: Bool
    /// ok | untrusted | expired | hostname | unreachable | error
    var reason: String
    var sha256: String
    var der: Data? = nil
    var pinned: String? = nil
    var message: String? = nil
  }

  enum RefuseWhy: String { case expired, hostname }

  enum Decision: Equatable {
    /// Not a certificate problem we can help with: the system trusts it, the pin matches, or nothing answered.
    case none
    /// First use: not from a recognised authority, no pin — offer "Trust this server".
    case offer(sha256: String)
    /// A pin exists and this is a different certificate — warn, show both, require re-trust.
    case changed(sha256: String, previous: String)
    /// A problem trust on first use must not paper over: out of date, or a public certificate for another name.
    case refuse(why: RefuseWhy, sha256: String)
  }

  /// ★★★ THE RULES, in one place (tlsPin.ts decideTls, unchanged):
  ///  - the system trusts it → nothing to ask (a pin is never needed for a certificate the platform accepts);
  ///  - expired / not yet valid → refused, pinned or not (trust on first use replaces "who signed it",
  ///    not "is it current");
  ///  - a publicly trusted certificate for ANOTHER name → refused: a misconfiguration or an interception;
  ///  - untrusted + no pin → offer; untrusted + the pin → nothing to ask (the failure was something else);
  ///    untrusted + a different pin → "this server's certificate has changed". Never silently.
  ///  ★ A self-signed certificate that does not name the address (OWRX+ owners' usual `openssl req -x509`
  ///    one names "openwebrx") is still offered: the person is trusting THAT certificate for THIS host:port.
  static func decide(_ r: Inspection?, pinned: String? = nil) -> Decision {
    guard let r, !r.systemTrusted else { return .none }
    let fp = normalise(r.sha256)
    guard !fp.isEmpty else { return .none }
    let pin = normalise(pinned ?? r.pinned)
    if r.reason == "expired" { return .refuse(why: .expired, sha256: fp) }
    if r.reason == "hostname" { return .refuse(why: .hostname, sha256: fp) }
    guard r.reason == "untrusted" else { return .none }
    if pin.isEmpty { return .offer(sha256: fp) }
    if pin == fp { return .none }
    return .changed(sha256: fp, previous: pin)
  }

  /// host and port of an http(s)/ws(s) address, when it is TLS — the only kind a certificate can be asked about.
  static func target(_ url: String) -> (host: String, port: Int)? {
    let s = url.trimmingCharacters(in: .whitespaces)
    let lower = s.lowercased()
    let rest: Substring
    if lower.hasPrefix("https://") { rest = s.dropFirst(8) }
    else if lower.hasPrefix("wss://") { rest = s.dropFirst(6) }
    else { return nil }
    let auth = rest.prefix { $0 != "/" && $0 != "?" && $0 != "#" }
    var host: String
    var portText = ""
    if auth.hasPrefix("[") {
      guard let close = auth.firstIndex(of: "]") else { return nil }
      host = String(auth[auth.index(after: auth.startIndex)..<close])
      let after = auth[auth.index(after: close)...]
      if after.hasPrefix(":") { portText = String(after.dropFirst()) } else if !after.isEmpty { return nil }
    } else if let colon = auth.firstIndex(of: ":") {
      host = String(auth[..<colon])
      portText = String(auth[auth.index(after: colon)...])
    } else {
      host = String(auth)
    }
    guard !host.isEmpty else { return nil }
    var port = 443
    if !portText.isEmpty {
      guard portText.count <= 5, portText.allSatisfy(\.isNumber), let p = Int(portText) else { return nil }
      port = p
    }
    guard port > 0, port < 65536 else { return nil }
    return (host, port)
  }

  static func target(_ url: URL) -> (host: String, port: Int)? { target(url.absoluteString) }
}
