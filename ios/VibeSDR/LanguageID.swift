import Foundation

/* ★★★ TRANSCRIBE AUTO'S LANGUAGE ID — the scoring, the lock and the budget, PURE (V12, 2026-10-17).
 *
 *  Everything here is plain Foundation: no Speech, no NaturalLanguage, no audio. VibeTranscriber.swift runs the
 *  models and NLLanguageRecognizer and hands the numbers in; this decides. That is so the SAME code the app runs is
 *  replayed against the bake-off's recorded windows (scripts/test_languageID_swift.sh, fixture
 *  test/fixtures/langid/h2-10s.json.gz) and must reproduce docs/v12/LANGUAGE-ID-BAKEOFF.md's numbers.
 *
 *  ★★ WHY NOT CONFIDENCE (measured 2026-10-17 on real off-air audio): Apple's French, German, Spanish, Italian,
 *     Portuguese and Chinese models write English audio AS ENGLISH, often more confidently than the English model —
 *     the old "most confident wins" picked the wrong language in 60 % of English windows. Rule H2 fixes it:
 *       score(L) = mean confidence × NaturalLanguage P(L | L's own transcript) × (0.75 + 0.5 × vote share)
 *     where the vote is every OTHER model's transcript (≥ 4 words), weighted by words, read by NaturalLanguage. Under
 *     4 words scores 0, and a script only one language uses (zh / ja / ko) must run at ≥ 1.5 tokens/s — NL cannot
 *     disagree with a kana or Hangul transcript, and those models write 8-18 junk tokens on English SSB and noise.
 *     10 s windows: 84 % right, 1 % wrong, 15 % no answer; 0 false answers on 234 noise/data/foreign windows.
 *  ★★ LOCK FAST, THEN STAY LOCKED (Stuart: "translation works better once it locks on"): lock on the first window
 *     whose best is ≥ 0.40 and 0.10 clear of the next; once locked, another language must beat it by 0.10 in TWO
 *     consecutive speech windows to take over. A window nobody reaches 0.40 in is not speech and holds the count.
 *     Simulated: 0 false switches on 35 clips, 0 false locks on 25 noise/foreign clips, a change caught in ~20 s.
 */
enum LanguageID {
  static let lockScore = 0.40
  static let lockMargin = 0.10
  static let switchMargin = 0.10
  static let switchWins = 2
  static let minWords = 4
  static let scriptUniqueTokensPerSecond = 1.5
  /// The window every decision is made on (the bake-off's knee: 5 s locked falsely, longer added little).
  static let windowSeconds = 10.0
  /// Analysis per window, wall time (a 30 % duty cycle — a decision within ~13 s of the first speech).
  static let budgetSeconds = 3.0
  /// Before anything is measured: an A13-class guess (the M4 measured 0.18 s), so a phone starts on a shortlist.
  static let assumedCostSeconds = 0.5
  /// Unconvincing speech windows on a shortlist before the rest of the pool is tried.
  static let widenAfter = 3

  /// "en-GB" / "en_GB" → "en". ★ Mandarin and Cantonese are ONE family here (the bake-off scored them together: their
  /// models read each other's audio, and NaturalLanguage reads both as Chinese).
  static func family(_ id: String) -> String {
    let b = (id.split(whereSeparator: { $0 == "-" || $0 == "_" }).first.map(String.init) ?? id).lowercased()
    return b == "yue" || b == "cmn" ? "zh" : b
  }
  /// NaturalLanguage's top language (raw value, "zh-Hans") → family; nil for none ("" / "-").
  static func nlFamily(_ raw: String) -> String? { raw.isEmpty || raw == "-" ? nil : family(raw) }
  static let scriptUnique: Set<String> = ["zh", "ja", "ko"]

  /// One language's model on one window.
  struct Heard: Equatable {
    let id: String              // the locale the model ran in ("en-GB")
    let words: Int              // words with a confidence (CJK: tokens)
    let meanConf: Double
    let tokensPerSecond: Double
    let nlOwn: Double           // NaturalLanguage P(this language | this model's transcript)
    let nlTop: String           // NaturalLanguage's top language for the transcript, raw ("" = none)
  }

