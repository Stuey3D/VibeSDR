// TRANSCRIBE AUTO language ID — replay of the bake-off through the app's own LanguageID.swift.
// Built and run by scripts/test_languageID_swift.sh: `t <fixture.json>`.
import Foundation

var fails = 0
func eq<T: Equatable>(_ name: String, _ got: T, _ want: T) {
  if got == want { print("ok   \(name)") } else { fails += 1; print("FAIL \(name)\n     got  \(got)\n     want \(want)") }
}

// ── 1. pure rules, synthetic ─────────────────────────────────────────────────────────────────────────────────────
typealias H = LanguageID.Heard
let en = H(id: "en-GB", words: 20, meanConf: 0.6, tokensPerSecond: 2, nlOwn: 0.99, nlTop: "en")
let esOnEnglish = H(id: "es-ES", words: 22, meanConf: 0.8, tokensPerSecond: 2.2, nlOwn: 0.02, nlTop: "en")
let itOnEnglish = H(id: "it-IT", words: 18, meanConf: 0.75, tokensPerSecond: 1.8, nlOwn: 0.03, nlTop: "en")
let jaJunk = H(id: "ja-JP", words: 12, meanConf: 0.5, tokensPerSecond: 1.2, nlOwn: 1.0, nlTop: "ja")
let s = LanguageID.scores([en, esOnEnglish, itOnEnglish, jaJunk])
eq("a foreign model writing English scores about zero", (s["es-ES"]! < 0.05), true)
eq("English wins with the others' vote", LanguageID.rank([en, esOnEnglish, itOnEnglish, jaJunk]).first?.id, "en-GB")
eq("English score = conf × NL × (0.75 + 0.5 × share of the OTHER transcripts)",
   (s["en-GB"]! * 1e6).rounded(), (0.6 * 0.99 * (0.75 + 0.5 * (40.0 / 52.0)) * 1e6).rounded())
eq("kana under 1.5 tokens/s scores 0", s["ja-JP"], 0)
eq("under 4 words scores 0", LanguageID.scores([H(id: "fr-FR", words: 3, meanConf: 0.9, tokensPerSecond: 1, nlOwn: 1, nlTop: "fr")])["fr-FR"], 0)
eq("Mandarin and Cantonese are one family", LanguageID.rank([
  H(id: "zh-CN", words: 25, meanConf: 0.7, tokensPerSecond: 2.5, nlOwn: 0.95, nlTop: "zh-Hans"),
  H(id: "yue-CN", words: 25, meanConf: 0.5, tokensPerSecond: 2.5, nlOwn: 0.9, nlTop: "zh-Hant")]).map { $0.family }, ["zh"])
eq("priors only break an exact tie", LanguageID.rank([
  H(id: "de-DE", words: 0, meanConf: 0, tokensPerSecond: 0, nlOwn: 0, nlTop: ""),
  H(id: "nl-NL", words: 0, meanConf: 0, tokensPerSecond: 0, nlOwn: 0, nlTop: "")], prior: ["nl", "de"]).first?.id, "nl-NL")

func R(_ pairs: [(String, Double)]) -> [LanguageID.Ranked] {
  pairs.map { LanguageID.Ranked(family: LanguageID.family($0.0), id: $0.0, score: $0.1) }.sorted { $0.score > $1.score }
}
var lk = LanguageID.Lock()
eq("no lock under 0.40", lk.observe(R([("fr-FR", 0.39), ("de-DE", 0.1)])), .nothing)
eq("no lock inside the 0.10 margin", lk.observe(R([("fr-FR", 0.55), ("de-DE", 0.47)])), .nothing)
eq("lock at 0.40 with 0.10 margin", lk.observe(R([("fr-FR", 0.51), ("de-DE", 0.40)])), .locked("fr-FR"))
eq("challenger, 1st win", lk.observe(R([("de-DE", 0.6), ("fr-FR", 0.45)])), .challenging("de-DE", 1))
eq("no speech holds the count", lk.observe(R([("de-DE", 0.3), ("fr-FR", 0.1)])), .held)
eq("2nd consecutive win switches", lk.observe(R([("de-DE", 0.62), ("fr-FR", 0.4)])), .switched(from: "fr-FR", to: "de-DE"))
eq("the locked language winning resets", { var l = lk; _ = l.observe(R([("es-ES", 0.7), ("de-DE", 0.5)]));
  _ = l.observe(R([("de-DE", 0.7), ("es-ES", 0.5)])); return l.observe(R([("es-ES", 0.7), ("de-DE", 0.5)])) }(), .challenging("es-ES", 1))
