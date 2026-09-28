import Foundation
import Darwin
import WatchKit

/// THE POINT OF THE WHOLE SPIKE.
///
/// The companion watch app costs ~34% of a core just to DRAW rows the phone has already
/// computed (measured on-device 2026-07-13). JR adds the FFT scaling, the Opus decode and
/// the network link on top of that. Nobody knows what that comes to, and no amount of
/// reasoning will tell us — so the app measures itself.
///
/// It logs to a FILE, not to a log: NSLog goes to the unified log and print() to stdout,
/// and NEITHER reaches `devicectl … --console` from a Release build. A file survives the
/// screen going off, which is exactly the state we care about.
///
/// The `gap` column is the headline. watchOS suspends an app when the screen sleeps, so a
/// suspended app CANNOT report its own CPU — a single `gap=90s` line is proof it spent
/// nothing, and it is the only way to measure a thing that isn't running.
@MainActor
final class Vitals: ObservableObject {

  @Published var cpu: Double = 0
  @Published var battery: Double = -1

  private var timer: Timer?
  private var last = Date()

  private lazy var url: URL = {
    let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    return docs.appendingPathComponent("jr-vitals.log")
  }()

  /// Sampled from the client each tick so the log says what the app was DOING when it cost
  /// what it cost — a CPU number with no workload attached to it is not evidence.
  var framesPerSec: () -> Double = { 0 }
  var audioPerSec:  () -> Double = { 0 }
  var audioLive:    () -> Bool   = { false }

  /// ★★★ THE PREVIOUS RUN'S DYING BREATH. Jr quits after minutes on UberSDR, at random, and
  /// leaves NO crash report of any kind — and `start()` then deleted the one record that
  /// existed, so every launch destroyed the evidence for the failure we were chasing. The tail
  /// of the old log is now copied aside BEFORE the wipe, and shown in the menu, so the last
  /// line before it died can simply be read off the wrist.
  ///
  /// ★ It also answers the question the log comment below says it cannot: a run that ended with
  /// a SCENE crumb (`phase=background`) was suspended, and one that stops mid-tick was KILLED.
  /// Those need opposite fixes, and until now we could not tell them apart.
  @Published var lastBreath = ""

  /// ★★★ NOTHING CALLS `start()`. The sampling timer has never run in this build — only the
  /// static `crumb` path does — which is why the rollover has to be a static launch step and
  /// not part of it. (It also means the log was never being wiped, so the crumbs from the run
  /// that died are ALREADY on the watch; we simply had no way to read them.)
  static private(set) var lastBreath = ""

  /// Call once at launch, before anything crumbs. Copies the previous run's tail aside and
  /// starts the live log clean, so "what happened just before it vanished" is one file and not
  /// a guess about which lines belong to which run.
  static func rollover() {
    let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    let live  = docs.appendingPathComponent("jr-vitals.log")
    let saved = docs.appendingPathComponent("jr-lastbreath.log")
    if let old = try? Data(contentsOf: live), !old.isEmpty {
      var tail = old.suffix(6 * 1024)
      if let nl = tail.firstIndex(of: 0x0A) { tail = tail[tail.index(after: nl)...] }
      try? Data(tail).write(to: saved, options: [.noFileProtection])
    }
    lastBreath = (try? String(contentsOf: saved, encoding: .utf8)) ?? ""
    try? Data().write(to: live, options: [.noFileProtection])
    crumb("LAUNCH")
  }

  private lazy var breathURL: URL = {
    FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("jr-lastbreath.log")
  }()

  func start() {
    WKInterfaceDevice.current().isBatteryMonitoringEnabled = true

    lastBreath = Vitals.lastBreath

    // Data protection OFF. iOS/watchOS default new files to complete protection, so the
    // instant the screen locks the app can no longer open its OWN log — every write fails
    // silently and the log stops at exactly the moment the experiment begins. (Learned the
    // hard way measuring the phone.)
    try? FileManager.default.removeItem(at: url)
    try? Data().write(to: url, options: [.noFileProtection])

    let t = Timer(timeInterval: 2, repeats: true) { [weak self] _ in
      Task { @MainActor in self?.tick() }
    }
    RunLoop.main.add(t, forMode: .common)
    timer = t
  }

