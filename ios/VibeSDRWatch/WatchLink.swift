import Foundation
import SwiftUI
import WatchConnectivity
import WatchKit

/// The watch end of the phone<->watch pipe.
///
/// The watch is a thin client: the phone owns all state (frequency, mode, step,
/// gain) and does all DSP. We receive rows that are already quantised to 0-255
/// with the phone's brightness/contrast/gain baked in, plus the phone's palette
/// as a raw 256-entry RGBA LUT — so the wrist is a pixel-faithful mirror of the
/// phone waterfall without any DSP or palette logic living here.
///
/// Keys are terse because every row rides this dictionary at ~10fps.
enum WK {
  static let kind    = "k"   // "row" | "state" | "fmdx" | "logo" | "settings" | "pong"
  static let meter   = "mt"  // String — the meter text the PHONE is drawing
  static let json    = "j"   // String — FM-DX state blob
  static let image   = "img" // Data — station logo PNG/JPEG bytes
  static let row     = "r"   // Data, 128 bytes, 0-255 intensity
  static let freq    = "f"   // Double, Hz — VFO centre (the row is centred on it)
  static let span    = "sp"  // Double, Hz — width the 128 bins cover
  static let snr     = "s"   // Double, dB
  static let level   = "lv"  // Double, 0..1 — the phone's own smoothed meter fill
  static let mode    = "m"   // String
  static let step    = "st"  // Double, Hz
  static let lut     = "l"   // Data, 1024 bytes RGBA x256
  static let smooth  = "sm"  // Double, 0..1 row blend factor
  static let filtLo  = "lo"  // Double, Hz offset from carrier (negative = below)
  static let filtHi  = "hi"  // Double, Hz offset from carrier
  static let needle  = "nc"  // String, "#rrggbb" — the phone's VFO colour
  static let needleI = "ni"  // Double, 1..10 — needle intensity
  static let sharp   = "sh"  // Double, 0..10 — waterfall sharpness
  static let peak    = "pk"  // Bool — the phone's peak-hold setting, mirrored
  static let why     = "wy"  // String — WHY there are no rows: live|paused|idle|reconnecting
  static let link    = "lk"  // Int 0…3 — the PHONE↔SERVER hop's quality (see serverLink)
  static let vol     = "vo"  // Double 0…1 — the iPhone's SYSTEM volume (see volume)
  static let muted   = "mu"  // Bool — phone muted. NOT volume-zero; the level is preserved.
  static let band    = "bn"  // String — ITU band plan name ("20m Ham Band"), "" = none
  static let bandCol = "bc"  // String "#rrggbb" — that band's colour, from the phone's plan
  static let bandLo  = "bl"  // Double, Hz — the band's lower edge (0 = none)
  static let bandHi  = "bh"  // Double, Hz — the band's upper edge (0 = none)
}

/// "#rrggbb" → Color. Returns nil for an empty or malformed string, so a band we have no
/// colour for simply doesn't tint anything rather than tinting it black.
func hexColor(_ s: String) -> Color? {
  var h = s.trimmingCharacters(in: .whitespaces)
  if h.hasPrefix("#") { h.removeFirst() }
  guard h.count == 6, let v = UInt32(h, radix: 16) else { return nil }
  return Color(red:   Double((v >> 16) & 0xFF) / 255,
               green: Double((v >>  8) & 0xFF) / 255,
               blue:  Double( v        & 0xFF) / 255)
}

final class WatchLink: NSObject, ObservableObject, WCSessionDelegate {
  static let shared = WatchLink()

  // Rendered state. The waterfall image is @Published so Canvas redraws on it.
  @Published var waterfall  = WaterfallBuffer()
  @Published var frequency  = 0.0
  @Published var span       = 0.0
  @Published var snr        = 0.0
  /// The meter text the PHONE is drawing, mirrored verbatim ("S9+10", "-72dB",
  /// "18db"). NOT derived here.
  ///
  /// The watch used to format SNR itself from the row header — but OWRX, Kiwi and
  /// FM-DX have no SNR to give (they send an absolute S-meter or dBf, with no noise
  /// reference), so the phone sent 0 and this readout was a permanent "—" on those
  /// backends, while the bar beneath it moved perfectly well. Mirroring the phone's
  /// own string means the two can never disagree, and a backend with some other
  /// metric works for free. (Same reasoning as shipping the palette as a LUT rather
  /// than reimplementing the colour maps here.)
  @Published var meter      = ""
  @Published var level      = 0.0
  /// Squelch threshold as a 0..1 bar position (mirrored from the phone via the row), -1 = off.
  /// Drives the red squelch line + the breathing "SQL" readout — see ContentView. Gate is CLOSED
  /// (muting) when `level < sql`.
  @Published var sql        = -1.0
  @Published var mode       = ""
  @Published var step       = 0.0
  @Published var reachable  = false
  /// Servers screen requested from the menu. NAVIGATION only — the phone keeps playing while you
  /// browse, so this must never tear the link down (see WatchLinkCompat.backToPicker).
  @Published var showServers = false
  /// When ANY message last arrived from the phone — the evidence that the watch↔phone hop is
  /// alive. Rows are the strongest signal, but the phone legitimately goes quiet on rows (paused
  /// for power, or a screen with no spectrum), while still sending state and favourites. So the
  /// iPhone glyph keys off "anything at all", not rows alone.
  @Published var lastAnyAt: Date? = nil
  @Published var everGotRow = false {
    didSet {
      if everGotRow && !oldValue { liveSince = Date() }   // fresh session came up — start the settle window
      else if !everGotRow { liveSince = nil }             // torn down for a new session
    }
  }
  /// When the CURRENT session first showed rows. For the first few seconds the link meter reads low
  /// while it's still coming up — the hint says "Initialising", not "Server link poor".
  @Published private(set) var liveSince: Date? = nil

  /// When a row last arrived. The watch used to know only two states — "iPhone not
  /// reachable" or "fine" — but there is a THIRD: the phone is right there and
  /// simply isn't sending. That happens whenever the SDR screen isn't up: the
  /// instance picker, a disconnect, or an FM-DX instance (which routes to a
  /// different screen entirely and has no spectrum at all). The watch just sat on
  /// its last frame looking frozen, which is indistinguishable from a lock-up.
  @Published var lastRowAt: Date? = nil
  /// WHY the phone isn't sending rows, straight from the phone. "No spectrum from
  /// iPhone" is a symptom, not a diagnosis — a paused socket, a stalled renderer and
  /// a dead link all look identical from here, and chasing that cost hours.
  @Published var why = "live"
  /// The phone app has been CLOSED by the user (goodbye received, or a 'closed' status).
  /// Drives the "Phone app closed" screen and gates the heartbeat off (anti-hijack).
  @Published var phoneClosed = false
  /// Wrist down = watch backgrounded. Set the instant scenePhase leaves .active (Buddy has no
  /// background audio, so watchOS suspends us right after and the row feed stops); the ported
  /// power-save branches read this to suppress the false 'reconnecting' glyph and to fast-resume.
  @Published var isBackground = false
  /// The phone was DELIBERATELY closed (a goodbye, or a headless-relaunch 'closed' status). Latches the
  /// closed state so a stale in-flight row can't un-close us and restart the phone-relaunching heartbeat.
  /// Cleared only by a user Reopen or a genuine live phone status. `private(set)` so the app router can
  /// pick the Start screen (phone is here, deliberately closed) vs Connect-to-iPhone (phone out of reach).
  @Published private(set) var deliberatelyClosed = false
  /// Stamped on wrist-up so the silence watchdog restarts its grace instead of firing instantly on a
  /// stale lastRowAt (a long wrist-down) — which was flashing the Start screen for a moment on the way back.
  private(set) var resumedAt = Date.distantPast
  @Published var lastStateAt: Date? = nil

