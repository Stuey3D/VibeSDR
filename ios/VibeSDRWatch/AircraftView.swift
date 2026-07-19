import SwiftUI
import WatchKit
import Combine
import MapKit

/// ADS-B: aircraft, not a waterfall — wired to SpikeLink, with a MapKit map toggle. 1090 MHz is a
/// whole-profile mode: the receiver decodes every aircraft at once, nothing to tune. The crown scrolls the
/// list; the map shows planes + the receiver's home marker. Chrome (battery / connection / lock) matches
/// the DAB screen; profiles/servers live in the hold-menu (long-press the header — works over the map too).
struct AircraftView: View {
  @EnvironmentObject var link: SpikeLink
  @State private var showMap = false
  @State private var showMenu = false
  @State private var showChat = false
  @State private var locked = false
  @AppStorage("seenAdsbTutorial") private var seenAdsbTut = false
  @State private var showAdsbTut = false

  private var planes: [Aircraft] {
    link.aircraft.sorted { a, b in
      switch (a.distKm, b.distKm) {
      case let (x?, y?): return x < y
      case (_?, nil):    return true
      case (nil, _?):    return false
      default:           return (a.rssi ?? -99) > (b.rssi ?? -99)
      }
    }
  }

  var body: some View {
    VStack(spacing: 0) {
      header
      if showMap { mapView }
      else if planes.isEmpty { empty } else { list }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    .background(Color.black.ignoresSafeArea())
    .ignoresSafeArea(edges: .top)   // reclaim the tall reserved top strip so content sits under the status band
    .onReceive(Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()) { _ in
      link.driverTick(now: ProcessInfo.processInfo.systemUptime)
    }
    .navigationDestination(isPresented: $showMenu) { ControlMenu { _ in }.environmentObject(link) }
    .sheet(isPresented: $showChat) { NavigationStack { ChatSheet().environmentObject(link) } }
    .onAppear { if !seenAdsbTut { DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { showAdsbTut = true } } }
    .sheet(isPresented: $showAdsbTut) {
      TutorialSheet(title: "ADS-B aircraft", tips: adsbTutorialTips()) { seenAdsbTut = true; showAdsbTut = false }
    }
    // Chrome DOUBLE-STACKED in the clock's band, top-LEFT — keeps it narrow so the system clock + focus
    // icons keep the right corner (the FM-DX pattern). ignoresSafeArea puts it up in that reserved strip.
    .overlay(alignment: .topLeading) { chrome }
  }

  // PASSIVE status icons only — safe to sit up in the clock's band (which doesn't take touches).
  private var chrome: some View {
    HStack(spacing: 6) {
      BatteryPill(level: link.battery)
      ConnGlyph(transport: link.transport).font(.system(size: 11))
      QualityGlyph(link: link)
    }
    .padding(.leading, 28).padding(.top, 3)   // clear the rounded top-left corner
    .ignoresSafeArea(edges: .top)
  }

  // MARK: - Header — a non-interactive COUNT label (rides high, under the status band) then the button
  //         row (Lock · Map · Menu) in the tappable area.

  private var header: some View {
    VStack(alignment: .leading, spacing: 5) {
      HStack(spacing: 6) {
        Image(systemName: "airplane").font(.system(size: 12, weight: .semibold)).foregroundStyle(.cyan)
        Text("\(link.aircraft.count)").font(.system(size: 15, weight: .bold, design: .rounded)).monospacedDigit().foregroundStyle(.white)
        Text("aircraft").font(.system(size: 11)).foregroundStyle(.white.opacity(0.6))
        Spacer(minLength: 0)
      }
      // Lock · Map · Menu · Chat. SPACERS, not a fixed gap: a hardcoded 24pt was fine for three
      // buttons on a 49mm and does not fit four on a 41mm. Even distribution adapts to any width.
      HStack(spacing: 0) {
        LockButton(locked: $locked, size: 18)
        Spacer(minLength: 2)
        Button { if !locked { showMap.toggle() } } label: {
          Image(systemName: showMap ? "list.bullet" : "map.fill").font(.system(size: 18, weight: .semibold))
            .foregroundStyle(locked ? .white.opacity(0.3) : .cyan)
            .padding(4).contentShape(Rectangle())
        }.buttonStyle(.plain).disabled(locked)
        Spacer(minLength: 2)
        Button { if !locked { showMenu = true } } label: {
          Image(systemName: "line.3.horizontal").font(.system(size: 18, weight: .semibold))
            .foregroundStyle(locked ? .white.opacity(0.3) : .white)
            .padding(4).contentShape(Rectangle())
        }.buttonStyle(.plain).disabled(locked)
        // Chat was missing from ADS-B and DAB entirely — the same OWRX server, the same room of
        // listeners, and no way to talk to them from these screens. The glyph carries the listener
        // COUNT as well, so one control answers "who else is here" and "say something".
        if link.supportsChat {
          Spacer(minLength: 2)
          ChatGlyph(clients: link.clients, activity: link.chatActivity) {
            if !locked { showChat = true }
          }
        } else {
          Spacer(minLength: 0)
        }
      }
    }
    .padding(.horizontal, 6).padding(.top, 40).padding(.bottom, 4)   // clears the status band (top ignored)
  }

  /// Altitude the way you would HEAR it: flight levels above the transition altitude, feet below.
  /// Ported 1:1 from the phone's `AircraftPanel.altText` so the wrist and the phone never disagree
  /// about the same aircraft.
  ///
  /// It also fixes a wrist-specific problem: the raw figure ("11,375ft") wrapped MID-NUMBER on a
  /// 41mm as "11,37" / "5ft", which reads as two different numbers. Rounding to the nearest 100
  /// drops the noise digits that caused it, and FL is shorter still.
  static func altText(_ ft: Double) -> String {
    if ft >= 18_000 {
      return "FL" + String(format: "%03d", Int((ft / 100).rounded()))
    }
    let r = Int((ft / 100).rounded()) * 100
    return (r.formatted(.number.grouping(.automatic))) + " ft"
  }

  private var empty: some View {
    VStack(spacing: 6) {
      Spacer()
      Image(systemName: "airplane.circle").font(.title2).foregroundStyle(.white.opacity(0.3))
      Text("Listening for aircraft…").font(.system(size: 12)).foregroundStyle(.white.opacity(0.5))
      Text(link.backendStatus).font(.system(size: 9, design: .monospaced)).foregroundStyle(.orange.opacity(0.7))
        .multilineTextAlignment(.center).padding(.horizontal, 6)
      Spacer()
    }.frame(maxWidth: .infinity, maxHeight: .infinity)
  }

  private var list: some View {
    ScrollView {
      LazyVStack(spacing: 4) {
        ForEach(planes) { p in row(p) }
      }
      .padding(.horizontal, 8).padding(.bottom, 8)
    }
  }

  private func row(_ p: Aircraft) -> some View {
    HStack(spacing: 8) {
      VStack(alignment: .leading, spacing: 1) {
        Text(p.flight?.isEmpty == false ? p.flight! : p.icao)
          .font(.system(size: 14, weight: .bold, design: .rounded)).foregroundStyle(.white).lineLimit(1)
        HStack(spacing: 6) {
          if let a = p.altitude {
            Text(Self.altText(a)).font(.system(size: 10)).foregroundStyle(.white.opacity(0.6))
              .lineLimit(1).fixedSize(horizontal: true, vertical: false)
          }
          if let s = p.speed { Text("\(Int(s))kt").font(.system(size: 10)).foregroundStyle(.white.opacity(0.6)) }
          if let c = p.ccode { Text("\(Self.flag(c)) \(c)").font(.system(size: 10)).foregroundStyle(.cyan.opacity(0.85)) }
        }
      }
      Spacer(minLength: 0)
      VStack(alignment: .trailing, spacing: 1) {
        if let d = p.distKm { Text("\(Int(d))km").font(.system(size: 12, weight: .semibold)).foregroundStyle(.white) }
        if let r = p.rssi { Text("\(Int(r))dB").font(.system(size: 9)).foregroundStyle(.white.opacity(0.5)) }
      }
    }
    .padding(.horizontal, 8).padding(.vertical, 6)
    .background(RoundedRectangle(cornerRadius: 8).fill(.white.opacity(0.08)))
  }

  // Cap the map to the 60 NEAREST located aircraft — rendering 100+ MapKit annotations on the watch
  // hangs the UI. The list is lazy so it can show them all; the map is the expensive one.
  /// ISO country code → flag emoji (regional indicator letters), like the companion/phone lists.
  static func flag(_ code: String) -> String {
    let c = code.uppercased()
    guard c.count == 2, c.unicodeScalars.allSatisfy({ $0.value >= 65 && $0.value <= 90 }) else { return "🏳️" }
    return String(c.unicodeScalars.compactMap { Unicode.Scalar(0x1F1E6 + $0.value - 65).map(Character.init) })
  }

  private var located: [Aircraft] {
    link.aircraft.filter { $0.lat != nil && $0.lon != nil }
      .sorted { ($0.distKm ?? 1e9) < ($1.distKm ?? 1e9) }
      .prefix(60).map { $0 }
  }

  private var mapView: some View {
    Map(initialPosition: .automatic) {
      // The receiver's own site — a home marker so the map has a fixed reference the planes move around.
      if let rlat = link.receiverLat, let rlon = link.receiverLon {
        Annotation("RX", coordinate: CLLocationCoordinate2D(latitude: rlat, longitude: rlon)) {
          Image(systemName: "house.fill").font(.system(size: 11, weight: .bold)).foregroundStyle(.orange)
        }
      }
      ForEach(located) { p in
        Annotation(p.flight ?? p.icao, coordinate: CLLocationCoordinate2D(latitude: p.lat!, longitude: p.lon!)) {
          Image(systemName: "airplane")
            .font(.system(size: 12, weight: .bold)).foregroundStyle(.cyan)
            .rotationEffect(.degrees((p.course ?? 0) - 90))   // point along track (SF plane points right)
        }
      }
    }
    .mapStyle(.standard(elevation: .flat))
  }
}

/// The connection-method glyph (iPhone relay / wifi / cellular), shared by the DAB + ADS-B headers.
struct ConnGlyph: View {
  let transport: Transport
  var body: some View {
    switch transport {
    case .iphone:   Image(systemName: "iphone").foregroundStyle(.white.opacity(0.8))
    case .wifi:     Image(systemName: "wifi").foregroundStyle(.white.opacity(0.8))
    case .cellular: Image(systemName: "antenna.radiowaves.left.and.right").foregroundStyle(.white.opacity(0.8))
    case .none:     Image(systemName: "xmark").foregroundStyle(.red)
    }
  }
}

/// The coloured server-link quality glyph (the instance triangle, green/yellow/red), next to the
/// connection method. Mirrors the main screen's quality indicator, derived from the link's health.
struct QualityGlyph: View {
  @ObservedObject var link: SpikeLink
  var body: some View {
    if link.transport == .none {
      Image(systemName: "xmark").foregroundStyle(.red)
    } else {
      let tint: Color = (link.why == "reconnecting" || link.serverLink <= 1) ? .yellow : .green
      InstanceNodes()
        .stroke(tint, style: StrokeStyle(lineWidth: 1.6, lineCap: .round, lineJoin: .round))
        .frame(width: 15, height: 15)
    }
  }
}

/// A padlock toggle for the list screens — tapping toggles; while locked, taps/long-press are ignored
/// (the padlock stays live so you can unlock). Crown + pinch still work, as in Water Lock.
struct LockButton: View {
  @Binding var locked: Bool
  var size: CGFloat = 12
  var body: some View {
    Button {
      locked.toggle()
      WKInterfaceDevice.current().play(locked ? .stop : .click)
    } label: {
      Image(systemName: locked ? "lock.fill" : "lock.open")
        .font(.system(size: size, weight: .semibold)).foregroundStyle(locked ? .orange : .white.opacity(0.8))
        .padding(4).contentShape(Rectangle())   // bigger hit area than the glyph
    }.buttonStyle(.plain)
  }
}