  /// BREADCRUMBS. A Release build on a watch gives you no console and no crash log you can
  /// reach — so the app writes down where it got to, and the LAST LINE BEFORE IT DIED is
  /// the answer. Cheap, ugly, and it works when nothing else does.
  /// SUSPENDED OR DEAD? The log cannot tell them apart, and they need opposite fixes.
  ///
  /// Both look identical from the outside: the ticks stop, and the audio fades as the
  /// headphones drain a buffer nobody is filling. We have spent an evening fixing a
  /// background-audio bug on the assumption watchOS was suspending us — but a suspended app
  /// COMES BACK when the wrist comes up, and this one never does. That is not a suspension.
  ///
  /// So catch the death and write it down. If a crumb appears, we crashed and the whole
  /// background-audio theory was chasing a ghost. If the log simply stops with no crumb, we
  /// really were suspended.
  ///
  /// (Signal handlers should only call async-signal-safe functions. This one does not — but
  /// a diagnostic that usually works beats a diagnosis we cannot make at all, and we are
  /// dying anyway.)
  /// (There was a home-made crash catcher here. It CRASHED — installing the signal handler
  /// itself trapped — so the diagnostic became the bug, and the app would not launch at all.
  /// It was never needed: the REAL crash reports are right there on the device and come
  /// symbolicated, which beats anything we can write in-process:
  ///
  ///   xcrun devicectl device copy from --device <id> --domain-type systemCrashLogs \
  ///     --source . --destination /tmp/wcrash
  ///   xcrun atos -o build/.../WristSDR.app.dSYM/Contents/Resources/DWARF/WristSDR \
  ///     -arch arm64 -l <base> <base+imageOffset>
  ///
  /// Use that. Do not re-invent this.

  /// ★★ BOUNDED. This appends for the life of the install — every route change,
  /// interruption and restart writes a line — and nothing ever truncated it, so a
  /// heavy listener accumulated a file on a device with very little room and no way
  /// to see or clear it. A diagnostic that grows without limit is a defect, however
  /// small each write is.
  ///
  /// Keeps the TAIL, not the head: the interesting lines are the ones just before
  /// whatever went wrong, so the newest are the ones worth having. Trimmed on a
  /// line boundary so the file never starts mid-record.
  private static let logCap = 64 * 1024
  private static let logKeep = 32 * 1024

  nonisolated static func crumb(_ s: String) {
    let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    let u = docs.appendingPathComponent("jr-vitals.log")
    let line = "\(ISO8601DateFormatter().string(from: Date())) · \(s)\n"
    guard let d = line.data(using: .utf8) else { return }

    let size = (try? FileManager.default.attributesOfItem(atPath: u.path)[.size] as? Int) ?? 0
    if (size ?? 0) > logCap, let all = try? Data(contentsOf: u) {
      var tail = all.suffix(logKeep)
      // Drop the partial first line so the file always starts at a record.
      if let nl = tail.firstIndex(of: 0x0A) { tail = tail[tail.index(after: nl)...] }
      try? (Data(tail) + d).write(to: u, options: [.noFileProtection])
      return
    }
    if let h = try? FileHandle(forWritingTo: u) {
      defer { try? h.close() }
      try? h.seekToEnd()
      try? h.write(contentsOf: d)
    } else {
      try? d.write(to: u, options: [.noFileProtection])
    }
  }

