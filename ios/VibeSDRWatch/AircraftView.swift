import SwiftUI
import WatchKit

/// ADS-B: aircraft, not a waterfall.
///
/// 1090 MHz is a whole-profile mode — the receiver is dedicated to that one block and
/// decodes it. There is nothing to hunt for and nowhere to tune to: the only thing a
/// crown could do is drag you off the block and stop every aircraft decoding. (The
/// phone refuses to tune here now too; ADS-B had no guard at all until this.) So the
/// spectrum is a slab of noise, and the AIRCRAFT are the content — same reasoning as
/// DAB's services and FM-DX's station.
///
/// The crown scrolls the list. It does not select anything: unlike DAB there's nothing
/// to switch TO — you're already receiving all of them at once.
struct AircraftView: View {
  @EnvironmentObject var link: WatchLink
  @State private var showFavs = false

  private var planes: [WatchLink.Aircraft] {
    // Nearest first, then by signal for the ones that haven't sent a position yet.
    // Plenty of records carry altitude and speed with no lat/lon at all — those are
    // real aircraft, just not locatable, so they sort last rather than vanish.
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
      if planes.isEmpty { empty } else { list }
    }
    .background(Color.black.ignoresSafeArea())
    .sheet(isPresented: $showFavs) {
      FavouritesList(favs: link.favourites) { url in
        link.selectInstance(url)
        showFavs = false
      }
    }
    .onAppear { link.ping() }
  }

  private var header: some View {
    HStack(spacing: 5) {
      Image(systemName: "airplane")
        .font(.system(size: 11, weight: .semibold))
        .foregroundStyle(.cyan)
      Text("\(link.aircraft.count)")
        .font(.system(size: 13, weight: .bold, design: .rounded))
        .monospacedDigit()
        .foregroundStyle(.white)
      Text("aircraft")
        .font(.system(size: 10, weight: .medium))
        .foregroundStyle(.white.opacity(0.6))
      // A way out: ADS-B has no menu (nothing to tune), so without this the wrist was
      // stranded on whatever server the phone happened to be on.
      ServersButton(show: $showFavs)
      Spacer(minLength: 0)
      Color.clear.frame(width: 58, height: 1)   // the clock's territory
    }
    .padding(.horizontal, 12)
    .padding(.top, 10)
    .padding(.bottom, 4)
  }

  private var empty: some View {
    VStack(spacing: 6) {
      Image(systemName: "antenna.radiowaves.left.and.right")
        .font(.title3)
        .foregroundStyle(.white.opacity(0.5))
      Text("Listening on 1090 MHz")
        .font(.caption2)
        .foregroundStyle(.white.opacity(0.6))
        .multilineTextAlignment(.center)
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
  }

  private var list: some View {
    ScrollView {
      LazyVStack(spacing: 4) {
        ForEach(planes) { a in row(a) }
      }
      .padding(.horizontal, 8)
      .padding(.bottom, 8)
    }
  }

  private func row(_ a: WatchLink.Aircraft) -> some View {
    VStack(alignment: .leading, spacing: 2) {
      HStack(spacing: 5) {
        // The flag is the aircraft's REGISTRY, straight from the server's `ccode` —
        // no ICAO-range table needed. It is NOT where the flight departed: a Ryanair
        // 737 is Irish wherever it took off from. That's what makes it interesting —
        // it's how you spot the unusual visitor.
        if let f = flag(a.ccode) { Text(f).font(.system(size: 13)) }

        Text(title(a))
          .font(.system(size: 14, weight: .bold, design: .rounded))
          .foregroundStyle(.white)
          .lineLimit(1)

        Spacer(minLength: 0)

        if let d = a.distKm {
          Text("\(Int(d)) km")
            .font(.system(size: 11, weight: .semibold, design: .rounded))
            .monospacedDigit()
            .foregroundStyle(.orange)
        }
      }

      HStack(spacing: 6) {
        if let alt = a.altitude {
          HStack(spacing: 1) {
            // Climbing / descending / level — the arrow tells you at a glance what
            // the number is about to do, which a raw altitude cannot.
            if let v = a.vspeed, abs(v) >= 100 {
              Image(systemName: v > 0 ? "arrow.up" : "arrow.down")
                .font(.system(size: 7, weight: .bold))
                .foregroundStyle(v > 0 ? .green : .cyan)
            }
            Text(altText(alt))
              .font(.system(size: 10, weight: .medium, design: .rounded))
              .monospacedDigit()
              .foregroundStyle(.white.opacity(0.8))
          }
        }
        if let s = a.speed {
          Text("\(Int(s)) kt")
            .font(.system(size: 10, weight: .medium, design: .rounded))
            .monospacedDigit()
            .foregroundStyle(.white.opacity(0.65))
        }
        if let b = a.bearing {
          Text(compass(b))
            .font(.system(size: 10, weight: .semibold, design: .rounded))
            .foregroundStyle(.white.opacity(0.55))
        }
        Spacer(minLength: 0)
        if let sq = a.squawk, sq == "7500" || sq == "7600" || sq == "7700" {
          // The emergency squawks. Rare, and worth shouting about when they appear.
          Text(sq)
            .font(.system(size: 9, weight: .bold, design: .rounded))
            .foregroundStyle(.white)
            .padding(.horizontal, 4)
            .padding(.vertical, 1)
            .background(Capsule().fill(.red))
        }
      }
    }
    .padding(.horizontal, 8)
    .padding(.vertical, 6)
    .background(RoundedRectangle(cornerRadius: 8).fill(.white.opacity(0.08)))
  }

  /// Callsign if we have it, else the registration, else the bare ICAO address —
  /// which is always present, so a row can never be nameless.
  private func title(_ a: WatchLink.Aircraft) -> String {
    if let f = a.flight, !f.isEmpty { return f }
    if let r = a.reg, !r.isEmpty { return r }
    return a.icao.uppercased()
  }

  private func altText(_ ft: Double) -> String {
    // Flight levels above the transition altitude read as FL370 to anyone who cares;
    // below it, feet. Both are what you'd hear on the radio.
    ft >= 18_000 ? String(format: "FL%03d", Int(ft / 100))
                 : "\(Int(ft.rounded(.toNearestOrEven) / 100) * 100) ft"
  }

  private func compass(_ deg: Double) -> String {
    let pts = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]
    return pts[Int((deg + 22.5) / 45) % 8]
  }

  /// ISO country code -> flag emoji, by regional-indicator offset.
  private func flag(_ iso: String?) -> String? {
    guard let iso, iso.count == 2 else { return nil }
    let base: UInt32 = 127397
    var s = ""
    for u in iso.uppercased().unicodeScalars {
      guard let scalar = UnicodeScalar(base + u.value) else { return nil }
      s.unicodeScalars.append(scalar)
    }
    return s
  }
}
