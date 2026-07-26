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

// ── Server: the one object that owns the core ────────────────────────────────
@MainActor
final class Server: ObservableObject {
    @Published var running = false
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
    @AppStorage("port")     var wantedPort = 0        // 0 = first free 48000-48049
    @AppStorage("serveWeb") var serveWeb   = true
    /// Owner policy — see the Settings toggle for the reasoning. Off by default:
    /// uncompressed audio is ~187 KB/s per listener out of THIS machine's uplink.
    @AppStorage("allowUncompressedAudio") var allowUncompressed = false
    // ★★ RECEIVER LOCATION. It is the SERVER's position, never the listener's: distances,
    // map centring and the ITU REGION all follow the ANTENNA (80m is 3.5-3.8 in R1 but
    // 3.5-4.0 in R2), and a VibeServer may well be left at a relative's house.
    // ★ It also decides the RDS COUNTRY and flag. The flag logic refuses to invent one —
    // it needs either an Extended Country Code off the air or the PI's country nibble
    // VALIDATED against the receiver's country — so with no location set, every station's
    // country stayed blank and it looked like a decoder fault (found on air 2026-07-26).
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
    func rescan() {
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
        if lat == nil && lon == nil && place.isEmpty && iso.isEmpty && grid.isEmpty { return "" }
        var parts: [String] = []
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
    private func maidenhead(_ lat: Double, _ lon: Double) -> String {
        let a = Int(UnicodeScalar("A").value)
        let lo = lon + 180.0, la = lat + 90.0
        let f1 = Int(lo / 20.0), f2 = Int(la / 10.0)
        let s1 = Int((lo - Double(f1) * 20.0) / 2.0), s2 = Int(la - Double(f2) * 10.0)
        let t1 = Int((lo - Double(f1) * 20.0 - Double(s1) * 2.0) * 12.0)
        let t2 = Int((la - Double(f2) * 10.0 - Double(s2)) * 24.0)
        func ch(_ i: Int) -> String { String(UnicodeScalar(UInt8(a + i))) }
        return ch(f1) + ch(f2) + "\(s1)\(s2)" + ch(t1).lowercased() + ch(t2).lowercased()
    }

    func start() {
        lastError = nil
        var cfg = VsConfig()
        vs_default_config(&cfg)
        cfg.centreHz = centreHz
        cfg.deviceIndex = Int32(deviceIndex)
        activeDevice = devices.indices.contains(deviceIndex) ? devices[deviceIndex] : ""
        cfg.port     = Int32(wantedPort)
        cfg.serveWebClient = serveWeb
        cfg.allowUncompressedAudio = allowUncompressed
        cfg.maxFftRate     = maxFps
        cfg.maxBandwidthHz = maxBwHz
        cfg.lockedRate     = lockedRate
        cfg.forceIdleSaver = forceIdleSaver
        let locJson = locationJson()

        // The C strings must outlive the call, so hold them across it.
        mode.withCString { modePtr in
          pin.withCString { pinPtr in
            locJson.withCString { locPtr in
                cfg.mode = modePtr
                cfg.pin  = pinPtr
                cfg.locationJson = locJson.isEmpty ? nil : locPtr
                var err = [CChar](repeating: 0, count: 256)
                let p = vs_start(&cfg, &err, 256)
                if p > 0 { port = Int(p); running = true; refreshEibi() }
                else     { lastError = String(cString: err); running = false }
            }
          }
        }
        if running { startPolling(); startAdvertising() }
        refreshDevices()
    }

    /// The operator flipped the Bonjour toggle — apply it live.
    func applyAdvertise() {
        if advertise, running, listeners == 0 { startAdvertising() } else { stopAdvertising() }
        onChange?()
    }

    func stop() {
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
            let n = server.listeners
            button.title = n > 0 ? " \(n)" : ""
            button.toolTip = n > 0
                ? "VibeServer — \(n) listening · \(server.address)"
                : "VibeServer — serving, nobody listening · \(server.address)"
        } else {
            button.title = ""
            button.toolTip = server.deviceCount == 0
                ? "VibeServer — no RTL-SDR connected"
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
            menu.addItem(withTitle: "Stop Serving", action: #selector(toggleServing), keyEquivalent: "")
        } else {
            let start = NSMenuItem(title: "Start Serving", action: #selector(toggleServing), keyEquivalent: "")
            start.isEnabled = server.deviceCount > 0
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
            return server.deviceCount == 0 ? "No RTL-SDR connected" : "Stopped"
        }
        if server.radioLost { return "Radio disconnected — plug it back in, then Reconnect Radio" }
        let n = server.listeners
        let who = n == 0 ? "nobody listening" : (n == 1 ? "1 listener" : "\(n) listeners")
        return "Serving on \(server.address) — \(who)"
    }

    // ── Actions ──────────────────────────────────────────────────────────────
    @objc private func openBrowser() { server.openInBrowser() }
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

    var body: some View {
        Form {
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
                TextField("Country", text: $server.rxIso, prompt: Text("GB"))
                Text("Two-letter code. Sets the RDS country and flag, and the band plan's ITU region. "
                     + "Filled in from this Mac's own region.")
                    .font(.caption).foregroundStyle(.secondary)
                TextField("Place", text: $server.rxPlace, prompt: Text("Northampton"))
                HStack {
                    TextField("Latitude",  text: $server.rxLat, prompt: Text("52.24"))
                    TextField("Longitude", text: $server.rxLon, prompt: Text("-0.90"))
                }
                TextField("Or locator", text: $server.rxGrid, prompt: Text("IO92ng"))
                Text("Optional, and either will do. Coordinates give exact distances and bearings; "
                     + "a locator gives the same to within a few km without publishing where you "
                     + "live. Coordinates win if you give both.")
                    .font(.caption).foregroundStyle(.secondary)
                if !server.locationJson().isEmpty && server.running {
                    Text("Restart the server to apply a change.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Section("Access") {
                TextField("PIN", text: $server.pin, prompt: Text("Open — no PIN"))
                Text("Network listeners must enter this. This Mac never has to.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Serve the browser client", isOn: $server.serveWeb)
                Toggle("Allow uncompressed audio (compatibility)", isOn: $server.allowUncompressed)
                Text("Off, listeners use the compressed audio stream — around 10 KB/s each. On, a "
                   + "client that cannot decode it gets raw audio instead: roughly 187 KB/s per "
                   + "listener out of YOUR connection, some twenty times more for no gain in "
                   + "quality.\n\n"
                   + "Everything that connects to VibeServer today decodes the compressed stream — "
                   + "VibeSDR 10 and later, VibeSDR Jr, and the browser client. So there is really "
                   + "only one reason to switch this on: ANY VibeSDR BEFORE VERSION 10 NEEDS IT. "
                   + "(A pre-2017 browser would too — Safari 11 was the last major browser to add "
                   + "Opus — but nothing that old can run the client anyway.)\n\n"
                   + "Leave it off unless someone tells you they have no audio, especially if your "
                   + "uplink is modest.")
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
