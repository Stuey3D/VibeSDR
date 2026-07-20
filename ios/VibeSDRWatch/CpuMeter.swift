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
  static let enabled = true

  @Published var cpu: Double = 0

  private var timer: Timer?

  func start() {
    guard Self.enabled, timer == nil else { return }
    // 2s cadence matches the spike's Vitals log, so the two readouts are directly
    // comparable rather than sampled on different clocks.
    let t = Timer(timeInterval: 2, repeats: true) { [weak self] _ in
      Task { @MainActor in self?.cpu = CpuMeter.processCpuPercent() }
    }
    RunLoop.main.add(t, forMode: .common)
    timer = t
    cpu = CpuMeter.processCpuPercent()
  }

  func stop() { timer?.invalidate(); timer = nil }

  /// Whole-process CPU as a percentage of ONE core (>100% is possible and normal —
  /// render, audio and any DSP are different threads). Copied verbatim in method
  /// from the spike's Vitals so the comparison is exact.
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