  /// Quality of the PHONE↔SERVER hop (0=down, 1=poor, 2=fluctuating, 3=good), as
  /// the phone's own link meter scores it.
  ///
  /// There are TWO radio links in series here — iPhone↔server (WebSocket over
  /// WiFi/cellular) and watch↔iPhone (WCSession over BT/WiFi) — and they fail
  /// INDEPENDENTLY. The watch can see its own hop fail, because rows stop arriving.
  /// It is blind to the far one. So a rough link could only ever be reported as
  /// "LINK ROUGH", which tells the user nothing they can act on and sent us round
  /// in circles in the field: a frozen waterfall with a working crown looks the
  /// same whichever hop broke.
  ///
  /// It rides the existing throttled state echo, so it costs no extra WCSession
  /// traffic — the one budget that must never move (see the wedge notes in
  /// VibeWatchModule.sendState). Defaults to good: a watch that has heard nothing
  /// yet must not accuse the server.
  @Published var serverLink = 3

  /// The iPhone's SYSTEM volume (0…1) — the real one, mirrored.
  ///
  /// THE WATCH CONTROLS EXACTLY ONE KNOB, and this is it. The first attempt drove an
  /// app-level GAIN instead: delivered loudness is `appGain × systemVolume`, two
  /// independent knobs, and the wrist could only see and turn one of them — so with the
  /// phone at 50% the meter read FULL while delivering half, and cranking it did
  /// nothing. The missing piece was never control. It was READBACK of the knob that
  /// actually matters.
  ///
  /// So this carries changes the watch did NOT make too — the phone's hardware buttons,
  /// Control Centre, a Bluetooth headset's own rocker — because a mirror that only
  /// reflects your own hand is not a mirror.
  @Published var volume = 1.0
  /// Muted is NOT volume-zero — that would lose the level you were listening at, so
  /// unmuting could not restore it. It gates playback and leaves the volume alone.
  @Published var muted = false

  /// THE WATCH'S OWN BATTERY, 0…1 (−1 = unknown).
  ///
  /// A live waterfall on a wrist costs ~34% of a core (measured), and this is the one app
  /// on the watch that a user might genuinely leave running on a hilltop with no charger.
  /// The system battery reading is two swipes away; the thing you are watching it for is
  /// right here. So it sits next to the clock, where a watch user already looks for it.
  ///
  /// Polled on a slow timer, NOT per frame — the reading changes on the order of minutes
  /// and this app has learned the hard way what per-frame work costs.
  @Published var battery: Double = -1

  /// WHERE YOU ARE, in words. "20m Ham Band", "MW Broadcast Band" — from the phone's own
  /// ITU-derived band plan, computed there and mirrored here.
  ///
  /// A frequency alone tells you nothing unless you already know the band plan by heart.
  /// The phone puts the band under the waterfall; the wrist had nowhere to say it — so it
  /// goes in the one piece of dead space a watch app has, the strip beside the clock.
  @Published var bandName = ""
  /// That band's colour, from the SAME table the phone's band plan draws with — red for
  /// ham, blue for broadcast, green for utility, orange for CB.
  @Published var bandColor: Color? = nil
  /// The band's EDGES, in Hz. Drawn as boundary marks on the ticker — which is worth more
  /// than tinting the strip: the label already says WHICH band you're in, but only a mark
  /// tells you how close you are to leaving it, and that is the thing you want to know
  /// while a crown is turning under your finger.
  @Published var bandLo = 0.0
  @Published var bandHi = 0.0

  private var batteryTimer: Timer?

  private func startBatteryMonitor() {
    let dev = WKInterfaceDevice.current()
    dev.isBatteryMonitoringEnabled = true
    let read = { [weak self] in
      let lvl = Double(WKInterfaceDevice.current().batteryLevel)   // −1 when unavailable
      DispatchQueue.main.async { self?.battery = lvl }
    }
    read()
    let t = Timer(timeInterval: 60, repeats: true) { _ in read() }
    RunLoop.main.add(t, forMode: .common)
    batteryTimer = t
  }

  // ── FM-DX ──────────────────────────────────────────────────────────────────
  //
  // A SECOND SCREEN, not a variant of the waterfall: FM-DX has no spectrum at all,
  // so the STATION is the content. We route on whichever message arrived last —
  // the phone never has to tell us which screen to be, because what it SENDS
  // already says: rows mean a spectrum, an fmdx blob means a station.
  @Published var fmdx: FmdxState? = nil
  /// Directory listings MIRRORED from the phone (keyed by directory id). Buddy keeps no server list of
  /// its own — the phone fetches on `browse` and sends the rows; Buddy just displays and references them.
  @Published var directories: [String: [SDRServer]] = [:]
  private struct DirMsg: Codable {
    let dir: String; let servers: [DirRow]
    struct DirRow: Codable { let id: String; let name: String; let type: String
                             let country: String?; let users: Int; let full: Bool; let dist: Double? }
  }
  /// The DAB multiplex, when the phone is on a DAB profile. DAB is a LIST, not a
  /// continuum: the crown SELECTS a service, it does not tune. (The phone already
  /// refuses to tune in DAB — a nudge knocks you off the ensemble block, killing the
  /// decode, and the block is hard to re-find.)
  @Published var dab: DabState? = nil
  /// OWRX ADS-B. Same shape of thing as DAB: the profile IS the content (1090 MHz),
  /// so there is nothing to tune and a waterfall of it is a slab of noise.
  @Published var aircraft: [Aircraft] = []
  @Published var logo: Data? = nil
  /// The phone's dial memory, mirrored — station names pinned to frequencies, learned
  /// from RDS as you tune. The wrist draws the same dial rather than inventing one.
  @Published var stations: [FmdxStation] = []
  /// The user's FAVOURITE instances — a curated handful, not the 2,000-server
  /// directory (which would be absurd on a wrist). This is what rescues the case
  /// where the watch LAUNCHES the phone and the phone has no default instance: the
  /// wrist can pick one instead of staring at nothing.
  @Published var favourites: [Favourite] = []
  /// What the PHONE is doing. A cold launch is a BOOT, not a fault — and the watch
  /// should say which, rather than reporting a missing waterfall as an error.
  /// starting | ready | pick | setup
  @Published var phoneStatus = "ready"
  /// Which screen the watch should be. COMPUTED, not "last message wins".
  ///
  /// FM-DX could be routed by arrival, because an FM-DX server sends no rows. DAB
  /// cannot: the spectrum keeps streaming on a DAB profile, so a row would flip us
  /// straight back to the waterfall. Route on the FACTS instead — what mode the phone
  /// is in, and whether we have a multiplex to show.
  var screen: Screen {
    if isFmdx { return .fmdx }
    if mode == "dab", let d = dab, !d.list.isEmpty { return .dab }
    if mode == "adsb" || !aircraft.isEmpty { return .adsb }
    return .sdr
  }

