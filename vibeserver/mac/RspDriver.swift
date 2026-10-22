// The RSP driver's service, run by the app — docs/OPENRSP-BUNDLE-PLAN.md §5.2 step 3.
//
// ★★★ WHY THE APP RUNS IT. OpenRSP's library (Contents/Frameworks) is only ever a CLIENT: it talks to a
//     device SERVICE over shared memory, and that service is what owns the receiver's USB and uploads its
//     firmware. On Linux systemd starts it (vibeserver-rsp.service); a drag-out-of-the-DMG Mac app has no
//     installer to register a launch daemon, so the app starts it as a child while the app is open. ONE
//     for the app: Simple mode (the server in this process) and Full mode (one engine process per radio)
//     are both clients of the same one, exactly as every VibeServer process on a Pi is.
// ★★★ ONLY WHEN NO SYSTEM STACK IS INSTALLED. SDRplay's own API registers com.sdrplay.service and owns the
//     receivers for every program on this Mac (SDRconnect, SDR++). Two services would race to upload firmware
//     and to claim the device, so if theirs is installed ours never starts and the server uses their library.
//     The decision is vs_sdrplay_run_bundled_service() — the C++ rule test-sdrplay-lib proves, not a copy.
// ★ RESTARTED IF IT EXITS, within reason: five unexpected exits in a minute is a service that cannot run on
//   this Mac, and starting it forever would only burn the CPU while saying nothing — so it stops and says so.
import Foundation

@MainActor
final class RspDriver {
    static let shared = RspDriver()

    private var proc: Process?
    private var stopping = false
    private var recentExits: [Date] = []
    /// ★ For the log and Settings: "bundled", "system" (theirs is installed), "absent" (no driver in this
    ///   bundle), or "failed" (gave up after repeated exits).
    private(set) var state = "absent"

    private var serviceURL: URL? { Bundle.main.url(forAuxiliaryExecutable: "vibeserver-rspd") }
    private var firmwareDir: URL? { Bundle.main.resourceURL?.appendingPathComponent("openrsp/firmware/own") }

    /// Starts our service if this Mac wants it. Returns true when it was started (the caller then gives it a
    /// moment before the first device scan, so a radio plugged in at launch is found on the first look).
    @discardableResult
    func startIfWanted() -> Bool {
        let has = serviceURL.map { FileManager.default.isExecutableFile(atPath: $0.path) } ?? false
        guard vs_sdrplay_run_bundled_service(has ? 1 : 0) != 0 else {
            state = has ? "system" : "absent"
            NSLog("VibeServer: RSP driver — %@", has ? "an installed SDRplay API owns the receivers; ours stays off"
                                                    : "none in this bundle")
            return false
        }
        reapStale()
        stopping = false
        return launch()
    }

    /// On quit. SIGTERM: the service releases the receiver and unlinks its shared objects on the way out.
    func stop() {
        stopping = true
        guard let p = proc, p.isRunning else { proc = nil; return }
        p.terminate()
        let deadline = Date().addingTimeInterval(2)
        while p.isRunning && Date() < deadline { usleep(50_000) }
        if p.isRunning { kill(p.processIdentifier, SIGKILL) }
        proc = nil
    }

    private func launch() -> Bool {
        guard let exe = serviceURL, let fw = firmwareDir else { return false }
        let p = Process()
        p.executableURL = exe
        var env = ProcessInfo.processInfo.environment
        // ★ The firmware lives in Resources/ (non-code must not sit in MacOS/); the service reads its set from here
        //   and checks every image against manifest.sha256 before uploading it.
        env["SDRPLAY_OWN_FIRMWARE_DIR"] = fw.path
        p.environment = env
        p.arguments = []
        p.standardOutput = FileHandle.nullDevice   // ★ it logs through the system log (LogSinkDarwin)
        p.standardError = FileHandle.nullDevice
        p.terminationHandler = { [weak self] _ in
            Task { @MainActor in self?.exited() }
        }
        do { try p.run() } catch {
            NSLog("VibeServer: RSP driver did not start: %@", error.localizedDescription)
            state = "failed"
            return false
        }
        proc = p
        state = "bundled"
        NSLog("VibeServer: RSP driver started (bundled OpenRSP, pid %d)", p.processIdentifier)
        return true
    }

    private func exited() {
        proc = nil
        if stopping { return }
        let now = Date()
        recentExits = recentExits.filter { now.timeIntervalSince($0) < 60 } + [now]
        if recentExits.count >= 5 {
            state = "failed"
            NSLog("VibeServer: RSP driver exited %d times in a minute — not restarting it; RSPs are unavailable "
                  + "until VibeServer is reopened", recentExits.count)
            return
        }
        NSLog("VibeServer: RSP driver exited — restarting it")
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
            Task { @MainActor in
                guard let self, !self.stopping, self.proc == nil else { return }
                _ = self.launch()
            }
        }
    }

    /// ★★ A SERVICE LEFT BY A CRASHED OR FORCE-QUIT APP still holds the receiver and the shared objects, and a
    ///    second one would fail to create them. Only OUR user's processes of this exact name are touched.
    private func reapStale() {
        let k = Process()
        k.executableURL = URL(fileURLWithPath: "/usr/bin/pkill")
        k.arguments = ["-TERM", "-x", "-U", String(getuid()), "vibeserver-rspd"]
        k.standardOutput = FileHandle.nullDevice
        k.standardError = FileHandle.nullDevice
        do { try k.run(); k.waitUntilExit() } catch { return }
        if k.terminationStatus == 0 {
            NSLog("VibeServer: stopped an RSP driver left running by an earlier VibeServer")
            usleep(300_000)
        }
    }
}
