import Foundation
import Combine

/// **Companion mode as an `SDRClient`** — the watch driven by the iPhone over WCSession.
///
/// This is the piece the whole merge turns on (`BRIEF-watch-app-merge.md` §1). The V9 companion
/// looked like a second, incompatible app because it "rendered differently" — but it does not:
///
///   - `WaterfallBuffer.push(row:)` takes **finished rows**, not bins.
///   - The direct backends do bins → `SignalProcessor` → row → push.
///   - The phone sends rows it has ALREADY rendered, so this pushes at exactly the same point,
///     skipping only the DSP stage.
///
/// So Phone Control is not a second renderer. It is a client that SOURCES rows from WCSession
/// instead of computing them from IQ — which is why the entire spike UI works above it unchanged,
/// and why the companion's own screens could be deleted outright rather than merged.
///
/// The transport is `WatchLink`, kept whole: it is field-proven in V9 and carries hard-won detail
/// (row batching because `sendMessage` wedges at 16 calls/sec, crown-flush coalescing, optimistic
/// tune/volume prediction). Rewriting it to "tidy" the merge would have thrown all of that away.
/// ★ [[jr_transport_ws_two_modes]]: a watch↔phone WebSocket is architecturally impossible out of
///   the house. WCSession is not a stopgap here, it is the only option.
@MainActor
final class PhoneClient: ObservableObject, SDRClient {
  /// The WCSession transport.
  ///
  /// ★ ALWAYS `WatchLink.shared`, never a fresh instance. `WCSession.default.delegate` is a SINGLE
  ///   slot: whoever activates last silently owns it. Since the launch gate has to activate a
  ///   session before any client exists (to find out whether the phone is even in use), a second
  ///   WatchLink here would steal the delegate from it — or have it stolen — and the symptom would
  ///   be messages simply vanishing, with both objects looking perfectly healthy.
  ///
  ///   One session, one delegate, one owner. We inject SpikeLink's waterfall into it instead, which
  ///   is the only per-session state that differs.
  let link = WatchLink.shared

  /// Volume arrives from the crown as an ABSOLUTE 0…1, but the phone protocol speaks in detents
  /// (`{"cmd":"vol","delta":n}`) because the phone owns the real level and mirrors back what it
  /// actually applied — including changes made ON the phone. So we convert, and track what we last
  /// sent to avoid re-sending the level the mirror just told us about.
  private var lastVolumeSent: Double = 1.0
  private static let volumeDetents = 16.0

  init(waterfall: WaterfallBuffer) {
    link.waterfall = waterfall
  }

  // ── State the UI mirrors. All of it is the PHONE's, echoed over the link ──
  var frequency: Double { link.frequency }
  var mode: String { link.mode }
  var displaySpanHz: Double { link.span }
  var bwLow: Double { link.filtLo }
  var bwHigh: Double { link.filtHi }
  var signalLevel: Double { link.level }
  var signalDb: Double { link.snr }
  var rowsPushed: Int { link.rowsPushed }

  /// The MEASURED row rate off the link (WatchLink counts arrivals per second).
  ///
  /// ★ This was hardcoded to 0, on the reasoning that inventing a frame rate would feed the link
  ///   glyph a fiction. That was backwards: SpikeLink derives the glyph FROM this
  ///   (`framesPerSec > 0 ? 3 : (everGotRow ? 1 : 3)`), so 0 is not "no claim" — it is the claim
  ///   that the link is dead, and it painted the node red as soon as one row had ever arrived.
  ///   Measuring it means the glyph goes red when rows really stop and green when they really flow,
  ///   which is the whole point of having it.
  var framesPerSec: Double { link.rowsPerSec }

  /// ★ In Phone Control the node glyph must report the PHONE's link to the server, not the watch's
  ///   — the phone owns that connection and the watch has no direct opinion about it (§2b).
  ///   `why` is the phone's own plain-English reason, already sent in the state frame.
  var status: String {
    if !link.reachable { return "iPhone unreachable" }
    return link.why
  }

  /// Refusals belong to the phone and are surfaced in its own status text, which `status` already
  /// carries. A second channel here would let the two disagree.
  var lastError: String? { nil }

