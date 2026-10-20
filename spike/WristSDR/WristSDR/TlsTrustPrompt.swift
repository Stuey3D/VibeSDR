import SwiftUI

/// ★★★ TRUST ON FIRST USE, FOR A SERVER WITH ITS OWN CERTIFICATE (GitHub #52) — the question and the answer.
///
/// The phone's services/tlsTrust.ts + components/TlsTrustPrompt.tsx, for the wrist:
///  - after a TLS connection to a server FAILED on its certificate, ask the probe (TlsTrust.probe — a
///    handshake cancelled at the certificate, nothing is sent) what the certificate was, decide with
///    TlsPin.decide, and put the question in front of the person;
///  - first use → host:port, who it is issued to ("— not this address" when it does not name it), the issuer
///    or "self-signed", the expiry and the SHA-256 fingerprint; Cancel / Trust. Yes → pinned, and the caller
///    simply tries again;
///  - a different certificate from the one trusted → "Certificate has changed", both fingerprints, re-trust;
///  - expired, or a public certificate for another name → explained, OK only. Never offered.
///  - every decision is crumbed to jr-vitals.log (Vitals.crumb) — Jr's own diagnostics.
///
/// ★★ ONLY AFTER A FAILURE THAT WAS THE CERTIFICATE. The probe costs one handshake; a refused port, a timeout
///    or a plain-HTTP server never reaches it (TlsTrust.isTrustFailure).
/// ★ One question per host:port at a time — the audio and spectrum sockets failing together share the
///   answer. A Cancel is remembered for a minute so the next socket does not ask again; picking the server
///   again (SpikeLink.start) is a new, deliberate connection and clears that.
@MainActor
final class TlsTrustCenter: ObservableObject {
  static let shared = TlsTrustCenter()

  enum Outcome: Sendable { case trusted, declined, refused, none }

  struct Prompt: Identifiable {
    let id = UUID()
    let host: String
    let port: Int
    let decision: TlsPin.Decision
    let cert: TlsPin.Cert?
  }

  @Published private(set) var current: Prompt?
  private var pending: [(Prompt, CheckedContinuation<Bool, Never>)] = []
  private var answerFor: CheckedContinuation<Bool, Never>?
  private var inFlight: [String: Task<Outcome, Never>] = [:]
  private var declinedAt: [String: Date] = [:]
  private static let declineMemory: TimeInterval = 60

  /// ★ WHO SHOWS THE QUESTION. A sheet cannot be presented over another sheet, and the custom-server sheet
  ///   (where detection runs) is itself one — so every place that can host the prompt registers here, and
  ///   only the top-most one presents it. The root registers first; a sheet that appears later sits above.
  @Published private(set) var presenters: [UUID] = []
  func register(_ id: UUID) { if !presenters.contains(id) { presenters.append(id) } }
  func unregister(_ id: UUID) { presenters.removeAll { $0 == id } }

  private func log(_ line: String) { Vitals.crumb("TLS \(line)") }

  /// ★★★ AFTER A CONNECTION TO host:port FAILED ON ITS CERTIFICATE: if it is one the person can trust (first
  ///     use, or a changed certificate), ask — and on yes, pin it, so the caller can simply try again. A
  ///     refusal we must not paper over (expired; a public certificate for another name) is explained, OK
  ///     only. Anything else (the system trusts it, the pin already matches, nothing answered) → .none, and
  ///     the caller reports its own error exactly as before.
  func checkAfterFailure(host: String, port: Int) async -> Outcome {
    let key = TlsPin.key(host: host, port: port)
    if let busy = inFlight[key] { return await busy.value }
    if let at = declinedAt[key], Date().timeIntervalSince(at) < Self.declineMemory { return .declined }
    let t = Task { await self.ask(host: host, port: port, key: key) }
    inFlight[key] = t
    let o = await t.value
    inFlight[key] = nil
    return o
  }