  /// Set by an FM-DX blob, cleared by a row — an FM-DX server has no spectrum, so
  /// these two can never both be true.
  @Published var isFmdx = false

  enum Screen { case sdr, fmdx, dab, adsb }

  struct Aircraft: Codable, Equatable, Identifiable {
    var icao = ""
    var flight: String? = nil     // callsign
    var reg: String? = nil        // registration
    var ccode: String? = nil      // ISO country of REGISTRY (not of departure)
    var country: String? = nil
    var altitude: Double? = nil   // ft
    var speed: Double? = nil      // kt
    var vspeed: Double? = nil     // ft/min
    var course: Double? = nil
    var squawk: String? = nil
    var rssi: Double? = nil
    var distKm: Double? = nil
    var bearing: Double? = nil
    var id: String { icao }
  }

  struct Favourite: Codable, Equatable, Identifiable {
    var name = ""
    var url = ""
    var type: String? = nil      // ubersdr | kiwi | owrx | fmdx | …
    var id: String { url }
  }

  struct DabService: Codable, Equatable, Identifiable {
    var id = 0            // audio_service_id — what you send to switch
    var name = ""
  }

  struct DabState: Codable, Equatable {
    var ensemble = ""     // the multiplex label, e.g. "BBC National DAB"
    var active = 0        // the audio_service_id currently decoding
    var list: [DabService] = []
  }

  struct FmdxStation: Codable, Equatable, Identifiable {
    var freqHz: Double = 0
    var name = ""
    var id: Double { freqHz }
  }

  struct FmdxState: Codable, Equatable {
    var freq: Double = 0      // Hz
    var ps = ""               // RDS station name
    var rt = ""               // RadioText
    var pi = ""               // PI code (hex)
    var sig: Double = 0       // dBf
    var users = 0             // listeners on the SERVER — see FmdxView
    var stereo = false
    var tx = ""               // transmitter / station name
    var city = ""             // transmitter site
    var dist: Double = 0      // km from the SERVER's QTH (not the listener's)
    var pty = ""              // programme type, already resolved by the phone
    var flag = ""             // country flag emoji
    var rx = ""               // where the RECEIVER is — `dist` is measured from HERE
    var meter = ""            // the phone's meter text, mirrored
    var level: Double = 0     // 0..1 bar fill
    // ── Server controls, relayed to/from the phone (thin-remote). ──
    var eq = false            // cEQ filter
    var ims = false           // iMS multipath suppression
    var ant = 0               // selected antenna (0-based)
    var antennas: [FmdxAntenna] = []   // advertised antennas; empty = no switch to show
  }

  /// Filter edges as Hz offsets from the carrier. NOT symmetric: LSB is entirely
  /// below (both negative), USB entirely above, CW offset. Drawing a single width
  /// about the centre would render every mode as AM.
  @Published var filtLo     = 0.0
  @Published var filtHi     = 0.0
  /// The phone's acrylic-VFO settings, mirrored.
  @Published var needle     = Color.white
  @Published var needleI    = 5.0
  /// Peak hold — MIRRORED from the phone, never decided here. The wrist showing peaks
  /// while the phone doesn't (or the reverse) would be two instruments disagreeing.
  @Published var peakHold   = true

  /// The unit the readout displays in — INPUT-AWARE, like the main app.
  ///
  /// Type 4582 in kHz and it reads back "4582.000 kHz", not "4.582 MHz". Rendering
  /// everything ≥1MHz as MHz was technically right and practically wrong: it threw
  /// away the frame of reference you were working in. Shortwave listeners think in
  /// kHz, hams think in MHz, and the unit you just typed says which you are.
  /// Crown-tuning keeps whatever you last chose.
  @Published var displayUnit: DisplayUnit = .auto {
    didSet { UserDefaults.standard.set(displayUnit.rawValue, forKey: "vibe.displayUnit") }
  }

  enum DisplayUnit: String {
    case auto, hz, khz, mhz
  }

  private var session: WCSession?

  private var heartbeat: Timer?

  override init() {
    super.init()
    if let raw = UserDefaults.standard.string(forKey: "vibe.displayUnit"),
       let u = DisplayUnit(rawValue: raw) {
      displayUnit = u
    }
  }

  func activate() {
    startBatteryMonitor()
    guard WCSession.isSupported() else { return }
    let s = WCSession.default
    s.delegate = self
    s.activate()
    session = s

    // ★ Drive the WaterfallBuffer like Jr: TELL it the feed rate so it doesn't slow-learn from the
    // 10fps default, drain dry and stall (which read as a creeping waterfall + laggy spectrum). The
    // phone always feeds Buddy at 5fps (MIN_ROW_MS 200 / forwarder 0.2s) — the right rate for BT.
    waterfall.setExpectedRowRate(Self.rowFps)

    // ★ NO AUTO COLD-BOOT (Stuart 2026-07-21). Buddy must NOT launch the phone app just by being
    // opened — an accidental open used to boot VibeSDR on the phone. Only RESUME the heartbeat for a
    // session that's already LIVE (wrist came back to a running, connected phone). A cold/accidental
    // open, or a phone we've been told is closed, lands on the START screen — the user taps ▶ Start
    // VibeSDR to launch/connect deliberately (that's the one place a ping is allowed to relaunch it).
    if everGotRow && !phoneClosed {
      startHeartbeat()   // resume a live session across a wrist-down/up
    } else {
      phoneClosed = true // show the Start screen; no ping, no boot
    }
  }

  /// The fixed phone→watch row rate. 10fps over Bluetooth (Stuart's rule); the buffer + the jitter-
  /// buffer slot interval are both driven off this, so it MUST match the phone forwarder's cadence
  /// (WatchSpectrumForwarder 60ms / watchProvider MIN_ROW_MS 60).
  static let rowFps = 10.0

  private func startHeartbeat() {
    heartbeat?.invalidate()
    let t = Timer(timeInterval: 4, repeats: true) { [weak self] _ in self?.ping() }
    RunLoop.main.add(t, forMode: .common)
    heartbeat = t
  }

