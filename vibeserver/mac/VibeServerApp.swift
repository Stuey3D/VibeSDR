// VibeServer for macOS — a menu-bar app around the shared C++ core.
//
// The GUI is a RENDERER, never a second source of truth: every decision (which port, whether a PIN
// is required, what the ceilings are) lives in the core, and this file only presents it and passes
// changes down. That is what keeps the Mac app, the CLI harness and the future Pi daemon behaving
// identically instead of drifting into three subtly different servers.
//
// ★ AppKit's NSStatusItem rather than SwiftUI's MenuBarExtra, deliberately. MenuBarExtra opens its
// panel on a left click and offers no way to separate the two buttons; the interaction we want is
// LEFT-CLICK = listen (open the web client), RIGHT-CLICK = manage. That is only reachable through
// NSStatusItem, so the menu bar is AppKit and the settings window is SwiftUI inside it.

import SwiftUI
import AppKit
import ServiceManagement
import CoreLocation

// ── Coarse location from the Mac itself ──────────────────────────────────────
// ★★ DELIBERATELY COARSE, and it fills the LOCATOR rather than the coordinate fields.
// The operator is publishing where their ANTENNA is to anyone who connects, and a 6-character
// Maidenhead square (~4 x 2.5 km) gives every consumer — distances, bearings, map centring,
// ITU region, RDS country — exactly what it needs without announcing a home address. That is
// the same trade the manual "Or locator" field already offers; this just removes the typing.
// ★ macOS is asked for REDUCED accuracy for the same reason: requesting precision we then
// throw away would be a worse permission prompt for no benefit.
@MainActor
final class LocationFinder: NSObject, ObservableObject, CLLocationManagerDelegate {
    @Published var busy = false
    /// Human-readable outcome for the Settings pane — including refusals, which must be
    /// explained rather than left as a button that silently does nothing.
    @Published var status = ""
    /// ★ Orange is the colour of a problem. The same field now carries a SUCCESS message, and
    /// showing "it worked" in warning orange would undo the reassurance it exists to give.
    @Published var statusIsError = true
    private let mgr = CLLocationManager()
    private var onFix: ((Double, Double, String?, String?) -> Void)?
    private var timeout: Timer?

    override init() {
        super.init()
        mgr.delegate = self
        mgr.desiredAccuracy = kCLLocationAccuracyKilometer   // we only need the square
    }

    /// Ask for permission if needed, then a fix. `done` gets lat, lon, and the
    /// reverse-geocoded town and ISO country when those are available.
    /// ★★ THREE FAULTS FIXED after HansVanEijsden hit all of them (2026-07-27): the button
    /// stuck on "Locating…" forever with no prompt, permission had to be granted by hand, and
    /// the eventual result was "kCLErrorDomain error 0".
    func find(_ done: @escaping (Double, Double, String?, String?) -> Void) {
        onFix = done
        status = ""
        // ★ SYSTEM-WIDE LOCATION SERVICES CAN BE OFF, and then requestWhenInUseAuthorization()
        // shows NO PROMPT and reports nothing — which is exactly the "no privacy popup, button
        // just greys out" Hans saw. It is a different condition from our app being denied, and
        // it needs a different instruction, so it is checked separately and first.
        guard CLLocationManager.locationServicesEnabled() else {
            statusIsError = true; status = "Location Services is switched off for this Mac — System Settings ▸ "
                   + "Privacy & Security ▸ Location Services. You can type a locator instead."
            return
        }
        switch mgr.authorizationStatus {
        case .notDetermined:
            begin()
            mgr.requestWhenInUseAuthorization()   // the fix follows in the delegate
        case .denied, .restricted:
            // ★ Point at the fix. "Denied" with no route to undo it reads as a broken button.
            statusIsError = true; status = "Location is turned off for VibeServer — System Settings ▸ Privacy & "
                   + "Security ▸ Location Services. You can still type a locator instead."
        default:
            begin()
            requestFix()
        }
    }

    /// ★★ ALWAYS TIME OUT. Every failure mode here is silent — no prompt, no callback, no
    /// error — and without a deadline the button simply says "Locating…" until the app is
    /// restarted, which is what Hans reported. A control that cannot fail visibly is worse
    /// than one that fails.
    private func begin() {
        busy = true
        timeout?.invalidate()
        timeout = Timer.scheduledTimer(withTimeInterval: 20.0, repeats: false) { [weak self] _ in
            Task { @MainActor in
                guard let self, self.busy else { return }
                self.finish()
                self.statusIsError = true; self.status = "Could not get a location in time. A Mac locates itself from "
                            + "nearby Wi-Fi networks, so this can fail indoors or on a wired "
                            + "connection — type a locator instead."
            }
        }
    }

    private func finish() {
        busy = false
        timeout?.invalidate(); timeout = nil
        mgr.stopUpdatingLocation()
    }

    /// ★ startUpdatingLocation, NOT requestLocation. requestLocation is ONE shot and gives up
    /// with kCLErrorLocationUnknown if the first attempt misses — which is what produced Hans's
    /// "kCLErrorDomain error 0" (that IS kCLErrorLocationUnknown, and it is usually transient).
    /// Continuous updates let a fix arrive late; our own timeout bounds the wait.
    private func requestFix() { mgr.startUpdatingLocation() }

    nonisolated func locationManagerDidChangeAuthorization(_ m: CLLocationManager) {
        Task { @MainActor in
            switch m.authorizationStatus {
            case .authorized, .authorizedAlways:
                self.begin()
                self.requestFix()
            case .denied, .restricted:
                self.finish()
                self.statusIsError = true; self.status = "Permission refused. Type a locator or coordinates instead."
            default: break   // still undetermined: the prompt is on screen
            }
        }
    }

    nonisolated func locationManager(_ m: CLLocationManager, didUpdateLocations locs: [CLLocation]) {
        guard let l = locs.last else { return }
        // Reverse-geocode for the town and country. ★ Best-effort: a fix with no name is still
        // a perfectly good fix, so the callback fires either way rather than failing the lot.
        CLGeocoder().reverseGeocodeLocation(l) { marks, _ in
            let pm = marks?.first
            Task { @MainActor in
                self.finish()
                self.status = ""
                self.onFix?(l.coordinate.latitude, l.coordinate.longitude,
                            pm?.locality ?? pm?.subAdministrativeArea, pm?.isoCountryCode)
            }
        }
    }

    nonisolated func locationManager(_ m: CLLocationManager, didFailWithError e: Error) {
        Task { @MainActor in
            // ★ kCLErrorLocationUnknown is TRANSIENT — Core Location is saying "not yet", not
            // "never". Reporting it as a failure is what turned a slow fix into an error
            // message for Hans. Keep waiting; the timeout is what ends this.
            if (e as? CLError)?.code == .locationUnknown { return }
            self.finish()
            self.statusIsError = true; self.status = "Could not get a location: \(e.localizedDescription)"
        }
    }
}

// ── Server: the one object that owns the core ────────────────────────────────
@MainActor
final class Server: ObservableObject {
    @Published var running = false

    /// ★ SIMPLE vs FULL. Simple is the plug-a-radio-in pane this app has always had. Full trims
    ///   it to the radio, the two secrets and Start, and moves the rest to the browser setup page
    ///   — the same page Linux uses, so a public receiver is configured one way everywhere.
    @AppStorage("fullMode") var fullMode = false

    /// ── Full mode's own state. None of it is read in Simple mode. ────────────
    /// The radios the owner has ticked to serve. Rebuilt by rescan, un-ticks preserved.
    @Published var fullRadios: [FullMode.Radio] = []
    /// Two attached radios reporting the same serial cannot be told apart, so each one's settings
    /// could follow the other. Surfaced in the pane, as the TUI surfaces it in the wizard.
    @Published var fullSerialsCollide = false
    /// Listeners across every radio, asked of the server itself — see startFullPolling().
    @Published var fullListeners = 0
    /// The front door we spawned. Non-nil only while Full mode is serving.
    /// ★ Held so we can stop it: an orphaned front door keeps the radios (and the port), and the
    ///   next Start would fail with "address already in use" for no visible reason.
    private var frontDoor: Process?

    /// True when the admin password below was GENERATED rather than chosen. The pane then shows
    /// it in the clear, because a secret nobody has ever seen protects nothing and helps no one.
    @AppStorage("generatedAdmin") var generatedAdmin = false

    /// ★ NO 0/O/1/I/l. This is read off a screen and typed on a phone, and one ambiguous
    ///   character turns a working password into "the admin login is broken".
    static func generatedPassword() -> String {
        let alphabet = Array("ABCDEFGHJKMNPQRSTUVWXYZ23456789")
        let word = { String((0..<4).map { _ in alphabet.randomElement()! }) }
        return "\(word())-\(word())"
    }
    @Published var port: Int = 0
    @Published var lastError: String?
    @Published var status = VsStatus()
    @Published var deviceName = ""
    @Published var deviceCount = 0
    /// ★ ALL attached receivers, not just the first. The app read vs_device_name(0) and
    /// started device 0 unconditionally — which was invisible while a dongle was the only
    /// thing anyone owned, and became "I can't see the SDRplay" the moment a second radio
    /// was plugged in beside it (Stuart, 2026-07-26).
    @Published var devices: [String] = []
    /// ★ The radio we are ACTUALLY serving from. Re-enumerating while running reports
    /// NOTHING on an SDRplay — the API hands a device to one process at a time, so once we
    /// have selected it, it stops appearing in the available list. The settings window then
    /// said "no radio found" over a perfectly working waterfall, which is precisely the kind
    /// of false alarm that destroys trust in every later warning (Stuart, 2026-07-26).
    @Published var activeDevice = ""
    /// ★ The SDRplay API is not answering. Shown, not waited on — see vs_sdrplay_api_stuck.
    @Published var sdrplayStuck = false
    /// Starting is asynchronous now, so the menu can say so instead of appearing frozen.
    @Published var starting = false
    /// Whether the radio in use is an RSP. Its capabilities differ enough from a dongle's
    /// that several menus would otherwise misrepresent it.
    var isSdrplayActive: Bool {
        let n = running && !activeDevice.isEmpty ? activeDevice : deviceName
        return n.localizedCaseInsensitiveContains("SDRplay")
    }
    /// Which one to serve. Persisted, and matched BY NAME on the next launch — indices
    /// renumber when something is unplugged, and silently serving a different radio than
    /// last time is worse than asking again.
    @AppStorage("deviceName") var wantedDevice = ""