  /// The same, from a URL; plain http/ws → .none.
  nonisolated static func check(url: URL) async -> Outcome {
    guard let t = TlsPin.target(url) else { return .none }
    return await shared.checkAfterFailure(host: t.host, port: t.port)
  }

  private func ask(host: String, port: Int, key: String) async -> Outcome {
    let r = await TlsTrust.shared.probe(host: host, port: port)
    let decision = TlsPin.decide(r)
    let fp: String
    switch decision {
    case .none: return .none
    case .offer(let s):
      fp = TlsPin.format(s)
      log("\(key) not from a recognised authority, sha256 \(fp) — asking")
    case .changed(let s, let prev):
      fp = TlsPin.format(s)
      log("\(key) CERTIFICATE CHANGED: was \(TlsPin.format(prev)), now \(fp) — refused until re-trusted")
    case .refuse(let why, let s):
      fp = TlsPin.format(s)
      log("\(key) refused (\(why.rawValue)) sha256 \(fp)")
    }
    let cert = r.der.flatMap { TlsPin.parse(der: $0) }
    let p = Prompt(host: host, port: port, decision: decision, cert: cert)
    let yes = await withCheckedContinuation { (c: CheckedContinuation<Bool, Never>) in show(p, c) }
    if case .refuse = decision { declinedAt[key] = Date(); return .refused }
    guard yes else {
      declinedAt[key] = Date()
      log("\(key) not trusted (cancelled)")
      return .declined
    }
    let sha: String
    switch decision {
    case .offer(let s), .changed(let s, _): sha = s
    default: return .none
    }
    TlsTrust.shared.pin(host: host, port: port, sha256: sha, subject: cert?.subjectCN ?? "")
    declinedAt[key] = nil
    if case .changed = decision { log("\(key) trusted, sha256 \(fp) (replaces the old certificate)") }
    else { log("\(key) trusted, sha256 \(fp)") }
    return .trusted
  }

  private func show(_ p: Prompt, _ c: CheckedContinuation<Bool, Never>) {
    if current == nil { current = p; answerFor = c } else { pending.append((p, c)) }
  }

  /// The person's answer to the prompt on screen: true = trust (offer / changed), anything else = no.
  func answer(_ yes: Bool, for id: UUID) {
    guard let cur = current, cur.id == id, let c = answerFor else { return }
    answerFor = nil
    if let (p, nc) = pending.first { pending.removeFirst(); current = p; answerFor = nc } else { current = nil }
    c.resume(returning: yes)
  }

  /// A new, deliberate connection (the person picked the server): a Cancel from earlier no longer stands.
  func clearDeclines() { declinedAt.removeAll() }

  /// Did the person just decline (or was refused) this address's certificate? So a caller can say THAT
  /// instead of "couldn't find it".
  func recentlyDeclined(url: String) -> Bool {
    guard let t = TlsPin.target(url), let at = declinedAt[TlsPin.key(host: t.host, port: t.port)] else { return false }
    return Date().timeIntervalSince(at) < Self.declineMemory
  }

  // ── pins, for the server options ────────────────────────────────────────────────────────────

  /// The trusted fingerprint for this address's host:port, or nil (plain http/ws never has one).
  func pin(forURL url: String) -> String? {
    guard let t = TlsPin.target(url) else { return nil }
    return TlsTrust.shared.pin(host: t.host, port: t.port)
  }

  /// Forget the trusted certificate for this address's host:port. True if there was one.
  @discardableResult
  func forget(url: String) -> Bool {
    guard let t = TlsPin.target(url), TlsTrust.shared.forget(host: t.host, port: t.port) else { return false }
    log("\(TlsPin.key(host: t.host, port: t.port)) trusted certificate forgotten")
    objectWillChange.send()
    return true
  }

  // ── HTTP to a server, with the question ─────────────────────────────────────────────────────

