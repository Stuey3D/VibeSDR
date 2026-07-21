import Foundation
import SwiftUI

// The data types Jr's SCREENS are written against, lifted from the direct-connection clients they
// live inside over in the spike.
//
// ★ Buddy has no clients — the phone owns every connection — but the whole point of the split is
//   that the two apps look IDENTICAL and run differently (Stuart). The views come over verbatim,
//   so the types they name have to exist here too. WatchLinkCompat maps the phone's own wire
//   structs onto these, which is the only translation layer in the app.
//
// Kept as a copy rather than a shared package: BRIEF-watch-app-split.md §Phase 1 is explicit that
// one shared file is not worth re-coupling two apps that have already diverged.

/// A grouped profile entry for the picker (SDR device → its profiles). `sdrName` is the device the
/// profile belongs to; `active` marks the one we're currently on.
struct SDRProfile: Identifiable, Hashable {
  let id: String        // OWRX profile id, "sdrId|profileId"
  let name: String      // profile display name (SDR prefix stripped)
  let sdrName: String   // owning SDR device name
  var active: Bool = false
}

/// A DAB service (programme) within the tuned ensemble — id is OWRX's service id, name the label.
struct DabProgramme: Identifiable, Equatable {
  let id: Int
  let name: String
}

/// One line in the server's shared chat. `mine` is our own message echoed back (OWRX broadcasts every
/// message to all clients including the sender), drawn aligned right so the conversation reads naturally.
struct ChatLine: Identifiable, Equatable {
  let id = UUID()
  let name: String
  let text: String
  var mine: Bool = false
}

/// One decoded aircraft from an ADS-B `secondary_demod` ADSB-LIST. Position is what the plane sends;
/// distance/bearing are computed client-side from the receiver location.
struct Aircraft: Identifiable, Equatable {
  let icao: String
  var flight: String?      // callsign
  var country: String?
  var ccode: String?       // ISO country of registry
  var altitude: Double?    // ft
  var speed: Double?       // kt
  var vspeed: Double?      // ft/min
  var course: Double?      // deg
  var squawk: String?
  var rssi: Double?
  var msgs: Int?
  var lat: Double?
  var lon: Double?
  var distKm: Double?
  var bearing: Double?
  var id: String { icao }
}

/// A snapshot of the FM-DX Webserver's tuner state — one whole-state JSON frame off the `/text` socket,
/// flattened into what the tuner screen reads. Mirrors the companion's `WatchLink.FmdxState`.
struct FmdxInfo: Equatable {
  var freq: Double = 0        // Hz (server sends MHz)
  var users: Int = 0
  var level: Double = 0       // 0…1 bar fill, derived from dBf
  var meter: String = ""      // "12.3 dBf"
  var ps: String = ""         // programme service (station) name
  var rt: String = ""         // current RadioText bank
  var pi: String = ""
  var pty: String = ""
  var stereo = false
  var rds = false
  var tx: String = ""         // transmitter/station name
  var city: String = ""
  var dist: Double = 0        // km from the receiver
  var rx: String = ""         // the receiver's own name (origin of `dist`)
  var flag: String = ""       // country flag emoji
  // ── Server-side controls (FM-DX Webserver). Mirrors the phone's FmdxAdapter. ──
  var eq = false              // cEQ filter
  var ims = false             // iMS (multipath suppression)
  var antenna = 0             // currently selected antenna (0-based, matches the `Z` command)
  /// Antennas this server advertises. EMPTY = no switch (single antenna, or the owner disabled it),
  /// in which case the control must not be shown at all — the same rule as OWRX's lockedRate.
  var antennas: [FmdxAntenna] = []
}

/// One selectable antenna on an FM-DX server. Keys arrive as `antN` (1-based) but the `Z` command
/// and the `ant` state field are 0-based, so `id` is N-1.
struct FmdxAntenna: Identifiable, Equatable, Codable {
  let id: Int
  let name: String
}

/// A learned FM station for the dial — PS name at a 100 kHz channel. Persisted in UserDefaults so the
/// spike's dial fills in over time (the standalone equivalent of the phone's RDS memory).
struct LearnedStation: Identifiable, Equatable, Codable {
  var id: Double { freqHz }
  let freqHz: Double
  var name: String

  private static let key = "vibe.fmdx.stations"
  static func load() -> [LearnedStation] {
    guard let d = UserDefaults.standard.data(forKey: key),
          let s = try? JSONDecoder().decode([LearnedStation].self, from: d) else { return [] }
    return s
  }
  static func save(_ s: [LearnedStation]) {
    // Cap so a long DX session can't grow it without bound (nearest-tuned wins on the dial anyway).
    let capped = Array(s.suffix(400))
    if let d = try? JSONEncoder().encode(capped) { UserDefaults.standard.set(d, forKey: key) }
  }
}