  /// The phone told us it is CLOSED (goodbye, or a 'closed' status after an anti-hijack
  /// relaunch). STOP the heartbeat: every ping is a `sendMessage` that would relaunch the
  /// phone headless and pour SDR audio out its speaker mid-call. We go silent until the user
  /// explicitly asks to Reopen. This is the whole anti-hijack on the watch side.
  private func stopHeartbeat() { heartbeat?.invalidate(); heartbeat = nil }

  /// True from "Reopen App" until the phone actually comes back. While set, the heartbeat sends
  /// `reopen` instead of `ping` — a single reopen can be dropped while the relaunching phone's JS
  /// is still mounting its listener, so we keep asking until rows/ready prove it landed.
  private var reopenPending = false

  /// User pressed "Reopen App" on the closed screen. Resume poking the phone (a sendMessage
  /// relaunches it) and keep asking it to reopen until it does.
  func reopen() {
    phoneClosed = false
    deliberatelyClosed = false   // the user explicitly wants it back — release the latch
    reopenPending = true
    startHeartbeat()
    send(["cmd": "reopen"])
  }

  /// User pressed "Close Buddy". We cannot terminate a watchOS app from code, so go dormant:
  /// heartbeat off (nothing relaunches the phone), and the closed screen stays up with no
  /// Reopen pressure. The user can swipe Buddy away, or reopen the phone app to resume.
  func closeBuddy() { stopHeartbeat() }

  // MARK: - Watch -> Phone

  /// Tune by `delta` steps. We send a DELTA, never an absolute frequency: the
  /// phone stays the single source of truth and multiplies by the current step.
  // ── Crown commands are COALESCED ─────────────────────────────────────────────
  //
  // Every crown detent used to send its own WCSession message. Spin the crown and
  // that's 40-60 messages/sec, fired from the main thread, on top of the row stream
  // and its acknowledgements — and WCSession is an interactive-message channel that
  // QUEUES under load rather than dropping (the same lesson the row feed already
  // taught us). It saturates, and the app wedges: the watch locked up mid-tune.
  //
  // So: accumulate detents and flush at most ~16/sec. Deltas ADD, so nothing is
  // lost — a fast spin arrives as one big delta instead of forty small ones, which
  // is exactly what the phone wants anyway (it multiplies by the step size).
  private var pendingTune = 0
  private var pendingZoom = 0
  private var flushScheduled = false

  /// PREDICT while turning, ADOPT when still — the same pattern as the phone's own
  /// view sender (UberSDRClient._sendView / _armSettle).
  ///
  /// The readout must move WITH THE CROWN, not with the radio. Anything the phone
  /// sends us is a report of where the radio WAS, a WCSession hop ago at best.
  /// Steering off it means steering off the past: you turn, nothing moves, you turn
  /// further to compensate, and the number sails past what you wanted — laggy AND
  /// overshooting, which is exactly what it was.
  ///
  /// So the crown owns the number while it is moving, and the phone's `state` echo
  /// takes over once it stops. We must ignore state echoes DURING the turn too:
  /// they are throttled to 4/sec, so mid-spin one carries a frequency we have
  /// already tuned past, and adopting it yanks the readout backwards.
  ///
  /// The settle is generous — longer than the phone's echo throttle plus a hop —
  /// because the echo is TRAILING-EDGE: once the crown is quiet the phone always
  /// sends the final truth, and that truth is worth waiting for. It clamps to the
  /// band edges and snaps to the step grid, and we may be wrong about either.
  ///
  /// The prediction MUST use the phone's grid-snap maths, or the adopted value
  /// would visibly jump.
  private var tuneSettle: DispatchWorkItem?
  /// True while the readout is ours rather than the phone's.
  private var tuning: Bool { tuneSettle != nil }

  func tune(delta: Int) {
    predictTune(delta: delta)
    pendingTune += delta
    scheduleFlush()
  }

  /// An ARMED tune, from the FM-DX screen.
  ///
  /// FM-DX has ONE shared receiver, so tuning it moves the frequency for every
  /// listener — which is why the crown is disarmed by default there. But that gate
  /// lived only in the watch's UI, and the phone accepted any tune command it was
  /// given. When a navigation bug briefly left the watch on the WATERFALL screen
  /// while the phone was on FM-DX, the waterfall's crown tuned the shared receiver
  /// with no arming at all.
  ///
  /// So the assertion travels with the command: the watch says "this tune is armed",
  /// and the phone REQUIRES that before it will touch a shared tuner. The gate no
  /// longer depends on which screen the watch happens to be showing.
  func tuneArmed(delta: Int) {
    send(["cmd": "tune", "delta": delta, "armed": true])
  }

  func zoom(delta: Int) { pendingZoom += delta; scheduleFlush() }

  /// The crown, in volume mode. Carbon-copy of the tuning architecture, because every
  /// piece of it has already been debugged: coalesce the detents, PREDICT while turning,
  /// ADOPT the phone's echo when still.
  ///
  /// One detent = one 1/16 step, because 1/16 IS the iPhone's volume quantisation. A
  /// finer step would round to the same value and the crown would feel dead for a click
  /// or two at a time.
  ///
  /// DELTAS, never absolutes — the phone owns the knob, the wrist only nudges it. Same
  /// direction-of-truth rule as tuning, and for the same reason: an absolute computed on
  /// the watch is computed from a value that is at best one hop old.
  func volume(delta: Int) {
    predictVolume(delta: delta)
    pendingVol += delta
    scheduleFlush()
  }

  func setMuted(_ m: Bool) {
    muted = m                       // optimistic: the phone has no mute echo to wait for
    send(["cmd": "mute", "val": m])
  }

  private var pendingVol = 0
  private var volSettle: DispatchWorkItem?
  /// True while the volume readout is OURS rather than the phone's — so a throttled
  /// state echo carrying a level we have already turned past cannot yank the meter
  /// backwards mid-spin. Exactly the `tuning` flag, for exactly the same reason.
  private var volAdjusting: Bool { volSettle != nil }

  private func predictVolume(delta: Int) {
    volume = min(1, max(0, volume + Double(delta) / 16))
    armVolSettle()
  }