  /// `session.data(for:)` (the session must carry TlsSessionDelegate — TlsTrust.session or one built with
  /// it). On a refused certificate, and only when `ask`, the question is put; trusted → tried once more.
  /// Not trusted → throws TlsNotTrusted, whose message is what the status line should say.
  nonisolated static func data(for req: URLRequest, session: URLSession = TlsTrust.session,
                               ask: Bool) async throws -> (Data, URLResponse) {
    do { return try await session.data(for: req) }
    catch {
      guard TlsTrust.isTrustFailure(error), let url = req.url, TlsPin.target(url) != nil else { throw error }
      guard ask else { throw TlsNotTrusted() }
      switch await check(url: url) {
      case .trusted: return try await session.data(for: req)
      case .none:    throw error
      default:       throw TlsNotTrusted()
      }
    }
  }
}

/// A connection refused because its certificate is not trusted (declined, refused, or not asked).
struct TlsNotTrusted: LocalizedError {
  var errorDescription: String? { "certificate not trusted" }
}

// ── The prompt ────────────────────────────────────────────────────────────────────────────────

extension View {
  /// Host the certificate question here. Put on the root and on any SHEET whose own work can fail on a
  /// certificate (a sheet cannot be presented over another one). Only the top-most host presents it.
  func tlsTrustPrompt() -> some View { modifier(TlsTrustHost()) }
}

private struct TlsTrustHost: ViewModifier {
  @ObservedObject private var center = TlsTrustCenter.shared
  @State private var id = UUID()

  func body(content: Content) -> some View {
    content
      .onAppear { center.register(id) }
      .onDisappear { center.unregister(id) }
      .sheet(item: Binding(
        get: { center.presenters.last == id ? center.current : nil },
        set: { _ in }
      )) { p in
        TlsTrustPromptView(prompt: p) { center.answer($0, for: p.id) }
          // ★ Gone without an answer (dismissed by the system) = Cancel. answer() ignores a prompt that has
          //   already been answered, so the normal close after a button does nothing here.
          .onDisappear { center.answer(false, for: p.id) }
      }
  }
}

/// ★ THE WRIST VERSION of the phone's prompt: the same three faces and the same facts, in short lines. One
///   ScrollView, so the Digital Crown reads it top to bottom; the fingerprint is four 23-character monospace
///   lines that never wrap mid-pair; Trust sits at the very bottom, under the fingerprint, so it is reached
///   only by scrolling past it. Cancel is also in the corner, always.
struct TlsTrustPromptView: View {
  let prompt: TlsTrustCenter.Prompt
  let answer: (Bool) -> Void

  private var whereText: String {
    "\(prompt.host.contains(":") ? "[\(prompt.host)]" : prompt.host):\(prompt.port)"
  }
  private var offerable: Bool { if case .refuse = prompt.decision { return false }; return true }
  private var issuedTo: String { prompt.cert.map { $0.subjectCN.isEmpty ? ($0.names.first ?? "") : $0.subjectCN } ?? "" }

  private static func day(_ d: Date?) -> String {
    guard let d else { return "" }
    let f = DateFormatter(); f.dateStyle = .medium; f.timeStyle = .none
    return f.string(from: d)
  }

  private var title: String {
    switch prompt.decision {
    case .offer: return "Untrusted certificate"
    case .changed: return "Certificate has changed"
    case .refuse(let why, _): return why == .expired ? "Certificate out of date" : "Certificate is for another address"
    case .none: return ""
    }
  }

