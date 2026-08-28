import SwiftUI
import UIKit   // UIImage — the bundled server-type logos load by name (no asset catalog)
import CoreLocation
import WatchKit

/// One-shot location for distance sorting. When-in-use only; if the user declines, the picker
/// falls back to country grouping. Coarsened to ~1 km — we only need rough distance to sort.
@MainActor
final class LocationProvider: NSObject, ObservableObject, CLLocationManagerDelegate {
  @Published var coord: CLLocationCoordinate2D? = nil
  private let mgr = CLLocationManager()
  override init() { super.init(); mgr.delegate = self; mgr.desiredAccuracy = kCLLocationAccuracyKilometer }
  func request() {
    switch mgr.authorizationStatus {
    case .notDetermined: mgr.requestWhenInUseAuthorization()
    case .authorizedWhenInUse, .authorizedAlways: mgr.requestLocation()
    default: break
    }
  }
  nonisolated func locationManagerDidChangeAuthorization(_ m: CLLocationManager) {
    if m.authorizationStatus == .authorizedWhenInUse || m.authorizationStatus == .authorizedAlways { m.requestLocation() }
  }
  nonisolated func locationManager(_ m: CLLocationManager, didUpdateLocations locs: [CLLocation]) {
    guard let c = locs.last?.coordinate else { return }
    Task { @MainActor in self.coord = c }
  }
  nonisolated func locationManager(_ m: CLLocationManager, didFailWithError error: Error) {}
}

private func haversineKm(_ a: CLLocationCoordinate2D, _ blat: Double, _ blon: Double) -> Double {
  let R = 6371.0, toRad = { (d: Double) in d * .pi / 180 }
  let dLat = toRad(blat - a.latitude), dLon = toRad(blon - a.longitude)
  let h = sin(dLat/2)*sin(dLat/2) + cos(toRad(a.latitude))*cos(toRad(blat))*sin(dLon/2)*sin(dLon/2)
  return R * 2 * atan2(sqrt(h), sqrt(1-h))
}

/// The spike's instance picker — a wrist port of the phone's InstancePickerScreen.
///
/// Favourites at the top (six sort modes + drag-to-reorder in Manual), then the directories
/// (UberSDR / Receiverbook / KiwiSDR / FM-DX) each expandable into their server list, plus a
/// manual custom-URL add. EVERY server type is connectable from here — Buddy names a server and
/// the PHONE connects it, so there is no protocol for the watch to lack. (Jr, which opens the
/// receiver itself, is the one with real limits; its copy of this file keeps them.)
/// ★ NO LOCAL DISCOVERY IN BUDDY (Stuart: "nah not needed in buddy"). mDNS found VibeServers for
///   the WATCH to open; here the phone owns the connection, so a server the watch can see but the
///   phone cannot reach would be a dead row.
struct InstancePickerView: View {
  @EnvironmentObject var favs: FavStore
  @EnvironmentObject var link: WatchLink   // Buddy mirrors the PHONE's directory list — no local fetch
  @StateObject private var loc = LocationProvider()
  let onConnect: (SDRServer) -> Void
  /// Non-nil ONLY when there's a live session to go back to (opened from the menu while connected).
  /// Shows a "Close Server List" row at the top; nil on a cold boot where there's nowhere to return.
  /// ★ "Nowhere to return" now includes the START screen: the list can be opened from there with no
  ///   session at all, and that route must be closable too or it is a dead end (see VibeSDRWatchApp).
  var onClose: (() -> Void)? = nil

  private static let amber = Color(red: 0xff/255, green: 0xaa/255, blue: 0x00/255)
  private static let cream = Color(red: 0xf5/255, green: 0xe6/255, blue: 0xc8/255)
  private static let dim   = Color.white.opacity(0.45)

  @State private var openDir: String? = nil
  @State private var openGroups: Set<String> = []   // "dirId|ISO" of expanded country groups
  @State private var lists: [String: [SDRServer]] = [:]     // directoryId -> servers
  @State private var loading: Set<String> = []
  @State private var errored: Set<String> = []
  @State private var meta: [String: (dist: Double?, snr: Double?)] = [:]   // url -> live dist/snr
  @State private var showCustom = false

