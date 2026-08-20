import Foundation
import SwiftUI

/// How the watch is reaching the server. Buddy is ALWAYS via the phone, but the ported views take
/// this as a parameter, so the type has to exist.
enum Transport { case iphone, wifi, cellular, none }

// The surface Jr's SCREENS expect, provided by Buddy's WatchLink.
//
// ★ THE POINT OF THE SPLIT: the two apps are visually IDENTICAL and run on different engines
//   (Stuart). Jr's views were written against SpikeLink, which owns a direct SDR client; Buddy's
//   run on WatchLink, which owns a WCSession pipe to the phone. Most of the surface already
//   matched by name — frequency, span, snr, meter, level, mode, filtLo/filtHi, waterfall, volume —
//   which is why the port is a retype rather than a rewrite.
//
//   What follows is the remainder. Each member is either MAPPED to something the phone genuinely
//   sends, or STUBBED with the reason it cannot exist here. A stub is not a shrug: in Buddy the
//   phone owns the radio, so a control that would act on the radio has nothing to act on.
//
// ★★ NOTHING HERE TOUCHES THE DATA PATH. WatchLink itself is the V9 companion's, unchanged and
//    field-proven — the lesson of build 78-80 being that the instability came from rederiving that
//    layer, not from the layer itself.
extension WatchLink {

  // ── Mapped: the phone really does tell us these ──────────────────────────────────────────────

  /// The backend's own status line. The phone sends `why` (live/paused/idle/reconnecting), which
  /// is the same idea expressed in the phone's words.
  var backendStatus: String { why }

  /// Live station name (RDS ps / DAB service), from whichever screen state the phone last sent.
  var stationName: String { fmdx?.ps ?? dab?.ensemble ?? "" }

  /// ★ Buddy is ALWAYS on the phone — that is what it is. The transport glyph therefore reports
  ///   the watch↔phone hop rather than how the watch reaches the internet, which it does not.
  var transport: Transport { .iphone }

  // ── Stubbed: these act on a radio Buddy does not own ─────────────────────────────────────────

  /// Link Management throttles OUR OWN socket. Buddy has none — the phone holds the server
  /// connection and runs its own ladder — so there is no rung here and nothing to report.
  var throttleRung: Int { 1 }
  /// Nor is there a rate to settle on, for the same reason: we never chose one.
  var linkSettling: Bool { false }
  /// The phone surfaces its own refusals in `why`; a second channel would let the two disagree.
  var connectError: String? { nil }
  /// "Heavy server for the phone relay" is a judgement about OUR socket. The phone makes its own.
  var bandwidthWarning: String? { nil }
  /// OWRX-specific tutorial line: the phone knows its backend, we do not (yet).
  var isOwrx: Bool { false }

  // isBackground now lives on the WatchLink class (a real @Published) — it must flip true the instant
  // the wrist drops so the row-gap that follows isn't mistaken for a rough link and painted with the
  // 'reconnecting' glyph. See WatchLink.suspend()/resume().
  /// ☐ CHAT IS NOT PORTED YET, and it is a PROTOCOL job rather than a UI one: neither the phone
  ///   nor WatchLink has any notion of chat today — nothing sends it, nothing receives it. Wiring
  ///   the sheet up to nothing would give a chat box that silently swallows messages on a SHARED
  ///   receiver, which is worse than not offering it. False until both ends exist.
  /// ★★★ NO LONGER FALSE — THE SHARED DIAL BROUGHT ITS OWN WIRE PROTOCOL (2026-08-20). This was
  /// stubbed off because Buddy's chat had none: the free-text backends' chats live on the phone
  /// and there was no remote command for them. A shared-dial VibeServer is different — the phone
  /// forwards `dial`/`said` and sends `cmd:say` back — so the glyph now has somewhere real to go,
  /// and ONLY on a receiver where several listeners share one tuner.
  var supportsChat: Bool { sharedDial }
  var chatActivity: Int { chatUnread }

  /// Hardware controls (gain, bias-T, AGC, ppm, rate) belong to whoever owns the dongle — the
  /// phone. There is no remote command for them, so the sheet must not appear at all rather than
  /// appear and do nothing.
  var hasHardwareControls: Bool { false }

  // ── The phone's wire structs, mapped onto the types Jr's views name ─────────────────────────

  /// FM-DX tuner state. WatchLink.FmdxState and FmdxInfo are the same thing described twice —
  /// mapping is cheaper than teaching every view a second type.
  var fmdxInfo: FmdxInfo? {
    guard let f = fmdx else { return nil }
    var i = FmdxInfo()
    i.freq = f.freq;  i.users = f.users; i.level = f.level; i.meter = f.meter
    i.ps = f.ps;      i.rt = f.rt;       i.pi = f.pi;       i.pty = f.pty
    i.stereo = f.stereo
    i.rds = !f.ps.isEmpty || !f.pi.isEmpty      // the phone sends no rds flag; RDS content IS one
    i.tx = f.tx;      i.city = f.city;    i.dist = f.dist;  i.rx = f.rx;  i.flag = f.flag
    i.eq = f.eq;      i.ims = f.ims;      i.antenna = f.ant; i.antennas = f.antennas
    return i
  }
  var isFmDx: Bool { isFmdx }

