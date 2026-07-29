import Foundation
import Darwin

/// A tiny on-wrist CPU readout for the COMPANION app, so its cost can be compared
/// directly against the standalone JR spike (spike/WristSDR `Vitals`). Same
/// measurement method as the spike — whole-process CPU as a percentage of ONE core
/// via task_threads + thread_info(THREAD_BASIC_INFO) — so the two numbers are
/// apples-to-apples. The companion only DRAWS rows the phone computed (~34% of a
/// core, measured 2026-07-13); the spike adds FFT scaling, Opus and the network
/// link on top, and this is how we see the gap on the same wrist.
///
/// TESTING AID: `enabled` gates the on-screen badge. Leave it OFF for any public
/// release — it's a developer comparison overlay, not a user feature.
@MainActor
final class CpuMeter: ObservableObject {

  /// Flip to false (or delete the badge in ContentView) before a store/TestFlight
  /// build. On while Stuart is comparing companion vs standalone.
  // ★★ ON for the Series 6 / watchOS 26 investigation (2026-07-29), shipped only to
  //    INTERNAL TestFlight. MUST be false before any App Store submission — it is a
  //    developer comparison overlay, not a feature.
  static let enabled = true

  @Published var cpu: Double = 0
  /// Resident footprint in MB — the number jetsam actually judges us on.
  @Published var memMB: Double = 0
  /// Highest footprint seen this launch. A watch app is killed at a LIMIT, so the peak is
  /// the number that mattered, not whatever it happens to be when you look.
  @Published var peakMB: Double = 0
  private static var crumbedPeak: Double = 0

  private var timer: Timer?

  func start() {
    guard Self.enabled, timer == nil else { return }
    // 2s cadence matches the spike's Vitals log, so the two readouts are directly
    // comparable rather than sampled on different clocks.
    let t = Timer(timeInterval: 2, repeats: true) { [weak self] _ in
      Task { @MainActor in
        guard let self else { return }
        self.cpu = CpuMeter.processCpuPercent()
        let m = CpuMeter.footprintMB()
        self.memMB = m
        if m > self.peakMB { self.peakMB = m }
        // ★★ THE LEAK TEST, WRITTEN DOWN. Jr dies after minutes on UberSDR with no crash report
        //    of any kind — consistent with jetsam, which leaves none. Crumbing only each new 2MB
        //    high-water mark turns jr-vitals.log into a growth curve instead of a flood: a
        //    footprint that climbs steadily until the app vanishes IS a leak; one that sits flat
        //    and still gets killed is system pressure, and those need opposite fixes.
        if m > CpuMeter.crumbedPeak + 2 {
          CpuMeter.crumbedPeak = m
          Vitals.crumb(String(format: "MEM peak %.1fMB cpu %.0f%%", m, self.cpu))
        }
      }
    }
    RunLoop.main.add(t, forMode: .common)
    timer = t
    cpu = CpuMeter.processCpuPercent()
  }

  func stop() { timer?.invalidate(); timer = nil }

  /// Resident footprint (phys_footprint) in MB — the same accounting jetsam uses, which is why
  /// it is this and not resident_size: footprint counts dirty + compressed pages charged to us,
  /// so compressed memory still shows up. On a watch the limit is small and the waterfall,
  /// the Opus decoder and the audio buffers are all real; guessing is not good enough.
  static func footprintMB() -> Double {
    var info = task_vm_info_data_t()
    var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size) / 4
    let kr = withUnsafeMutablePointer(to: &info) {
      $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
        task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
      }
    }
    guard kr == KERN_SUCCESS else { return 0 }
    return Double(info.phys_footprint) / 1_048_576.0
  }

  /// Whole-process CPU as a percentage of ONE core (>100% is possible and normal —
  /// render, audio and any DSP are different threads). Copied verbatim in method
  /// from the spike's Vitals so the comparison is exact.
  // ★★★ CUMULATIVE, NOT INSTANTANEOUS. `thread_basic_info.cpu_usage` is the scheduler's
  // SNAPSHOT of what each thread is doing at the instant you ask — so it misses everything that
  // happened between samples, and on watchOS 27 it now reads 0 for this process every time
  // (Stuart's screenshot, 2026-07-29: a proud "CPU 0%" over a running waterfall). The July notes
  // already flagged it as noisy — "catches lulls between frames" — and a method that can report
  // zero for a busy app is not a measurement, it is an accident that used to work.
  //
  // ★ This instead totals the process's CONSUMED CPU TIME (live threads via TASK_THREAD_TIMES_INFO
  // plus already-exited ones via MACH_TASK_BASIC_INFO — you need BOTH, or short-lived worker
  // threads vanish from the total) and divides the delta by elapsed wall time. Nothing can happen
  // between two samples without appearing in it.
  //
  // ★ Returns 0 on the FIRST call — there is no interval yet. That is honest; the reading settles
  // one tick later.
  private static var lastCpuSecs: Double = 0
  private static var lastSampleAt: Double = 0
  private static var lastPct: Double = 0

  static func processCpuPercent() -> Double {
    var secs = 0.0

    var basic = mach_task_basic_info()
    var bCount = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info>.size / MemoryLayout<natural_t>.size)
    let krB = withUnsafeMutablePointer(to: &basic) {
      $0.withMemoryRebound(to: integer_t.self, capacity: Int(bCount)) {
        task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), $0, &bCount)
      }
    }
    if krB == KERN_SUCCESS {
      secs += Double(basic.user_time.seconds)   + Double(basic.user_time.microseconds)   / 1e6
      secs += Double(basic.system_time.seconds) + Double(basic.system_time.microseconds) / 1e6
    }

    var times = task_thread_times_info()
    var tCount = mach_msg_type_number_t(MemoryLayout<task_thread_times_info>.size / MemoryLayout<natural_t>.size)
    let krT = withUnsafeMutablePointer(to: &times) {
      $0.withMemoryRebound(to: integer_t.self, capacity: Int(tCount)) {
        task_info(mach_task_self_, task_flavor_t(TASK_THREAD_TIMES_INFO), $0, &tCount)
      }
    }
    if krT == KERN_SUCCESS {
      secs += Double(times.user_time.seconds)   + Double(times.user_time.microseconds)   / 1e6
      secs += Double(times.system_time.seconds) + Double(times.system_time.microseconds) / 1e6
    }
    guard krB == KERN_SUCCESS || krT == KERN_SUCCESS else { return -1 }

    let now = ProcessInfo.processInfo.systemUptime
    let prevSecs = lastCpuSecs, prevAt = lastSampleAt
    // ★ Too short an interval is noise, not a reading — hold the last answer rather than divide
    //   a rounding error by a rounding error. Makes the meter safe to call at any cadence.
    if prevAt > 0, now - prevAt < 0.5 { return lastPct }
    lastCpuSecs = secs; lastSampleAt = now
    guard prevAt > 0, now > prevAt else { return 0 }
    // % of ONE core, the same denominator every earlier watch measurement used.
    lastPct = max(0, (secs - prevSecs) / (now - prevAt) * 100.0)
    return lastPct
  }

  /// The OLD instantaneous method, kept only so the two can be compared on the same wrist if the
  /// difference is ever questioned. ★ Do not use it for a reading — see above.
  static func processCpuPercentInstantaneous() -> Double {
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