  var body: some View {
    List {
      if let onClose {
        Button(action: onClose) {
          HStack(spacing: 8) {
            Image(systemName: "xmark.circle.fill").font(.system(size: 17))
            Text("Close Server List").font(.system(size: 14, weight: .semibold))
          }.foregroundColor(.orange).frame(maxWidth: .infinity)
        }.buttonStyle(.plain)
      }
      favouritesSection
      directoriesSection
      customSection
    }
    .listStyle(.carousel)
    // PLAIN STRING, deliberately. The ViewBuilder form of navigationTitle lets you style the
    // text — but it also demotes it out of the LARGE left-aligned wordmark into the small
    // inline slot beside the clock. A coloured "Jr" is not worth losing the wordmark for.
    .navigationTitle("Servers")
    .onAppear { loc.request() }
    .sheet(isPresented: $showCustom) { CustomServerSheet { name, url, type in
      favs.addCustom(name: name, url: url, type: type)
    } }
  }

  // ── Discovered VibeServers (mDNS `_vibesdr._tcp` on the LAN) ─────────────────────
  // Only shown once something's actually resolved. Cold auto-discovery is flaky on watchOS (the resolve
  // stalls until a real streaming connection wakes the stack), so in practice this fills in after your
  // first server connection of the session; the saved FAVOURITE is the reliable path.



  // ── Favourites ────────────────────────────────────────────────────────────────
  @ViewBuilder private var favouritesSection: some View {
    let list = favs.sorted(meta: meta)
    Section {
      if list.isEmpty {
        Text("No favourites yet — tap ♥ on a server below.")
          .font(.system(size: 12)).foregroundColor(Self.dim)
      } else {
        ForEach(list) { f in favRow(f) }
          .onMove(perform: favs.sort == .manual ? { favs.move(from: $0, to: $1) } : nil)
      }
    } header: {
      HStack {
        Text("FAVOURITES").font(.system(size: 13, weight: .bold)).foregroundColor(Self.amber)
        Spacer()
        Button { favs.sort = favs.sort.next } label: {
          Text("⇅ \(favs.sort.label)").font(.system(size: 10, weight: .semibold)).foregroundColor(Self.amber)
        }.buttonStyle(.plain)
      }
    } footer: {
      if favs.sort == .manual {
        Text("≡ Tap and hold a server to drag it into order").font(.system(size: 10)).foregroundColor(Self.dim)
      }
    }
  }

  @ViewBuilder private func favRow(_ f: Favourite) -> some View {
    Button {
      if f.serverType == .vibeserver {
        // A saved VibeServer carries its own host + PIN — connect straight through (works out of house
        // over a reachable address, without needing mDNS).
        favs.registerVisit(f.url)
        onConnect(SDRServer(name: f.name, url: f.url, host: f.host, serverType: .vibeserver, pin: f.pin))
      } else {
        connect(url: f.url, name: f.name, type: f.serverType)
      }
    } label: {
      HStack(spacing: 8) {
        typeBadge(f.serverType)
        VStack(alignment: .leading, spacing: 1) {
          Text(f.name).font(.system(size: 15)).foregroundColor(Self.cream).lineLimit(1)
          Text(favSubtitle(f)).font(.system(size: 9.5)).foregroundColor(Self.dim).lineLimit(1)
        }
        Spacer()
        Button { favs.toggle(SDRServer(name: f.name, url: f.url, host: "", serverType: f.serverType)) } label: {
          Image(systemName: "heart.fill").font(.system(size: 13)).foregroundColor(.red)
        }.buttonStyle(.plain)
      }
    }.buttonStyle(.plain)
  }

  private func favSubtitle(_ f: Favourite) -> String {
    switch favs.sort {
    case .nearest:
      let d = meta[f.url.trimmedTrailingSlash.lowercased()]?.dist
      return d != nil ? "◍ \(Int(d!.rounded())) km" : "◍ distance unknown"
    case .snr:
      let s = meta[f.url.trimmedTrailingSlash.lowercased()]?.snr ?? f.bestSnr
      return s != nil ? "▲ SNR \(Int(s!.rounded())) dB" : "▽ no SNR data"
    case .type: return "▣ \(f.serverType.display)"
    default:    return f.visits > 0 ? "★ \(f.visits) visit\(f.visits == 1 ? "" : "s")" : f.url
    }
  }

