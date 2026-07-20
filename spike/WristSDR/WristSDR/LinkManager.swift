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
  private static let recoverAfter = 20     // seconds above `healthyRatio`, AFTER a real failure

  /// ★ THE FIRST CLIMB IS NOT A RECOVERY, so it does not deserve the same caution.
  ///
  /// `recoverAfter` is 20s because stepping up after a genuine failure risks re-triggering it —
  /// oscillation is worse than staying low. But at connect we have not failed; we are simply being
  /// careful, and 20s of deliberately-throttled waterfall on a perfectly good link is a poor first
  /// impression. Until the link has actually starved once, climb on a shorter probe.
  ///
  /// ★★ BUT NOT TOO SHORT — IT MUST OUTLAST THE CONNECT BURST (Stuart: "keep it long enough to ride
  ///    the connection spike of data; once this settles bump it up"). Servers dump a backlog on
  ///    connect, and MEASURED it is large: the same Kiwi read 22 KB/s over a window starting at
  ///    connect and 4.5 KB/s once ~5s of settle was discarded. OWRX behaved the same way.
  ///
  ///    That burst is dangerous here precisely because it looks GOOD — frames arrive fast, the
  ///    ratio is healthy, and a short probe would climb on the strength of data the link was never
  ///    really carrying, arriving at full rate just as the burst ends. 8s clears the measured
  ///    ~5s spike with margin, and the 5-sample average smooths what is left.
  private static let firstClimbAfter = 8
  /// Has this session ever genuinely starved? Distinguishes "cautious start" from "recovering".
  private var everStarved = false
  private static let starveRatio  = 0.6
  private static let healthyRatio = 0.85

  /// ★★ START IN THE MIDDLE, EARN THE TOP (Stuart, 2026-07-20). Connection setup is the most
  ///    fragile moment there is — handshake, buffers filling, DSP spinning up, audio starting —
  ///    and asking for the MAXIMUM frame rate exactly then is what tips a marginal link over.
  ///    Measured: a fast Kiwi pushes 18.9 KB/s at full rate against a Bluetooth ceiling that can
  ///    be as low as 25 KB/s once audio is counted.
  ///
  ///    So open on the middle rung and climb only once the link has proved it can carry it. The
  ///    cost is nearly nothing: the waterfall interpolates to a 20fps render clock regardless, so
  ///    5fps still SCROLLS smoothly — what you briefly lose is time resolution, not fluidity.
  ///
  ///    It also makes the indicator honest from the first second: we open on two bars because we
  ///    are genuinely throttled, and it goes green when we have earned it.
  ///
  /// `startRung` is clamped into the ladder; pass 1 for a backend that should open at full rate.
  init(ladder: [Double], lowDataRung: Int, startRung: Int = 2,
       apply: @escaping (Int, Double) -> Void) {
    self.ladder = ladder
    self.lowDataRung = min(max(1, lowDataRung), ladder.count)
    self.apply = apply
    // ★★ THE USER'S CHOICE OUTRANKS THE CAUTIOUS START COMPLETELY (Stuart). Auto Link Management
    //    off means FULL RATE FROM THE FIRST FRAME — someone who turned adaptation off did not ask
    //    us to be careful on their behalf, and a "why is it slow at first" that they cannot switch
    //    off is worse than the stutter it avoids. Low Data likewise opens on its pinned rung.
    //    Only .adaptive gets the mid-ladder start.
    switch Self.mode {
    case .full:    self.rung = 1
    case .lowData: self.rung = self.lowDataRung
    case .adaptive:
      // ★ NEVER OPEN ON THE BOTTOM RUNG: "we don't want everything looking garbage as that gives a
      //   bad impression". The floor is for a link that has PROVED it cannot do better; arriving
      //   there before a single frame has landed judges a good link by a bad one. Clamped to at
      //   most one rung above the floor — the middle, on the 3-rung ladders we have.
      self.rung = min(max(1, startRung), max(1, ladder.count - 1))
    }
    // adaptiveRung drives the link GLYPH, and a pinned rate is a preference, not a symptom — it
    // must never light the indicator. Only the adaptive start shows as throttled, which it is.
    self.adaptiveRung = (Self.mode == .adaptive) ? self.rung : 1
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
        everStarved = true          // from here on, climbing back is a RECOVERY: be slow about it
        set(rung + 1, adaptive: true)
        starvedSecs = 0
      }
    } else if avgRatio >= Self.healthyRatio {
      healthySecs += 1; starvedSecs = 0
      let needed = everStarved ? Self.recoverAfter : Self.firstClimbAfter
      if healthySecs >= needed, rung > 1 {
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
