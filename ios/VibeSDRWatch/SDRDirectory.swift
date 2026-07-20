import Foundation
import SwiftUI

/// Server directories + favourites for the spike's instance picker — a Swift port of the phone's
/// `src/services/directories.ts` / `favourites.ts`, trimmed for the wrist.
///
/// SCOPE: lists every directory (UberSDR / Receiverbook / KiwiSDR / FM-DX) plus manual custom URLs,
/// exactly like the phone. The spike can only *connect* to UberSDR today (its `UberClient`); the
/// other backends are being added — the picker is the front door they'll plug into.

// ── Model ──────────────────────────────────────────────────────────────────────

enum ServerType: String, Codable, CaseIterable {
  case ubersdr, kiwi, owrx, fmdx, spyserver, rtltcp, vibeserver

  /// Display name for the by-type sort + row badge.
  var display: String {
    switch self {
    case .ubersdr:   return "UberSDR"
    case .kiwi:      return "KiwiSDR"
    case .owrx:      return "OpenWebRX"
    case .fmdx:      return "FM-DX"
    case .spyserver: return "SpyServer"
    case .rtltcp:    return "RTL-TCP"
    case .vibeserver: return "VibeServer"
    }
  }
  /// Grouping order for the by-type sort (mirrors the phone's TYPE_ORDER).
  var order: Int {
    switch self {
    case .ubersdr: return 0; case .kiwi: return 1; case .owrx: return 2
    case .fmdx: return 3; case .vibeserver: return 4; case .spyserver: return 5; case .rtltcp: return 6
    }
  }
  /// Can the spike actually connect to this yet? (Others land as adapters arrive.)
  var connectable: Bool { self == .ubersdr || self == .kiwi || self == .owrx || self == .fmdx || self == .vibeserver }
}

/// A server row — from a directory or a saved favourite. `url` is the connect key.
struct SDRServer: Identifiable, Codable, Hashable {
  var id: String { url }
  var name: String
  var url: String
  var host: String
  var serverType: ServerType
  var location: String = ""
  var countryCode: String? = nil
  var latitude: Double? = nil
  var longitude: Double? = nil
  var distance: Double? = nil      // km, when the directory provides it
  var bestSnr: Double? = nil
  var users: Int = 0
  var maxUsers: Int = 0
  var full: Bool = false
  var pin: String = ""             // VibeServer PIN (saved per host when the user enters one)
}

// ── Directory metadata ───────────────────────────────────────────────────────────

struct DirectoryMeta: Identifiable {
  let id: String
  let name: String
  let desc: String
}

/// Probe a custom URL's landing page to work out which backend it is — a Swift port of the phone's
/// `detectServerType` (sdrTypes.ts). ORDER MATTERS (a later backend's page contains an earlier one's
/// marker). VibeServer speaks the UberSDR protocol, so it maps to `.ubersdr` on the spike.
func detectServerType(_ url: String) async -> ServerType? {
  var base = url.trimmingCharacters(in: .whitespaces)
  while base.hasSuffix("/") { base.removeLast() }
  base = base.replacingOccurrences(of: "ws://", with: "http://").replacingOccurrences(of: "wss://", with: "https://")
  guard let u = URL(string: base + "/") else { return nil }
  var req = URLRequest(url: u); req.timeoutInterval = 5
  do {
    let (data, _) = try await URLSession.shared.data(for: req)
    let body = (String(data: data, encoding: .utf8) ?? "").lowercased()
    if body.contains("vibeserver") { return .ubersdr }                          // UberSDR-protocol
    if body.contains("ubersdr") { return .ubersdr }
    if body.range(of: "kiwisdr|kiwi sdr|/kiwi/|kiwi_util|owrx_ws_open", options: .regularExpression) != nil { return .kiwi }
    if body.contains("openwebrx") { return .owrx }
    if body.range(of: "fm-dx|fmdx", options: .regularExpression) != nil { return .fmdx }
    return .ubersdr   // reachable but unidentifiable → assume UberSDR
  } catch { return nil }
}