  /// H2 for every model on one window, keyed by locale id.
  static func scores(_ heard: [Heard]) -> [String: Double] {
    var votes: [String: Int] = [:], total = 0
    for h in heard where h.words >= minWords {
      guard let f = nlFamily(h.nlTop) else { continue }
      votes[f, default: 0] += h.words; total += h.words
    }
    var out: [String: Double] = [:]
    for h in heard {
      guard h.words >= minWords else { out[h.id] = 0; continue }
      let c = family(h.id)
      if scriptUnique.contains(c) && h.tokensPerSecond < scriptUniqueTokensPerSecond { out[h.id] = 0; continue }
      // ★ The model's OWN transcript does not vote for it — a kana / Hangul transcript always reads as its own.
      let own = nlFamily(h.nlTop) == c ? h.words : 0
      let others = total - own
      let share = others > 0 ? Double((votes[c] ?? 0) - own) / Double(others) : 0
      out[h.id] = h.meanConf * h.nlOwn * (0.75 + 0.5 * share)
    }
    return out
  }

  struct Ranked: Equatable {
    let family: String
    let id: String              // the locale that gave the family its score
    let score: Double
  }

  /// One entry per family, best first. ★ PRIORS ONLY BREAK TIES (EiBi was wrong for 3 of the frequencies recorded):
  /// `prior` — locale ids or codes in prior order — orders languages whose scores are EQUAL, never anything else.
  static func rank(_ heard: [Heard], prior: [String] = []) -> [Ranked] {
    let s = scores(heard)
    var best: [String: Ranked] = [:]
    for h in heard {
      let f = family(h.id), v = s[h.id] ?? 0
      if let cur = best[f], cur.score >= v { continue }
      best[f] = Ranked(family: f, id: h.id, score: v)
    }
    let pf = prior.map(family)
    let at = { (f: String) in pf.firstIndex(of: f) ?? Int.max }
    return best.values.sorted { a, b in
      a.score != b.score ? a.score > b.score : (at(a.family) != at(b.family) ? at(a.family) < at(b.family) : a.family < b.family)
    }
  }

  /// Did this window convince anyone (the best reached the lock score)?
  static func convincing(_ r: [Ranked]) -> Bool { (r.first?.score ?? 0) >= lockScore }

  // MARK: lock, then hold

  struct Lock: Equatable {
    enum Event: Equatable {
      case nothing                       // unlocked, nothing convincing
      case locked(String)                // locale id
      case held                          // locked, the locked language won or nobody spoke
      case challenging(String, Int)      // a challenger is ahead by the margin: id, consecutive wins so far
      case switched(from: String, to: String)
    }
    private(set) var locked: Ranked?
    private(set) var challenger: String?        // family
    private(set) var wins = 0

    /// One scored window (from `rank`). Exactly the bake-off's simulated rule (lid_lock.py run(), K = 1, N = 2).
    mutating func observe(_ r: [Ranked]) -> Event {
      let top = r.first, s1 = top?.score ?? 0
      guard let cur = locked else {
        let s2 = r.count > 1 ? r[1].score : 0
        if let top, s1 >= LanguageID.lockScore, s1 - s2 >= LanguageID.lockMargin {
          locked = top; challenger = nil; wins = 0
          return .locked(top.id)
        }
        return .nothing
      }
      guard let top, s1 >= LanguageID.lockScore else { return .held }      // not speech: the count holds
      if top.family == cur.family { challenger = nil; wins = 0; return .held }
      let sx = r.first(where: { $0.family == cur.family })?.score ?? 0
      guard s1 - sx >= LanguageID.switchMargin else { challenger = nil; wins = 0; return .held }
      wins = challenger == top.family ? wins + 1 : 1
      challenger = top.family
      if wins >= LanguageID.switchWins {
        locked = top; challenger = nil; wins = 0
        return .switched(from: cur.id, to: top.id)
      }
      return .challenging(top.id, wins)
    }
  }

  // MARK: device budget

