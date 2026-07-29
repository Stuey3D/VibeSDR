import Foundation
import Network
import Combine

/// A VibeServer advertised on the local network via Bonjour (`_vibesdr._tcp`). `host` is the resolved
/// `host:port` ready to drop into a `ws://` URL; `pinRequired` comes from the TXT record.
struct VibeAd: Identifiable, Equatable {
  let id: String          // Bonjour service name (stable key)
  let name: String        // friendly name from the TXT `name` (falls back to the service name)
  let host: String        // resolved host:port
  let pinRequired: Bool
}

/// Watch-side mDNS discovery of VibeServers. The phone advertises `_vibesdr._tcp` with TXT keys
/// name / proto / pin; the watch browses for them so a user's own phone-server appears in the picker
/// without typing an address. (Info.plist must declare `_vibesdr._tcp` in NSBonjourServices — watchOS
/// silently blocks NWBrowser for undeclared types.)
@MainActor
final class VibeMdns: ObservableObject {
  @Published var found: [VibeAd] = []
  private var browser: NWBrowser?
  private var resolvers: [String: NWConnection] = [:]
  private var pending: [String: (endpoint: NWEndpoint, name: String, pin: Bool)] = [:]   // for resolve retries
  private var stopped = true

  func start() {
    stopped = false
    startKeepAwake()
    guard browser == nil else { return }
    spawnBrowser()
  }

  private var keepAwake: NWConnection?

  /// KEEP THE NETWORK STACK AWAKE. On watchOS the Bonjour RESOLVE stalls forever until there's a REAL
  /// sustained outbound connection — an active UberSDR session made discovery start working; a URLSession
  /// GET did not. So hold a live TLS connection to a real host open the whole time we're searching, and
  /// drop it the instant we've found something (see upsert). Re-establishes if it drops while still looking.
  private func startKeepAwake() {
    guard keepAwake == nil, found.isEmpty else { return }
    let c = NWConnection(host: "www.apple.com", port: 443, using: .tls)
    c.stateUpdateHandler = { [weak self] st in
      switch st {
      case .failed, .cancelled:
        Task { @MainActor in
          guard let self, self.keepAwake === c else { return }
          self.keepAwake = nil
          if !self.stopped, self.found.isEmpty { self.startKeepAwake() }
        }
      default: break
      }
    }
    c.start(queue: .global())
    keepAwake = c
  }

  private func stopKeepAwake() { keepAwake?.cancel(); keepAwake = nil }

  private func spawnBrowser() {
    let params = NWParameters.tcp
    params.includePeerToPeer = false
    let b = NWBrowser(for: .bonjourWithTXTRecord(type: "_vibesdr._tcp", domain: nil), using: params)
    b.stateUpdateHandler = { [weak self] st in
      // watchOS reaps the browser periodically (-65569 DefunctConnection). Respawn on failure unless we
      // were deliberately stopped — WITHOUT clearing `found`, so the list doesn't flicker out and back.
      guard case .failed = st else { return }
      Task { @MainActor in
        guard let self, !self.stopped else { return }
        self.browser?.cancel(); self.browser = nil
        try? await Task.sleep(nanoseconds: 800_000_000)
        guard !self.stopped else { return }
        self.spawnBrowser()
      }
    }
    b.browseResultsChangedHandler = { [weak self] results, _ in
      Task { @MainActor in self?.handle(results) }
    }
    b.start(queue: .main)
    browser = b
  }

  /// USER-TRIGGERED RESCAN. Cold auto-discovery is flaky on watchOS (the resolve stalls until real network
  /// activity), so give the user a button: tear down + restart the browser and re-warm the stack. Keeps any
  /// servers already found.
  func refresh() {
    stopped = false
    resolvers.values.forEach { $0.cancel() }; resolvers.removeAll()
    pending.removeAll()
    browser?.cancel(); browser = nil
    startKeepAwake()
    spawnBrowser()
  }

  func stop() {
    stopped = true
    browser?.cancel(); browser = nil
    resolvers.values.forEach { $0.cancel() }; resolvers.removeAll()
    stopKeepAwake()
    found = []
  }