enum Directories {
  static let all: [DirectoryMeta] = [
    .init(id: "ubersdr",      name: "UberSDR",      desc: "Official UberSDR instances"),
    .init(id: "receiverbook", name: "Receiverbook", desc: "OpenWebRX + KiwiSDR (receiverbook.de)"),
    .init(id: "kiwisdr",      name: "KiwiSDR",      desc: "Public KiwiSDR network"),
    .init(id: "fmdx",         name: "FM-DX",        desc: "FM-DX Webserver network"),
  ]

  static func fetch(_ id: String) async throws -> [SDRServer] {
    switch id {
    case "ubersdr":      return try await fetchUberSDR()
    case "fmdx":         return try await fetchFmdx()
    case "kiwisdr":      return try await fetchKiwiList()
    case "receiverbook": return try await fetchReceiverbook()
    default:             return []
    }
  }

  // ── UberSDR — clean JSON API ─────────────────────────────────────────────────
  private static func fetchUberSDR() async throws -> [SDRServer] {
    let url = URL(string: "https://instances.ubersdr.org/api/instances?conditions=true")!
    let (data, _) = try await URLSession.shared.data(from: url)
    let json = try JSONSerialization.jsonObject(with: data) as? [String: Any]
    let items = json?["instances"] as? [[String: Any]] ?? []
    return items.compactMap { it in
      let host = (it["host"] as? String) ?? ""
      var publicUrl = (it["public_url"] as? String) ?? ""
      if publicUrl.isEmpty, !host.isEmpty {
        let tls = (it["tls"] as? Bool) ?? false
        let port = (it["port"] as? Int) ?? (tls ? 443 : 80)
        let scheme = tls ? "https" : "http"
        publicUrl = (port == (tls ? 443 : 80)) ? "\(scheme)://\(host)" : "\(scheme)://\(host):\(port)"
      }
      publicUrl = publicUrl.trimmedTrailingSlash
      guard !publicUrl.isEmpty else { return nil }
      // best SNR across all reported band conditions
      var bestSnr: Double? = nil
      if let bc = it["band_conditions"] as? [String: Any] {
        for v in bc.values { if let n = (v as? NSNumber)?.doubleValue, bestSnr == nil || n > bestSnr! { bestSnr = n } }
      }
      let cc = it["country_code"] as? String
      return SDRServer(
        name: (it["name"] as? String) ?? (it["callsign"] as? String) ?? host,
        url: publicUrl,
        host: URL(string: publicUrl)?.host ?? host,
        serverType: .ubersdr,
        location: (it["location"] as? String) ?? "",
        countryCode: (cc?.count == 2) ? cc?.uppercased() : nil,
        latitude: (it["latitude"] as? NSNumber)?.doubleValue,
        longitude: (it["longitude"] as? NSNumber)?.doubleValue,
        distance: (it["distance"] as? NSNumber)?.doubleValue,
        bestSnr: bestSnr,
        users: (it["available_clients"] as? Int) ?? 0,
        maxUsers: (it["max_clients"] as? Int) ?? 0,
        full: ((it["available_clients"] as? Int) ?? 1) <= 0
      )
    }
  }

