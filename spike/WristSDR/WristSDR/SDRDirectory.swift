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
    // ★ VibeServer serves the UberSDR-style spectrum protocol BUT needs the VibeServer client mode
    // (isVibe: /ws/audio + PIN + Opus/ADPCM). Returning .ubersdr made Jr run the UberSDR /ws register
    // handshake, which VibeServer never answers — the "stuck on registering" bug. It IS a VibeServer.
    if body.contains("vibeserver") { return .vibeserver }
    if body.contains("ubersdr") { return .ubersdr }
    if body.range(of: "kiwisdr|kiwi sdr|/kiwi/|kiwi_util|owrx_ws_open", options: .regularExpression) != nil { return .kiwi }
    if body.contains("openwebrx") { return .owrx }
    if body.range(of: "fm-dx|fmdx", options: .regularExpression) != nil { return .fmdx }
    return .ubersdr   // reachable but unidentifiable → assume UberSDR
  } catch { return nil }
}

enum Directories {
  static let all: [DirectoryMeta] = [
    .init(id: "ubersdr",      name: "UberSDR",      desc: "Official UberSDR servers"),
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
        // ★ NORMALISED TO 'IN USE', because UberSDR reports the opposite of Kiwi: its
        // `available_clients` is slots FREE (0 = full), while Kiwi's `users` is slots TAKEN.
        // Left raw, one directory's "8/8" would mean empty and the other's would mean full.
        users: max(0, ((it["max_clients"] as? Int) ?? 0) - ((it["available_clients"] as? Int) ?? 0)),
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
      let kiwiUsers = Int((r["users"] as? String) ?? "") ?? 0
      let kiwiMax   = Int((r["users_max"] as? String) ?? "") ?? 0
      return SDRServer(
        name: (r["name"] as? String) ?? "KiwiSDR",
        url: u,
        host: URL(string: u)?.host ?? u,
        serverType: .kiwi,
        location: (r["loc"] as? String) ?? "",
        latitude: parseCoord(r["gps"], 0),
        longitude: parseCoord(r["gps"], 1),
        bestSnr: snr,
        users: kiwiUsers,
        maxUsers: kiwiMax,
        // ★ The two directories COUNT THE OPPOSITE WAY. UberSDR reports FREE slots
        // (available_clients: 0 = full); KiwiSDR reports slots IN USE (users 8 of users_max 8 =
        // full). Getting this backwards greys out every empty receiver and offers every full one.
        full: kiwiMax > 0 && kiwiUsers >= kiwiMax
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

// ── Offline coordinate → ISO country, for grouping the picker (Kiwi/Receiverbook carry no code).
//    Bounding boxes only (Natural Earth 110m bbox, public domain) — country-level, fine for grouping.
enum CountryBBox {
  static let boxes: [String: [Double]] = [   // ISO: [minLon, minLat, maxLon, maxLat]
    "FJ": [-180, -18.29, 180, -16.02],
    "TZ": [29.34, -11.72, 40.32, -0.95],
    "EH": [-17.06, 21, -8.67, 27.66],
    "CA": [-141, 41.68, -52.65, 83.23],
    "US": [-171.79, 18.92, -66.96, 71.36],
    "KZ": [46.47, 40.66, 87.36, 55.39],
    "UZ": [55.93, 37.14, 73.06, 45.59],
    "PG": [141, -10.65, 156.02, -2.5],
    "ID": [95.29, -10.36, 141.03, 5.48],
    "AR": [-73.42, -55.25, -53.63, -21.83],
    "CL": [-75.64, -55.61, -66.96, -17.58],
    "CD": [12.18, -13.26, 31.17, 5.26],
    "SO": [40.98, -1.68, 51.13, 12.02],
    "KE": [33.89, -4.68, 41.86, 5.51],
    "SD": [21.94, 8.23, 38.41, 22],
    "TD": [13.54, 7.42, 23.89, 23.41],
    "HT": [-74.46, 18.03, -71.62, 19.92],
    "DO": [-71.95, 17.6, -68.32, 19.88],
    "RU": [-180, 41.15, 180, 81.25],
    "BS": [-78.98, 23.71, -77, 27.04],
    "FK": [-61.2, -52.3, -57.75, -51.1],
    "NO": [4.99, 58.08, 31.29, 80.66],
    "GL": [-73.3, 60.04, -12.21, 83.65],
    "TF": [68.72, -49.77, 70.56, -48.62],
    "TL": [124.97, -9.39, 127.34, -8.27],
    "ZA": [16.34, -34.82, 32.83, -22.09],
    "LS": [27, -30.65, 29.33, -28.65],
    "MX": [-117.13, 14.54, -86.81, 32.72],
    "UY": [-58.43, -34.95, -53.21, -30.11],
    "BR": [-73.99, -33.77, -34.73, 5.24],
    "BO": [-69.59, -22.87, -57.5, -9.76],
    "PE": [-81.41, -18.35, -68.67, -0.06],
    "CO": [-78.99, -4.3, -66.88, 12.44],
    "PA": [-82.97, 7.22, -77.24, 9.61],
    "CR": [-85.94, 8.23, -82.55, 11.22],
    "NI": [-87.67, 10.73, -83.15, 15.02],
    "HN": [-89.35, 12.98, -83.15, 16.01],
    "SV": [-90.1, 13.15, -87.72, 14.42],
    "GT": [-92.23, 13.74, -88.23, 17.82],
    "BZ": [-89.23, 15.89, -88.11, 18.5],
    "VE": [-73.3, 0.72, -59.76, 12.16],
    "GY": [-61.41, 1.27, -56.54, 8.37],
    "SR": [-58.04, 1.82, -53.96, 6.03],
    "FR": [-54.52, 2.05, 9.56, 51.15],
    "EC": [-80.97, -4.96, -75.23, 1.38],
    "PR": [-67.24, 17.95, -65.59, 18.52],
    "JM": [-78.34, 17.7, -76.2, 18.52],
    "CU": [-84.97, 19.86, -74.18, 23.19],
    "ZW": [25.26, -22.27, 32.85, -15.51],
    "BW": [19.9, -26.83, 29.43, -17.66],
    "NA": [11.73, -29.05, 25.08, -16.94],
    "SN": [-17.63, 12.33, -11.47, 16.6],
    "ML": [-12.17, 10.1, 4.27, 24.97],
    "MR": [-17.06, 14.62, -4.92, 27.4],
    "BJ": [0.77, 6.14, 3.8, 12.24],
    "NE": [0.3, 11.66, 15.9, 23.47],
    "NG": [2.69, 4.24, 14.58, 13.87],
    "CM": [8.49, 1.73, 16.01, 12.86],
    "TG": [-0.05, 5.93, 1.87, 11.02],
    "GH": [-3.24, 4.71, 1.06, 11.1],
    "CI": [-8.6, 4.34, -2.56, 10.52],
    "GN": [-15.13, 7.31, -7.83, 12.59],
    "GW": [-16.68, 11.04, -13.7, 12.63],
    "LR": [-11.44, 4.36, -7.54, 8.54],
    "SL": [-13.25, 6.79, -10.23, 10.05],
    "BF": [-5.47, 9.61, 2.18, 15.12],
    "CF": [14.46, 2.27, 27.37, 11.14],
    "CG": [11.09, -5.04, 18.45, 3.73],
    "GA": [8.8, -3.98, 14.43, 2.33],
    "GQ": [9.31, 1.01, 11.29, 2.28],
    "ZM": [21.89, -17.96, 33.49, -8.24],
    "MW": [32.69, -16.8, 35.77, -9.23],
    "MZ": [30.18, -26.74, 40.78, -10.32],
    "SZ": [30.68, -27.29, 32.07, -25.66],
    "AO": [11.64, -17.93, 24.08, -4.44],
    "BI": [29.02, -4.5, 30.75, -2.35],
    "IL": [34.27, 29.5, 35.84, 33.28],
    "LB": [35.13, 33.09, 36.61, 34.64],
    "MG": [43.25, -25.6, 50.48, -12.04],
    "PS": [34.93, 31.35, 35.55, 32.53],
    "GM": [-16.84, 13.13, -13.84, 13.88],
    "TN": [7.52, 30.31, 11.49, 37.35],
    "DZ": [-8.68, 19.06, 12, 37.12],
    "JO": [34.92, 29.2, 39.2, 33.38],
    "AE": [51.58, 22.5, 56.4, 26.06],
    "QA": [50.74, 24.56, 51.61, 26.11],
    "KW": [46.57, 28.53, 48.42, 30.06],
    "IQ": [38.79, 29.1, 48.57, 37.39],
    "OM": [52, 16.65, 59.81, 26.4],
    "VU": [166.63, -16.6, 167.84, -14.63],
    "KH": [102.35, 10.49, 107.61, 14.57],
    "TH": [97.38, 5.69, 105.59, 20.42],
    "LA": [100.12, 13.88, 107.56, 22.46],
    "MM": [92.3, 9.93, 101.18, 28.34],
    "VN": [102.17, 8.6, 109.34, 23.35],
    "KP": [124.27, 37.67, 130.78, 42.99],
    "KR": [126.12, 34.39, 129.47, 38.61],
    "MN": [87.75, 41.6, 119.77, 52.05],
    "IN": [68.18, 7.97, 97.4, 35.49],
    "BD": [88.08, 20.67, 92.67, 26.45],
    "BT": [88.81, 26.72, 92.1, 28.3],
    "NP": [80.09, 26.4, 88.17, 30.42],
    "PK": [60.87, 23.69, 77.84, 37.13],
    "AF": [60.53, 29.32, 75.16, 38.49],
    "TJ": [67.44, 36.74, 74.98, 40.96],
    "KG": [69.46, 39.28, 80.26, 43.3],
    "TM": [52.5, 35.27, 66.55, 42.75],
    "IR": [44.11, 25.08, 63.32, 39.71],
    "SY": [35.7, 32.31, 42.35, 37.23],
    "AM": [43.58, 38.74, 46.51, 41.25],
    "SE": [11.03, 55.36, 23.9, 69.11],
    "BY": [23.2, 51.32, 32.69, 56.17],
    "UA": [22.09, 45.29, 40.08, 52.34],
    "PL": [14.07, 49.03, 24.03, 54.85],
    "AT": [9.48, 46.43, 16.98, 49.04],
    "HU": [16.2, 45.76, 22.71, 48.62],
    "MD": [26.62, 45.49, 30.02, 48.47],
    "RO": [20.22, 43.69, 29.63, 48.22],
    "LT": [21.06, 53.91, 26.59, 56.37],
    "LV": [21.06, 55.62, 28.18, 57.97],
    "EE": [23.34, 57.47, 28.13, 59.61],
    "DE": [5.99, 47.3, 15.02, 54.98],
    "BG": [22.38, 41.23, 28.56, 44.23],
    "GR": [20.15, 34.92, 26.6, 41.83],
    "TR": [26.04, 35.82, 44.79, 42.14],
    "AL": [19.3, 39.62, 21.02, 42.69],
    "HR": [13.66, 42.48, 19.39, 46.5],
    "CH": [6.02, 45.78, 10.44, 47.83],
    "LU": [5.67, 49.44, 6.24, 50.13],
    "BE": [2.51, 49.53, 6.16, 51.48],
    "NL": [3.31, 50.8, 7.09, 53.51],
    "PT": [-9.53, 36.84, -6.39, 42.28],
    "ES": [-9.39, 35.95, 3.04, 43.75],
    "IE": [-9.98, 51.67, -6.03, 55.13],
    "NC": [164.03, -22.4, 167.12, -20.11],
    "SB": [156.49, -10.83, 162.4, -6.6],
    "NZ": [166.51, -46.64, 178.52, -34.45],
    "AU": [113.34, -43.63, 153.57, -10.67],
    "LK": [79.7, 5.97, 81.79, 9.82],
    "CN": [73.68, 18.2, 135.03, 53.46],
    "TW": [120.11, 21.97, 121.95, 25.3],
    "IT": [6.75, 36.62, 18.48, 47.12],
    "DK": [8.09, 54.8, 12.69, 57.73],
    "GB": [-7.57, 49.96, 1.68, 58.64],
    "IS": [-24.33, 63.5, -13.61, 66.53],
    "AZ": [44.79, 38.27, 50.39, 41.86],
    "GE": [39.96, 41.06, 46.64, 43.55],
    "PH": [117.17, 5.58, 126.54, 18.51],
    "MY": [100.09, 0.77, 119.18, 6.93],
    "BN": [114.2, 4.01, 115.45, 5.45],
    "SI": [13.7, 45.45, 16.56, 46.85],
    "FI": [20.65, 59.85, 31.52, 70.16],
    "SK": [16.88, 47.76, 22.56, 49.57],
    "CZ": [12.24, 48.56, 18.85, 51.12],
    "ER": [36.32, 12.46, 43.08, 18],
    "JP": [129.41, 31.03, 145.54, 45.55],
    "PY": [-62.69, -27.55, -54.29, -19.34],
    "YE": [42.6, 12.59, 53.11, 19],
    "SA": [34.63, 16.35, 55.67, 32.16],
    "AQ": [-180, -90, 180, -63.27],
    "CY": [32.26, 34.57, 34, 35.17],
    "MA": [-17.02, 21.42, -1.12, 35.76],
    "EG": [24.7, 22, 36.87, 31.59],
    "LY": [9.32, 19.58, 25.16, 33.14],
    "ET": [32.95, 3.42, 47.79, 14.96],
    "DJ": [41.66, 10.93, 43.32, 12.7],
    "UG": [29.58, -1.44, 35.04, 4.25],
    "RW": [29.02, -2.92, 30.82, -1.13],
    "BA": [15.75, 42.65, 19.6, 45.23],
    "MK": [20.46, 40.84, 22.95, 42.32],
    "RS": [18.83, 42.25, 22.99, 46.17],
    "ME": [18.45, 41.88, 20.34, 43.52],
    "XK": [20.07, 41.85, 21.78, 43.27],
    "TT": [-61.95, 10, -60.89, 10.89],
    "SS": [23.89, 3.51, 35.3, 12.25],
  ]
  static func iso(lat: Double, lon: Double) -> String? {
    var best: String? = nil; var bestArea = Double.greatestFiniteMagnitude
    for (code, b) in boxes {
      if lon < b[0] || lon > b[2] || lat < b[1] || lat > b[3] { continue }
      let area = (b[2] - b[0]) * (b[3] - b[1])
      if area < bestArea { bestArea = area; best = code }
    }
    return best
  }
  static func flag(_ iso: String) -> String {
    let up = iso.uppercased(); guard up.count == 2 else { return "" }
    var s = ""
    for u in up.unicodeScalars { if let f = Unicode.Scalar(127397 + u.value) { s.unicodeScalars.append(f) } }
    return s
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Bookmarks — LOCAL to the watch. A saved spot on the dial: name + frequency + demod.
// No server is attached (the phone's `UserBookmark` binds a scope; Jr deliberately does
// not — a bookmark is just a frequency you can recall on whatever server reaches it).
// Field names track the phone's UserBookmark so a future WCSession/iCloud sync is a
// straight map. Persisted exactly like FavStore (UserDefaults + JSON).
// ─────────────────────────────────────────────────────────────────────────────

struct Bookmark: Codable, Identifiable, Hashable {
  var id = UUID()
  var name: String
  var frequency: Double   // Hz
  var mode: String        // lowercase demod: am/sam/usb/lsb/cw/nfm/wfm…

  private enum CodingKeys: String, CodingKey { case id, name, frequency, mode }
}

@MainActor
final class BookmarkStore: ObservableObject {
  @Published private(set) var bookmarks: [Bookmark] = []

  private static let key = "vibe.spike.bookmarks"

  init() {
    if let raw = UserDefaults.standard.data(forKey: Self.key),
       let b = try? JSONDecoder().decode([Bookmark].self, from: raw) { bookmarks = b }
  }

  private func persist() {
    if let d = try? JSONEncoder().encode(bookmarks) { UserDefaults.standard.set(d, forKey: Self.key) }
  }

  func add(name: String, frequency: Double, mode: String) {
    // De-dupe on frequency+mode so tapping Save twice on the same spot doesn't stack.
    if let i = bookmarks.firstIndex(where: { $0.frequency == frequency && $0.mode == mode }) {
      bookmarks[i].name = name
    } else {
      bookmarks.append(Bookmark(name: name, frequency: frequency, mode: mode))
    }
    bookmarks.sort { $0.frequency < $1.frequency }
    persist()
  }

  func remove(_ b: Bookmark) {
    bookmarks.removeAll { $0.id == b.id }
    persist()
  }

  func remove(at offsets: IndexSet, from shown: [Bookmark]) {
    let ids = offsets.map { shown[$0].id }
    bookmarks.removeAll { ids.contains($0.id) }
    persist()
  }

  func removeAll() {
    bookmarks.removeAll()
    persist()
  }
}
