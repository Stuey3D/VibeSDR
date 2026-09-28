import Foundation

/// ★★★ AUDIO SELF-HEALING — IS WHAT ARRIVES ACTUALLY BEING PLAYED?
///
/// The Swift port of src/services/audioSelfHeal.ts. ONE FILE, TWO APPS: Jr compiles it from here
/// (spike/WristSDR/WristSDR is a synchronised folder) and the iOS app's project references this
/// same file — so the phone and the watch cannot drift apart. The Kotlin port is
/// android/.../AudioSelfHeal.kt. scripts/test_audioSelfHeal.ts is the executable spec, and
/// scripts/test_audioSelfHeal_swift.sh runs THIS file against the same scenarios.
///
/// Stuart, 2026-09-28: audio stopped (FM stereo, DAB) while the server kept sending and everything
/// else looked fine; only a reconnect cured it. The native watchdogs reopen the socket when FRAMES
/// STOP ARRIVING. Nothing caught frames arriving and NOTHING PLAYING — a stopped player node, an
/// engine suspended or re-routed, a decoder that stopped emitting, frames dropped on this side.
///
/// Two monotonic counters: `rx` (packets off the socket) and `played` (frames of REAL audio the
/// output consumed). Receiving + nothing played for `stallMs` → rebuild the local pipeline; still
/// silent → reopen the socket; round again, slower (back-off, 4-a-minute cap, never permanent).
/// While `expected` is false (muted, paused, disconnected for data saver, stopped) or during a
/// `hold` (a DAB switch, a retune flush) every clock is reset, so it can never judge a pause as a
/// stall.
///
/// ★★★ arm64_32: below watchOS 27 Swift's `Int` is 32-BIT and a Double→Int overflow TRAPS (memory
///     arm64_32_int_trap). Counters are Int64 and times are Double seconds — no `Int(...)` of
///     anything time-derived anywhere in this file.
final class AudioSelfHeal {
  enum Action: String { case none, rebuildPipeline = "rebuild-pipeline", reopenSocket = "reopen-socket" }
  enum State: String {
    case idle, starting, healthy
    case receivingNotPlaying = "receiving-not-playing"
    case notReceiving = "not-receiving"
  }
  struct Decision {
    let action: Action
    let state: State
    let attempt: Int64
    let reason: String
  }

  // Seconds, all of them (the TS original is in ms; same values).
  let stallS: Double
  let rxFreshS: Double
  /// nil = another watchdog owns "nothing arriving" (the native reviveIfDead) and this stays out.
  let notRecvS: Double?
  let settleS: Double
  let backoffMaxS: Double
  let maxPerMin: Int
  let healthyResetS: Double

  private var lastRx: Int64 = -1
  private var lastPlayed: Int64 = -1
  private var lastRxAt: Double = 0
  private var lastPlayAt: Double = 0
  private var windowStart: Double = 0
  private var healthySince: Double = 0
  private var holdUntil: Double = 0
  private var nextRepairAt: Double = 0
  private var stage: Int64 = 0
  private var recent: [Double] = []
  private(set) var repairs: Int64 = 0
  private(set) var lastReason = ""
  private(set) var state: State = .idle

  init(stallS: Double = 3, rxFreshS: Double = 1.5, notRecvS: Double? = nil, settleS: Double = 3,
       backoffMaxS: Double = 60, maxPerMin: Int = 4, healthyResetS: Double = 10) {
    self.stallS = stallS; self.rxFreshS = rxFreshS; self.notRecvS = notRecvS
    self.settleS = settleS; self.backoffMaxS = backoffMaxS; self.maxPerMin = maxPerMin
    self.healthyResetS = healthyResetS
  }

  /// Suppress judgement for `s` seconds — a legitimate transition.
  func hold(now: Double, seconds s: Double) { holdUntil = max(holdUntil, now + s) }

  func reset() {
    lastRx = -1; lastPlayed = -1; lastRxAt = 0; lastPlayAt = 0; windowStart = 0
    healthySince = 0; holdUntil = 0; nextRepairAt = 0; stage = 0; recent = []; state = .idle
  }

  /// `now` is monotonic seconds (ProcessInfo.systemUptime), NEVER 0 in practice.
  func tick(now: Double, rx: Int64, played: Int64, expected: Bool) -> Decision {
    let rxAdv = lastRx >= 0 && rx > lastRx
    let plAdv = lastPlayed >= 0 && played > lastPlayed
    lastRx = rx
    lastPlayed = played

    if !expected || now < holdUntil {
      lastRxAt = 0; lastPlayAt = 0; windowStart = 0; healthySince = 0
      state = .idle
      return Decision(action: .none, state: .idle, attempt: stage,
                      reason: expected ? "held (transition)" : "not expected")
    }
    if windowStart == 0 { windowStart = now }
    if rxAdv { lastRxAt = now }
    if plAdv { lastPlayAt = now }

    let receiving = lastRxAt > 0 && now - lastRxAt <= rxFreshS
    let quiet = now - max(lastPlayAt, windowStart)
    let deaf = now - max(lastRxAt, windowStart)

    var fault: State? = nil
    if receiving && quiet >= stallS { fault = .receivingNotPlaying }
    else if !receiving, let nr = notRecvS, deaf >= nr { fault = .notReceiving }

    guard let f = fault else {
      let playing = lastPlayAt > 0 && quiet < stallS
      if receiving && playing {
        if healthySince == 0 { healthySince = now }
        if stage > 0 && now - healthySince >= healthyResetS { stage = 0; nextRepairAt = 0 }
        state = .healthy
      } else {
        healthySince = 0
        state = (receiving || lastRxAt == 0) ? .starting : .notReceiving
      }
      return Decision(action: .none, state: state, attempt: stage, reason: "")
    }

    healthySince = 0
    state = f
    if now < nextRepairAt {
      return Decision(action: .none, state: f, attempt: stage, reason: "waiting (back-off)")
    }
    recent = recent.filter { now - $0 < 60 }
    if recent.count >= maxPerMin {
      nextRepairAt = recent[0] + 60
      return Decision(action: .none, state: f, attempt: stage, reason: "rate-limited")
    }
    let action: Action = f == .notReceiving
      ? .reopenSocket
      : (stage % 2 == 0 ? .rebuildPipeline : .reopenSocket)
    stage += 1
    repairs += 1
    recent.append(now)
    let backoff = min(backoffMaxS, settleS * pow(2, Double(max(0, stage - 2))))
    nextRepairAt = now + backoff
    windowStart = now; lastPlayAt = 0
    let reason = f == .receivingNotPlaying
      ? String(format: "receiving but nothing played for %.1f s", quiet)
      : String(format: "nothing received for %.1f s", deaf)
    lastReason = reason
    return Decision(action: action, state: f, attempt: stage, reason: reason)
  }
}