  private func tick() {
    let now = Date()
    let gap = now.timeIntervalSince(last)
    last = now

    let c = Self.processCpuPercent()
    cpu = c
    battery = Double(WKInterfaceDevice.current().batteryLevel)

    let line = String(
      format: "%@ cpu=%.1f%% batt=%.0f%% fps=%.1f audio/s=%.1f audioLive=%d gap=%.1fs\n",
      ISO8601DateFormatter().string(from: now),
      c, battery * 100,
      framesPerSec(), audioPerSec(), audioLive() ? 1 : 0, gap)

    guard let d = line.data(using: .utf8) else { return }
    if let h = try? FileHandle(forWritingTo: url) {
      defer { try? h.close() }
      try? h.seekToEnd()
      try? h.write(contentsOf: d)
    }
  }

  /// Whole-process CPU as a percentage of ONE core (so >100% is possible and normal — the
  /// DSP, the audio and the render are different threads).
  static func processCpuPercent() -> Double {
    var threadList: thread_act_array_t?
    var threadCount = mach_msg_type_number_t(0)
    guard task_threads(mach_task_self_, &threadList, &threadCount) == KERN_SUCCESS,
          let threads = threadList else { return -1 }
    defer {
      vm_deallocate(mach_task_self_, vm_address_t(UInt(bitPattern: threads)),
                    vm_size_t(Int(threadCount) * MemoryLayout<thread_t>.stride))
    }
    var total = 0.0
    for i in 0..<Int(threadCount) {
      var info = thread_basic_info()
      // THREAD_BASIC_INFO_COUNT is a C macro and does not reach Swift — it is just the
      // struct's size in natural_t words, which is what thread_info actually wants.
      var count = mach_msg_type_number_t(
        MemoryLayout<thread_basic_info_data_t>.size / MemoryLayout<natural_t>.size)
      let kr = withUnsafeMutablePointer(to: &info) {
        $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
          thread_info(threads[i], thread_flavor_t(THREAD_BASIC_INFO), $0, &count)
        }
      }
      guard kr == KERN_SUCCESS, info.flags & TH_FLAGS_IDLE == 0 else { continue }
      total += Double(info.cpu_usage) / Double(TH_USAGE_SCALE) * 100.0
    }
    return total
  }
}

// ── Bad messages ─────────────────────────────────────────────────────────────────────────────
//
/// ★★★ ONE BAD MESSAGE IS ONE DROPPED MESSAGE — NEVER A DEAD STREAM, AND NEVER A SILENT ONE.
///
/// Stuart, 2026-09-28: "did you at least do the hardening we discussed to prevent a bad packet from
/// taking down the whole app like the broken RDS used to". On the phone a throw can be caught; on
/// the watch a malformed number does not throw at all — Swift TRAPS: `Int(someDouble)` on a value
/// outside Int's range, `Int(someUInt32)` above 2^31 on arm64_32 (Int is 32-bit below watchOS 27),
/// an index past the end of a frame. A trap is the whole app gone, audio and all, off the wrist,
/// with no crash report we can read. So the parsers are made TOTAL — every input either decodes or
/// is refused — and every refusal comes through here.
///
/// ★★ COUNTED AND LOGGED, BECAUSE A REFUSAL NOBODY HEARS IS THE OLD BUG IN A NEW PLACE. The
///    sockets used to `try?` their JSON and `return` on a short frame, so a server sending garbage
///    looked exactly like a server sending nothing — a black waterfall with a healthy frame count.
///    Every refusal is tallied per source+kind, and written to the vitals log (jr-vitals.log, which
///    survives into LAST RUN) and the system log.
/// ★★ RATE-LIMITED PER KIND. A server that sends the same broken frame twenty times a second must
///    not turn the log into that frame: the first is written at once, then at most one line per
///    kind every `logEvery` seconds, carrying how many were counted and not written in between.
///    The COUNT is never limited — only the writing.
/// ★ Thread-safe and actor-free: the sockets call this from their own queues, the decoders from
///   theirs, and none of them may wait for the main actor to report a fault.
enum MsgFaults {
  private nonisolated static let lock = NSLock()
  nonisolated(unsafe) private static var counts: [String: Int] = [:]
  nonisolated(unsafe) private static var lastLogAt: [String: Double] = [:]
  nonisolated(unsafe) private static var unlogged: [String: Int] = [:]
  private nonisolated static let logEvery = 10.0