    // Persisted so a restart keeps the operator's choices; the core still owns what they MEAN.
    @AppStorage("centreHz") var centreHz   = 96_600_000.0
    @AppStorage("mode")     var mode       = "wfm"
    @AppStorage("pin")      var pin        = ""
    /// ★ A SECOND secret, gating CONTROL not ACCESS — see VsConfig.adminPassword. A public
    /// receiver is usually open to every listener and must still refuse a stranger switching
    /// the bias-T on.
    @AppStorage("adminPassword") var adminPassword = ""
    /// ★ Per-listener time limit, minutes. 0 = unlimited, and the right answer for a private
    /// receiver. See the help text beside the picker.
    @AppStorage("sessionLimitMin") var sessionLimitMin = 0
    @AppStorage("port")     var wantedPort = 0        // 0 = first free 48000-48049
    @AppStorage("serveWeb") var serveWeb   = true
    /// Owner policy — see the Settings picker for the reasoning. Off by default:
    /// uncompressed audio is ~187 KB/s per listener out of THIS machine's uplink.
    /// 0 = off, 1 = listener's choice, 2 = compatibility fallback only (VsUncompressedAudio).
    /// ★ This Mac's own browser is not governed by it and always gets uncompressed audio.
    @AppStorage("uncompressedAudio") var uncompressedAudio = 0
    // ★★ RECEIVER LOCATION. It is the SERVER's position, never the listener's: distances,
    // map centring and the ITU REGION all follow the ANTENNA (80m is 3.5-3.8 in R1 but
    // 3.5-4.0 in R2), and a VibeServer may well be left at a relative's house.
    // ★ It also decides the RDS COUNTRY and flag. The flag logic refuses to invent one —
    // it needs either an Extended Country Code off the air or the PI's country nibble
    // VALIDATED against the receiver's country — so with no location set, every station's
    // country stayed blank and it looked like a decoder fault (found on air 2026-07-26).
    /// ★ The receiver's PUBLISHED NAME — what listeners see over the spectrum, and the one
    /// piece of identity a shared receiver really needs ("Northampton RSP1B", "G0XYZ shack").
    /// Empty is fine: the overlay then shows the location alone rather than inventing a name.
    @AppStorage("rxName")  var rxName  = ""
    @AppStorage("rxPlace") var rxPlace = ""
    @AppStorage("rxLat")   var rxLat   = ""
    @AppStorage("rxLon")   var rxLon   = ""
    // Defaults to the Mac's own region, so the country works with no effort at all — and
    // an operator who has moved the machine can still override it.
    @AppStorage("rxIso")   var rxIso   = Locale.current.region?.identifier ?? ""
    // ★★ A MAIDENHEAD LOCATOR INSTEAD OF COORDINATES — the privacy fallback the Android app
    // already offers. A 6-character square is about 4 x 2.5 km, which is ample for distances
    // and bearings and does not publish where you live. Coordinates win when both are given;
    // otherwise the square's CENTRE is used, and everything downstream behaves identically
    // because it only ever sees a lat/lon (Stuart, 2026-07-26).
    @AppStorage("rxGrid")  var rxGrid  = ""
    /// Advertise on the LAN via Bonjour so clients auto-discover us. Off = reachable only by typing
    /// the address (a privacy choice — don't announce the radio to everyone on the network).
    @AppStorage("advertise") var advertise = true
    // ── Link management ceilings ─────────────────────────────────────────────
    // What the OPERATOR is willing to spend on each listener. The server enforces these, and —
    // crucially — publishes them so an adaptive client can see the ceiling instead of reading it
    // as a failing link and stepping down forever chasing a limit it can never reach.
    // 0 = no cap. Tiers match the Android server's (FPS_TIERS in src/services/vibeServer.ts) so a
    // setting means the same thing on every host.
    @AppStorage("maxFps")   var maxFps     = 0.0      // 0 = server default (20), else 20/10/5
    @AppStorage("eibi")     var eibiStations = false  // download the EiBi schedule for /stations search
    @Published var eibiStatus = ""                    // "1234 stations" / "downloading…" / "offline"
    @AppStorage("maxBwHz")  var maxBwHz    = 0.0      // 0 = uncapped demod bandwidth
    @AppStorage("lockRate") var lockedRate = 0.0      // 0 = client may change the capture rate
    /// Listeners normally choose whether to let the waterfall idle down. A host on solar and
    /// cellular cannot afford that choice, so its owner can make the saving mandatory.
    @AppStorage("forceIdle") var forceIdleSaver = false
    @AppStorage("autoStart") var autoStart = true     // start serving as soon as the app launches
    /** Surfaced when macOS refuses to register the login item — silence would look like a toggle
     *  that does nothing. */
    @Published var loginItemError: String?

    /**
     * Open at login.
     *
     * ★ NOT stored in our own preferences. macOS owns this — the user can remove the login item in
     * System Settings ▸ General ▸ Login Items at any time, and a mirrored bool would then be a lie.
     * Read the real status; write through to the real registration.
     *
     * With "start serving when VibeServer opens" (above), the pair means a rebooted Mac is back on
     * the air before anyone touches it — which is the whole point for a machine left serving.
     */
    var openAtLogin: Bool {
        get { SMAppService.mainApp.status == .enabled }
        set {
            do {
                if newValue { try SMAppService.mainApp.register() }
                else        { try SMAppService.mainApp.unregister() }
                loginItemError = nil
            } catch {
                // Commonly: the app is not where macOS expects it. Say so usefully rather than
                // leaving a switch that silently springs back.
                loginItemError = "macOS refused: \(error.localizedDescription). "
                    + "Move VibeServer to your Applications folder and try again."
            }
            objectWillChange.send()
        }
    }

    /// Called whenever anything the menu bar draws has changed.
    var onChange: (() -> Void)?

    private var timer: Timer?