  private var message: String {
    let c = prompt.cert
    switch prompt.decision {
    case .offer:
      return "This server's certificate isn't from a recognised authority — usual for a receiver with its own "
        + "self-signed certificate. Only trust it if this is your server, or you know its owner."
    case .changed:
      return "This server now has a different certificate from the one you trusted. If its owner replaced it, "
        + "trust the new one. If you weren't expecting a change, cancel — someone could be intercepting the connection."
    case .refuse(.expired, _):
      let when = (c?.notAfter).map { $0 < Date() ? "expired on \(Self.day($0))" : "is not valid yet" } ?? "is out of date"
      return "This server's certificate \(when). Jr won't connect with an out-of-date certificate, even one you "
        + "trusted before — its owner needs to renew it. (If the date looks wrong, check the watch's clock.)"
    case .refuse(.hostname, _):
      let names = !(c?.names.isEmpty ?? true) ? " (\(c!.names.prefix(3).joined(separator: ", ")))"
        : (issuedTo.isEmpty ? "" : " (\(issuedTo))")
      return "This server's certificate is from a recognised authority, but for a different name\(names). "
        + "Check the address — the name in the certificate is usually the one to use."
    case .none:
      return ""
    }
  }

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 7) {
        Text(title).font(.system(size: 15, weight: .bold)).foregroundColor(.orange)
          .fixedSize(horizontal: false, vertical: true)
        Text(whereText).font(.system(size: 11, design: .monospaced)).foregroundColor(.white.opacity(0.6))
          .lineLimit(2).minimumScaleFactor(0.7)
        Text(message).font(.system(size: 11)).foregroundColor(.white.opacity(0.85))
          .fixedSize(horizontal: false, vertical: true)
        VStack(alignment: .leading, spacing: 6) {
          if !issuedTo.isEmpty {
            let namesIt = prompt.cert.map { TlsPin.namesHost($0, host: prompt.host) } ?? true
            row("Issued to", namesIt ? issuedTo : "\(issuedTo) — not this address")
          }
          if let c = prompt.cert {
            row("Issued by", c.selfSigned ? "itself (self-signed)" : (c.issuerCN.isEmpty ? "unknown" : c.issuerCN))
            if c.notAfter != nil { row("Expires", Self.day(c.notAfter)) }
          }
          if case .changed(_, let prev) = prompt.decision { fingerprint("Trusted before (SHA-256)", prev) }
          fingerprint(isChanged ? "Now (SHA-256)" : "SHA-256 fingerprint", sha)
        }
        .padding(7)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color.white.opacity(0.08)))
        if offerable {
          Text("Jr will accept this exact certificate for \(whereText) only. To forget it later, hold the server in the list → Forget certificate.")
            .font(.system(size: 9.5)).foregroundColor(.white.opacity(0.5))
            .fixedSize(horizontal: false, vertical: true)
          Button("Cancel") { answer(false) }
            .font(.system(size: 13))
          Button(isChanged ? "Trust new certificate" : "Trust this server") { answer(true) }
            .font(.system(size: 13, weight: .semibold)).tint(.orange)
        } else {
          Button("OK") { answer(false) }
            .font(.system(size: 14, weight: .semibold)).tint(.orange)
        }
      }
      .padding(.horizontal, 2).padding(.bottom, 6)
    }
    // ★ The corner button the system would put on a sheet, made OURS so it answers the question.
    .toolbar {
      ToolbarItem(placement: .cancellationAction) {
        Button(offerable ? "Cancel" : "OK") { answer(false) }
      }
    }
  }

  private var isChanged: Bool { if case .changed = prompt.decision { return true }; return false }
  private var sha: String {
    switch prompt.decision {
    case .offer(let s), .changed(let s, _), .refuse(_, let s): return s
    case .none: return ""
    }
  }

  @ViewBuilder private func row(_ label: String, _ value: String) -> some View {
    VStack(alignment: .leading, spacing: 1) {
      Text(label).font(.system(size: 9.5)).foregroundColor(.white.opacity(0.5))
      Text(value).font(.system(size: 12)).foregroundColor(.white)
        .fixedSize(horizontal: false, vertical: true)
    }
  }

  @ViewBuilder private func fingerprint(_ label: String, _ fp: String) -> some View {
    VStack(alignment: .leading, spacing: 1) {
      Text(label).font(.system(size: 9.5)).foregroundColor(.white.opacity(0.5))
      ForEach(Array(TlsPin.lines(fp).enumerated()), id: \.offset) { _, line in
        Text(line).font(.system(size: 10.5, design: .monospaced)).foregroundColor(.white)
          .lineLimit(1).minimumScaleFactor(0.6)
      }
    }
  }
}