  // ── FM-DX — { dataset: [...] } JSON ──────────────────────────────────────────
  private static func fetchFmdx() async throws -> [SDRServer] {
    let url = URL(string: "http://servers.fmdx.org/api/")!
    let (data, _) = try await URLSession.shared.data(from: url)
    let json = try JSONSerialization.jsonObject(with: data) as? [String: Any]
    let rows = json?["dataset"] as? [[String: Any]] ?? []
    return rows.compactMap { r in
      guard (r["status"] as? NSNumber)?.intValue == 1 else { return nil }   // 1 = active
      let u = ((r["url"] as? String) ?? "").trimmedTrailingSlash
      guard !u.isEmpty else { return nil }
      // The FM-DX API returns coords as STRINGS (e.g. ["52.1","-0.9"]) — the phone reads them with
      // Number(), which coerces strings; a plain NSNumber cast returns nil, leaving lat/lon empty so
      // the distance sort never runs. Coerce both number and string forms.
      let coords = r["coords"] as? [Any] ?? []
      func num(_ v: Any?) -> Double? { (v as? NSNumber)?.doubleValue ?? Double((v as? String) ?? "") }
      let lat = coords.count >= 2 ? num(coords[0]) : nil
      let lon = coords.count >= 2 ? num(coords[1]) : nil
      return SDRServer(
        name: (r["name"] as? String) ?? "FM-DX",
        url: u,
        host: URL(string: u)?.host ?? u,
        serverType: .fmdx,
        location: (r["city"] as? String) ?? (r["countryName"] as? String) ?? "",
        countryCode: (r["country"] as? String)?.uppercased(),
        latitude: lat, longitude: lon
      )
    }
  }

  // ── KiwiSDR — linkfanel snapshot: `var kiwisdr_com = [ {...} ]` ────────────────
  private static func fetchKiwiList() async throws -> [SDRServer] {
    let url = URL(string: "http://rx.linkfanel.net/kiwisdr_com.js")!
    let (data, _) = try await URLSession.shared.data(from: url)
    let js = String(decoding: data, as: UTF8.self)
    guard let arr = extractJsArray(js, marker: "var kiwisdr_com") else { return [] }
    return arr.compactMap { r in
      guard let u0 = r["url"] as? String, !u0.isEmpty else { return nil }
      let u = u0.trimmedTrailingSlash
      let snr = (r["snr"] as? String)?.split(separator: ",").compactMap { Double($0) }.max()
      return SDRServer(
        name: (r["name"] as? String) ?? "KiwiSDR",
        url: u,
        host: URL(string: u)?.host ?? u,
        serverType: .kiwi,
        location: (r["loc"] as? String) ?? "",
        latitude: parseCoord(r["gps"], 0),
        longitude: parseCoord(r["gps"], 1),
        bestSnr: snr,
        users: Int((r["users"] as? String) ?? "") ?? 0,
        maxUsers: Int((r["users_max"] as? String) ?? "") ?? 0
      )
    }
  }

  // ── Receiverbook — HTML embeds `var receivers = [ {label,url,location,receivers:[…]} ]` ──
  private static func fetchReceiverbook() async throws -> [SDRServer] {
    let url = URL(string: "https://www.receiverbook.de/map")!
    let (data, _) = try await URLSession.shared.data(from: url)
    let html = String(decoding: data, as: UTF8.self)
    guard let sites = extractJsArray(html, marker: "var receivers") else { return [] }
    var out: [SDRServer] = []
    for site in sites {
      let coords = (site["location"] as? [String: Any])?["coordinates"] as? [Any] ?? []
      let slon = coords.count >= 1 ? (coords[0] as? NSNumber)?.doubleValue : nil
      let slat = coords.count >= 2 ? (coords[1] as? NSNumber)?.doubleValue : nil
      for ro in (site["receivers"] as? [[String: Any]] ?? []) {
        let t = ((ro["type"] as? String) ?? "").lowercased()
        let kind: ServerType? = t == "openwebrx" ? .owrx : t == "kiwisdr" ? .kiwi : nil
        guard let kind else { continue }
        guard let u0 = (ro["url"] as? String) ?? (site["url"] as? String), !u0.isEmpty else { continue }
        let u = u0.trimmedTrailingSlash
        let label = ((ro["label"] as? String) ?? (site["label"] as? String) ?? "Unknown")
          .replacingOccurrences(of: "<[^>]*>", with: "", options: .regularExpression)
        out.append(SDRServer(
          name: String(label.prefix(120)),
          url: u, host: URL(string: u)?.host ?? u, serverType: kind,
          latitude: slat, longitude: slon
        ))
      }
    }
    return out
  }