  // ── TWO HOPS, TWO GLYPHS (§2b) ───────────────────────────────────────────────────────────────
  //
  //   watch ──(A)── iPhone ──(B)── server
  //
  // Standalone has ONE hop and the node glyph reports it. Phone Control has TWO, and they fail
  // independently, so each glyph must report its OWN hop or they cannot localise a fault:
  //
  //   node red + iPhone green  ->  the phone lost the server
  //   node green + iPhone red  ->  you have walked away from your phone (or rows are being lost)
  //
  // Collapsing both into one number is what made tonight's missing waterfall opaque: the node
  // glyph went red off the WATCH's row flow, which said nothing about whether the phone was still
  // happily connected — and it was.

  /// (B) phone ↔ SERVER — the PHONE's own reported link health, straight from its state frame.
  /// The watch has no direct opinion about the server in this mode and must not invent one.
  var phoneServerLink: Int { link.serverLink }

  /// (A) watch ↔ PHONE. Rows flowing, or a recent state frame if the phone simply has nothing to
  /// send (idle on a frequency, or a screen with no spectrum at all).
  var phoneHopHealthy: Bool {
    if link.rowsPerSec > 0 { return true }
    if let t = link.lastStateAt, Date().timeIntervalSince(t) < 8 { return true }
    return false
  }

  func start() {
    link.activate()
    // ★ ASK FOR EVERYTHING. The phone sends the palette LUT, favourites, station memory and
    //   settings ON CHANGE only, and re-sends them when it sees a ping after an 8s gap — which in
    //   V9 was guaranteed, because the first ping the phone ever saw WAS the watch app launching.
    //
    //   Jr broke that assumption: the launch gate starts pinging at app start to find out whether
    //   the phone is even there, so by the time Companion mode is entered the phone has been
    //   hearing pings for a while and the gap never opens. Entering the mode is precisely when we
    //   have nothing, so say so outright (`cmd:need` -> flushAll) rather than relying on a silence
    //   that no longer happens.
    link.requestMissing()
  }

  /// Nothing to drain: rows are pushed straight into the buffer as they arrive. The direct backends
  /// hold a ~1s queue to line the spectrum up with their own audio cushion — here the AUDIO IS THE
  /// PHONE'S, so the phone has already done that alignment. Re-delaying would double it.
  func drainSpectrum(now: Double) {}

  // ── Controls. Every one is a command TO the phone; none of it acts locally ──
  // `step` is ignored: the phone owns the tuning step (set via `cmd:step` from the menu) so that a
  // detent means the same thing on both screens. Honouring a local step would silently desync them.
  func tune(delta: Int, step: Double) { link.tune(delta: delta) }
  func tuneTo(_ hz: Double) { link.tune(toHz: hz) }
  func zoom(delta: Int) { link.zoom(delta: delta) }
  func setMode(_ m: String) { link.setMode(m) }

  func setVolume(_ v: Double) {
    let delta = Int(((v - lastVolumeSent) * Self.volumeDetents).rounded())
    guard delta != 0 else { return }
    lastVolumeSent = v
    link.volume(delta: delta)
  }

  /// The phone owns its own filter edges per mode, and the link has no bandwidth command — changing
  /// the mode is what moves them. Silently doing nothing is correct; pretending otherwise would put
  /// the watch's idea of the passband out of step with the audio actually being sent.
  func setBandwidth(_ low: Double, _ high: Double) {}

  func resumeSpectrum() { link.requestMissing() }
  func reconnectIfNeeded() { link.ping() }

  /// The phone keeps streaming while its screen is up; we simply stop being asked to draw.
  func suspend() {}

  /// ★ THIS WAS A NO-OP, AND THAT WAS THE BUG. The reasoning was "no sockets of our own to close" —
  ///   true, and irrelevant. WCSession has no close: the delegate stays registered and the phone
  ///   keeps sending, so after leaving Companion the phone's rows kept landing in the buffer the
  ///   DIRECT client had started drawing into. Two writers, one buffer — the waterfall bounced and
  ///   flickered.
  ///
  ///   Precedent, and it should have been the first thing checked: UberClient.goIdle() carries a
  ///   `goingIdle` latch for the same reason — without it, its retry Tasks reopened sockets while
  ///   the next server started on top (the "2nd server 93% hang"). A client being discarded has to
  ///   stop DOING things; having nothing to close is not the same as having nothing to stop.
  func goIdle() { link.detach() }