  var dabProgrammes: [DabProgramme] { (dab?.list ?? []).map { DabProgramme(id: $0.id, name: $0.name) } }
  var dabActiveId: Int { dab?.active ?? -1 }
  var dabEnsembleName: String { dab?.ensemble ?? "" }

  /// ADS-B: the phone decodes and sends finished aircraft.
  /// ★★ lat/lon ARE on the wire and always were — the phone JSON-encodes its whole Aircraft record.
  ///   What was missing was this mapping and the `lat`/`lon` fields on WatchLink.Aircraft, so the
  ///   decoder dropped them silently and the map iterated an always-empty array while the list
  ///   (sorted by distance) looked perfect. Fixed 2026-07-30.
  /// ★★★ THE ORIGIN IS THE RECEIVER, NEVER THE WATCH (Stuart). Every distance is measured from the
  ///   SDR's site, so centring on the wearer would misplace every aircraft — worst when you are
  ///   furthest from the receiver, which is exactly the remote-SDR case.
  ///
  /// ★ MODULE-QUALIFIED ON PURPOSE. Inside `extension WatchLink` the bare name `Aircraft` resolves
  ///   to the NESTED `WatchLink.Aircraft` (the wire struct), not the global one the views use — so
  ///   the map silently returned the wrong type and the error surfaced three files away. Swift
  ///   shadowing, and worth the noise of spelling out the module.
  var aircraftList: [VibeSDRWatch.Aircraft] {
    aircraft.map { a in
      var p = VibeSDRWatch.Aircraft(icao: a.icao)
      p.flight = a.flight; p.country = a.country; p.ccode = a.ccode
      p.altitude = a.altitude; p.speed = a.speed; p.vspeed = a.vspeed
      p.course = a.course; p.squawk = a.squawk; p.rssi = a.rssi
      // ★★★ AND THE POSITION. `located` filters on lat/lon, so leaving these out made the map's
      //   annotation loop iterate an ALWAYS-EMPTY array — the list worked, the map drew nothing.
      //   Two places had to be wrong at once for that: the wire struct did not decode them (fixed
      //   in WatchLink.Aircraft) and this mapping did not carry them.
      p.lat = a.lat; p.lon = a.lon
      p.distKm = a.distKm; p.bearing = a.bearing
      return p
    }
  }
  /// The receiver's own position — the map's home marker, the fixed reference the aircraft move
  /// around. Buddy cannot derive it (it never talks to OWRX), so the phone sends it on the `rx`
  /// message kind: its own kind rather than folded into the aircraft table, because the table
  /// churns every few seconds and this changes about once a session. Jr needs none of this — it
  /// reads the receiver position straight from the server.
  var receiverLat: Double? { rxLat }
  var receiverLon: Double? { rxLon }

  /// OWRX profiles and listener count are the PHONE's session; it does not forward them yet.
  var profiles: [SDRProfile] { [] }
  var clients: Int { 0 }
  var chatLog: [ChatLine] { [] }
  /// The phone's learned FM-DX station names — WatchLink already receives these, so they only need
  /// the type it sends mapping onto the one the views name.
  var learnedStations: [LearnedStation] {
    stations.map { LearnedStation(freqHz: $0.freqHz, name: $0.name) }
  }

  // ── Actions ──────────────────────────────────────────────────────────────────────────────────

  /// Show the servers screen. NAVIGATION in Buddy, never teardown: the phone keeps playing while
  /// you browse, and choosing a server simply redirects it.
  func backToPicker() { showServers = true }

  func reconnectIfNeeded() { ping() }

  /// ✅ Chat now HAS a wire protocol, for the shared dial only (see `supportsChat` above).
  /// ☐ The free-text backend chats still have none. `supportsChat` stays false for those;
  ///   this exists only so the ported sheet compiles.
  func sendChat(_ text: String) {}

  /// OWRX profile switching is EXPLICIT-only on a shared receiver, and the phone owns the session.
  /// The wire has no profile command, so the menu shows no profiles (see `profiles`) and this
  /// exists only so the ported menu compiles.
  func selectProfile(_ id: String) {}

  /// DAB speed-fix ("chipmunk") is applied by whoever decodes — the phone. It sends the corrected
  /// audio already, so there is nothing for the watch to scale.
  var dabScale: Double { 1.0 }
  func setDabScale(_ scale: Double) {}
  /// Selecting a DAB service redirects the PHONE, exactly as tuning does.
  func selectDabService(_ id: Int) { selectDab(id) }
  // suspend(graceSeconds:) and resume() live in WatchLink.swift — they call the private send().
}