  private func handle(_ results: Set<NWBrowser.Result>) {
    var live = Set<String>()
    for r in results {
      guard case let .service(name, _, _, _) = r.endpoint else { continue }
      var pin = false
      var friendly = name
      if case let .bonjour(txt) = r.metadata {
        if let proto = Self.txt(txt, "proto"), proto != "vibeserver" { continue }   // ignore rtltcp adverts
        pin = Self.txt(txt, "pin") == "1"
        if let n = Self.txt(txt, "name"), !n.isEmpty { friendly = n }
      }
      live.insert(name)
      pending[name] = (r.endpoint, friendly, pin)   // keep the freshest endpoint for (re)tries
      // Resolve host:port (Bonjour gives a service endpoint, not an address). Kick it off if not already.
      if resolvers[name] == nil, !found.contains(where: { $0.id == name }) { resolve(name) }
    }
    found.removeAll { !live.contains($0.id) }
    pending = pending.filter { live.contains($0.key) }
  }

  /// Resolve a service to host:port with a TIMEOUT+RETRY. The browse finds the service instantly, but the
  /// resolve NWConnection sometimes sticks in `.preparing` forever on watchOS until there's other network
  /// activity — that was the "only appears after connecting elsewhere" bug. Rather than churn the BROWSER
  /// (which kills the in-flight resolve), we retry just the RESOLVE on a 4s timeout until it lands.
  private func resolve(_ name: String) {
    guard let p = pending[name], resolvers[name] == nil, !found.contains(where: { $0.id == name }) else { return }
    let conn = NWConnection(to: p.endpoint, using: .tcp)
    resolvers[name] = conn
    conn.stateUpdateHandler = { [weak self] state in
      switch state {
      case .ready:
        guard let path = conn.currentPath, case let .hostPort(host, port)? = path.remoteEndpoint else { return }
        let h = "\(host)".split(separator: "%").first.map(String.init) ?? "\(host)"
        let hp = "\(h):\(port.rawValue)"
        Task { @MainActor in self?.upsert(VibeAd(id: name, name: p.name, host: hp, pinRequired: p.pin)) }
        conn.cancel()
      case .failed:
        Task { @MainActor in self?.retryResolve(name, after: conn) }
      default: break
      }
    }
    conn.start(queue: .main)
    // The stuck-in-.preparing backstop. The resolve ONLY lands when done immediately off a FRESH browse
    // result — a stale endpoint stalls forever (proven: every success is the same second as a browser
    // respawn). So a short 2s timeout, then respawn the BROWSER to mint a hot endpoint, which handle()
    // re-resolves instantly. (This is exactly what "connect to a server then back out" did by hand.)
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: 2_000_000_000)
      guard !self.stopped, self.resolvers[name] === conn, !self.found.contains(where: { $0.id == name }) else { return }
      self.retryResolve(name, after: conn)
    }
  }

  private func retryResolve(_ name: String, after conn: NWConnection) {
    guard resolvers[name] === conn else { return }        // superseded
    conn.cancel(); resolvers[name] = nil
    guard !stopped, pending[name] != nil, !found.contains(where: { $0.id == name }) else { return }
    respawnBrowser()   // fresh browse result → handle() → resolve() with a HOT endpoint
  }

  /// Cancel + respawn the browser to mint a fresh, resolvable endpoint. Does NOT clear `found`.
  private func respawnBrowser() {
    browser?.cancel(); browser = nil
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: 300_000_000)
      guard !stopped, browser == nil else { return }
      spawnBrowser()
    }
  }

  private func upsert(_ ad: VibeAd) {
    if let i = found.firstIndex(where: { $0.id == ad.id }) { found[i] = ad } else { found.append(ad) }
    resolvers[ad.id]?.cancel(); resolvers[ad.id] = nil
    stopKeepAwake()          // found one — no need to hold the wake connection open any more
  }

  private static func txt(_ txt: NWTXTRecord, _ key: String) -> String? {
    if case let .string(v) = txt.getEntry(for: key) { return v }
    return nil
  }
}