  /// Record one refused message. `source` is the socket ("uber spec", "kiwi SND"), `kind` the
  /// message type or the check that failed, `detail` what was wrong with it — never a payload body.
  nonisolated static func note(_ source: String, _ kind: String, _ detail: String) {
    let key = "\(source) \(kind)"
    let now = ProcessInfo.processInfo.systemUptime
    lock.lock()
    let n = (counts[key] ?? 0) &+ 1
    counts[key] = n
    let write = now - (lastLogAt[key] ?? -Double.infinity) >= logEvery
    var held = 0
    if write { lastLogAt[key] = now; held = unlogged[key] ?? 0; unlogged[key] = 0 }
    else { unlogged[key] = (unlogged[key] ?? 0) &+ 1 }
    lock.unlock()
    guard write else { return }
    let line = "BAD MSG \(key) #\(n)" + (held > 0 ? " (+\(held) since last line)" : "")
             + ": \(detail.prefix(160))"
    NSLog("[Jr] %@", line)
    Vitals.crumb(line)
  }

  /// Parse one JSON object, or say why not. Replaces `try? JSONSerialization…` on the socket paths,
  /// which turned a malformed message into a silent nothing.
  nonisolated static func json(_ source: String, _ data: Data) -> [String: Any]? {
    do {
      guard let j = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
        note(source, "json", "not an object (\(data.count) B)")
        return nil
      }
      return j
    } catch {
      note(source, "json", "unparseable (\(data.count) B): \(error.localizedDescription)")
      return nil
    }
  }

  /// Every refusal so far, most frequent first — for the debug block in the menu.
  nonisolated static func summary() -> String {
    lock.lock(); let c = counts; lock.unlock()
    return c.sorted { $0.value > $1.value }.prefix(6).map { "\($0.key) ×\($0.value)" }
            .joined(separator: "\n")
  }

  nonisolated static var total: Int { lock.lock(); defer { lock.unlock() }; return counts.values.reduce(0, &+) }
}

/// ★★★ NUMBERS FROM A SERVER, MADE SAFE TO CONVERT. `Int(x)` on a Double traps on NaN, on infinity
/// and on anything outside Int's range — and on arm64_32 that range is ±2.1 billion, so a centre
/// frequency of 2.4 GHz, or a corrupt one of 1e30, took the whole app down from a log line. These
/// clamp instead; a nonsense value is refused at the parser, and these make sure a value that got
/// past it can still only be WRONG, never fatal.
enum Wire {
  /// Double → Int64, NaN → 0, clamped to Int64's range. For logs and for the wire.
  nonisolated static func i64(_ d: Double) -> Int64 {
    guard d.isFinite else { return 0 }
    return Int64(max(-9.0e18, min(9.0e18, d.rounded())))
  }
  /// Double → Int, NaN → 0, clamped to THIS platform's Int (32-bit on arm64_32).
  nonisolated static func int(_ d: Double) -> Int {
    guard d.isFinite else { return 0 }
    // ★ Not Double(Int.max): at 64 bits that rounds UP to 2^63, which is itself out of range.
    let lim = Int.bitWidth == 32 ? 2_147_483_000.0 : 9.0e18
    return Int(max(-lim, min(lim, d.rounded())))
  }
  /// A finite number in a range, or nil — the gate for anything the server sends as a quantity.
  nonisolated static func inRange(_ v: Any?, _ r: ClosedRange<Double>) -> Double? {
    guard let d = (v as? NSNumber)?.doubleValue, d.isFinite, r.contains(d) else { return nil }
    return d
  }
}