  // ── JS-array extraction: find `marker … = [` then brace-match to the closing `]` ──
  private static func extractJsArray(_ src: String, marker: String) -> [[String: Any]]? {
    guard let mr = src.range(of: marker),
          let br = src.range(of: "[", range: mr.upperBound..<src.endIndex) else { return nil }
    var depth = 0, inStr = false, esc = false
    var strCh: Character = "\""
    let start = br.lowerBound
    var i = start
    while i < src.endIndex {
      let c = src[i]
      if inStr {
        if esc { esc = false }
        else if c == "\\" { esc = true }
        else if c == strCh { inStr = false }
      } else {
        if c == "\"" || c == "'" { inStr = true; strCh = c }
        else if c == "[" { depth += 1 }
        else if c == "]" { depth -= 1; if depth == 0 { i = src.index(after: i); break } }
      }
      i = src.index(after: i)
    }
    let jsonSlice = String(src[start..<i])
    // The snapshots are JSON-compatible arrays; if single-quoted, normalise the quotes.
    guard let d = jsonSlice.data(using: .utf8),
          let obj = (try? JSONSerialization.jsonObject(with: d)) as? [[String: Any]] else {
      return nil
    }
    return obj
  }

  private static func parseCoord(_ gps: Any?, _ idx: Int) -> Double? {
    // Kiwi "gps" is like "(52.30, -1.08)" — pull the two numbers out.
    guard let s = gps as? String else { return nil }
    let nums = s.replacingOccurrences(of: "(", with: "").replacingOccurrences(of: ")", with: "")
      .split(separator: ",").compactMap { Double($0.trimmingCharacters(in: .whitespaces)) }
    return nums.count > idx ? nums[idx] : nil
  }
}

// ── Favourites (local for now; a WCSession sync to the phone lands at the companion merge) ──

enum FavSort: String, CaseIterable, Codable {
  case used, alpha, nearest, snr, type, manual
  var label: String {
    switch self {
    case .used: return "★ MOST USED"; case .alpha: return "A–Z"; case .nearest: return "NEAREST"
    case .snr: return "SNR"; case .type: return "TYPE"; case .manual: return "MANUAL"
    }
  }
  var next: FavSort {
    let all = FavSort.allCases
    return all[(all.firstIndex(of: self)! + 1) % all.count]
  }
}

/// A saved favourite. Mirrors the phone's `Favourite` shape so a future WCSession sync is a
/// straight field map. `visits` is the Most-Used tally, bumped on every connect.
struct Favourite: Codable, Identifiable, Hashable {
  var id: String { url }
  var name: String
  var url: String
  var serverType: ServerType
  var visits: Int = 0
  var latitude: Double? = nil
  var longitude: Double? = nil
  var bestSnr: Double? = nil
  var host: String = ""            // VibeServer host:port (the url is a display key)
  var pin: String = ""             // VibeServer PIN, saved so it auto-fills next time
}

@MainActor
final class FavStore: ObservableObject {
  @Published private(set) var favourites: [Favourite] = []
  @Published var sort: FavSort = .used { didSet { UserDefaults.standard.set(sort.rawValue, forKey: Self.sortKey) } }

  private static let key = "vibe.spike.favourites"
  private static let sortKey = "vibe.spike.favsort"

  init() {
    if let raw = UserDefaults.standard.data(forKey: Self.key),
       let f = try? JSONDecoder().decode([Favourite].self, from: raw) { favourites = f }
    if let s = UserDefaults.standard.string(forKey: Self.sortKey), let fs = FavSort(rawValue: s) { sort = fs }
  }

  private func persist() {
    if let d = try? JSONEncoder().encode(favourites) { UserDefaults.standard.set(d, forKey: Self.key) }
  }

