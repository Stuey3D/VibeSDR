#!/bin/bash
# test_audioSelfHeal_swift.sh — run the Swift port of the audio self-heal rules
# (spike/WristSDR/WristSDR/AudioSelfHeal.swift, compiled into BOTH Jr and the iOS app) against the
# SAME scenarios as the TypeScript original (scripts/test_audioSelfHeal.ts --json). A port that
# decides differently on the same input is a bug in the port.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
npx tsx scripts/test_audioSelfHeal.ts --json > "$T/scenarios.json"
cat > "$T/main.swift" <<'SWIFT'
import Foundation
struct Step: Decodable { let t: Double; let rx: Int64; let played: Int64; let expected: Bool; let hold: Double? }
struct Cfg: Decodable { let notRecvMs: Double? }
struct Scenario: Decodable { let name: String; let cfg: Cfg?; let steps: [Step]; let want: [[AnyCodable]] }
struct AnyCodable: Decodable {
  let n: Double?; let s: String?
  init(from d: Decoder) throws {
    let c = try d.singleValueContainer()
    if let v = try? c.decode(Double.self) { n = v; s = nil } else { n = nil; s = try c.decode(String.self) }
  }
}
let data = FileManager.default.contents(atPath: CommandLine.arguments[1])!
let scs = try! JSONDecoder().decode([Scenario].self, from: data)
var fail = 0
for sc in scs {
  let m = AudioSelfHeal(notRecvS: sc.cfg?.notRecvMs.map { $0 / 1000 })
  var got: [String] = []
  for st in sc.steps {
    // +1000 s: a real monotonic clock is never 0, and 0 means "never" inside the monitor —
    // the TS test starts at t=0, where the ms-based original treats the same way.
    let now = st.t / 1000 + 1000
    if let h = st.hold { m.hold(now: now, seconds: h / 1000) }
    let d = m.tick(now: now, rx: st.rx, played: st.played, expected: st.expected)
    if d.action != .none { got.append("\(Int64(st.t)):\(d.action.rawValue)") }
  }
  let want = sc.want.map { "\(Int64($0[0].n!)):\($0[1].s!)" }
  if got == want { print("ok   \(sc.name)") } else { fail += 1; print("FAIL \(sc.name)\n     want \(want)\n     got  \(got)") }
}
print(fail == 0 ? "swift port: all \(scs.count) scenarios pass" : "swift port: \(fail) FAILED")
exit(fail == 0 ? 0 : 1)
SWIFT
xcrun swiftc -O -o "$T/t" spike/WristSDR/WristSDR/AudioSelfHeal.swift "$T/main.swift"
"$T/t" "$T/scenarios.json"