  /// Hand the meter back to the phone once the crown is still. The phone's echo carries
  /// the SNAPPED truth (iOS quantises to 1/16), which may differ from our prediction if
  /// we ran into 0 or 1 — so the phone's answer always wins, one clean step later.
  private func armVolSettle() {
    volSettle?.cancel()
    let work = DispatchWorkItem { [weak self] in
      guard let self else { return }
      self.volSettle = nil
      // ASK, don't wait — same trap as tuning. The phone echoes volume only when it
      // CHANGES, so a prediction that ran past 0 or 1 produced no change, no echo, and
      // the wrist would sit on a wrong value forever. A ping makes the phone state its
      // truth unconditionally.
      self.ping()
    }
    volSettle = work
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.6, execute: work)
  }

  private func predictTune(delta: Int) {
    guard step > 0, frequency > 0 else { return }   // nothing to predict from yet
    // Same snap the phone applies (SDRScreen.onTuneDelta): a detent lands ON the
    // step grid rather than offsetting the current fraction.
    let base = delta > 0 ? floor(frequency / step) : ceil(frequency / step)
    frequency = (base + Double(delta)) * step
    armTuneSettle()
  }

  /// Hand the readout back to the phone once the crown has been still long enough
  /// for the trailing-edge echo (≤250ms) plus a hop to land. Adopting early is what
  /// produces the backwards yank.
  private func armTuneSettle() {
    tuneSettle?.cancel()
    let work = DispatchWorkItem { [weak self] in
      guard let self else { return }
      self.tuneSettle = nil
      // ASK, don't wait. The phone only echoes the frequency when it CHANGES — so
      // if our prediction ran past a band edge (or the phone rejected the tune),
      // its frequency stopped moving, no echo was ever sent, and the watch would sit
      // on a wrong prediction forever with no way back. A ping makes the phone state
      // its truth unconditionally, so the wrist ALWAYS resyncs when the crown stops.
      self.ping()
    }
    tuneSettle = work
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.6, execute: work)
  }

  /// DispatchQueue, NOT Timer.
  ///
  /// `Timer.scheduledTimer` installs on the main run loop in DEFAULT mode — and
  /// while you are turning the crown or touching the screen, the run loop is in
  /// TRACKING mode, where default-mode timers DO NOT FIRE. So the flush never ran
  /// while the crown was moving, detents piled up, nothing was sent, and (because
  /// `flushScheduled` stayed true) it never rescheduled: the crown went
  /// permanently dead. The coalescing added to cure a hang caused a worse one.
  ///
  /// asyncAfter doesn't care what the run loop is doing.
  private func scheduleFlush() {
    guard !flushScheduled else { return }
    flushScheduled = true
    // 100ms, matching the UberSDR tune debounce the main app + Jr already use (UberClient
    // sendTuneThrottled — 100ms is UberSDR's supported rate). The phone can only feed the
    // SERVER at 1/100ms, so coalescing tighter than that just floods the watch→phone WC leg
    // with tune commands that compete with the rows for the one Bluetooth radio — the tuning
    // equivalent of the row-rate saturation. The readout stays instant either way: predictTune
    // moves it locally, the send rate only governs how often the phone hears about it.
    // Deltas ACCUMULATE across the window (pendingTune sums), so a longer window loses nothing.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
      self?.flushCrown()
    }
  }

  private func flushCrown() {
    flushScheduled = false
    if pendingTune != 0 { send(["cmd": "tune", "delta": pendingTune]); pendingTune = 0 }
    if pendingZoom != 0 { send(["cmd": "zoom", "delta": pendingZoom]); pendingZoom = 0 }
    if pendingVol  != 0 { send(["cmd": "vol",  "delta": pendingVol]);  pendingVol  = 0 }
  }
  /// Pick a DAB service. NOT a tune — the phone calls setAudioServiceId(), which
  /// re-sends the demod without touching the frequency.
  func selectDab(_ id: Int) { send(["cmd": "dab", "val": id]) }

  /// Switch the PHONE to another instance. Handled outside the SDR screen on the
  /// phone, because the whole point is that it works when no SDR screen is up.
  /// Pick a server for the PHONE to connect to. Send the TYPE (and name) too — a directory server the
  /// user hasn't favourited isn't in the phone's list, so without the type the phone can't tell an
  /// OWRX/Kiwi from an UberSDR and the connect fails.
  func selectInstance(_ url: String, type: String = "", name: String = "") {
    var msg: [String: Any] = ["cmd": "inst", "val": url]
    if !type.isEmpty { msg["type"] = type }
    if !name.isEmpty { msg["name"] = name }
    send(msg)
  }

  func setMode(_ m: String) { send(["cmd": "mode", "val": m]) }
  func setStep(_ hz: Double) { send(["cmd": "step", "val": hz]) }

  /// Ask the PHONE to browse a directory and send us its servers (we mirror; we never fetch our own).
  func browse(_ dir: String) { send(["cmd": "browse", "dir": dir]) }

  /// STOP — tell the phone to leave the SDR screen: that tears down the client (audio + sockets stop)
  /// and returns the phone to its server list to WAIT, idle. Unlike pause (which stays on the stopped
  /// waterfall), this disconnects AND goes back. Buddy follows to the picker via the phone's status.
  func stopServer() { send(["cmd": "stopsrv"]) }

  // ── Thin-remote server controls: the watch relays the tap; the phone runs the command. ──
  /// SDR passband — send both edges (Hz offsets from carrier); the phone calls setBandwidth.
  func setBandwidth(lo: Double, hi: Double) { send(["cmd": "bw", "lo": lo, "hi": hi]) }
  /// FM-DX server toggles + antenna select.
  func setFmdxEq(_ on: Bool)  { send(["cmd": "fmdxEq",  "val": on]) }
  func setFmdxIms(_ on: Bool) { send(["cmd": "fmdxIms", "val": on]) }
  func setFmdxAntenna(_ id: Int) { send(["cmd": "fmdxAnt", "val": id]) }
  func ping() {
    // Anti-hijack: never poke a phone the user deliberately closed — a ping relaunches it.
    guard !phoneClosed else { return }

    // SILENCE WATCHDOG. If the phone has sent us NOTHING — no rows AND no state — for a good while
    // while we had a live session, it has almost certainly been closed. The goodbye (transferUserInfo)
    // is slow and unreliable, and the sendMessage goodbye needs isReachable, which is false at
    // termination — so we can't count on being TOLD. Raise the Start screen ourselves and go silent
    // (stop pinging a phone that's gone — that ping is what relaunches it, the hijack). NOT
    // deliberatelyClosed: if the phone was merely blipping and pushes a row, handleRow un-closes us.
    if everGotRow, !isBackground {
      // Give a wrist-up (resumedAt) or a fresh row/state a few seconds to reconnect before concluding
      // the phone is gone — the ContentView shows a "Reconnecting to VibeSDR" pill over the held
      // waterfall meanwhile. Only past the grace do we raise the Start / Connect-to-iPhone screen.
      let lastHeard = max(lastRowAt ?? .distantPast, lastStateAt ?? .distantPast, resumedAt)
      if Date().timeIntervalSince(lastHeard) > 5 {
        phoneClosed = true
        stopHeartbeat()
        return
      }
    }

    // Mid-reopen: keep asking to reopen (a single reopen can be dropped during relaunch).
    send(reopenPending ? ["cmd": "reopen"] : ["cmd": "ping"])

    // ONE-WAY LINK RECOVERY, from this end.
    //
    // Our messages clearly reach the phone (the crown keeps tuning), and nothing comes
    // back. WCSession can get stuck like that after a transport hop, and neither side is
    // told. The phone re-activates when its sends start failing; do the same here, since
    // a session the WATCH is holding wrong is not something the phone can fix.
    //
    // Re-activating an active session is a no-op, so this is safe — but rate-limit it,
    // because it is a blunt instrument.
    if let t = lastRowAt, Date().timeIntervalSince(t) > 12,
       Date().timeIntervalSince(lastReviveAt) > 15 {
      lastReviveAt = Date()
      session?.activate()
    }
  }

  private var lastReviveAt = Date.distantPast

  /// ASK FOR WHAT WE HAVEN'T GOT.
  ///
  /// The palette LUT is sent ONCE, when the colormap changes. If that single message
  /// is dropped — very likely while WCSession is still settling right after a launch —
  /// nothing ever sends it again, and the watch sits in its greyscale fallback until
  /// something unrelated happens to trigger a resend. (It came back when the screen
  /// woke, because that pings.)
  ///
  /// The watch KNOWS it has no palette. So it should say so rather than wait to be
  /// noticed. Same principle as the heartbeat: recency beats hope.
  private var lastNeedAt = Date.distantPast
  func requestMissing() {
    guard Date().timeIntervalSince(lastNeedAt) > 3 else { return }
    lastNeedAt = Date()
    send(["cmd": "need"])
  }


  /// Absolute tune, from the numpad. The one place the watch sends a frequency
  /// rather than a delta — the phone still clamps it to the receiver's range.
  ///
  /// Shows the typed frequency at once (you typed it; waiting a round trip to see
  /// it is absurd), but ALSO drops the settle so the phone's answer is adopted as
  /// soon as it lands — that answer may differ, because the phone clamps to the
  /// band edges and we don't know where those are.
  func tune(toHz hz: Double) {
    send(["cmd": "freq", "val": hz])
    frequency = hz
    armTuneSettle()
  }

  private func send(_ msg: [String: Any]) {
    guard let s = session, s.isReachable else { return }
    s.sendMessage(msg, replyHandler: nil, errorHandler: nil)
  }

  /// Wrist dropped. Buddy has no socket of its own to suspend, but it MUST mark itself backgrounded
  /// immediately: watchOS suspends us in a moment (no background audio), the row feed stops, and
  /// without this the resulting gap reads as a rough link and shows the 'reconnecting' glyph. Also
  /// tell the phone to stop forwarding the waterfall we can't see after the user's grace period —
  /// battery on BOTH ends (the phone owns the grace timer; ours would freeze the instant we suspend).
  func suspend(graceSeconds: Double = 0) {
    isBackground = true
    send(["cmd": "wrist", "down": true, "grace": graceSeconds])
  }

  /// Wrist back up: clear the background flag (the hint logic runs again) and tell the phone to resume
  /// forwarding, then re-request anything we missed.
  func resume() {
    isBackground = false
    resumedAt = Date()   // restart the silence grace — don't flash Start before rows can return
    send(["cmd": "wrist", "down": false])
    requestMissing()
  }

  // MARK: - Phone -> Watch

  func session(_ s: WCSession, didReceiveMessage message: [String: Any]) {
    // Hop to main: we mutate @Published state and the pixel buffer.
    DispatchQueue.main.async { self.apply(message) }
  }

  /// The GOODBYE arrives here: the phone sends it via transferUserInfo on termination so it
  /// survives the process death (a fire-and-forget sendMessage would be dropped mid-exit).
  func session(_ s: WCSession, didReceiveUserInfo userInfo: [String: Any]) {
    DispatchQueue.main.async { self.apply(userInfo) }
  }

  /// Rows arrive as a flat binary blob (see VibeWatchModule.sendRow) — dictionary
  /// messages cost a plist serialise per send, and at 10fps that flooded the
  /// channel into delivering in bursts.
  func session(_ s: WCSession, didReceiveMessageData data: Data) {
    handleRow(data)
  }

  /// Kept only in case the phone ever sends with a reply handler — answer at once
  /// and never make the ack wait on rendering. (The phone does NOT do this: rows
  /// are fire-and-forget. Ack-based backpressure was tried and reverted — it made
  /// the send rate a hostage to the round trip and the link degraded badly.)
  func session(_ s: WCSession, didReceiveMessageData data: Data,
               replyHandler: @escaping (Data) -> Void) {
    replyHandler(Data())
    handleRow(data)
  }

  /// Bytes the meter text occupies in the row blob — MUST match
  /// VibeWatchModule.meterBytes, or the row is sliced at the wrong offset and every
  /// row is silently dropped for being the wrong length.
  private static let meterBytes = 12
  /// One row's payload: 7 doubles (freq, span, snr, level, lo, hi, SQL) + the meter field + the bins.
  private static let blockSize = 8 * 7 + meterBytes + WaterfallBuffer.width

  // ── Jitter buffer (Stuart, 2026-07-21: "Buddy feels super sluggish vs Jr") ──────────────────────
  //
  // Buddy USED to push rows straight into the waterfall the instant a WCSession message landed. But
  // WCSession delivers over Bluetooth in CLUMPS — several rows at once, then a gap — so the buffer's
  // scroll would fast-forward through a clump and then stall until the next one (the sluggish, lurchy
  // feel), and worse, the buffer's own interval estimator learned the ~0.6s BATCH cadence instead of
  // the true 5fps, scrolled too slow, backed up past its 4-row cap and DROPPED frames.
  //
  // Jr never had this because its clients drain a jitter queue every frame (drainSpectrum). Buddy now
  // does the same: rows land HERE, and driverTick() releases them onto a smooth ~5fps cadence — so the
  // waterfall gets fed evenly (its interval estimator settles on the real rate) and the trace/scroll
  // interpolation has an even cadence to glide along, exactly as on Jr.
  private var pendingRows: [(slot: Double, row: [UInt8])] = []
  private var lastSlot = 0.0
  /// A small cushion (like Jr's spectrumDelay) so there's always a little buffer to absorb WCSession
  /// micro-jitter before a row is due — rebuilt after any gap.
  private static let rowCushion = 0.12

  /// Schedule a batch of just-arrived rows onto EVENLY-SPACED release slots. A Bluetooth clump lands
  /// with the same timestamp, so instead of pushing them together we hand each the next slot one
  /// `interval` after the last — spreading the clump across future frames at the true row rate. After
  /// a gap the schedule snaps back to `now + cushion`, so a slow feed never accrues artificial latency.
  private func enqueueRows(_ rows: [[UInt8]]) {
    let now = ProcessInfo.processInfo.systemUptime
    let interval = 1.0 / Self.rowFps
    for r in rows {
      lastSlot = max(now + Self.rowCushion, lastSlot + interval)
      pendingRows.append((lastSlot, r))
    }
    if pendingRows.count > 12 { pendingRows.removeFirst(pendingRows.count - 12) }   // hard backstop
  }

  /// Called every render frame from ContentView. Release any rows whose scheduled slot has arrived —
  /// a FIXED-cadence jitter buffer (Jr's model), NOT depth-adaptive: a second rate loop fighting
  /// tickScroll's own depth adaptation is what left the trace jerky while the waterfall looked fine.
  func driverTick(now: Double) {
    while let first = pendingRows.first, first.slot <= now {
      waterfall.push(row: first.row)
      pendingRows.removeFirst()
    }
  }

  /// Rows arrive BATCHED — several in one message.
  ///
  /// Not an optimisation: a correction. `WCSession.sendMessage` is an INTERACTIVE
  /// channel (individually framed and queued, meant for occasional request/response),
  /// and we were making sixteen calls a second at it. The data was trivial — ~5 KB/s —
  /// but the MESSAGE RATE was far outside what the channel is built for, and that is
  /// what kept wedging it, backing it up, and leaving it one-way after a transport hop.
  ///
  /// Fewer, bigger messages. The jitter buffer already expects bursty arrivals, so a
  /// pair landing together is exactly what it was designed for.
  private func handleRow(_ data: Data) {
    guard data.count > 2, data[data.startIndex] == 2 else { return }
    let count = Int(data[data.startIndex + 1])
    guard count > 0, data.count >= 2 + count * Self.blockSize else { return }

    var rows: [[UInt8]] = []
    var meterText = ""
    var f = [Double](repeating: 0, count: 7)

    for b in 0..<count {
      let base = data.startIndex + 2 + b * Self.blockSize
      for i in 0..<7 {
        let lo = base + i * 8
        let bits = data[lo..<(lo + 8)].withUnsafeBytes { $0.loadUnaligned(as: UInt64.self) }
        f[i] = Double(bitPattern: UInt64(littleEndian: bits))
      }
      let mStart = base + 8 * 7
      let mBytes = data[mStart..<(mStart + Self.meterBytes)].prefix { $0 != 0 }
      let t = String(decoding: mBytes, as: UTF8.self)
      if !t.isEmpty { meterText = t }

      let rStart = mStart + Self.meterBytes
      rows.append([UInt8](data[rStart..<(rStart + WaterfallBuffer.width)]))
    }

    let latest = f   // the newest block's header wins — it IS the current state
    DispatchQueue.main.async {
      self.enqueueRows(rows)   // jitter buffer — driverTick releases them on evenly-spaced slots
      self.lastRowAt = Date()
      // Rows streaming again means the phone is back (reopened) — drop the closed screen.
      // ★ UNLESS the phone was DELIBERATELY closed (goodbye): a stale in-flight row that lands just
      // after the goodbye must NOT un-close us and restart the heartbeat — that heartbeat is exactly
      // what relaunches the phone headless (the hijack). Only a user Reopen or a real phone status
      // clears the deliberate-close latch. (Stuart, 2026-07-21: "if I close the phone, Buddy reopens it.")
      if !self.deliberatelyClosed {
        self.reopenPending = false
        if self.phoneClosed { self.phoneClosed = false; if self.heartbeat == nil { self.startHeartbeat() } }
      }
      if !self.waterfall.hasLUT { self.requestMissing() }
      if !meterText.isEmpty { self.meter = meterText }
      if rows.first?.count == WaterfallBuffer.width { self.everGotRow = true }
      // A spectrum row means we've left FM-DX/DAB — clear their state so an old RDS name (stationName
      // = fmdx?.ps) or DAB ensemble can't bleed onto the SDR ticker (Stuart: "PRIDE on a shortwave
      // session"). Guarded so it publishes on the TRANSITION only, not on every 10fps row.
      if self.isFmdx { self.isFmdx = false }     // see `screen`
      if self.fmdx != nil { self.fmdx = nil }
      if self.dab != nil { self.dab = nil }
      // NOTE: the row's frequency is deliberately IGNORED — rows are lossy pixels and
      // can be queued; the readout comes from the throttled `state` echo and from our
      // own prediction while the crown moves.
      self.span      = latest[1]
      self.snr       = latest[2]
      self.level     = latest[3]
      self.filtLo    = latest[4]
      self.filtHi    = latest[5]
      self.sql       = latest[6]   // squelch threshold (0..1 bar position, -1 = off)
    }
  }

  private func apply(_ m: [String: Any]) {
    lastAnyAt = Date()      // stamped before the switch, so every message kind counts
    switch m[WK.kind] as? String {
    case "row":
      if let d = m[WK.row] as? Data {
        enqueueRows([[UInt8](d)])   // jitter buffer — driverTick releases it on an evenly-spaced slot
        if d.count == WaterfallBuffer.width { everGotRow = true }
      }
      // Rows never set the frequency — see handleRow.
      if let sp = m[WK.span] as? Double { span = sp }
      if let s = m[WK.snr] as? Double { snr = s }
      if let lv = m[WK.level] as? Double { level = lv }
      if let l = m[WK.filtLo] as? Double { filtLo = l }
      if let hh = m[WK.filtHi] as? Double { filtHi = hh }

    case "state":
      if let f = m[WK.freq] as? Double, !tuning { frequency = f }
      if let md = m[WK.mode] as? String {
        mode = md
        // A STATE MESSAGE MUST BE ABLE TO CONTRADICT THE CURRENT SCREEN, not just
        // confirm it. The screen is computed from facts — but nothing ever cleared
        // the facts, so they went stale and outvoted reality.
        //
        // Symptom: force-quit the phone app with the watch open on ADS-B. The watch
        // relaunches the phone (as a background process), the phone comes up on its
        // default UberSDR instance — and the watch sits there STILL SHOWING THE
        // AIRCRAFT LIST, because it was still holding the last aircraft table and
        // routing on it. It was faithfully rendering a radio that no longer exists.
        //
        // The phone always announces its mode, so let the mode retire whatever it
        // isn't: leaving ADS-B clears the aircraft, leaving DAB clears the mux.
        if md != "adsb" { aircraft = [] }
        if md != "dab"  { dab = nil }
        // Only the SDR screen sends `state` at all — FM-DX sends its own blob — so
        // receiving one is itself proof we are no longer on an FM-DX server.
        //
        // THIS IS AN INVARIANT AND IT IS LOAD-BEARING. Volume briefly rode the `state`
        // echo, and because system volume changes on every screen, an FM-DX volume echo
        // sent a `state` message — which landed here and threw the wrist off FM-DX and
        // onto the waterfall, mid-turn, while the crown was being rolled. Volume now has
        // its own message (case "vol" below), which asserts nothing about the screen.
        // Anything that is true on BOTH screens must never travel on this one.
        isFmdx = false
      }
      if let st = m[WK.step] as? Double { step = st }
      if let mt = m[WK.meter] as? String { meter = mt }
      if let w = m[WK.why] as? String { why = w }
      if let lk = m[WK.link] as? Int { serverLink = lk }
      if let bn = m[WK.band] as? String { bandName = bn }
      if let bc = m[WK.bandCol] as? String { bandColor = hexColor(bc) }
      if let bl = m[WK.bandLo] as? Double { bandLo = bl }
      if let bh = m[WK.bandHi] as? Double { bandHi = bh }
      // NO VOLUME HERE — it has its own message (case "vol"). See the isFmdx note above.
      lastStateAt = Date()
      if let lv = m[WK.level] as? Double { level = lv }

    // The iPhone's SYSTEM volume. Its OWN message, deliberately: it is a fact about the
    // DEVICE, true on every screen, so it must not travel on `state` (which asserts that
    // the SDR screen is up — see the isFmdx note above). Note this case touches NOTHING
    // but volume and mute: it must never route, never clear, never claim a screen.
    case "vol":
      if !volAdjusting, let vo = m[WK.vol] as? Double {
        volume = min(1, max(0, vo))
      }
      if let mu = m[WK.muted] as? Bool { muted = mu }

    case "fmdx":
      if let j = m[WK.json] as? String,
         let d = j.data(using: .utf8),
         let st = try? JSONDecoder().decode(FmdxState.self, from: d) {
        fmdx = st
        isFmdx = true
        lastRowAt = Date()        // "the phone is talking to us" — same staleness clock
        everGotRow = true
      }

    case "air":
      if let j = m[WK.json] as? String, let d = j.data(using: .utf8),
         let list = try? JSONDecoder().decode([Aircraft].self, from: d) {
        aircraft = list
      }

    case "dab":
      if let j = m[WK.json] as? String,
         let d = j.data(using: .utf8),
         let st = try? JSONDecoder().decode(DabState.self, from: d) {
        dab = st
      }

    case "goodbye":
      // The phone is gone (user swiped it closed). Show the closed screen and go silent.
      phoneClosed = true
      deliberatelyClosed = true
      stopHeartbeat()

    case "phone":
      if let st = m["st"] as? String {
        // 'starting' EDGE = a NEW session is beginning (connect / reopen / instance switch).
        let newSession = st == "starting" && phoneStatus != "starting"
        phoneStatus = st
        if st == "closed" {
          // A heartbeat relaunched the phone headless but it refused to auto-connect and told
          // us so. Same destination as a goodbye — UNLESS we're mid-reopen, where a stale
          // 'closed' from the launch race must not bounce us back off the reopen.
          if !reopenPending { phoneClosed = true; deliberatelyClosed = true; stopHeartbeat() }
        } else if deliberatelyClosed {
          // ★ THE HIJACK GUARD. We were deliberately closed (goodbye). A stray heartbeat ping — sent
          //   in the seconds after the swipe while isReachable was still stale-true — can relaunch the
          //   phone headless; if it then auto-connects and sends 'starting'/'ready', that status must
          //   NOT drag us back off the Start screen. Staying latched here (heartbeat already stopped)
          //   means the watch sends nothing more, so there are no further pings to relaunch it either.
          //   ONLY a user Reopen (reopen() clears the latch) brings us back. We still record the raw
          //   status above for display, but take no action on it.
          break
        } else {
          // Any other status means the phone is alive and doing something real (a fresh connect, or a
          // reopen we asked for) — leave the closed screen and stop asking to reopen.
          reopenPending = false
          if phoneClosed { phoneClosed = false; if heartbeat == nil { startHeartbeat() } }
          // TEAR DOWN the previous session (Stuart: "everything between sessions needs tearing
          // down"). Otherwise an old FM-DX RDS name (fmdx.ps) / DAB ensemble / aircraft list /
          // band bleeds into a different backend — e.g. "PRIDE" showing on a shortwave session.
          if newSession {
            fmdx = nil; isFmdx = false; dab = nil; aircraft = []
            everGotRow = false; meter = ""
            bandName = ""; bandColor = nil; bandLo = 0; bandHi = 0
            waterfall.reset(); waterfall.setExpectedRowRate(Self.rowFps)  // fresh buffer, seeded rate
            pendingRows.removeAll(); lastSlot = 0                          // and a fresh jitter queue
          }
        }
      }

    case "favs":
      if let j = m[WK.json] as? String,
         let d = j.data(using: .utf8),
         let list = try? JSONDecoder().decode([Favourite].self, from: d) {
        favourites = list
      }

    case "dir":
      // A directory listing the PHONE fetched (we asked via browse). Mirror it — no local fetching.
      if let j = m[WK.json] as? String, let d = j.data(using: .utf8),
         let msg = try? JSONDecoder().decode(DirMsg.self, from: d) {
        directories[msg.dir] = msg.servers.map { r in
          SDRServer(name: r.name, url: r.id, host: URL(string: r.id)?.host ?? r.id,
                    serverType: ServerType(rawValue: r.type) ?? .ubersdr,
                    countryCode: r.country, distance: r.dist, users: r.users, full: r.full)
        }
      }

    case "stations":
      if let j = m[WK.json] as? String,
         let d = j.data(using: .utf8),
         let list = try? JSONDecoder().decode([FmdxStation].self, from: d) {
        stations = list
      }

    case "logo":
      // Empty = no logo found. Keep it nil so the view falls back to the app icon:
      // glass over nothing reads as a broken grey box.
      if let d = m[WK.image] as? Data { logo = d.isEmpty ? nil : d }

    case "settings":
      if let l = m[WK.lut] as? Data, l.count == 1024 { waterfall.setLUT([UInt8](l)) }
      if let sm = m[WK.smooth] as? Double { waterfall.smoothing = sm }
      if let nc = m[WK.needle] as? String, let c = Color(hex: nc) { needle = c }
      if let ni = m[WK.needleI] as? Double { needleI = ni }
      if let sh = m[WK.sharp] as? Double { waterfall.sharpness = sh }
      if let pk = m[WK.peak] as? Bool {
        peakHold = pk
        waterfall.peakHold = pk   // clears what's held when turned off
      }

    default:
      break
    }
  }

  // MARK: - WCSessionDelegate

  func session(_ s: WCSession, activationDidCompleteWith state: WCSessionActivationState, error: Error?) {
    DispatchQueue.main.async { self.reachable = s.isReachable }
  }

  func sessionReachabilityDidChange(_ s: WCSession) {
    DispatchQueue.main.async { self.reachable = s.isReachable }
  }
}

extension Color {
  /// "#rrggbb" as sent by the phone's VFO colour picker.
  init?(hex: String) {
    var s = hex.trimmingCharacters(in: .whitespaces)
    if s.hasPrefix("#") { s.removeFirst() }
    guard s.count == 6, let v = UInt32(s, radix: 16) else { return nil }
    self.init(
      red:   Double((v >> 16) & 0xff) / 255,
      green: Double((v >>  8) & 0xff) / 255,
      blue:  Double( v        & 0xff) / 255
    )
  }
}
