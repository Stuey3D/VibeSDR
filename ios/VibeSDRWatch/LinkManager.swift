import Foundation

/// ADAPTIVE WATERFALL RATE — one controller, every backend.
///
/// Ask the server for fewer waterfall frames when the link can't carry them, and step back up when
/// it recovers. The waterfall already interpolates onto a 20fps render clock, so a lower frame rate
/// costs TIME RESOLUTION, not scroll smoothness — which is the whole reason this is a good trade.
/// A stuttering link is visible and ugly; a slower one mostly is not.
///
/// Every backend has the same shape (a rate lever and a 1s frame counter) and a different ladder,
/// so the policy lives here once and each client supplies only its own rungs. Measured rates:
///
///   UberSDR    `set_rate` divisor 1/2/3  →  10 / 5 / 3.3 fps   (12.4 / 6.1 / 4.2 KB/s)
///   KiwiSDR    `wf_speed`  4/3/2         →  23 / 13 / 5  fps
///   VibeServer `fftRate`   20/10/5       →  20 / 10 / 5  fps
///   OpenWebRX  — no lever at all; fps/fft_fps/fft_size are ignored. No LinkManager.
@MainActor
final class LinkManager {

  /// What the user asked for. Not a boolean, because "slow on purpose" and "slow because the link
  /// is bad" are different states that must look different in the UI.
  enum Mode: String {
    case full        // never throttle — max quality, may stutter
    case adaptive    // follow the link (default)
    case lowData     // pin the low-data floor, no adaptation (metered plans)
  }

  static var mode: Mode {
    get { Mode(rawValue: UserDefaults.standard.string(forKey: "vibeLinkMode") ?? "") ?? .adaptive }
    set { UserDefaults.standard.set(newValue.rawValue, forKey: "vibeLinkMode") }
  }

  /// Expected fps at each rung, rung 1 (full rate) first. The LAST rung is the adaptive floor.
  private let ladder: [Double]
  /// The deepest rung a USER may pin via Low Data. Rungs below it are ADAPTIVE-ONLY — UberSDR's
  /// 3.3fps rung is jerky and reserved for a genuinely poor connection, never a user preference.
  /// Stuart: "low data rate minimum is 5fps as the interpolation can hide that."
  private let lowDataRung: Int
  /// Hands the backend a 1-based rung AND the fps that rung is expected to deliver. The backend
  /// maps the rung to its own wire value, and seeds the waterfall's cadence with the fps so the
  /// interpolator doesn't have to rediscover a rate we already know (see
  /// WaterfallBuffer.setExpectedRowRate).
  private let apply: (Int, Double) -> Void

  /// The rate actually requested (1 = full). Includes a user-pinned Low Data floor.
  private(set) var rung = 1
  /// How far the CONTROLLER has had to back off. Stays 1 in Low Data mode: a rate the user chose
  /// is a preference, not a symptom, and must never light the link indicator red.
  private(set) var adaptiveRung = 1

  private var starvedSecs = 0
  private var healthySecs = 0
  /// Recent per-second frame counts, for the RECOVERY test only.
  ///
  /// ★ `framesPerSec` is an INTEGER COUNT in a 1s window, and that quantisation is brutal at low
  /// rungs: at 5fps one late frame gives 4/5 = 0.80, which lands in the hold band and resets the
  /// healthy counter. A single late frame per second therefore blocks recovery FOREVER — the ladder
  /// holds a lower rung on a link that is actually fine (observed on-wrist, UberSDR, 2026-07-19).
  /// Averaging over several seconds turns ±1 frame from ±20% into ±4%. Starvation still uses the
  /// instantaneous value, so stepping DOWN stays fast.
  private var recent: [Double] = []
  private static let recoveryWindow = 5

  /// Degrade fast, recover slow — asymmetric on purpose. A wrong step DOWN costs a little time
  /// resolution nobody notices; a wrong step UP costs a visible stutter.
  private static let degradeAfter = 3      // seconds below `starveRatio`
  private static let recoverAfter = 20     // seconds above `healthyRatio`
  private static let starveRatio  = 0.6
  private static let healthyRatio = 0.85

  init(ladder: [Double], lowDataRung: Int, apply: @escaping (Int, Double) -> Void) {
    self.ladder = ladder
    self.lowDataRung = min(max(1, lowDataRung), ladder.count)
    self.apply = apply
  }

  /// Call once a second with the observed frame rate. `settled` is false while a tune/zoom
  /// re-subscription is in flight — frames legitimately pause there and it must not read as a
  /// bad link. `live` is false when there's no working session to judge.
  func tick(fps: Double, live: Bool, settled: Bool) {
    guard ladder.count > 1 else { return }        // backend has no lever (OWRX)

    switch Self.mode {
    case .full:
      set(1, adaptive: false)
      return
    case .lowData:
      set(lowDataRung, adaptive: false)           // pinned by choice — adaptiveRung stays 1
      return
    case .adaptive:
      break
    }

    guard live else { return }
    guard settled else { starvedSecs = 0; return }

    let expected = ladder[rung - 1]
    let ratio = expected > 0 ? fps / expected : 1

    recent.append(fps)
    if recent.count > Self.recoveryWindow { recent.removeFirst(recent.count - Self.recoveryWindow) }
    let avg = recent.isEmpty ? fps : recent.reduce(0, +) / Double(recent.count)
    let avgRatio = expected > 0 ? avg / expected : 1

    if ratio < Self.starveRatio {
      starvedSecs += 1; healthySecs = 0
      if starvedSecs >= Self.degradeAfter, rung < ladder.count {
        set(rung + 1, adaptive: true)
        starvedSecs = 0
      }
    } else if avgRatio >= Self.healthyRatio {
      healthySecs += 1; starvedSecs = 0
      if healthySecs >= Self.recoverAfter, rung > 1 {
        set(rung - 1, adaptive: true)
        healthySecs = 0
      }
    } else {
      starvedSecs = 0; healthySecs = 0            // in between — hold this rung
    }
  }

  /// Re-assert the current rung — call after a reconnect, where the server starts at its default.
  func reassert() { if rung != 1 { apply(rung, ladder[rung - 1]) } }

  private func set(_ r: Int, adaptive: Bool) {
    let clamped = min(max(1, r), ladder.count)
    adaptiveRung = adaptive ? clamped : 1
    guard clamped != rung else { return }
    rung = clamped
    starvedSecs = 0; healthySecs = 0
    recent.removeAll()          // the old rung's counts say nothing about the new one
    apply(clamped, ladder[clamped - 1])
  }
}