eq("a lead under 0.10 resets", { var l = lk; _ = l.observe(R([("es-ES", 0.7), ("de-DE", 0.5)]));
  _ = l.observe(R([("es-ES", 0.65), ("de-DE", 0.6)])); return l.observe(R([("es-ES", 0.7), ("de-DE", 0.5)])) }(), .challenging("es-ES", 1))

var c = LanguageID.Costs()
eq("unmeasured: the assumed cost (3 s / 0.5 s = 6), one at a time", LanguageID.plan(c, budget: 3), .init(perWindow: 6, twoWide: false))
for _ in 0..<3 { c.addSequential("en-GB", 0.18); c.addSequential("fr-FR", 0.18) }
eq("M4 one at a time: 16, and try two next", LanguageID.plan(c, budget: 3), .init(perWindow: 16, twoWide: true))
c.addTwoWide(languages: 13, wall: 13 * 0.11)
eq("2-wide 1.64× faster: ×2", LanguageID.plan(c, budget: 3), .init(perWindow: 32, twoWide: true))
var slow = LanguageID.Costs(); slow.addSequential("en", 0.8); slow.addTwoWide(languages: 4, wall: 2.4)
eq("A13 guess, 2-wide only 1.33×: 3, one at a time", LanguageID.plan(slow, budget: 3), .init(perWindow: 3, twoWide: false))
var vslow = LanguageID.Costs(); vslow.addSequential("en", 2.5)
eq("never fewer than 2", LanguageID.plan(vslow, budget: 1.5).perWindow, 2)
eq("budget halves when hot", LanguageID.budget(thermal: 2, lowPower: false), 1.5)
eq("budget halves in Low Power Mode", LanguageID.budget(thermal: 0, lowPower: true), 1.5)
eq("budget full when cool", LanguageID.budget(thermal: 1, lowPower: false), 3)

let pool = ["zh-CN", "pt-BR", "en-GB", "fr-FR", "de-DE", "es-ES", "it-IT"]
eq("pool fits: all", LanguageID.pick(pool: pool, locked: nil, perWindow: 8, step: 0), pool)
eq("shortlist in prior order", LanguageID.pick(pool: pool, locked: nil, perWindow: 3, step: 0), ["zh-CN", "pt-BR", "en-GB"])
eq("widened slice 1", LanguageID.pick(pool: pool, locked: nil, perWindow: 3, step: 1), ["fr-FR", "de-DE", "es-ES"])
eq("widened slice 2 + locked kept in", LanguageID.pick(pool: pool, locked: "en-GB", perWindow: 3, step: 2), ["en-GB", "it-IT"])
eq("slices beyond the shortlist", LanguageID.slices(pool: 7, perWindow: 3), 2)
var w = LanguageID.Widen()
w.after(convincing: false, slices: 2); w.after(convincing: false, slices: 2)
eq("2 unconvincing: still the shortlist", w.step, 0)
w.after(convincing: false, slices: 2); eq("3rd: widen to slice 1", w.step, 1)
w.after(convincing: false, slices: 2); eq("then slice 2", w.step, 2)
w.after(convincing: false, slices: 2); eq("then back to the shortlist", w.step, 0)
w.after(convincing: true, slices: 2); eq("convincing resets", w, LanguageID.Widen())

// ── 2. the bake-off's recorded 10 s windows ──────────────────────────────────────────────────────────────────────
struct Fixture: Decodable {
  struct F: Decodable { let name: String; let lang: String }
  let files: [F]; let rows: [[Row]]
  enum Row: Decodable {
    case n(Double), s(String)
    init(from d: Decoder) throws {
      let c = try d.singleValueContainer()
      if let v = try? c.decode(Double.self) { self = .n(v) } else { self = .s(try c.decode(String.self)) }
    }
    var num: Double { if case .n(let v) = self { return v }; return 0 }
    var str: String { if case .s(let v) = self { return v }; return "" }
  }
}
let fx = try! JSONDecoder().decode(Fixture.self, from: FileManager.default.contents(atPath: CommandLine.arguments[1])!)
var windows: [Int: [Double: [H]]] = [:]
for r in fx.rows {
  let h = H(id: r[2].str, words: Int(r[3].num), meanConf: r[4].num, tokensPerSecond: r[5].num, nlOwn: r[6].num, nlTop: r[7].str)
  windows[Int(r[0].num), default: [:]][r[1].num, default: []].append(h)
}
func seq(_ f: Int) -> [[H]] { (windows[f] ?? [:]).sorted { $0.key < $1.key }.map { $0.value } }

