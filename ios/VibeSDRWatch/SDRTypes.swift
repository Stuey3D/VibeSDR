import Foundation
import Combine

// The data types Buddy's SCREENS are written against, extracted from the direct-connection
// clients they used to live inside.
//
// ★ Buddy has no direct clients — the iPhone owns every connection — but its screens ARE Jr's
//   screens, and those were written against these types. Extracting them is what allows the whole
//   direct stack to be DELETED rather than carried along dead: UberClient, KiwiClient, OwrxClient,
//   FmDxClient, the DSP, the audio chain and libopus itself.
//
//   That deletion is the point of the split. BRIEF-watch-app-split.md §1.1: the merged watch app
//   was "all of VibeSDR's backend surface plus a companion remote, in one bundle, on a wrist".
//
// PhoneClient is the ONLY implementer of SDRClient here; every value it returns is mapped from
// what the phone sends over WCSession.

/// The surface SpikeLink drives — implemented by both UberClient and KiwiClient so the wrist UI
/// is backend-agnostic. Everything here is read/called on the main actor.
@MainActor
protocol SDRClient: AnyObject {
  var frequency: Double { get }
  var mode: String { get }
  var displaySpanHz: Double { get }
  var bwLow: Double { get }
  var bwHigh: Double { get }
  var signalLevel: Double { get }
  var signalDb: Double { get }
  var rowsPushed: Int { get }
  var framesPerSec: Double { get }
  var status: String { get }
  /// How far Link Management has had to throttle the waterfall (1 = full rate). Drives the link
  /// glyph. A backend with no rate lever (OWRX) keeps the default 1 via the extension below.
  var adaptiveRung: Int { get }
  /// A plain-English refusal/timeout reason to show the user (nil = fine). Kiwi sets this on
  /// badp/too_busy/handshake-block/connect-timeout so nobody waits forever for a dead connection.
  var lastError: String? { get }

  func start()
  func drainSpectrum(now: Double)
  func tune(delta: Int, step: Double)
  func tuneTo(_ hz: Double)
  func zoom(delta: Int)
  func setVolume(_ v: Double)
  func setMode(_ m: String)
  func setBandwidth(_ low: Double, _ high: Double)
  func resumeSpectrum()
  func reconnectIfNeeded()
  func suspend()
  func goIdle()
  // OWRX profile surface — REQUIREMENTS (not just extension members) so dynamic dispatch reaches
  // OwrxClient's real list through `any SDRClient`. UberSDR/Kiwi get the default-empty extension.
  var profiles: [SDRProfile] { get }
  var clients: Int { get }
  func selectProfile(_ id: String)
  /// Live inbound link load (KB/s of all WS bytes). Drives the "heavy server" advisory when on the
  /// phone relay. REQUIREMENT (not just extension) so `any SDRClient` reaches the real number. 0 = the
  /// backend doesn't measure it (fine — no advisory).
  var inboundKbPerSec: Int { get }
  /// Live station name (RDS ps / DAB service) to show in place of the band label. "" = none.
  var stationName: String { get }
  /// DAB services in the tuned ensemble (empty unless on a DAB profile), the current speed-fix factor,
  /// and the controls to change them. OWRX-only; defaults make the other backends inert.
  var dabProgrammes: [DabProgramme] { get }
  var dabScale: Double { get }
  var dabActiveId: Int { get }
  var dabEnsembleName: String { get }
  func selectDabService(_ id: Int)
  func setDabScale(_ scale: Double)
  /// ADS-B decoded aircraft (empty unless on a 1090 MHz ADS-B profile). OWRX-only; default inert.
  var aircraft: [Aircraft] { get }
  /// The receiver's own location (SDR site), for the ADS-B map centre + aircraft distances. nil = unknown.
  var receiverLat: Double? { get }
  var receiverLon: Double? { get }
  /// Shared server chat (OWRX today; FM-DX later). `supportsChat` gates the whole UI; `chatActivity`
  /// bumps per inbound line to breathe the glyph. Default inert so non-chat backends need nothing.
  var supportsChat: Bool { get }
  var chatLog: [ChatLine] { get }
  var chatActivity: Int { get }
  func sendChat(_ text: String)
  /// FM-DX tuner state (nil unless this is an FM-DX server). Default inert.
  var fmdxInfo: FmdxInfo? { get }
}

extension SDRClient {
  /// OpenWebRX has no waterfall-rate lever at all (fps/fft_fps/fft_size are ignored), so it never
  /// throttles and is never blamed for one.
  var adaptiveRung: Int { 1 }
  var profiles: [SDRProfile] { [] }
  var clients: Int { 0 }
  func selectProfile(_ id: String) {}
  var inboundKbPerSec: Int { 0 }
  var stationName: String { "" }
  var dabProgrammes: [DabProgramme] { [] }
  var dabScale: Double { 1.0 }
  var dabActiveId: Int { -1 }
  var dabEnsembleName: String { "" }
  func selectDabService(_ id: Int) {}
  func setDabScale(_ scale: Double) {}
  var aircraft: [Aircraft] { [] }
  var receiverLat: Double? { nil }
  var receiverLon: Double? { nil }
  var supportsChat: Bool { false }
  var chatLog: [ChatLine] { [] }
  var chatActivity: Int { 0 }
  func sendChat(_ text: String) {}
  var fmdxInfo: FmdxInfo? { nil }
}

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
struct FmdxAntenna: Identifiable, Equatable {
  let id: Int
  let name: String
}

/// A station name learned against a frequency, for the FM-DX dial.
///
/// ★ In Buddy this is the PHONE's memory, not ours. The phone already learns stations and pushes
///   the list over WCSession (`stations`), so Buddy mirrors it and persists nothing — one memory,
///   on the device that does the listening. Jr keeps its own local copy, which is why the type is
///   shared but the STORAGE is not.
struct LearnedStation: Identifiable, Equatable, Codable {
  var id: Double { freqHz }
  let freqHz: Double
  var name: String
}