    init() {
        refreshDevices()
        startPlugWatch()      // notice a radio attached after launch — see startPlugWatch()
        // ★ Resolve the .local hostname ONCE, OFF THE MAIN THREAD. BOTH Host.current().name AND
        // ProcessInfo.hostName can do a BLOCKING reverse-DNS lookup that stalls for up to ~30s on a
        // slow/flaky network — and doing it synchronously on the menu path HUNG the whole app (the
        // menu bar showed a connection but right-click drew nothing). Resolve it in the background;
        // accessAddresses simply omits the .local line until it lands.
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let h = ProcessInfo.processInfo.hostName
            DispatchQueue.main.async { self?.localName = h }
        }
    }

    /// How many listeners are connected right now.
    ///
    /// ★ HONEST LIMIT: the core currently reports a single `clientConnected` flag, not a count —
    /// multi-client is the protocol-foundations work and is not built yet. So this reads 0 or 1
    /// today and becomes a real count for free once the core exposes one. It is wired now so the
    /// menu bar does not need revisiting later.
    var listeners: Int { status.clientConnected ? 1 : 0 }

    func refreshDevices() {
        deviceCount = Int(vs_device_count())
        sdrplayStuck = vs_sdrplay_api_stuck() != 0
        var found = (0..<max(0, deviceCount)).map { String(cString: vs_device_name(Int32($0))) }
        // A radio we are already serving will not enumerate — keep it in the list, because it
        // is the most present radio there is.
        if running, !activeDevice.isEmpty, !found.contains(activeDevice) {
            found.insert(activeDevice, at: 0)
        }
        devices = found
        deviceName = running && !activeDevice.isEmpty
            ? activeDevice
            : (devices.indices.contains(deviceIndex) ? devices[deviceIndex] : (devices.first ?? ""))
        onChange?()
    }

    /// The chosen receiver's index, resolved by NAME so an unplug elsewhere cannot silently
    /// hand a listener a different radio. Falls back to the first attached device.
    var deviceIndex: Int {
        if let i = devices.firstIndex(of: wantedDevice) { return i }
        return devices.isEmpty ? 0 : 0
    }

    /// Rescan for radios, and start serving if a radio has appeared and we were idle.
    ///
    /// ★ Deliberately NOT `reconnectRadio()`, which stops and restarts. If a radio is already
    /// serving happily, pressing Refresh must not drop the listener — the button means "look
    /// again", not "start over".
    /// ★★ WATCH FOR A RADIO BEING PLUGGED IN, so the app can genuinely "live in the
    /// menu bar waiting for a radio" — plug one in, click once, you are serving.
    /// Until now a radio attached after launch stayed INVISIBLE until you pressed
    /// Refresh, which is a step nobody should have to know about.
    ///
    /// ★ IT DOES NOT AUTO-START. `rescan()` starts serving when it finds a device, and
    /// that is right for an explicit "Refresh" (the user is saying "try again") but
    /// wrong here: plugging a dongle in must never put a receiver ON AIR by itself.
    /// This only makes the radio APPEAR; going live stays a deliberate click.
    ///
    /// ★ It polls ONLY while idle with nothing attached — precisely the window where
    /// you are waiting for a plug-in — and stops the moment a radio shows up or the
    /// server starts. That keeps the ~0% idle CPU that makes this app forgettable in
    /// the best way, which is the property worth protecting.
    private var plugWatch: Timer?
    func startPlugWatch() {
        plugWatch?.invalidate()
        plugWatch = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { [weak self] t in
            guard let self else { t.invalidate(); return }
            Task { @MainActor in
                guard !self.running, self.devices.isEmpty else { return }   // idle and empty only
                let before = self.deviceCount
                self.refreshDevices()
                if self.deviceCount != before, self.deviceCount > 0 {
                    NSSound(named: "Tink")?.play()   // a radio appeared; the click is yours
                }
            }
        }
    }

    func rescan() {
        // ★ An explicit Refresh is the user saying "I have fixed it" — the one moment it is
        // right to re-probe an API we had written off.
        vs_sdrplay_retry()
        refreshDevices()
        if !running && deviceCount > 0 { start() }
    }

    /// Download the EiBi schedule and hand it to the core for /stations, or clear it when off.
    /// Runs off the main actor; only the small status string comes back to the UI.
    func refreshEibi() {
        guard eibiStations else { EibiStations.clear(); eibiStatus = ""; return }
        eibiStatus = "downloading…"
        Task { @MainActor in
            switch await EibiStations.refresh() {
            case .loaded(let n): self.eibiStatus = "\(n) stations on air now — in search"
            case .failed(let e): self.eibiStatus = "couldn't load: \(e)"
            }
        }
    }

    /// The JSON served at GET /location. Empty when nothing useful is known — every consumer
    /// degrades honestly rather than guessing, so an empty string is a valid answer.
    /// ★ The COUNTRY alone is worth publishing even with no coordinates: it is all the RDS
    /// flag needs, and it costs the operator nothing because it defaults from the Mac itself.
    func locationJson() -> String {
        var lat = Double(rxLat.trimmingCharacters(in: .whitespaces))
        var lon = Double(rxLon.trimmingCharacters(in: .whitespaces))
        let grid = rxGrid.trimmingCharacters(in: .whitespaces)
        // Exact coordinates win; a locator is the deliberately coarse alternative.
        if lat == nil || lon == nil, let c = gridCentre(grid) { lat = c.lat; lon = c.lon }
        let place = rxPlace.trimmingCharacters(in: .whitespaces)
        let iso = rxIso.trimmingCharacters(in: .whitespaces).uppercased()
        let name = rxName.trimmingCharacters(in: .whitespaces)
        if lat == nil && lon == nil && place.isEmpty && iso.isEmpty && grid.isEmpty
            && name.isEmpty { return "" }
        var parts: [String] = []
        if !name.isEmpty  { parts.append("\"name\":\"\(jsonEscaped(name))\"") }
        if !iso.isEmpty   { parts.append("\"iso\":\"\(jsonEscaped(iso))\"") }
        if !place.isEmpty { parts.append("\"label\":\"\(jsonEscaped(place))\"") }
        if let la = lat, let lo = lon, la >= -90, la <= 90, lo >= -180, lo <= 180 {
            parts.append("\"lat\":\(la)")
            parts.append("\"lon\":\(lo)")
            // Echo the operator's OWN locator when they gave one — recomputing it from the
            // square's centre would return the same square, but showing back exactly what
            // was typed is less confusing than showing a value they did not enter.
            parts.append("\"grid\":\"\(jsonEscaped(grid.isEmpty ? maidenhead(la, lo) : grid))\"")
        }
        return "{" + parts.joined(separator: ",") + "}"
    }

    private func jsonEscaped(_ s: String) -> String {
        s.replacingOccurrences(of: "\\", with: "\\\\")
         .replacingOccurrences(of: "\"", with: "\\\"")
    }

    /// Centre of a Maidenhead square, or nil if it does not parse. 4 or 6 characters.
    /// ★ The CENTRE, not the corner: a corner biases every distance by half a square in a
    /// fixed direction, which is a systematic error rather than the honest rounding the
    /// operator asked for by giving a locator in the first place.
    private func gridCentre(_ g: String) -> (lat: Double, lon: Double)? {
        let u = g.uppercased()
        let c = Array(u)
        guard c.count == 4 || c.count == 6 else { return nil }
        let A = Int(UnicodeScalar("A").value)
        guard c[0].isLetter, c[1].isLetter, c[2].isNumber, c[3].isNumber else { return nil }
        let f1 = Int(c[0].asciiValue.map { Int($0) - A } ?? -1)
        let f2 = Int(c[1].asciiValue.map { Int($0) - A } ?? -1)
        guard f1 >= 0, f1 < 18, f2 >= 0, f2 < 18 else { return nil }
        let s1 = Int(String(c[2])) ?? 0, s2 = Int(String(c[3])) ?? 0
        var lon = Double(f1) * 20.0 + Double(s1) * 2.0 - 180.0
        var lat = Double(f2) * 10.0 + Double(s2) * 1.0 - 90.0
        if c.count == 6 {
            guard c[4].isLetter, c[5].isLetter else { return nil }
            let t1 = Int(c[4].asciiValue.map { Int($0) - A } ?? -1)
            let t2 = Int(c[5].asciiValue.map { Int($0) - A } ?? -1)
            guard t1 >= 0, t1 < 24, t2 >= 0, t2 < 24 else { return nil }
            lon += Double(t1) * (2.0 / 24.0) + (1.0 / 24.0)      // + half a sub-square
            lat += Double(t2) * (1.0 / 24.0) + (0.5 / 24.0)
        } else {
            lon += 1.0; lat += 0.5                                // + half a square
        }
        return (lat, lon)
    }

    /// Maidenhead locator — what every other operator will ask for, and it is pure arithmetic
    /// on the coordinates, so asking the user for it as well would be asking twice.
    func maidenhead(_ lat: Double, _ lon: Double) -> String {
        let a = Int(UnicodeScalar("A").value)
        let lo = lon + 180.0, la = lat + 90.0
        let f1 = Int(lo / 20.0), f2 = Int(la / 10.0)
        let s1 = Int((lo - Double(f1) * 20.0) / 2.0), s2 = Int(la - Double(f2) * 10.0)
        let t1 = Int((lo - Double(f1) * 20.0 - Double(s1) * 2.0) * 12.0)
        let t2 = Int((la - Double(f2) * 10.0 - Double(s2)) * 24.0)
        func ch(_ i: Int) -> String { String(UnicodeScalar(UInt8(a + i))) }
        return ch(f1) + ch(f2) + "\(s1)\(s2)" + ch(t1).lowercased() + ch(t2).lowercased()
    }

    /// Opens the shared setup page. ★ The LAN address, not loopback — an owner reading this pane
    /// may well be screen-sharing or on the machine's own display, and either works from here.
    func openSetupInBrowser() {
        guard running, let host = accessAddresses.first,
              let url = URL(string: "http://\(host)/setup") else { return }
        NSWorkspace.shared.open(url)
    }

    /// ★★★ TWO SERVERS BEHIND ONE BUTTON. Simple runs the core in-process; Full writes a config and
    ///     spawns the front door. Dispatching here — rather than threading `if fullMode` through
    ///     the body below — is the same lesson the Settings pane learned the hard way: a single
    ///     path carrying that many conditionals silently does the opposite of what it reads.
    func start() {
        if fullMode { startFull() } else { startSimple() }
    }

    func stop() {
        if fullMode { stopFull() } else { stopSimple() }
    }

    // ── FULL MODE ────────────────────────────────────────────────────────────

    /// ★★★ NO PASSWORD MUST NOT MEAN NO CONTROL — shared by BOTH modes, because both need it.
    ///
    /// The hardware gate refuses gain, bias-T, direct sampling and calibration to everyone when no
    /// admin password is set — the only safe reading, since a blank secret cannot tell the owner
    /// from a stranger. But that leaves a receiver NOBODY can control: a new one starts at MINIMUM
    /// GAIN by design, so the waterfall is flat until the gain is raised, and an owner browsing
    /// from another room has nothing that raises it. They conclude the RADIO is broken.
    ///
    /// ★★ So GENERATE one rather than DEMAND one. Forcing the owner to invent a password taxes the
    ///    plug-and-play flow this app exists to protect; a generated one costs them nothing, is
    ///    shown with a Copy button, and unlocks the controls from ANY browser. This is the EXISTING
    ///    admin credential, not a new mechanism.
    ///
    /// ★ Written through and synchronised rather than left to @AppStorage's timing: measured the
    ///   server coming up with a generated password while the preference stayed EMPTY, so the next
    ///   launch generated a DIFFERENT one and the password the owner had copied off the screen
    ///   stopped working. Of everything here, this must not drift.
    ///
    /// ★★ It is extracted rather than duplicated for Full mode deliberately: two copies of a rule
    ///    this subtle would drift, and the half that drifted would be the one that ships a
    ///    receiver nobody can turn the gain up on.
    func ensureAdminPassword() {
        guard adminPassword.isEmpty else { return }
        let generated = Self.generatedPassword()
        UserDefaults.standard.set(generated, forKey: "adminPassword")
        UserDefaults.standard.set(true, forKey: "generatedAdmin")
        UserDefaults.standard.synchronize()
        adminPassword = generated
        generatedAdmin = true
    }

    /// Rescan, keeping whatever the owner has already un-ticked.
    func rescanFullRadios() {
        fullRadios = FullMode.detect(preserving: fullRadios)
        fullSerialsCollide = FullMode.serialsCollide
    }

    /// ★★★ THE LISTENER COUNT IN FULL MODE — ASKED FOR, NOT INFERRED.
    ///
    /// vs_status() reports on the core running INSIDE this app, and in Full mode the server is a
    /// separate process, so that struct stays empty however busy the receiver is. My first pass
    /// simply said nothing rather than print a confident zero — right, but not good enough: Simple
    /// mode shows the count and Full mode looked broken beside it (Stuart, 2026-08-11).
    ///
    /// ★★ So ASK THE SERVER. The front door publishes the radio list, and each radio's own
    ///    /vibeserver.json carries its live listener count — which is the same route the web
    ///    client takes, and the only honest one: the directory knows what EXISTS, and only each
    ///    radio's own process knows who is listening to it.
    /// ★ Every 5 s, not every second: it is two HTTP round trips per radio and a menu-bar number
    ///   that is five seconds stale has never misled anyone.
    private func startFullPolling() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, self.running, self.fullMode else { return }
                let base = "http://127.0.0.1:\(self.port)"
                guard let dirURL = URL(string: base + "/vibeserver/radios") else { return }
                do {
                    let (data, _) = try await URLSession.shared.data(from: dirURL)
                    let dir = try JSONSerialization.jsonObject(with: data) as? [String: Any]
                    let radios = (dir?["radios"] as? [[String: Any]]) ?? []
                    var total = 0
                    for r in radios {
                        guard let id = r["id"] as? String,
                              let u = URL(string: base + "/r/\(id)/vibeserver.json") else { continue }
                        // ★ A radio that does not answer contributes NOTHING rather than failing the
                        //   whole count — one down radio must not blank the number for the others.
                        if let (d2, _) = try? await URLSession.shared.data(from: u),
                           let j = try? JSONSerialization.jsonObject(with: d2) as? [String: Any] {
                            total += (j["listeners"] as? Int) ?? 0
                        }
                    }
                    self.fullListeners = total
                    self.onChange?()
                } catch {
                    // The server is up (we started it) but not answering yet — leave the last
                    // count rather than flashing a zero at the user.
                }
            }
        }
    }

    private func startFull() {
        lastError = nil
        guard let bin = FullMode.binaryURL,
              FileManager.default.isExecutableFile(atPath: bin.path) else {
            lastError = "The VibeServer engine is missing from this app bundle."
            return
        }
        // ★ Same generated-password rule as Simple mode: a receiver nobody can control reads as
        //   broken hardware. See the long note in startSimple().
        ensureAdminPassword()
        if fullRadios.isEmpty { rescanFullRadios() }
        guard fullRadios.contains(where: { $0.serve }) else {
            lastError = "Tick at least one radio — a server with none cannot receive anything."
            return
        }
        do {
            try FullMode.writeConfig(radios: fullRadios, adminPassword: adminPassword, pin: pin)
        } catch {
            lastError = "Could not save the settings: \(error.localizedDescription)"
            return
        }

        let p = Process()
        p.executableURL = bin
        var env = ProcessInfo.processInfo.environment
        // ★ The core already honours this, so the app points it at the per-user location rather
        //   than the core growing a second idea of where its config lives. The front door passes
        //   it on to every radio it forks, so all of them read the same file.
        env["VIBESERVER_CONFIG"] = FullMode.configURL.path
        p.environment = env
        // ★★ NO ARGUMENTS. `vibeserver` with none IS the front door — the same invocation systemd
        //    uses on the Pi. Passing --device or --port here would take the front-door branch out
        //    of play and quietly start a single-radio server wearing Full mode's clothes.
        p.arguments = []
        // ★★★ IF THE APP DIES, THE SERVER MUST NOT LIVE ON. A front door outliving the app keeps
        //     the radios and the port, and nothing in the UI can then stop it. The child watches
        //     for our exit itself (parent_watch.cpp) — this is the tidy half of the same contract.
        p.terminationHandler = { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                if self.running && self.fullMode {
                    self.running = false
                    self.lastError = "The server stopped unexpectedly."
                    self.onChange?()
                }
            }
        }
        do { try p.run() } catch {
            lastError = "Could not start the server: \(error.localizedDescription)"
            return
        }
        frontDoor = p
        running = true
        port = wantedPort > 0 ? wantedPort : 48000
        fullListeners = 0
        startFullPolling()
        onChange?()

        // ★★ AND OPEN THE SETUP PAGE, because in Full mode everything except the radio, the
        //    password and the PIN is set in the browser — so a Start that left the user looking at
        //    an unchanged window would have finished only half the job. Briefly delayed: the front
        //    door has to be listening before the page can load, and a failed first load reads as a
        //    broken server rather than an early click.
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) { [weak self] in
            self?.openSetupInBrowser()
        }
    }

    private func stopFull() {
        stopAdvertising()
        if let p = frontDoor, p.isRunning {
            p.terminate()                       // SIGTERM: the front door reaps its radios on exit
            // ★ Give it a moment to take the radios down with it, then insist. A radio process
            //   left holding an SDR is the failure that makes hardware look broken.
            let deadline = Date().addingTimeInterval(3)
            while p.isRunning && Date() < deadline { usleep(50_000) }
            if p.isRunning { kill(p.processIdentifier, SIGKILL) }
        }
        frontDoor = nil
        running = false
        port = 0
        fullListeners = 0
        timer?.invalidate(); timer = nil
        onChange?()
    }

    private func startSimple() {
        lastError = nil
        var cfg = VsConfig()
        vs_default_config(&cfg)
        cfg.centreHz = centreHz
        cfg.deviceIndex = Int32(deviceIndex)
        activeDevice = devices.indices.contains(deviceIndex) ? devices[deviceIndex] : ""
        cfg.port     = Int32(wantedPort)
        cfg.serveWebClient = serveWeb
        cfg.uncompressedAudio = Int32(uncompressedAudio)
        cfg.maxFftRate     = maxFps
        cfg.maxBandwidthHz = maxBwHz
        cfg.lockedRate     = lockedRate
        cfg.forceIdleSaver = forceIdleSaver
        let locJson = locationJson()

        // The C strings must outlive the call, so hold them across it.
        // ★★ OFF THE MAIN THREAD. Starting a radio can legitimately take seconds — and on an
        // SDRplay it can take FOREVER: sdrplay_api_SelectDevice waits on a SYSTEM-WIDE lock,
        // so a process that crashed inside the API leaves every later caller blocked. Doing
        // that on the main thread froze the whole app the moment "Start serving" was pressed,
        // with no icon, no crash report and nothing to act on (Stuart, 2026-07-26).
        // ★ A hang is worse than a failure: a failure can be SHOWN.
        starting = true
        lastError = nil
        // ★★★ NO PASSWORD MUST NOT MEAN NO CONTROL.
        //
        // The hardware gate refuses gain, bias-T, direct sampling and calibration to everyone
        // when no admin password is set — the only safe reading, since a blank secret cannot
        // tell the owner from a stranger. But that leaves a receiver NOBODY can control: a new
        // one starts at MINIMUM GAIN by design, so the waterfall is flat until the gain is
        // raised, and an owner browsing from another room has nothing that raises it. They
        // conclude the RADIO is broken. Android is worse — no browser on the machine at all.
        //
        // ★★ So GENERATE one rather than DEMAND one. Forcing the owner to invent a password
        //    taxes the plug-and-play flow this app exists to protect; a generated one costs
        //    them nothing, is shown with a Copy button, and unlocks the controls from ANY
        //    browser. This is the EXISTING admin credential, not a new mechanism.
        //
        // ★ Written through and synchronised rather than left to @AppStorage's timing: measured
        //   the server coming up with a generated password while the preference stayed EMPTY,
        //   so the next launch generated a DIFFERENT one and the password the owner had copied
        //   off the screen stopped working. Of everything here, this must not drift.
        ensureAdminPassword()
        let modeS = mode, pinS = pin, admS = adminPassword
        let limitS = sessionLimitMin
        DispatchQueue.global(qos: .userInitiated).async {
            var cfg2 = cfg
            var port2 = -1
            var errStr = ""
            modeS.withCString { modePtr in
              pinS.withCString { pinPtr in
                admS.withCString { admPtr in
                locJson.withCString { locPtr in
                    cfg2.mode = modePtr
                    cfg2.pin  = pinPtr
                    cfg2.adminPassword = admPtr
                    cfg2.sessionLimitMin = Int32(limitS)
                    cfg2.locationJson = locJson.isEmpty ? nil : locPtr
                    var err = [CChar](repeating: 0, count: 256)
                    port2 = Int(vs_start(&cfg2, &err, 256))
                    if port2 <= 0 { errStr = String(cString: err) }
                }
                }
              }
            }
            DispatchQueue.main.async {
                self.starting = false
                if port2 > 0 {
                    self.port = port2; self.running = true; self.refreshEibi()
                    self.startPolling(); self.startAdvertising()
                } else {
                    self.lastError = errStr.isEmpty ? "could not start" : errStr
                    self.running = false
                }
                self.refreshDevices()
            }
        }
    }

    /// The operator flipped the Bonjour toggle — apply it live.
    func applyAdvertise() {
        if advertise, running, listeners == 0 { startAdvertising() } else { stopAdvertising() }
        onChange?()
    }

    private func stopSimple() {
        stopAdvertising()
        vs_stop()
        running = false
        port = 0
        timer?.invalidate(); timer = nil
        onChange?()
    }

    private func startPolling() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                var s = VsStatus()
                vs_status(&s)
                let before = self.listeners
                self.status = s
                // The core is the authority on whether it is still alive — if the capture died,
                // say so rather than showing a cheerful menu bar over a dead radio.
                if !vs_is_running() && self.running {
                    self.running = false
                    self.lastError = "The receiver stopped — was the dongle unplugged?"
                    self.timer?.invalidate(); self.timer = nil
                }
                if before != self.listeners {
                    // ★ WITHDRAW the Bonjour advert while the one slot is taken, re-publish when it
                    // frees. A server that cannot accept anyone should not appear in Discovered
                    // lists inviting a connection that will only be refused. (When multi-radio
                    // lands this becomes "withdraw when ALL slots are full".) A manually-added
                    // server still answers /connection with "in-use", so nothing is hidden that a
                    // determined client cannot still ask about.
                    if self.running {
                        if self.listeners > 0 { self.stopAdvertising() }
                        else                  { self.startAdvertising() }
                    }
                }
                if before != self.listeners || !self.running { self.onChange?() }
            }
        }
    }

    // ── Bonjour ──────────────────────────────────────────────────────────────
    //
    // ★ The Mac MUST advertise this itself. The C++ core's mDNS responder answers HOSTNAME queries
    // only (`vibesdr.local` A records) and deliberately serves no PTR/SRV/TXT — on Android the
    // SERVICE registration comes from NsdManager, which has no macOS equivalent. Without this the
    // phone, watch and every other client simply never see the Mac in their Discovered list.
    //
    // The contract is fixed by the existing clients (VibeMdnsModule.kt / src/services/mdns.ts) and
    // must match exactly, or discovery silently finds nothing:
    //   type `_vibesdr._tcp.`, TXT `proto=vibeserver`, TXT `pin` = "1" when a PIN is required.
    private var bonjour: NetService?

    private func startAdvertising() {
        stopAdvertising()
        guard running, port > 0, advertise else { return }   // advertise=false → discoverable only by address
        // ★★★ IN FULL MODE THE ENGINE ADVERTISES, NOT US. Each radio process runs its own mDNS
        //     responder and publishes its own name ("advertising as a.local"), which is what makes
        //     several radios on one machine individually discoverable. A second advertiser here
        //     would publish a competing record for the same host — two answers to one question,
        //     which is worse than none.
        guard !fullMode else { return }
        let svc = NetService(domain: "local.", type: "_vibesdr._tcp.",
                             name: serviceName, port: Int32(port))
        svc.setTXTRecord(NetService.data(fromTXTRecord: [
            "proto": Data("vibeserver".utf8),
            "pin":   Data((pin.isEmpty ? "0" : "1").utf8),
        ]))
        svc.publish()
        bonjour = svc
    }

    private func stopAdvertising() {
        bonjour?.stop()
        bonjour = nil
    }

    /// What clients see in their list. The Mac's own name is the least surprising label — it is
    /// what the owner already calls this machine everywhere else on the network.
    private var serviceName: String {
        let host = Host.current().localizedName ?? "Mac"
        return "VibeServer on \(host)"
    }

    /** The radio has gone (unplugged or failed) while the server is still up. */
    var radioLost: Bool { running && status.deviceLost }

    /**
     * Take the radio back after a replug.
     *
     * ★ A FULL STOP AND START, deliberately — not a clever in-place reopen. Reopening the device
     * underneath the running threads is exactly what crashed the server earlier: the HTTP and
     * control threads were still calling into librtlsdr on a handle being closed. stop() joins
     * everything first, so this is the same path as quitting and reopening the app, which is known
     * to work. Listeners reconnect on their own.
     */
    func reconnectRadio() {
        stop()
        refreshDevices()
        guard deviceCount > 0 else {
            lastError = "No radio found. Plug the receiver in and try again."
            onChange?()
            return
        }
        start()
    }

    var address: String { "http://localhost:\(port)/" }

    /// The LAN IPv4 of the first real interface, or nil when there is no network.
    ///
    /// ★ "localhost" is useless to the person the server is FOR. An owner needs the string to hand
    /// to someone else, and Android already shows it — a Mac that only offers localhost looks like
    /// it cannot be reached from anywhere, which is precisely backwards.
    private func lanIPv4() -> String? {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return nil }
        defer { freeifaddrs(head) }
        var best: String?
        for ptr in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let f = ptr.pointee
            guard f.ifa_addr?.pointee.sa_family == sa_family_t(AF_INET) else { continue }
            let name = String(cString: f.ifa_name)
            // Skip loopback and the software bridges/tunnels a Mac is full of; en* is the real one.
            guard name != "lo0", name.hasPrefix("en") else { continue }
            var buf = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            guard getnameinfo(f.ifa_addr, socklen_t(f.ifa_addr.pointee.sa_len),
                              &buf, socklen_t(buf.count), nil, 0, NI_NUMERICHOST) == 0 else { continue }
            let ip = String(cString: buf)
            if best == nil { best = ip }
            if name == "en0" { return ip }   // prefer the primary interface
        }
        return best
    }

    /// The Bonjour .local name, resolved ONCE in the background at init (see init) and cached here.
    /// ★ Resolving it is a BLOCKING call — reading it must never be. Empty until it lands; the menu
    /// then just gains the .local line, it never stalls waiting for it. Stable once set, so the menu
    /// doesn't reshuffle (the flicker this replaced was Host.current().name re-resolving each open).
    @Published var localName: String = ""

    /// Every way in, best first — for the menu and for copying to a listener. STABLE (no live
    /// lookups), so the menu doesn't reshuffle.
    var accessAddresses: [String] {
        guard running, port > 0 else { return [] }
        var out: [String] = []
        if let ip = lanIPv4() { out.append("\(ip):\(port)") }
        if !localName.isEmpty { out.append("\(localName):\(port)") }
        return out
    }

    /// Live throughput, as the listener is actually being served right now.
    var liveRateLine: String? {
        guard running, status.clientConnected else { return nil }
        let kbs = (status.specBytesPerSec + status.audioBytesPerSec) / 1024.0
        return String(format: "%.0f KB/s · %.1f fps", kbs, status.fftRate)
    }

    /// Who is listening, for the takeover warning. Empty when nobody is.
    var listenerAddr: String {
        withUnsafeBytes(of: status.clientAddr) {
            String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
    }
    /// Is the current listener this very Mac? Then re-opening the page interrupts nobody but the
    /// person clicking, and asking would be noise.
    var listenerIsLocal: Bool {
        let a = listenerAddr
        return a.isEmpty || a.hasPrefix("127.") || a == "::1" || a == "::ffff:127.0.0.1"
    }

    func openInBrowser() {
        guard running, let url = URL(string: address) else { return }
        // ★ ALREADY LISTENING FROM THIS MAC? Then a second tab is not what was wanted — the user
        // is looking for the window they already have.
        //
        // We do NOT go hunting through the browser for it. Asking a browser what tabs it has open
        // means AppleScript, which means an Automation consent prompt, and a permission dialog is
        // far too high a price for a convenience. Instead we use the connection we ALREADY have:
        // tell the listening client it is being summoned, and let it flash and try to focus itself.
        // Then just bring the browser forward, which needs no permission at all.
        if listeners > 0 && listenerIsLocal {
            vs_summon()
            activateDefaultBrowser()
            return
        }
        NSWorkspace.shared.open(url)
    }

    /// Bring the default browser to the front. No Automation consent needed — this is the same
    /// thing clicking its Dock icon does. We cannot pick the TAB this way, but the page itself
    /// answers the summons, so the user is never left wondering which window we meant.
    ///
    /// ★ ACTIVATE a running browser; do not LAUNCH it. `openApplication` asks the app to open, and
    /// a browser asked to open with nothing to show makes a new blank tab — which is what Edge did
    /// (Safari happened not to). If it is already running, and it must be since it is listening to
    /// us, activating is both correct and side-effect free.
    private func activateDefaultBrowser() {
        guard let probe = URL(string: "http://localhost"),
              let app = NSWorkspace.shared.urlForApplication(toOpen: probe) else { return }
        if let id = Bundle(url: app)?.bundleIdentifier,
           let running = NSRunningApplication.runningApplications(withBundleIdentifier: id).first {
            running.activate(options: [.activateAllWindows])
            return
        }
        // Not running at all — then launching it IS what the user wants, so open the page properly.
        if let url = URL(string: address) { NSWorkspace.shared.open(url) }
    }

}

