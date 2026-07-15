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

  func start() {
    WKInterfaceDevice.current().isBatteryMonitoringEnabled = true

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
  nonisolated static func crumb(_ s: String) {
    let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    let u = docs.appendingPathComponent("jr-vitals.log")
    let line = "\(ISO8601DateFormatter().string(from: Date())) · \(s)\n"
    guard let d = line.data(using: .utf8) else { return }
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
