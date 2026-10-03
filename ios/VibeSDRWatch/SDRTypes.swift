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
// Kept as a copy rather than a shared package: briefs/BRIEF-watch-app-split.md §Phase 1 is explicit that
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

/// ★★★ NUMBERS FROM A SERVER, MADE SAFE TO CONVERT — a copy of Jr's `Wire` (spike Vitals.swift).
/// `Int(x)` on a Double traps on NaN, infinity and anything outside Int's range, and on arm64_32
/// (32-bit Int below watchOS 27) that range is ±2.1 billion. These clamp instead: a value that got
/// past a parser can only be WRONG, never fatal.
enum Wire {
  /// Double → Int64, NaN → 0, clamped to Int64's range.
  static func i64(_ d: Double) -> Int64 {
    guard d.isFinite else { return 0 }
    return Int64(max(-9.0e18, min(9.0e18, d.rounded())))
  }
  /// Double → Int, NaN → 0, clamped to THIS platform's Int (32-bit on arm64_32).
  static func int(_ d: Double) -> Int {
    guard d.isFinite else { return 0 }
    let lim = Int.bitWidth == 32 ? 2_147_483_000.0 : 9.0e18
    return Int(max(-lim, min(lim, d.rounded())))
  }
  /// A finite number in a range, or nil — the gate for anything the server sends as a quantity.
  /// Accepts an NSNumber or a numeric string (directories send both).
  static func inRange(_ v: Any?, _ r: ClosedRange<Double>) -> Double? {
    let d: Double?
    if let n = v as? NSNumber { d = n.doubleValue }
    else if let s = v as? String { d = Double(s.trimmingCharacters(in: .whitespaces)) }
    else { d = nil }
    guard let d, d.isFinite, r.contains(d) else { return nil }
    return d
  }
}

/// ★★★ TEXT FROM ANY SERVER IS UNTRUSTED — the Swift port of the app's `src/utils/safeText.ts`
/// `cleanText`, so the watch cleans a name exactly as the phone and the web client do. A station name
/// can come from an admin's typing, from RDS/DAB decoded off the air (garbled, or sent by anyone with
/// a transmitter), from a directory row, or from a stranger in an OpenWebRX chat.
///  - C0/C1 control characters and DEL → a space.
///  - Bidi overrides and isolates (U+202A–202E, U+2066–2069) → removed ("Trojan Source").
///    Ordinary right-to-left scripts are untouched.
///  - Zero-width space, word joiner and BOM (U+200B, U+2060, U+FEFF) → removed.
///    ★ NOT the joiners U+200C/U+200D — Indic scripts and multi-part emoji need them; Tibetan,
///    Arabic, Thai, Devanagari… all pass through.
///  - Runs of whitespace collapsed to one space, trimmed, and capped at `max` CHARACTERS (grapheme
///    clusters, so a cut never splits a surrogate pair, a combining mark or an emoji).
///  ★ The scan itself is bounded: a 16 MB chat message is not walked to the end to keep 300 of it.
enum SafeText {
  static func clean(_ v: Any?, max: Int = 64) -> String {
    guard let s = v as? String, max > 0 else { return "" }
    var out = String.UnicodeScalarView()
    var pendingSpace = false
    var kept = 0
    // Enough scalars for `max` characters even with heavy combining marks, and no more.
    let scalarBudget = max * 8
    for u in s.unicodeScalars {
      let c = u.value
      if c < 0x20 || (c >= 0x7F && c <= 0x9F) { pendingSpace = true; continue }   // C0, DEL, C1
      if (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) { continue } // bidi
      if c == 0x200B || c == 0x2060 || c == 0xFEFF { continue }                     // zero-width
      if u.properties.isWhitespace { pendingSpace = true; continue }
      if pendingSpace && !out.isEmpty { out.append(" "); kept += 1 }
      pendingSpace = false
      out.append(u); kept += 1
      if kept >= scalarBudget { break }
    }
    var r = String(out)
    if r.count > max {
      r = String(r.prefix(max))
      while r.last?.isWhitespace == true { r.removeLast() }
    }
    return r
  }
  /// Optional form for fields where "absent" must stay absent rather than become "".
  static func cleanOpt(_ v: Any?, max: Int = 64) -> String? {
    let s = clean(v, max: max)
    return s.isEmpty ? nil : s
  }
}

extension Array where Element: Identifiable {
  /// ★★ One element per id, first occurrence wins. A SwiftUI List/ForEach with two rows of the same
  ///    id is undefined behaviour that crashes ("ID occurs multiple times") — and every list Buddy
  ///    draws arrives from the phone, which relays what a server or directory said.
  func uniquedByID() -> [Element] {
    var seen = Set<Element.ID>()
    return filter { seen.insert($0.id).inserted }
  }
}