  func isFav(_ url: String) -> Bool { favourites.contains { $0.url == url } }

  func toggle(_ s: SDRServer) {
    if let i = favourites.firstIndex(where: { $0.url == s.url }) {
      favourites.remove(at: i)
    } else {
      favourites.append(Favourite(name: s.name, url: s.url, serverType: s.serverType,
                                  latitude: s.latitude, longitude: s.longitude, bestSnr: s.bestSnr))
    }
    persist()
  }

  /// Save (or update) a VibeServer favourite with its host + PIN, so next time it auto-fills / auto-connects.
  func saveVibe(name: String, host: String, pin: String) {
    let url = "ws://\(host)"
    if let i = favourites.firstIndex(where: { $0.url == url }) {
      favourites[i].host = host; favourites[i].pin = pin; favourites[i].name = name
    } else {
      favourites.append(Favourite(name: name.isEmpty ? host : name, url: url, serverType: .vibeserver, host: host, pin: pin))
    }
    persist()
  }

  /// The saved PIN for a VibeServer host, to pre-fill the prompt.
  func savedPin(host: String) -> String { favourites.first(where: { $0.host == host })?.pin ?? "" }

  func addCustom(name: String, url: String, type: ServerType) {
    guard !favourites.contains(where: { $0.url == url }) else { return }
    favourites.append(Favourite(name: name.isEmpty ? url : name, url: url, serverType: type))
    persist()
  }

  /// Bump the Most-Used tally when a favourite is connected. No-op for non-favourites.
  func registerVisit(_ url: String) {
    guard let i = favourites.firstIndex(where: { $0.url == url }) else { return }
    favourites[i].visits += 1
    persist()
  }

  /// Refresh distance/SNR snapshots from a freshly-loaded directory (so Nearest/SNR aren't stale).
  func mergeMeta(_ servers: [SDRServer]) {
    var changed = false
    let byUrl = Dictionary(servers.map { ($0.url.trimmedTrailingSlash.lowercased(), $0) }, uniquingKeysWith: { a, _ in a })
    for i in favourites.indices {
      if let s = byUrl[favourites[i].url.trimmedTrailingSlash.lowercased()] {
        if let sn = s.bestSnr, favourites[i].bestSnr != sn { favourites[i].bestSnr = sn; changed = true }
        if let la = s.latitude, favourites[i].latitude != la { favourites[i].latitude = la; changed = true }
        if let lo = s.longitude, favourites[i].longitude != lo { favourites[i].longitude = lo; changed = true }
      }
    }
    if changed { persist() }
  }

  /// Persist a drag reorder (Manual mode).
  func move(from: IndexSet, to: Int) { favourites.move(fromOffsets: from, toOffset: to); persist() }

  /// Favourites in the current sort order. `meta` carries live directory distance/snr by url.
  func sorted(meta: [String: (dist: Double?, snr: Double?)]) -> [Favourite] {
    func dist(_ f: Favourite) -> Double { meta[f.url.trimmedTrailingSlash.lowercased()]?.dist ?? .infinity }
    func snr(_ f: Favourite) -> Double { meta[f.url.trimmedTrailingSlash.lowercased()]?.snr ?? f.bestSnr ?? -.infinity }
    switch sort {
    case .manual: return favourites
    case .used:   return favourites.sorted { $0.visits > $1.visits }
    case .alpha:  return favourites.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    case .nearest: return favourites.sorted { dist($0) < dist($1) }
    case .snr:    return favourites.sorted { snr($0) > snr($1) }
    case .type:   return favourites.sorted {
      $0.serverType.order != $1.serverType.order ? $0.serverType.order < $1.serverType.order
        : $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }
  }
}

extension String {
  var trimmedTrailingSlash: String {
    var s = self
    while s.hasSuffix("/") { s.removeLast() }
    return s
  }
}