  // ── Directories ───────────────────────────────────────────────────────────────
  @ViewBuilder private var directoriesSection: some View {
    Section("DIRECTORIES") {
      ForEach(Directories.all) { dir in
        Button { toggleDir(dir.id) } label: {
          HStack {
            VStack(alignment: .leading, spacing: 1) {
              Text(dir.name).font(.system(size: 15)).foregroundColor(Self.amber)
              Text(dir.desc).font(.system(size: 9.5)).foregroundColor(Self.dim).lineLimit(1)
            }
            Spacer()
            if openDir == dir.id && link.directories[dir.id] == nil { ProgressView().scaleEffect(0.6) }
            else { Image(systemName: openDir == dir.id ? "chevron.up" : "chevron.down").foregroundColor(Self.dim) }
          }
        }.buttonStyle(.plain)

        if openDir == dir.id {
          // MIRROR the phone: nil = still waiting for its reply, [] = it couldn't load, else the servers.
          if link.directories[dir.id] == nil {
            HStack(spacing: 6) { ProgressView().scaleEffect(0.6); Text("Loading…").font(.system(size: 12)).foregroundColor(Self.dim) }
          } else if (link.directories[dir.id] ?? []).isEmpty {
            // ★★ TWO DIFFERENT FAILURES, TWO DIFFERENT SENTENCES. A blocked directory host and a
            // phone that is not running produce the identical empty list, but "tap to retry" only
            // helps the first — for the second it sends someone round the same loop for ever. The
            // watch is a REMOTE: it cannot fetch anything itself, so when the phone is silent the
            // only action that works is on the phone.
            if link.directoryPhoneSilent.contains(dir.id) {
              VStack(alignment: .leading, spacing: 2) {
                Text("iPhone didn't answer").font(.system(size: 12)).foregroundColor(.orange)
                Text("Open VibeSDR on your iPhone, then tap to retry")
                  .font(.system(size: 10)).foregroundColor(Self.dim)
              }.onTapGesture { link.browse(dir.id) }
            } else {
              Text("Couldn't load — tap to retry").font(.system(size: 12)).foregroundColor(.orange)
                .onTapGesture { link.browse(dir.id) }
            }
          }
          let servers = sortedServers(link.directories[dir.id] ?? [])
          // Small directories stay flat; big ones (KiwiSDR) collapse into country groups so the
          // whole world isn't one endless scroll on the wrist.
          if servers.count > 12 {
            ForEach(countryGroups(servers, dirId: dir.id), id: \.key) { g in
              Button { toggleGroup(g.key) } label: {
                HStack(spacing: 6) {
                  Text(g.iso.isEmpty ? "🏳️" : CountryBBox.flag(g.iso)).font(.system(size: 15))
                  Text(g.iso.isEmpty ? "Other" : g.iso).font(.system(size: 13, weight: .semibold))
                    .foregroundColor(Self.cream)
                  Spacer()
                  Text("\(g.servers.count)").font(.system(size: 11)).foregroundColor(Self.dim)
                  Image(systemName: openGroups.contains(g.key) ? "chevron.up" : "chevron.down")
                    .font(.system(size: 11)).foregroundColor(Self.dim)
                }
              }.buttonStyle(.plain)
              if openGroups.contains(g.key) {
                ForEach(g.servers) { serverRow($0) }
              }
            }
          } else {
            ForEach(servers) { serverRow($0) }
          }
        }
      }
    }
  }

  @ViewBuilder private func serverRow(_ s: SDRServer) -> some View {
    Button { connect(url: s.url, name: s.name, type: s.serverType) } label: {
      HStack(spacing: 8) {
        typeBadge(s.serverType)
        VStack(alignment: .leading, spacing: 1) {
          Text(s.name).font(.system(size: 14)).foregroundColor(s.full ? Self.dim : Self.cream).lineLimit(1)
          Text(serverSubtitle(s)).font(.system(size: 9)).foregroundColor(Self.dim).lineLimit(1)
        }
        Spacer()
        Button { favs.toggle(s) } label: {
          Image(systemName: favs.isFav(s.url) ? "heart.fill" : "heart")
            .font(.system(size: 13)).foregroundColor(favs.isFav(s.url) ? .red : Self.dim)
        }.buttonStyle(.plain)
      }.opacity(s.full ? 0.5 : 1)
    }.buttonStyle(.plain).disabled(s.full)
  }

  /// Simplified wrist sort: by distance from the user when the directory reports it (UberSDR gives
  /// server-side distance); otherwise by country with the user's own country first, rest alphabetical.
  private func toggleGroup(_ key: String) {
    if openGroups.contains(key) { openGroups.remove(key) } else { openGroups.insert(key) }
  }