// ── Entry point ──────────────────────────────────────────────────────────────
@main
enum Main {
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.accessory)   // menu-bar resident: no Dock icon
        app.run()
    }
}

// @MainActor throughout: it touches AppKit and the Server, both main-actor bound. Marking the
// class avoids scattering hops through every @objc action.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let server = Server()
    private var item: NSStatusItem!
    private var settingsWindow: NSWindow?

    func applicationDidFinishLaunching(_ note: Notification) {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

        if let button = item.button {
            button.image = Self.templateIcon()
            button.imagePosition = .imageLeading
            button.target = self
            button.action = #selector(clicked(_:))
            // Ask for BOTH buttons, or a right click never reaches us.
            button.sendAction(on: [.leftMouseUp, .rightMouseUp])
        }

        server.onChange = { [weak self] in self?.render() }
        if server.autoStart && server.deviceCount > 0 { server.start() }
        render()
    }

    func applicationWillTerminate(_ note: Notification) { server.stop() }

    // ── The menu-bar face ────────────────────────────────────────────────────
    /// Icon plus a listener COUNT, so a glance answers "is anyone using my radio?" — which matters
    /// most when several receivers are being run at once. Dimmed when not serving, so "available
    /// for use here" is equally readable without opening anything.
    private func render() {
        guard let button = item.button else { return }
        button.appearsDisabled = !server.running
        if server.radioLost {
            // Serving, but with nothing to serve. Say so in the menu bar rather than showing a
            // healthy icon over a dead radio.
            button.appearsDisabled = true
            button.title = " !"
            button.toolTip = "VibeServer — radio disconnected"
        } else if server.running {
            // ★★★ THE COUNT HAS TWO SOURCES AND THE BADGE ONLY KNEW ONE. `listeners` reads the core
            //     running INSIDE this app, which in Full mode is not the server at all — the radios
            //     are separate processes, so the badge silently vanished the moment you switched to
            //     Full and reappeared in Simple. It read as a Simple-mode-only decoration
            //     (Stuart, 2026-08-11: "in simple mode there is a listener number next to the icon
            //     too"). The MENU already picked the right source per mode; the badge did not.
            //     ★ Same rule as statusLine() below, and deliberately written the same way so the
            //       two cannot drift: whoever adds a third mode has to answer this question twice
            //       in one file, not once here and once somewhere they never looked.
            let n = server.fullMode ? server.fullListeners : server.listeners
            button.title = n > 0 ? " \(n)" : ""
            button.toolTip = n > 0
                ? "VibeServer — \(n) listening · \(server.address)"
                : "VibeServer — serving, nobody listening · \(server.address)"
        } else {
            button.title = ""
            button.toolTip = server.deviceCount == 0
                ? "VibeServer — no SDR connected"
                : "VibeServer — stopped"
        }
    }

    @objc private func clicked(_ sender: NSStatusBarButton) {
        let rightClick = NSApp.currentEvent?.type == .rightMouseUp
            || NSApp.currentEvent?.modifierFlags.contains(.control) == true
        if rightClick { showMenu() } else { primaryAction() }
    }

    /// LEFT CLICK = listen. The common case by far is "I want to hear my radio", so it costs one
    /// click and no reading. If the server is not up yet, start it and then open — the user's
    /// intent is the same either way.
    ///
    /// EXCEPT when someone else is already listening. The server currently serves ONE client, so
    /// opening a second session takes the radio from whoever has it. That must never happen from a
    /// stray click — Stuart lost his own iPhone session to exactly this. Ask first, and name who is
    /// about to be interrupted. When multi-client lands this prompt simply stops appearing.
    private func primaryAction() {
        if !server.running {
            guard server.deviceCount > 0 else { showMenu(); return }
            server.start()
            guard server.running else { showMenu(); return }   // failed: show why
        }
        if server.listeners > 0 && !server.listenerIsLocal {
            let a = NSAlert()
            a.messageText = "Someone is already listening"
            a.informativeText = server.listenerAddr
                + " is using this radio. VibeServer can only serve one listener at a time, so"
                + " opening it here will disconnect them."
            a.addButton(withTitle: "Listen Here Anyway")
            a.addButton(withTitle: "Cancel")
            a.alertStyle = .warning
            NSApp.activate(ignoringOtherApps: true)
            guard a.runModal() == .alertFirstButtonReturn else { return }
        }
        server.openInBrowser()
    }

    /// RIGHT CLICK = manage.
    private func showMenu() {
        let menu = NSMenu()

        let header = NSMenuItem(title: statusLine(), action: nil, keyEquivalent: "")
        header.isEnabled = false
        menu.addItem(header)
        if let err = server.lastError {
            let e = NSMenuItem(title: err, action: nil, keyEquivalent: "")
            e.isEnabled = false
            menu.addItem(e)
        }
        menu.addItem(.separator())

        if server.radioLost {
            let r = NSMenuItem(title: "Reconnect Radio", action: #selector(reconnectRadio), keyEquivalent: "r")
            menu.addItem(r)
            menu.addItem(.separator())
        }
        if server.running {
            menu.addItem(withTitle: "Open in Browser", action: #selector(openBrowser), keyEquivalent: "o")
            // ★ Full mode is administered in the browser, so the menu bar must offer the way in.
            //   Without this the only route to setup was the Settings pane's button, which is two
            //   clicks further away and not where anyone looks for "manage this server".
            if server.fullMode {
                menu.addItem(withTitle: "Set Up in Browser…", action: #selector(openSetup), keyEquivalent: "")
            }
            menu.addItem(withTitle: "Stop Serving", action: #selector(toggleServing), keyEquivalent: "")
        } else {
            let start = NSMenuItem(title: "Start Serving", action: #selector(toggleServing), keyEquivalent: "")
            // ★ Full mode serves the radios the owner TICKED, which is not the same as "a radio is
            //   plugged in": with everything un-ticked there is nothing to serve, and the Settings
            //   pane disables its button for exactly this reason. The menu must agree with it.
            start.isEnabled = server.fullMode ? server.fullRadios.contains { $0.serve }
                                              : server.deviceCount > 0
            menu.addItem(start)
        }

        menu.addItem(.separator())
        menu.addItem(withTitle: "Settings…", action: #selector(openSettings), keyEquivalent: ",")
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit VibeServer", action: #selector(quit), keyEquivalent: "q")

        for i in menu.items where i.action != nil { i.target = self }

        // Attaching the menu to the item makes the NEXT click open it; popping it up here keeps
        // left-click free for the primary action.
        item.menu = menu
        item.button?.performClick(nil)
        item.menu = nil
    }

    private func statusLine() -> String {
        if !server.running {
            // ★ NOT "RTL-SDR": the Mac build drives an SDRplay RSP and an Airspy HF+ as well, so
//   naming the dongle told an Airspy owner their working radio was absent. Same fix as
//   the Android module and the shim (2026-07-30).
            return server.deviceCount == 0 ? "No SDR connected" : "Stopped"
        }
        if server.radioLost { return "Radio disconnected — plug it back in, then Reconnect Radio" }
        // ★★★ IN FULL MODE WE DO NOT KNOW WHO IS LISTENING, SO WE MUST NOT SAY. The listener count
        //     comes from vs_status(), which reports on the core running INSIDE this app — and in
        //     Full mode the server is a separate process, so that struct stays empty and every
        //     menu would have read "nobody listening" however busy the receiver was. Saying
        //     nothing is the honest answer; a confident wrong number is the one that gets
        //     believed. (Same rule as "no inferred hardware readouts".)
        if server.fullMode {
            let r = server.fullRadios.filter { $0.serve }.count
            let n = server.fullListeners
            let who = n == 0 ? "nobody listening" : (n == 1 ? "1 listener" : "\(n) listeners")
            return "Serving \(r) radio\(r == 1 ? "" : "s") on \(server.address) — \(who)"
        }
        let n = server.listeners
        let who = n == 0 ? "nobody listening" : (n == 1 ? "1 listener" : "\(n) listeners")
        return "Serving on \(server.address) — \(who)"
    }

    // ── Actions ──────────────────────────────────────────────────────────────
    @objc private func openBrowser() { server.openInBrowser() }
    @objc private func openSetup() { server.openSetupInBrowser() }
    @objc private func copyAddress(_ sender: NSMenuItem) {
        guard let a = sender.representedObject as? String else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString("http://\(a)/", forType: .string)
    }
    @objc private func reconnectRadio() { server.reconnectRadio(); render() }
    @objc private func toggleServing() { server.running ? server.stop() : server.start(); render() }
    @objc private func quit() { server.stop(); NSApp.terminate(nil) }

    @objc private func openSettings() {
        if settingsWindow == nil {
            let host = NSHostingController(rootView: SettingsView(server: server))
            // Don't let the hosting controller drive the window size back to the Form's full ideal
            // height — that is exactly what pushed the lower sections off-screen. We size the
            // window ourselves below and let the Form scroll inside it.
            host.sizingOptions = []
            let w = NSWindow(contentViewController: host)
            w.title = "VibeServer Settings"
            // ★ RESIZABLE, and BOUNDED. Without an explicit content size the window grows to the
            // Form's full ideal height — taller than the screen — and, top-anchored with no resize
            // handle, the lower sections (Startup) fall off the bottom with no way to scroll to
            // them. Pinning a sensible height shorter than the smallest screen lets the Form's own
            // scrolling engage, and .resizable lets a user who wants to see more just drag it open.
            w.styleMask = [.titled, .closable, .resizable]
            w.setContentSize(NSSize(width: 420, height: 620))
            w.contentMinSize = NSSize(width: 380, height: 360)
            w.isReleasedWhenClosed = false
            settingsWindow = w
        }
        NSApp.activate(ignoringOtherApps: true)
        settingsWindow?.center()
        settingsWindow?.makeKeyAndOrderFront(nil)
    }

    /// The menu-bar glyph: our own mark (radio + triangle node), not an SF Symbol.
    ///
    /// ★ A macOS TEMPLATE image — pure black with alpha, no colour of its own — so the system tints
    /// it: black on a light menu bar, white on a dark one, and highlighted correctly when a menu is
    /// open. Shipping the green artwork here would look wrong in one appearance or the other and
    /// would ignore the user's accent and contrast settings.
    private static func templateIcon() -> NSImage? {
        guard let url = Bundle.main.url(forResource: "MenuBar", withExtension: "png"),
              let img = NSImage(contentsOf: url) else {
            return NSImage(systemSymbolName: "antenna.radiowaves.left.and.right", accessibilityDescription: "VibeServer")
        }
        img.isTemplate = true
        img.size = NSSize(width: 18, height: 18)
        return img
    }
}

// ── Settings (SwiftUI inside the AppKit shell) ───────────────────────────────
struct SettingsView: View {
    @ObservedObject var server: Server
    @StateObject private var finder = LocationFinder()

    // ★★★ TWO MODES, TWO VIEWS — NOT ONE FORM FULL OF `if`s.
    //
    // The first attempt threaded `if !fullMode` through the single shipped Form. It compiled, and
    // then rendered the OPPOSITE of what it said: in Simple the Access section — the PIN and the
    // admin password — was absent, while Full showed everything. A Form carrying that many
    // conditional children regroups them in ways the source does not describe, and the failure is
    // SILENT, so the code reads correct while the pane is wrong.
    //
    // ★★ Two separate views cannot do that. Simple is the shipped Form, untouched; Full is its own
    //    short one. Neither can lose a section the other owns.
    var body: some View {
        if server.fullMode { fullForm } else { simpleForm }
    }

    /// The mode switch itself, FIRST in both panes, because it decides what the rest of them show.
    @ViewBuilder private var modeSection: some View {
        Section {
            Picker("Setup", selection: $server.fullMode) {
                Text("Simple").tag(false)
                Text("Full").tag(true)
            }
            .pickerStyle(.segmented)
            Text(server.fullMode
                 ? "Everything else is set up in a browser — one settings page shared by every "
                   + "VibeServer, rather than a different thin strip of controls per platform."
                 : "Plug a radio in and press Start. Switch to Full to share this receiver "
                   + "publicly and set it up in a browser.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    /// ★ FULL — deliberately four things: the radio, the two secrets, and Start. Everything else
    ///   moved to the browser setup page, which is where a public receiver is actually configured.
    private var fullForm: some View {
        Form {
            modeSection
            // ★★★ A LIST, NOT A PICKER. Full mode's whole point is that a machine can serve
            //     SEVERAL radios at once, each as its own receiver — so the question is not
            //     "which one" but "which of these". A picker can only ever express the former,
            //     and quietly capped Full mode at one radio however many were plugged in.
            // ★★ ALL TICKED BY DEFAULT, as the Linux TUI does: someone who plugged three radios
            //    in wants three receivers, and making them opt each one in asks a question their
            //    hands have already answered. Un-ticking is for the odd one out — the dongle on
            //    loan to another program.
            Section("Radios") {
                if server.fullRadios.isEmpty {
                    HStack {
                        Text("No radio detected").foregroundStyle(.secondary)
                        Spacer()
                        Button("Look again") { server.rescanFullRadios() }
                    }
                    Text("Plug an SDR in — RTL-SDR, Airspy HF+ or SDRplay RSP.")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    ForEach($server.fullRadios) { $r in
                        Toggle(isOn: $r.serve) {
                            VStack(alignment: .leading, spacing: 1) {
                                Text(r.name)
                                // ★ The serial only when it adds something: the Airspy's own name
                                //   already ends with it, and printing it twice reads as a bug.
                                if !r.serial.isEmpty, !r.name.contains(r.serial) {
                                    Text(r.serial).font(.caption).foregroundStyle(.secondary)
                                }
                            }
                        }
                        .disabled(server.running)
                    }
                    HStack {
                        Spacer()
                        Button("Look again") { server.rescanFullRadios() }
                            .disabled(server.running)
                    }
                    Text("Each ticked radio becomes its own receiver, with its own settings. "
                       + "You set each one up in the browser afterwards.")
                        .font(.caption).foregroundStyle(.secondary)
                    // ★★ RTL dongles ship with the SAME serial, and two of them are then
                    //    indistinguishable — so settings would follow the wrong radio. Say so
                    //    HERE, where the owner can still act, rather than letting them discover it
                    //    when their locked frequency range moves. The TUI warns about this too.
                    if server.fullSerialsCollide {
                        Text("Two radios report the same serial number, so they cannot be told "
                           + "apart. Give one a new one:  vibeserver --set-rtl-serial <number> "
                           + "<serial>")
                            .font(.caption).foregroundStyle(.orange)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            Section("Access") {
                adminPasswordRow
                SecureField("PIN", text: $server.pin, prompt: Text("Open — no PIN"))
                Text("PASSWORD unlocks the settings and the hardware — the gain, bias-T, direct "
                   + "sampling and calibration — from any browser, and opens the setup page "
                   + "below. PIN decides who may connect and listen at all; a public receiver "
                   + "usually has none.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            // ★★★ ONE BUTTON: SAVE, START, AND OPEN THE BROWSER. In Full mode everything except
            //     the radios, the password and the PIN is set in the browser — so a Start that
            //     left the user looking at an unchanged window had finished only half the job,
            //     and the second button was disabled at exactly the moment they needed it.
            //     Opening the page IS the rest of the action, not a follow-up the user must know
            //     to take.
            Section {
                Button(server.running ? "Stop serving" : "Save, start and set up in the browser") {
                    server.running ? server.stop() : server.start()
                }
                .disabled(!server.running && !server.fullRadios.contains { $0.serve })
                // ★ Still offered while running, for the second visit — the page is where a
                //   running server is administered, not only where it is first configured.
                if server.running {
                    Button("Open setup in your browser…") { server.openSetupInBrowser() }
                }
                Text(server.running
                     ? "Name, location, sharing, session limits, bandwidth and the link settings "
                       + "are all on that page."
                     : "Everything else — name, location, sharing, limits and each radio's own "
                       + "settings — is set on the page this opens.")
                    .font(.caption).foregroundStyle(.secondary)
                if let err = server.lastError {
                    Text(err).font(.caption).foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Section("Startup") {
                Toggle("Open VibeServer at login", isOn: Binding(
                    get: { server.openAtLogin },
                    set: { server.openAtLogin = $0 }))
                Toggle("Start serving when VibeServer opens", isOn: $server.autoStart)
            }
        }
        .formStyle(.grouped)
        .frame(minWidth: 380)
    }

    /// ★★ ONE ROW, NOT TWO. The first attempt drew a SecureField AND a separate "your control
    ///    password" block, so the pane showed the same secret twice — once as dots and once in
    ///    the clear — and gave no hint which was live.
    ///
    /// ★★★ VISIBLE ONLY WHILE IT IS OURS. A generated password must be readable: the owner has
    ///     never seen it, and dots would lock them out of their own radio. One THEY chose is
    ///     already known to them, so it goes behind dots — a settings pane is exactly where
    ///     someone reads over your shoulder.
    @ViewBuilder private var adminPasswordRow: some View {
        // ★ The setter clears the flag on the FIRST keystroke. Left set, a password the owner had
        //   typed themselves stayed on screen in plain text — which is how a real one came to be
        //   displayed to the room during testing.
        let bound = Binding(get: { server.adminPassword },
                            set: { server.adminPassword = $0; server.generatedAdmin = false })
        if server.generatedAdmin && !server.adminPassword.isEmpty {
            HStack {
                TextField("Admin password", text: bound)
                    .font(.system(.body, design: .monospaced))
                Button("Copy") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(server.adminPassword, forType: .string)
                }
            }
            Text("MADE FOR YOU, and shown in full because you have not seen it before. Without a "
               + "password NOBODY may change this radio's GAIN, bias-T, direct sampling or "
               + "calibration — not a stranger, and not you from another machine — and a receiver "
               + "deliberately starts at MINIMUM GAIN, so the waterfall would stay flat with "
               + "nothing able to lift it. Enter this in the client to unlock the controls.\n\n"
               + "Type over it to use your own, and it is hidden from then on.")
                .font(.caption).foregroundStyle(.secondary)
        } else {
            SecureField("Admin password", text: bound,
                        prompt: Text("One will be made for you"))
        }
    }

    /// ★ SIMPLE — the pane exactly as it shipped, plus the mode switch. Stuart, 2026-08-06:
    ///   "Simple mode on both is what it is now with 0 changes."
    private var simpleForm: some View {
        Form {
            modeSection
            // ── The addresses to point a client at, and what the server is pushing right now. AT THE
            // TOP because "how do I reach this / what's it doing" is the first question — and the
            // reason a user typed the wrong port (couldn't see which one it bound). ──
            Section("Connect") {
                if server.running {
                    if server.accessAddresses.isEmpty {
                        Text("Starting…").font(.caption).foregroundStyle(.secondary)
                    } else {
                        ForEach(server.accessAddresses, id: \.self) { a in
                            HStack {
                                Text(a).font(.system(.body, design: .monospaced)).textSelection(.enabled)
                                Spacer()
                                Button {
                                    NSPasteboard.general.clearContents()
                                    NSPasteboard.general.setString(a, forType: .string)
                                } label: { Image(systemName: "doc.on.doc") }.buttonStyle(.borderless)
                            }
                        }
                        // ★ The old copy ordered everyone to type the port, which is wrong for
                        // almost everyone: VibeSDR and Jr already SCAN 48000–48100, so the
                        // default server is found without it. Telling people to do work the app
                        // does for them makes the product look fussier than it is — and it buried
                        // the one case that genuinely needs typing, a port outside that range.
                        Text("Point VibeSDR or Jr at one of these. They scan ports 48000–48100, "
                             + "so you only need to add the port by hand if you have moved this "
                             + "server outside that range.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    LabeledContent("Now sending", value: server.status.clientConnected
                        ? String(format: "spectrum %.0f · audio %.0f KB/s · %.0f fps",
                                 server.status.specBytesPerSec/1024, server.status.audioBytesPerSec/1024, server.status.fftRate)
                        : "nobody listening")
                } else {
                    Text("Not serving — start it from the menu-bar icon.").font(.caption).foregroundStyle(.secondary)
                }
            }
            Section("Radio") {
                // ★ AT THE TOP, and always visible. A radio plugged in after launch is invisible
                // until something rescans, and the only workaround was quitting the app — which is
                // an absurd thing to ask of someone who just plugged a USB stick in.
                HStack {
                    LabeledContent("Receiver",
                                   value: server.devices.isEmpty ? "none connected" : server.deviceName)
                    Spacer()
                    Button("Refresh") { server.rescan() }
                }
                // ★ A PICKER once there is a choice. The app used to serve whichever radio
                // enumerated first, with no way to say otherwise — which was invisible while a
                // dongle was the only thing anyone owned, and became "I can't see the SDRplay"
                // the moment a second radio was plugged in beside it.
                if server.devices.count > 1 {
                    Picker("Use", selection: $server.wantedDevice) {
                        ForEach(server.devices, id: \.self) { d in Text(d).tag(d) }
                    }
                    Text("Restart the server to change radio.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                // ★ These are the LISTENER'S STARTING POINT, not a property of the radio — the
                // web client remembers where it was last, so in practice this only decides what a
                // brand-new listener lands on. Labelled so an owner can tell that from the name.
                TextField("Listener's starting frequency (Hz)", value: $server.centreHz, format: .number)
                Picker("Listener's starting mode", selection: $server.mode) {
                    Text("WFM").tag("wfm"); Text("AM").tag("am")
                    Text("USB").tag("usb"); Text("LSB").tag("lsb"); Text("NFM").tag("nfm")
                }
                Text("Where a listener's radio is tuned when they first open the page. Their browser remembers where they were after that, so this mostly affects first-time visitors.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            // ★★ WHERE THE ANTENNA IS. Not where the listener is — distances, map centring and
            // the ITU region all follow the receiver, and a VibeServer may be left anywhere.
            // ★ Country alone is enough for the RDS flag, and it defaults from the Mac, so the
            // common case needs no typing at all.
            Section("Location") {
                TextField("Receiver name", text: $server.rxName, prompt: Text("Coastal SDR — 60 m vertical"))
                Text("Shown to listeners over the spectrum, with the place below it. Leave it "
                     + "blank and they just see the location.")
                    .font(.caption).foregroundStyle(.secondary)
                TextField("Country", text: $server.rxIso, prompt: Text("GB"))
                Text("Two-letter code. Sets the RDS country and flag, and the band plan's ITU region. "
                     + "Filled in from this Mac's own region.")
                    .font(.caption).foregroundStyle(.secondary)
                TextField("Place", text: $server.rxPlace, prompt: Text("Cardiff"))
                HStack {
                    TextField("Latitude",  text: $server.rxLat, prompt: Text("52.24"))
                    TextField("Longitude", text: $server.rxLon, prompt: Text("-0.90"))
                }
                TextField("Or locator", text: $server.rxGrid, prompt: Text("IO81jm"))
                HStack {
                    // ★★ THE LABEL SETS THE EXPECTATION. "Use this Mac's location" promises a location and
                    // delivers a SQUARE — Hans pressed it expecting coordinates, got "JO32bl", and had
                    // to search the web to learn it was his own town (2026-07-27). Naming the output
                    // costs nothing and removes the surprise before it happens.
                    Button(finder.busy ? "Locating…" : "Fill in locator from this Mac") {
                        finder.find { lat, lon, town, iso in
                            // ★ Fill the LOCATOR, not the coordinates. See LocationFinder:
                            // publishing a square is the whole point of doing this coarsely,
                            // and the operator can still type exact coordinates if they want
                            // them. Anything already typed is left alone.
                            let sq = server.maidenhead(lat, lon)
                            server.rxGrid = sq
                            if server.rxPlace.trimmingCharacters(in: .whitespaces).isEmpty,
                               let t = town { server.rxPlace = t }
                            if let c = iso { server.rxIso = c.uppercased() }
                            // ★★ SAY WHAT IT DID. It succeeded silently before: a locator
                            // appeared in a field the user was not looking at, no coordinates
                            // arrived because we deliberately do not publish them, and there
                            // was no way to tell success from the failure they had just been
                            // having. HansVanEijsden had to search the web for "JO32bl" to
                            // find out whether it was his own town (2026-07-27).
                            // ★ The reassurance matters as much as the fact: this is the ONE
                            // control that touches their location, so "your exact position is
                            // not published" belongs in the confirmation, not only in the help
                            // text underneath that nobody reads before pressing a button.
                            finder.statusIsError = false
                            finder.status = town.map {
                                "Filled in locator \(sq) — a square a few km across near \($0). "
                                + "Your exact coordinates are not published."
                            } ?? "Filled in locator \(sq) — a square a few km across. "
                                + "Your exact coordinates are not published."
                        }
                    }
                    .disabled(finder.busy)
                    Spacer()
                }
                if !finder.status.isEmpty {
                    Text(finder.status).font(.caption)
                        .foregroundStyle(finder.statusIsError ? .orange : .green)
                }
                // ★ Leads with WHY it is a grid square. A listener seeing only a locator reads it
                // as the app being coy or limited; said plainly, it is a deliberate protection.
                // Most operators are not happy publishing a home address to everyone who connects
                // (Stuart, 2026-07-27, after Hans expected exact coordinates and had to look the
                // square up to check it was even his town).
                Text("A receiver's position is published to everyone who connects to it, so this "
                     + "is a MAIDENHEAD GRID SQUARE rather than an address — roughly 4 km across, "
                     + "which is all a distance or a bearing needs, and not enough to find you by."
                     + "\n\n"
                     + "\"Fill in locator from this Mac\" works that square out for you. macOS "
                     + "asks for permission the first time, and needs Wi-Fi switched on — a Mac "
                     + "locates itself from nearby networks, so it can fail on a wired connection."
                     + "\n\n"
                     + "You can type a locator by hand instead, or give exact coordinates if you "
                     + "would rather — those win if you give both.")
                    .font(.caption).foregroundStyle(.secondary)
                if !server.locationJson().isEmpty && server.running {
                    Text("Restart the server to apply a change.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Section("Access") {
                // ★ Also a secret, and shoulder-surfable for the same reason.
                SecureField("PIN", text: $server.pin, prompt: Text("Open — no PIN"))
                Text("Network listeners must enter this. This Mac never has to.")
                    .font(.caption).foregroundStyle(.secondary)
                // ★ SecureField, not TextField. It is a password, and a settings pane is exactly where
                // someone reads over your shoulder (Stuart, 2026-07-27).
                // ★★ SECURITY, as ONE section. Two secrets with two different jobs; kept
                // apart on the screen, they invited the reading that one replaces the other
                // (Stuart, 2026-07-27). PIN = the door. Password = the controls inside.
                Text("SECURITY").font(.headline).padding(.top, 6)
                Text("Two separate protections, and they are independent on purpose.\n\n"
                   + "PIN — PROTECTS THE CONNECTION. Decides who may connect and listen at all. "
                   + "Without it, anyone who can reach this server can use the radio.\n\n"
                   + "PASSWORD — PROTECTS THE SERVER AND HARDWARE. Anyone already listening can "
                   + "still tune and set the gain; that is what a receiver is for. The password "
                   + "guards the few settings that can damage equipment or leave the radio "
                   + "broken for the next person: bias-T, direct sampling and calibration.\n\n"
                   + "A public receiver typically has NO PIN, so everyone can listen — and a "
                   + "password, so no visitor can put DC on your feedline.")
                    .font(.caption).foregroundStyle(.secondary)
                adminPasswordRow
                Text("A SECOND password, for a different job. The PIN decides who may listen; "
                   + "this decides who may change the settings a visitor has no business "
                   + "touching on someone else's radio:\n\n"
                   + "• BIAS-T — it puts DC on the feedline, and a stranger switching it on can "
                   + "damage whatever is connected.\n"
                   + "• DIRECT SAMPLING — reconfigures the front end; left on, the receiver "
                   + "looks broken to everyone after.\n"
                   + "• CALIBRATION — miscalibrates the radio invisibly and permanently.\n\n"
                   + "Tuning, mode and the audio controls stay open to listeners — those are "
                   + "what anyone needs to actually use the receiver, and they undo in a click.\n\n"
                   + "Listeners see those "
                   + "controls locked with a box to unlock them — which is how YOU change them "
                   + "on your own server from anywhere.")
                    .font(.caption).foregroundStyle(.secondary)
                // ★★ THE TIME LIMIT. Only earns its place on a PUBLIC receiver, which is
                // why the default is Unlimited and the help says plainly when to leave it alone.
                Picker("Time limit per listener", selection: $server.sessionLimitMin) {
                    Text("Unlimited").tag(0)
                    Text("15 minutes").tag(15)
                    Text("30 minutes").tag(30)
                    Text("45 minutes").tag(45)
                    Text("1 hour").tag(60)
                    Text("2 hours").tag(120)
                }
                Text("For a receiver you have put on the internet. This server serves ONE "
                   + "listener at a time, so without a limit the first person to connect can "
                   + "hold it all evening and everyone else just sees IN USE.\n\n"
                   + "A listener is warned at two minutes and again at thirty seconds, then "
                   + "disconnected with an explanation. Their address is then held off for two "
                   + "minutes — otherwise their client would simply reconnect and carry on, and "
                   + "the limit would achieve nothing.\n\n"
                   + "YOU are not affected: listening on this Mac is exempt, and so is any "
                   + "session unlocked with the admin password. Leave it Unlimited for a private "
                   + "receiver.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Serve the browser client", isOn: $server.serveWeb)
                Picker("Uncompressed audio", selection: $server.uncompressedAudio) {
                    Text("Off").tag(0)
                    Text("Listener's choice").tag(1)
                    Text("Compatibility only").tag(2)
                }
                Text("Listeners normally get compressed audio — around 10 KB/s each. Uncompressed "
                   + "is roughly 187 KB/s per listener out of YOUR connection, some twenty times "
                   + "more.\n\n"
                   + "OFF — nobody gets it. A client too old to decode the compressed stream is "
                   + "turned away with an explanation rather than left in silence.\n\n"
                   + "LISTENER'S CHOICE — a switch appears in each listener's audio menu, off by "
                   + "default. Compression is audible on good headphones, so a DXer on a fast link "
                   + "may well want the raw stream; this lets them take it without imposing it on "
                   + "everyone else.\n\n"
                   + "COMPATIBILITY ONLY — no switch is offered, but a client that cannot decode "
                   + "the compressed stream still gets raw audio rather than silence. Choose this "
                   + "to keep the safety net without advertising a 187 KB/s option.\n\n"
                   + "This Mac's own browser always gets uncompressed audio whatever you pick "
                   + "here — it never touches your uplink, so there is nothing to ration.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Advertise on the network", isOn: Binding(
                    get: { server.advertise },
                    set: { server.advertise = $0; server.applyAdvertise() }))
                Text("On, clients auto-discover this server (Bonjour). Off, it's reachable only by typing its address — a privacy choice if you'd rather not announce the radio to everyone on the network.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Station search") {
                // Off by default: it is a network fetch, and a headless/offline server should not
                // reach out uninvited. On, the server downloads the EiBi shortwave schedule and
                // serves it so a listener's search finds broadcast stations, not just their own
                // bookmarks and the band plan. The list is time-filtered to what is on air now.
                Toggle("Shortwave station list (EiBi)", isOn: Binding(
                    get: { server.eibiStations },
                    set: { server.eibiStations = $0; server.refreshEibi() }))
                if !server.eibiStatus.isEmpty {
                    Text(server.eibiStatus).font(.caption).foregroundStyle(.secondary)
                }
                Text("Downloads the EiBi schedule (eibispace.de) so listeners can search broadcast stations. A browser can't fetch it directly, so the server does. Refreshed once a day; cached for offline.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Link Management") {
                Picker("Waterfall rate", selection: $server.maxFps) {
                    Text("Full · 20 fps").tag(0.0)
                    Text("Half · 10 fps").tag(10.0)
                    Text("Quarter · 5 fps").tag(5.0)
                }
                // ★★ THE RATES THIS RADIO CAN ACTUALLY DO. The list was the RTL2832U's,
                // hardcoded — so an RSP, which runs to 10 MSPS where a dongle tops out near
                // 2.56, was capped at less than a quarter of its capability by a menu that
                // had never heard of it (Stuart, 2026-07-26).
                Picker("Capture rate", selection: $server.lockedRate) {
                    Text("Listener chooses").tag(0.0)
                    if server.isSdrplayActive {
                        // ★ 8 MHz is the real ceiling — 10 is accepted and then not sustained.
                        Text("Up to · 8 MHz").tag(8_000_000.0)
                        Text("Up to · 6 MHz").tag(6_000_000.0)
                        Text("Up to · 5 MHz").tag(5_000_000.0)
                        Text("Up to · 4 MHz").tag(4_000_000.0)
                        Text("Up to · 3 MHz").tag(3_000_000.0)
                        Text("Up to · 2 MHz").tag(2_000_000.0)
                        Text("Up to · 1 MHz").tag(1_000_000.0)
                    } else {
                        Text("Up to · 2.4 MHz").tag(2_400_000.0)
                        Text("Up to · 1.8 MHz").tag(1_800_000.0)
                        Text("Up to · 1.2 MHz").tag(1_200_000.0)
                        Text("Up to · 960 kHz").tag(960_000.0)
                    }
                }
                Text("A CEILING, not a lock: a listener can still pick a LOWER rate (a narrower span, less server CPU) — they just cannot go above this. Single radio per user, so their choice affects only them.")
                    .font(.caption).foregroundStyle(.secondary)
                Picker("Demod bandwidth", selection: $server.maxBwHz) {
                    Text("Uncapped").tag(0.0)
                    Text("Up to 200 kHz").tag(200_000.0)
                    Text("Up to 50 kHz").tag(50_000.0)
                    Text("Up to 16 kHz").tag(16_000.0)
                }
                Text("Caps what each listener may ask for. The server tells them the limit, so they settle at it instead of mistaking it for a bad connection.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Require idle power saving", isOn: $server.forceIdleSaver)
                Text("When nobody is touching the waterfall it slows down, which cuts this machine's CPU and the data it sends. Listeners can normally switch that off — turn this on to make it compulsory. Worth doing if you have a data allowance to protect, or the server runs on battery or solar.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Advanced · Network") {
                // ★ "0" is a programmer's sentinel, not an answer to a question a person asked.
                // Say what the default IS, then what typing here would do.
                TextField("Port", value: $server.wantedPort, format: .number,
                          prompt: Text("48000"))
                Text("VibeServer uses port 48000. Leave this at 0 to keep that — it will pick the next free port if 48000 is already taken. Enter a number only if you need a specific port, for example to match a router rule you have already set up.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if server.radioLost {
                Section {
                    Label("Radio disconnected", systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.orange)
                    Text("Plug the receiver back in, then press Reconnect.")
                        .font(.caption).foregroundStyle(.secondary)
                    Button("Reconnect Radio") { server.reconnectRadio() }
                }
            }
            Section("Startup") {
                Toggle("Open VibeServer at login", isOn: Binding(
                    get: { server.openAtLogin },
                    set: { server.openAtLogin = $0 }))
                Toggle("Start serving when VibeServer opens", isOn: $server.autoStart)
                Text("Both on = a rebooted Mac is serving again before anyone touches it.")
                    .font(.caption).foregroundStyle(.secondary)
                if let err = server.loginItemError {
                    Text(err).font(.caption).foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Section {
                Text(server.running
                     ? "Radio and access changes apply next time you start serving."
                     : "Radio and access changes apply when you start serving.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        // ★ NO .fixedSize(vertical:) here — it forced the Form to its full intrinsic HEIGHT, which
        // is exactly what defeated scrolling: a Form pinned to its content height cannot scroll, so
        // a window shorter than the content just clipped both ends. Let the grouped Form keep its
        // native scrolling and fill whatever height the window gives it.
        .frame(minWidth: 380)
    }
}
