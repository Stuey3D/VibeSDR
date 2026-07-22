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

    // Persisted so a restart keeps the operator's choices; the core still owns what they MEAN.
    @AppStorage("centreHz") var centreHz   = 96_600_000.0
    @AppStorage("mode")     var mode       = "wfm"
    @AppStorage("pin")      var pin        = ""
    @AppStorage("port")     var wantedPort = 0        // 0 = first free 48000-48049
    @AppStorage("serveWeb") var serveWeb   = true
    // ── Link management ceilings ─────────────────────────────────────────────
    // What the OPERATOR is willing to spend on each listener. The server enforces these, and —
    // crucially — publishes them so an adaptive client can see the ceiling instead of reading it
    // as a failing link and stepping down forever chasing a limit it can never reach.
    // 0 = no cap. Tiers match the Android server's (FPS_TIERS in src/services/vibeServer.ts) so a
    // setting means the same thing on every host.
    @AppStorage("maxFps")   var maxFps     = 0.0      // 0 = server default (20), else 20/10/5
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

    init() { refreshDevices() }

    /// How many listeners are connected right now.
    ///
    /// ★ HONEST LIMIT: the core currently reports a single `clientConnected` flag, not a count —
    /// multi-client is the protocol-foundations work and is not built yet. So this reads 0 or 1
    /// today and becomes a real count for free once the core exposes one. It is wired now so the
    /// menu bar does not need revisiting later.
    var listeners: Int { status.clientConnected ? 1 : 0 }

    func refreshDevices() {
        deviceCount = Int(vs_device_count())
        deviceName  = deviceCount > 0 ? String(cString: vs_device_name(0)) : ""
        onChange?()
    }

    func start() {
        lastError = nil
        var cfg = VsConfig()
        vs_default_config(&cfg)
        cfg.centreHz = centreHz
        cfg.port     = Int32(wantedPort)
        cfg.serveWebClient = serveWeb
        cfg.maxFftRate     = maxFps
        cfg.maxBandwidthHz = maxBwHz
        cfg.lockedRate     = lockedRate
        cfg.forceIdleSaver = forceIdleSaver

        // The C strings must outlive the call, so hold them across it.
        mode.withCString { modePtr in
            pin.withCString { pinPtr in
                cfg.mode = modePtr
                cfg.pin  = pinPtr
                var err = [CChar](repeating: 0, count: 256)
                let p = vs_start(&cfg, &err, 256)
                if p > 0 { port = Int(p); running = true }
                else     { lastError = String(cString: err); running = false }
            }
        }
        if running { startPolling(); startAdvertising() }
        refreshDevices()
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
        guard running, port > 0 else { return }
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
    @objc private func reconnectRadio() { server.reconnectRadio(); render() }
    @objc private func toggleServing() { server.running ? server.stop() : server.start(); render() }
    @objc private func quit() { server.stop(); NSApp.terminate(nil) }

    @objc private func openSettings() {
        if settingsWindow == nil {
            let host = NSHostingController(rootView: SettingsView(server: server))
            let w = NSWindow(contentViewController: host)
            w.title = "VibeServer Settings"
            w.styleMask = [.titled, .closable]
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
            Section("Radio") {
                LabeledContent("Receiver", value: server.deviceCount > 0 ? server.deviceName : "none connected")
                TextField("Frequency (Hz)", value: $server.centreHz, format: .number)
                Picker("Mode", selection: $server.mode) {
                    Text("WFM").tag("wfm"); Text("AM").tag("am")
                    Text("USB").tag("usb"); Text("LSB").tag("lsb"); Text("NFM").tag("nfm")
                }
            }
            Section("Access") {
                TextField("PIN", text: $server.pin, prompt: Text("Open — no PIN"))
                Text("Network listeners must enter this. This Mac never has to.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Serve the browser client", isOn: $server.serveWeb)
            }
            Section("Link Management") {
                Picker("Waterfall rate", selection: $server.maxFps) {
                    Text("Full · 20 fps").tag(0.0)
                    Text("Half · 10 fps").tag(10.0)
                    Text("Quarter · 5 fps").tag(5.0)
                }
                Picker("Capture rate", selection: $server.lockedRate) {
                    Text("Listener chooses").tag(0.0)
                    Text("Pinned · 2.4 MHz").tag(2_400_000.0)
                    Text("Pinned · 1.8 MHz").tag(1_800_000.0)
                    Text("Pinned · 1.2 MHz").tag(1_200_000.0)
                    Text("Pinned · 960 kHz").tag(960_000.0)
                }
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
                TextField("Port", value: $server.wantedPort, format: .number,
                          prompt: Text("Automatic"))
                Text("Leave at 0 unless something else needs this port.")
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
        .frame(width: 380)
        .fixedSize(horizontal: false, vertical: true)
    }
}