  /// Bucket an ALREADY-SORTED server list by country (code if the directory gave one, else derived
  /// from coordinates). Groups are ordered nearest-first (min distance to the user), then by size.
  private func countryGroups(_ servers: [SDRServer], dirId: String)
    -> [(key: String, iso: String, servers: [SDRServer])] {
    var buckets: [String: [SDRServer]] = [:]
    var order: [String] = []
    for s in servers {
      let iso = s.countryCode
        ?? { if let la = s.latitude, let lo = s.longitude { return CountryBBox.iso(lat: la, lon: lo) }; return nil }()
        ?? ""
      if buckets[iso] == nil { order.append(iso) }
      buckets[iso, default: []].append(s)
    }
    func nearest(_ list: [SDRServer]) -> Double {
      guard let c = loc.coord else { return .infinity }
      return list.compactMap { s in s.latitude.flatMap { la in s.longitude.map { haversineKm(c, la, $0) } } }.min() ?? .infinity
    }
    return order.map { (key: "\(dirId)|\($0)", iso: $0, servers: buckets[$0] ?? []) }
      .sorted { a, b in
        let da = nearest(a.servers), db = nearest(b.servers)
        if da != db { return da < db }
        return a.servers.count > b.servers.count
      }
  }

  private func sortedServers(_ list: [SDRServer]) -> [SDRServer] {
    // Preferred: real distance from the user's own location to each server's coords (works across
    // every directory that publishes lat/lon — UberSDR/Kiwi/Receiverbook/FMDX). The server-reported
    // `distance` was unreliable (IP-geolocated, wildly off), so we compute it ourselves.
    if let c = loc.coord {
      func km(_ s: SDRServer) -> Double {
        guard let la = s.latitude, let lo = s.longitude else { return .infinity }
        return haversineKm(c, la, lo)
      }
      return list.sorted { km($0) < km($1) }
    }
    // No location permission → country grouping, user's country first then alphabetical.
    let userCC = Locale.current.region?.identifier.uppercased()
    return list.sorted { a, b in
      let ac = a.countryCode ?? "ZZ", bc = b.countryCode ?? "ZZ"
      let aUser = (ac == userCC), bUser = (bc == userCC)
      if aUser != bUser { return aUser }                        // user's country first
      if ac != bc { return ac < bc }                            // then alphabetical by country
      return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
    }
  }

  private func serverSubtitle(_ s: SDRServer) -> String {
    var bits: [String] = []
    if let cc = s.countryCode { bits.append(cc) } else if !s.location.isEmpty { bits.append(s.location) }
    if let d = s.distance { bits.append("\(Int(d.rounded())) km") }
    if let sn = s.bestSnr { bits.append("SNR \(Int(sn.rounded()))") }
    // ★ Kept, and now never fires: `connectable` is true for every type Buddy forwards. If one is
    //   ever added that the phone cannot drive, this is where the row says so.
    if !s.serverType.connectable { bits.append("· soon") }
    return bits.joined(separator: " · ")
  }

  // ── Custom URL ────────────────────────────────────────────────────────────────
  @ViewBuilder private var customSection: some View {
    Section {
      Button { showCustom = true } label: {
        Label("Add custom server", systemImage: "plus.circle").font(.system(size: 14)).foregroundColor(Self.amber)
      }.buttonStyle(.plain)
    }
  }

  // ── Badge ─────────────────────────────────────────────────────────────────────
  // Same server-type logos as the phone picker (bundled PNGs in Logos/). SpyServer + RTL-TCP
  // reuse the rtl_tcp mark. Falls back to a monogram if an image is missing.
  private static let logoName: [ServerType: String] = [
    .ubersdr: "logo_ubersdr", .kiwi: "logo_kiwi", .owrx: "logo_owrx",
    .fmdx: "logo_fmdx", .spyserver: "rtltcp", .rtltcp: "rtltcp",
    // ★ OURS WAS THE ONE MISSING, and it fell back to a "V" monogram rather than failing visibly
    //   — so the entry that leads the list was the only one without its mark. Same root as the
    //   comment in SDRDirectory: this file was forked from Jr's before the VibeServer directory
    //   existed, and each thing it needs has had to be carried across one at a time.
    .vibeserver: "logo_vibeserver",
  ]
  @ViewBuilder private func typeBadge(_ t: ServerType) -> some View {
    if let name = Self.logoName[t], let img = UIImage(named: name) {
      Image(uiImage: img).resizable().scaledToFit().frame(width: 22, height: 22)
    } else {
      Text(String(t.display.prefix(1)))
        .font(.system(size: 11, weight: .bold)).foregroundColor(.black)
        .frame(width: 20, height: 20).background(Self.amber.opacity(0.85), in: Circle())
    }
  }