// Per window, the bake-off's table rule (T 0.40, margin 0.05).
var speech = 0, correct = 0, wrong = 0, abstain = 0, nonTarget = 0, falseAns = 0
for (f, file) in fx.files.enumerated() {
  for hs in seq(f) {
    let r = LanguageID.rank(hs)
    let ans: String? = (r.first.map { $0.score >= 0.40 && (r.count < 2 || $0.score - r[1].score >= 0.05) } ?? false) ? r[0].family : nil
    if file.lang == "none" || file.lang == "other" { nonTarget += 1; if ans != nil { falseAns += 1 }; continue }
    guard hs.contains(where: { LanguageID.family($0.id) == file.lang && $0.tokensPerSecond >= 1.0 }) else { continue }
    speech += 1
    if ans == file.lang { correct += 1 } else if ans == nil { abstain += 1 } else { wrong += 1 }
  }
}
eq("H2 10 s windows: speech / correct / wrong / no answer (bake-off: 184, 84 %, 1 %, 15 %)", [speech, correct, wrong, abstain], [184, 155, 2, 27])
eq("H2 10 s windows: non-target / false answers (bake-off: 234, 0)", [nonTarget, falseAns], [234, 0])

// Lock, then hold — lid_lock.py's simulation (10 s, N = 2).
func run(_ ws: [[H]]) -> (locked: String?, switches: [(t: Int, from: String, to: String)]) {
  var l = LanguageID.Lock(); var sw: [(Int, String, String)] = []
  for (i, hs) in ws.enumerated() {
    if case .switched(let a, let b) = l.observe(LanguageID.rank(hs)) {
      sw.append(((i + 1) * 10, LanguageID.family(a), LanguageID.family(b)))
    }
  }
  return (l.locked?.family, sw.map { (t: $0.0, from: $0.1, to: $0.2) })
}
let good = fx.files.indices.filter { !["none", "other"].contains(fx.files[$0].lang) && !seq($0).isEmpty }
let bad = fx.files.indices.filter { ["none", "other"].contains(fx.files[$0].lang) && !seq($0).isEmpty }
var firstOK = 0, firstWrong = 0, never = 0, falseSw = 0, endsOK = 0
for f in good {
  let (locked, sw) = run(seq(f)); let truth = fx.files[f].lang
  let first = sw.first?.from ?? locked
  if first == nil { never += 1 } else if first == truth { firstOK += 1 } else { firstWrong += 1 }
  falseSw += sw.filter { $0.to != truth }.count
  if locked == truth { endsOK += 1 }
}
eq("single-language clips: first lock right / wrong / never (bake-off: 28 / 2 / 5 of 35)", [good.count, firstOK, firstWrong, never], [35, 28, 2, 5])
eq("false switches 0, ends right 30", [falseSw, endsOK], [0, 30])
eq("false locks on noise / foreign clips (bake-off: 0 of 25)", [bad.filter { run(seq($0)).locked != nil }.count, bad.count], [0, 25])
var ok = 0, miss = 0, wr = 0, delays: [Int] = []
for a in good { for b in good where a != b && fx.files[a].lang != fx.files[b].lang {
  let wa = seq(a), wb = seq(b)
  guard run(wa).locked == fx.files[a].lang else { continue }
  let (locked, sw) = run(wa + wb)
  let post = sw.filter { $0.t > wa.count * 10 }
  if locked == fx.files[b].lang && !post.isEmpty { ok += 1; delays.append(post.last!.t - wa.count * 10) }
  else if locked == fx.files[a].lang { miss += 1 } else { wr += 1 }
} }
eq("switch tests caught / missed / wrong (bake-off: 651 / 142 / 0)", [ok, miss, wr], [651, 142, 0])
eq("median switch delay 20 s", delays.sorted()[delays.count / 2], 20)

print(fails == 0 ? "\nLanguageID: all passed" : "\nLanguageID: \(fails) FAILED")
exit(fails == 0 ? 0 : 1)