  /// Measured analysis cost. Running medians of the last 9, so a hot phone's slower windows take over.
  struct Costs: Equatable {
    private var seq: [String: [Double]] = [:]
    private var wide: [Double] = []
    init() {}
    mutating func addSequential(_ id: String, _ seconds: Double) {
      var a = seq[family(id), default: []]; a.append(seconds); if a.count > 9 { a.removeFirst(a.count - 9) }
      seq[family(id)] = a
    }
    /// A 2-wide window: `languages` analysed in `wall` seconds.
    mutating func addTwoWide(languages: Int, wall: Double) {
      guard languages >= 2 else { return }
      wide.append(wall / Double(languages)); if wide.count > 9 { wide.removeFirst(wide.count - 9) }
    }
    /// Median per-language cost one at a time (the median of each language's median).
    var sequential: Double? { seq.isEmpty ? nil : LanguageID.median(seq.values.map { LanguageID.median($0) }) }
    /// Median per-language cost (wall) two at a time.
    var twoWide: Double? { wide.isEmpty ? nil : LanguageID.median(wide) }
  }
  static func median(_ a: [Double]) -> Double {
    let s = a.sorted(); return s.isEmpty ? 0 : s[s.count / 2]
  }

  struct Plan: Equatable { let perWindow: Int; let twoWide: Bool }
  /// Languages per window = floor(budget / median cost), ×2 when 2-wide measured ≥ 1.5× faster. Never fewer than 2
  /// (one language has nothing to be compared with). Unmeasured: the assumed cost, one at a time; measured one at a
  /// time but not yet two: try two (that window measures it).
  static func plan(_ c: Costs, budget: Double) -> Plan {
    guard let c1 = c.sequential, c1 > 0 else {
      return Plan(perWindow: max(2, Int((budget / assumedCostSeconds).rounded(.down))), twoWide: false)
    }
    let n = max(2, Int((budget / c1).rounded(.down)))
    guard let c2 = c.twoWide, c2 > 0 else { return Plan(perWindow: n, twoWide: true) }
    return c1 / c2 >= 1.5 ? Plan(perWindow: n * 2, twoWide: true) : Plan(perWindow: n, twoWide: false)
  }
  /// The budget for the phone's state: halved when hot (.serious and worse — ProcessInfo.ThermalState raw 2+) or in
  /// Low Power Mode. (.critical also stops the background check — the engine does that.)
  static func budget(thermal: Int, lowPower: Bool) -> Double {
    thermal >= 2 || lowPower ? budgetSeconds / 2 : budgetSeconds
  }

  /// Where a shortlist device is in its widening. `step` 0 = the shortlist; 1… = the rest of the pool, a slice at a
  /// time, round and round (back to the shortlist after the last slice).
  struct Widen: Equatable {
    private(set) var unconvincing = 0
    private(set) var step = 0
    init() {}
    /// After a scored speech window (audio-level gated): a convincing one goes back to the shortlist.
    mutating func after(convincing: Bool, slices: Int) {
      if convincing { unconvincing = 0; step = 0; return }
      unconvincing += 1
      if unconvincing >= LanguageID.widenAfter, slices > 0 { step = (step + 1) % (slices + 1) }
    }
  }
  /// How many slices of the rest a pool has beyond its shortlist.
  static func slices(pool: Int, perWindow: Int) -> Int {
    pool <= perWindow || perWindow <= 0 ? 0 : (pool - perWindow + perWindow - 1) / perWindow
  }
  /// Which of the pool (prior order) to score this window. The whole pool when it fits; else the shortlist (or the
  /// widening slice) — always with the locked language in it, so the background check can compare against it.
  static func pick(pool: [String], locked: String?, perWindow: Int, step: Int) -> [String] {
    guard pool.count > perWindow, perWindow > 0 else { return pool }
    var out: [String]
    if step == 0 {
      out = Array(pool.prefix(perWindow))
    } else {
      let rest = Array(pool.dropFirst(perWindow))
      let lo = (step - 1) * perWindow
      out = lo < rest.count ? Array(rest[lo..<min(rest.count, lo + perWindow)]) : Array(pool.prefix(perWindow))
    }
    if let l = locked, !out.contains(where: { family($0) == family(l) }) {
      if out.count >= perWindow { out.removeLast() }
      out.insert(l, at: 0)
    }
    return out
  }
}
