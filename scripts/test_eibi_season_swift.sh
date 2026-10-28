#!/bin/bash
# test_eibi_season_swift.sh — the Mac app's EiBi season pick + fallback
# (vibeserver/mac/EibiStations.swift) against the same scenarios as vibeserver/test-eibi.cpp and
# scripts/test_eibi_season.ts. ★★★ NickB, 2026-10-27: B26 a 404 two days after we switched to it —
# use A26, say so in words, re-ask every few hours, report a real network failure as one.
# A fake fetcher and clock and a temp cache dir; no network. vs_set_stations is stubbed.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cat > "$T/main.swift" <<'SWIFT'
import Foundation
func vs_set_stations(_ s: UnsafePointer<CChar>?) {}

var fails = 0, passes = 0
func eq<T: Equatable>(_ what: String, _ got: T, _ want: T) {
  if got == want { passes += 1 } else { fails += 1; print("FAIL \(what)\n   got  \(got)\n   want \(want)") }
}
func at(_ y: Int, _ m: Int, _ d: Int, _ h: Int = 12) -> Date {
  var c = DateComponents(); c.year = y; c.month = m; c.day = d; c.hour = h
  var cal = Calendar(identifier: .gregorian); cal.timeZone = TimeZone(identifier: "UTC")!
  return cal.date(from: c)!
}
func csv(_ name: String, _ n: Int) -> Data {
  var s = "kHz:75;Time(UTC):93;Days:59;ITU:49;Station:201;Lng:49;Target:62;Remarks:135;P:35;Start:60;Stop:60;\r\n"
  for i in 0..<n { s += "\(5000 + i * 5);0000-2400;;G;\(name);E;Eu;;1;;\r\n" }
  return s.data(using: .utf8)!
}
func describe(_ r: EibiStations.Schedule) -> String {
  switch r {
  case .loaded(let c, let note): return "loaded[\(c.contains("Winter") ? "winter" : c.contains("Summer") ? "summer" : "?")] \(note)"
  case .failed(let e): return "failed \(e)"
  }
}

let dir = URL(fileURLWithPath: CommandLine.arguments[1])
EibiStations.cacheDirOverride = dir
var site: [String: Data] = [:]
var down = false
var asked: [String] = []
let fetch: (String) async -> EibiStations.Fetch = { code in
  asked.append(code)
  if down { return .failed("The Internet connection appears to be offline.") }
  if let d = site[code] { return .ok(d) }
  return .unpublished
}
func fresh() {
  for f in (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? [] {
    try? FileManager.default.removeItem(at: dir.appendingPathComponent(f))
  }
  site = [:]; down = false; asked = []
}
let NOTE = "EiBi hasn't published the B26 (winter) schedule yet — using A26 until it appears."

eq("27 Oct 2026 is B26", EibiStations.seasonCode(for: at(2026, 10, 27)), "b26")
eq("24 Oct 2026 is A26", EibiStations.seasonCode(for: at(2026, 10, 24)), "a26")
eq("15 Jan 2027 is B26", EibiStations.seasonCode(for: at(2027, 1, 15)), "b26")
eq("24 Mar 2027 is B26", EibiStations.seasonCode(for: at(2027, 3, 24)), "b26")
eq("25 Mar 2027 is A27", EibiStations.seasonCode(for: at(2027, 3, 25)), "a27")
eq("before b26", EibiStations.previousSeason("b26"), "a26")
eq("before a27", EibiStations.previousSeason("a27"), "b26")
eq("before a00", EibiStations.previousSeason("a00"), "b99")

// B26 not published, nothing cached → A26, in words.
fresh()
let now = at(2026, 10, 27, 21)
site["a26"] = csv("Summer", 3)
var r = await EibiStations.schedule(now: now, fetch: fetch)
eq("falls back to A26", describe(r), "loaded[summer] \(NOTE)")
eq("asked B26 then A26", asked, ["b26", "a26"])

// An hour later: no request at all (re-asks only every 6 h).
asked = []
r = await EibiStations.schedule(now: now.addingTimeInterval(3600), fetch: fetch)
eq("1 h later: A26 from cache", describe(r), "loaded[summer] \(NOTE)")
eq("1 h later: nothing asked", asked, [])

// 7 h later B26 appears: switches, no note.
site["b26"] = csv("Winter", 5); asked = []
r = await EibiStations.schedule(now: now.addingTimeInterval(7 * 3600), fetch: fetch)
eq("switches to B26", describe(r), "loaded[winter] ")
eq("asked B26 only", asked, ["b26"])

// A real network failure with nothing cached says so.
fresh(); down = true
r = await EibiStations.schedule(now: now, fetch: fetch)
eq("network failure", describe(r), "failed couldn't reach eibispace.de — The Internet connection appears to be offline.")

// A page with no entries counts as not published.
fresh()
site["b26"] = "<html>Not Found</html>".data(using: .utf8)!; site["a26"] = csv("Summer", 2)
r = await EibiStations.schedule(now: now, fetch: fetch)
eq("empty page → A26", describe(r), "loaded[summer] \(NOTE)")

// March: A27 unpublished → B26.
fresh(); site["b26"] = csv("Winter", 4)
r = await EibiStations.schedule(now: at(2027, 3, 26), fetch: fetch)
eq("A27 → B26", describe(r), "loaded[winter] EiBi hasn't published the A27 (summer) schedule yet — using B26 until it appears.")

print(fails == 0 ? "swift port: all \(passes) passed" : "swift port: \(fails) FAILED")
exit(fails == 0 ? 0 : 1)
SWIFT
mkdir -p "$T/cache"
xcrun swiftc -O -o "$T/t" vibeserver/mac/EibiStations.swift "$T/main.swift"
"$T/t" "$T/cache"