  // ── THE PHONE'S CURRENT BACKEND, so the watch follows it off the waterfall ───────────────────
  //
  // ★ The spike routes screens off the CLIENT TYPE (`client is FmDxClient`), which can never be
  //   true here — PhoneClient is the only client in Companion no matter what the phone is on. So
  //   switching the phone to FM-DX changed the audio while the wrist sat on a FROZEN UberSDR
  //   waterfall (Stuart, build 71). The phone was already sending everything needed; nothing was
  //   reading it.
  //
  //   These types are the companion's and the spike's versions of the same thing, near
  //   field-for-field. Mapping is mechanical — the alternative, teaching the spike's views to read
  //   WatchLink's types, would fork every screen in the app.

  var fmdxInfo: FmdxInfo? {
    guard let f = link.fmdx else { return nil }
    var i = FmdxInfo()
    i.freq = f.freq;   i.users = f.users;  i.level = f.level; i.meter = f.meter
    i.ps = f.ps;       i.rt = f.rt;        i.pi = f.pi;       i.pty = f.pty
    i.stereo = f.stereo
    i.rds = !f.ps.isEmpty || !f.pi.isEmpty   // the phone sends no rds flag; RDS content IS the flag
    i.tx = f.tx;       i.city = f.city;    i.dist = f.dist;   i.rx = f.rx;  i.flag = f.flag
    return i
  }

  /// The phone decodes ADS-B and sends finished aircraft; only the struct differs.
  var aircraft: [Aircraft] {
    link.aircraft.map { a in
      var p = Aircraft(icao: a.icao)
      p.flight = a.flight;   p.country = a.country;  p.ccode = a.ccode
      p.altitude = a.altitude; p.speed = a.speed;    p.vspeed = a.vspeed
      p.course = a.course;   p.squawk = a.squawk;    p.rssi = a.rssi
      p.distKm = a.distKm;   p.bearing = a.bearing
      // ☐ NO lat/lon: the phone sends distance + bearing, not position, so the ADS-B MAP has
      //   nothing to plot in Companion. The list (sorted by distance) is unaffected.
      //
      // ★★ WHATEVER FIXES THIS, THE ORIGIN IS THE RECEIVER — NEVER THE WATCH (Stuart). Every
      //    distance and bearing here is measured from the SDR's own site, so plotting them around
      //    the wearer would put every aircraft somewhere it isn't, most wrongly when you are
      //    furthest from the receiver — i.e. exactly when you are using a remote SDR. AircraftView
      //    is already correct on this (it centres on `receiverLat`/`receiverLon` with no fallback);
      //    the missing piece is that the phone does not SEND the receiver's position over WCSession
      //    at all, so both stay nil in Companion. Add rx lat/lon to the protocol, then either send
      //    positions or derive them from receiver + distance + bearing.
      return p
    }
  }

  var dabProgrammes: [DabProgramme] {
    (link.dab?.list ?? []).map { DabProgramme(id: $0.id, name: $0.name) }
  }
  var dabActiveId: Int { link.dab?.active ?? -1 }
  var dabEnsembleName: String { link.dab?.ensemble ?? "" }
  /// Selecting a service redirects the PHONE, exactly as tuning does.
  func selectDabService(_ id: Int) { link.selectDab(id) }

  /// The screen the PHONE is on, for SpikeLink's router. `isFmdx` is cleared by the arrival of any
  /// spectrum row, so it cannot get stuck showing FM-DX after the phone returns to a waterfall.
  var phoneScreen: SpikeLink.Screen {
    if link.isFmdx || link.fmdx != nil { return .fmdx }
    if let d = link.dab, !d.list.isEmpty { return .dab }
    if !link.aircraft.isEmpty { return .adsb }
    return .sdr
  }

  // ── Not yet proxied. Each is a deliberate stub, not an oversight ──
  // Chat MUST route through the phone rather than opening a second connection — one server
  // connection, one participant, one history (§2c). That needs a read-watermark syncing both ways,
  // which does not exist in the protocol yet, so chat stays off in this mode until it does.
  var supportsChat: Bool { false }
}
