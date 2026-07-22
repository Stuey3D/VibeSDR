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
    @AppStorage("autoStart") var autoStart = true     // start serving as soon as the app launches

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
        NSWorkspace.shared.open(url)
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
        if server.running {
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
        let n = server.listeners
        let who = n == 0 ? "nobody listening" : (n == 1 ? "1 listener" : "\(n) listeners")
        return "Serving on \(server.address) — \(who)"
    }

    // ── Actions ──────────────────────────────────────────────────────────────
    @objc private func openBrowser() { server.openInBrowser() }
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
            Section("Advanced · Network") {
                TextField("Port", value: $server.wantedPort, format: .number,
                          prompt: Text("Automatic"))
                Text("Leave at 0 unless something else needs this port.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section {
                Toggle("Start serving when VibeServer opens", isOn: $server.autoStart)
                Text(server.running
                     ? "Changes apply next time you start serving."
                     : "Changes apply when you start serving.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .frame(width: 380)
        .fixedSize(horizontal: false, vertical: true)
    }
}
