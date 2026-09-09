// VibeIQ.app — a native window around the VibeIQ page. The Go binary beside this one in
// Contents/MacOS (`vibeiq-bridge`) is the whole bridge; this app starts it with `--gui --no-open`,
// reads the page's address from its first line of output, and shows the page in a WKWebView.
// Quit kills it.
//
// ★★★ The bridge is called `vibeiq-bridge`, not `vibeiq`: on case-insensitive APFS `vibeiq` and
//   this shell's own `VibeIQ` are the SAME FILE, so the first cut launched ITSELF, which launched
//   itself… and the Mac went down under millions of copies (2026-09-09). Two guards below make
//   that impossible even if the bundle is ever built wrong again: the child is never the same
//   file as this executable, and a process that finds VIBEIQ_CHILD in its environment exits
//   at once instead of spawning.
//
// ★ Why a window and not a bare binary: a bare Mach-O cannot carry a stapled notarisation ticket,
//   so Gatekeeper phones home on first run, and double-clicking one opens Terminal — which is
//   not how people run apps (Stuart, 2026-09-09). A bundle is stapled, has an icon, and quits
//   like an app.
import SwiftUI
import WebKit

@main
struct VibeIQApp: App {
    @NSApplicationDelegateAdaptor(Delegate.self) var delegate
    var body: some Scene {
        WindowGroup("VibeIQ") {
            ContentView(url: delegate.url)
                .frame(minWidth: 600, idealWidth: 620, maxWidth: 720, minHeight: 640, idealHeight: 720, maxHeight: 900)
        }
        .windowResizability(.contentSize)
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}

final class Delegate: NSObject, NSApplicationDelegate, ObservableObject {
    @Published var url: URL? = nil
    private var proc: Process?

    func applicationDidFinishLaunching(_ n: Notification) {
        if ProcessInfo.processInfo.environment["VIBEIQ_CHILD"] != nil {
            NSLog("VibeIQ: launched as my own child — the bundle is built wrong; refusing to spawn")
            exit(70)
        }
        let exe = Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/vibeiq-bridge")
        let me = Bundle.main.executableURL?.resolvingSymlinksInPath()
        if me == nil || exe.resolvingSymlinksInPath() == me!
            || sameFile(exe, me!) || !FileManager.default.isExecutableFile(atPath: exe.path) {
            NSLog("VibeIQ: the bridge at \(exe.path) is missing or is this app itself; refusing to spawn")
            return
        }
        let p = Process()
        p.executableURL = exe
        p.arguments = ["--gui", "--no-open", "--ui-port", "0"]
        var env = ProcessInfo.processInfo.environment
        env["VIBEIQ_CHILD"] = "1"
        p.environment = env
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = FileHandle.standardError
        pipe.fileHandleForReading.readabilityHandler = { [weak self] h in
            let s = String(decoding: h.availableData, as: UTF8.self)
            // "VibeIQ window: http://127.0.0.1:PORT/"
            // ★ Search for the closing "/" AFTER the prefix, not from its start — the first "/"
            //   from the start is the one in "http://", and the window loaded "http:/" — blank
            //   (Stuart, 2026-09-09: "VibeIQ GUI is just a blank screen").
            if let r = s.range(of: "http://127.0.0.1:") {
                let rest = s[r.upperBound...]
                let digits = rest.prefix(while: { $0.isNumber })
                if !digits.isEmpty {
                    let addr = "http://127.0.0.1:" + digits + "/"
                    DispatchQueue.main.async { self?.url = URL(string: addr) }
                    h.readabilityHandler = nil
                }
            }
        }
        do { try p.run() } catch { NSLog("VibeIQ: cannot start the bridge: \(error)") }
        proc = p
    }
    func applicationWillTerminate(_ n: Notification) { proc?.terminate() }

    /// Same inode on the same device — the only test that survives case-insensitive names.
    private func sameFile(_ a: URL, _ b: URL) -> Bool {
        guard let x = try? FileManager.default.attributesOfItem(atPath: a.path),
              let y = try? FileManager.default.attributesOfItem(atPath: b.path) else { return false }
        return (x[.systemFileNumber] as? Int) == (y[.systemFileNumber] as? Int)
            && (x[.systemNumber] as? Int) == (y[.systemNumber] as? Int)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ s: NSApplication) -> Bool { true }
}

struct ContentView: View {
    @EnvironmentObject var d: Delegate
    let url: URL?
    var body: some View {
        ZStack {
            Color(red: 0.02, green: 0.027, blue: 0.04).ignoresSafeArea()
            if let u = d.url ?? url { WebView(url: u).ignoresSafeArea() }
            else { Text("Starting…").foregroundColor(.gray).font(.system(.body, design: .monospaced)) }
        }
    }
}

struct WebView: NSViewRepresentable {
    let url: URL
    func makeNSView(context: Context) -> WKWebView {
        let cfg = WKWebViewConfiguration()
        let v = WKWebView(frame: .zero, configuration: cfg)
        v.setValue(false, forKey: "drawsBackground")
        v.load(URLRequest(url: url))
        return v
    }
    func updateNSView(_ v: WKWebView, context: Context) {}
}