  // ── Actions ───────────────────────────────────────────────────────────────────
  private func connect(url: String, name: String, type: ServerType) {
    guard type.connectable else { return }   // other protocols land as adapters are added
    let host = URL(string: url)?.host ?? url.replacingOccurrences(of: "https://", with: "")
      .replacingOccurrences(of: "http://", with: "").trimmedTrailingSlash
    favs.registerVisit(url)
    onConnect(SDRServer(name: name, url: url, host: host, serverType: type))
  }

  private func toggleDir(_ id: String) {
    if openDir == id { openDir = nil; return }
    openDir = id
    if link.directories[id] == nil { link.browse(id) }   // ask the phone to fetch + send it
  }

  private func load(_ id: String) async {
    errored.remove(id); loading.insert(id)
    defer { loading.remove(id) }
    do {
      let servers = try await Directories.fetch(id)
      lists[id] = servers
      ingestMeta(servers)
      favs.mergeMeta(servers)
    } catch { errored.insert(id) }
  }

  /// Preload every directory once so Nearest/SNR on favourites have data without opening each.
  private func preloadForFavourites() async {
    await withTaskGroup(of: [SDRServer].self) { group in
      for dir in Directories.all { group.addTask { (try? await Directories.fetch(dir.id)) ?? [] } }
      for await servers in group { ingestMeta(servers); favs.mergeMeta(servers) }
    }
  }

  private func ingestMeta(_ servers: [SDRServer]) {
    for s in servers {
      let k = s.url.trimmedTrailingSlash.lowercased()
      let cur = meta[k]
      meta[k] = (dist: s.distance ?? cur?.dist, snr: s.bestSnr ?? cur?.snr)
    }
  }
}

// ── Custom-server sheet ────────────────────────────────────────────────────────
struct CustomServerSheet: View {
  @Environment(\.dismiss) private var dismiss
  let onAdd: (_ name: String, _ url: String, _ type: ServerType) -> Void
  @State private var url = ""
  @State private var name = ""
  @State private var auto = true
  @State private var type: ServerType = .ubersdr
  @State private var detecting = false
  @State private var detectMsg = ""

  var body: some View {
    List {
      Section("ADDRESS") {
        TextField("sdr.example.com", text: $url).font(.system(size: 14)).autocorrectionDisabled()
        TextField("Name (optional)", text: $name).font(.system(size: 14))
      }
      Section("TYPE") {
        Toggle("Auto-detect", isOn: $auto).font(.system(size: 14))
        if !auto {
          Picker("Type", selection: $type) {
            ForEach(ServerType.allCases, id: \.self) { Text($0.display).tag($0) }
          }
        }
        if detecting { HStack(spacing: 6) { ProgressView().scaleEffect(0.7); Text("Detecting…").font(.system(size: 12)) } }
        else if !detectMsg.isEmpty { Text(detectMsg).font(.system(size: 11)).foregroundColor(.orange) }
      }
      Section {
        Button(auto ? "Detect & save" : "Save favourite") { save() }
          .disabled(url.trimmingCharacters(in: .whitespaces).isEmpty || detecting)
      }
    }
    .navigationTitle("Custom server")
  }

  private func save() {
    let clean = url.trimmingCharacters(in: .whitespaces)
    guard !clean.isEmpty else { return }
    if !auto {
      let full = clean.contains("://") ? clean : "https://\(clean)"
      onAdd(name.trimmingCharacters(in: .whitespaces), full.trimmedTrailingSlash, type); dismiss(); return
    }
    detecting = true; detectMsg = ""
    Task {
      // Bare host → try http first (self-hosted SDRs on a port are usually plain HTTP), then https.
      let candidates: [String] = clean.contains("://") ? [clean] : ["http://\(clean)", "https://\(clean)"]
      for cand in candidates {
        if let t = await detectServerType(cand) {
          await MainActor.run { onAdd(name.trimmingCharacters(in: .whitespaces), cand.trimmedTrailingSlash, t); dismiss() }
          return
        }
      }
      await MainActor.run {
        detecting = false; auto = false
        detectMsg = "Couldn't reach the server — pick the type below and save."
      }
    }
  }
}
