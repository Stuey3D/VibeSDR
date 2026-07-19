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
/// manual custom-URL add. Only UberSDR is connectable today; other rows show a "soon" hint until
/// their protocol lands in the spike.
struct InstancePickerView: View {
  @EnvironmentObject var favs: FavStore
  @StateObject private var loc = LocationProvider()
  let onConnect: (SDRServer) -> Void

  private static let amber = Color(red: 0xff/255, green: 0xaa/255, blue: 0x00/255)
  private static let cream = Color(red: 0xf5/255, green: 0xe6/255, blue: 0xc8/255)
  private static let dim   = Color.white.opacity(0.45)

  @State private var openDir: String? = nil
  @State private var lists: [String: [SDRServer]] = [:]     // directoryId -> servers
  @State private var loading: Set<String> = []
  @State private var errored: Set<String> = []
  @State private var meta: [String: (dist: Double?, snr: Double?)] = [:]   // url -> live dist/snr
  @State private var showCustom = false
  @StateObject private var mdns = VibeMdns()
  @State private var pinFor: VibeAd? = nil
  @State private var pinEntry = ""

  var body: some View {
    List {
      discoveredSection      // your own local servers first — the fastest, highest-quality link
      favouritesSection
      directoriesSection
      customSection
    }
    .listStyle(.carousel)
    // PLAIN STRING, deliberately. The ViewBuilder form of navigationTitle lets you style the
    // text — but it also demotes it out of the LARGE left-aligned wordmark into the small
    // inline slot beside the clock. A coloured "Jr" is not worth losing the wordmark for.
    .navigationTitle("VibeSDR Jr")
    .task { await preloadForFavourites() }
    .onAppear { loc.request(); mdns.start() }
    .onDisappear { mdns.stop() }
    .sheet(isPresented: $showCustom) { CustomServerSheet { name, url, type in
      favs.addCustom(name: name, url: url, type: type)
    } }
    .sheet(item: $pinFor) { ad in vibePinSheet(ad) }
  }

  // ── Discovered VibeServers (mDNS `_vibesdr._tcp` on the LAN) ─────────────────────
  // Only shown once something's actually resolved. Cold auto-discovery is flaky on watchOS (the resolve
  // stalls until a real streaming connection wakes the stack), so in practice this fills in after your
  // first server connection of the session; the saved FAVOURITE is the reliable path.
  @ViewBuilder private var discoveredSection: some View {
    if !mdns.found.isEmpty {
      Section("ON YOUR NETWORK") {
        ForEach(mdns.found) { ad in
          Button {
            if ad.pinRequired { pinEntry = favs.savedPin(host: ad.host); pinFor = ad }
            else { connectVibe(ad, pin: "") }
          } label: {
            HStack(spacing: 8) {
              typeBadge(.vibeserver)
              VStack(alignment: .leading, spacing: 1) {
                Text(ad.name).font(.system(size: 15)).foregroundColor(Self.cream).lineLimit(1)
                Text(ad.host).font(.system(size: 9.5)).foregroundColor(Self.dim).lineLimit(1)
              }
              Spacer()
              if ad.pinRequired {
                Image(systemName: "lock.fill").font(.system(size: 11)).foregroundColor(Self.amber)
              }
            }
          }.buttonStyle(.plain)
        }
      }
    }
  }

  private func vibePinSheet(_ ad: VibeAd) -> some View {
    List {
      Section("PIN — \(ad.name)") {
        TextField("PIN", text: $pinEntry)
          .font(.system(size: 18, design: .rounded)).multilineTextAlignment(.center)
        Button {
          let p = pinEntry.trimmingCharacters(in: .whitespaces)
          pinFor = nil
          connectVibe(ad, pin: p)
        } label: {
          Text("Connect").font(.system(size: 15, weight: .semibold)).frame(maxWidth: .infinity)
        }.tint(Self.amber)
        // Save the PIN as a favourite so it auto-fills next time (matches the phone).
        Button {
          let p = pinEntry.trimmingCharacters(in: .whitespaces)
          favs.saveVibe(name: ad.name, host: ad.host, pin: p)
          pinFor = nil
          connectVibe(ad, pin: p)
        } label: {
          Text("Save & Connect").font(.system(size: 15, weight: .semibold)).frame(maxWidth: .infinity)
        }.tint(.green)
      }
    }
  }

  private func connectVibe(_ ad: VibeAd, pin: String) {
    favs.registerVisit("ws://\(ad.host)")
    onConnect(SDRServer(name: ad.name, url: "ws://\(ad.host)", host: ad.host, serverType: .vibeserver, pin: pin))
  }

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
            if loading.contains(dir.id) { ProgressView().scaleEffect(0.6) }
            else { Image(systemName: openDir == dir.id ? "chevron.up" : "chevron.down").foregroundColor(Self.dim) }
          }
        }.buttonStyle(.plain)

        if openDir == dir.id {
          if errored.contains(dir.id) {
            Text("Couldn't load — tap to retry").font(.system(size: 12)).foregroundColor(.orange)
              .onTapGesture { Task { await load(dir.id) } }
          }
          ForEach(sortedServers(lists[dir.id] ?? [])) { serverRow($0) }
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
    if lists[id] == nil { Task { await load(id) } }
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
