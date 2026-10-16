#!/bin/bash
# test_transcribePlaces_swift.sh — which speech language gives up its place when Apple's cap is reached
# (ios/VibeSDR/TranscribePlaces.swift). V12 467 removed ENGLISH on an English iPhone to make room for Vietnamese:
# these cases pin the rule that the phone's language, the translation target and the language in use never go.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cat > "$T/main.swift" <<'SWIFT'
import Foundation
var fail = 0, n = 0
func check(_ name: String, _ got: TranscribePlaces.Decision, _ want: TranscribePlaces.Decision) {
  n += 1
  if got == want { print("ok   \(name)") } else { fail += 1; print("FAIL \(name)\n     want \(want)\n     got  \(got)") }
}
let full = ["en-GB", "zh-CN", "fr-FR", "de-DE", "cs-CZ"]
// Stuart's case: English heard least (never), but it is the phone's language and the translation target.
check("phone language is never removed, least-used other one is",
      TranscribePlaces.decide(held: full, max: 5, incoming: "vi-VN", protectedBases: ["en"],
                              lastUsed: ["zh-CN": 300, "fr-FR": 200, "de-DE": 400, "cs-CZ": 100]),
      .remove("cs-CZ"))
check("translation target given as a bare code protects every variant",
      TranscribePlaces.decide(held: ["en-US", "zh-CN"], max: 2, incoming: "vi-VN", protectedBases: ["en"], lastUsed: [:]),
      .remove("zh-CN"))
check("language being transcribed now is never removed",
      TranscribePlaces.decide(held: full, max: 5, incoming: "vi-VN", protectedBases: ["en", "cs"],
                              lastUsed: ["zh-CN": 300, "fr-FR": 200, "de-DE": 400, "cs-CZ": 100]),
      .remove("fr-FR"))
check("translation-target use counts (bare 'fr' key) — then German is least used",
      TranscribePlaces.decide(held: ["zh-CN", "fr-FR", "de-DE"], max: 3, incoming: "vi-VN", protectedBases: [],
                              lastUsed: ["zh-CN": 300, "fr-FR": 10, "fr": 900, "de-DE": 200]),
      .remove("de-DE"))
check("never-used beats used",
      TranscribePlaces.decide(held: ["zh-CN", "ru-RU"], max: 2, incoming: "vi-VN", protectedBases: [], lastUsed: ["zh-CN": 5]),
      .remove("ru-RU"))
check("every place protected → ask, remove nothing",
      TranscribePlaces.decide(held: ["en-GB", "vi-VN", "fr-FR"], max: 3, incoming: "de-DE", protectedBases: ["en", "vi", "fr"],
                              lastUsed: [:]),
      .ask)
check("room below the cap → nothing removed",
      TranscribePlaces.decide(held: ["en-GB", "zh-CN"], max: 5, incoming: "vi-VN", protectedBases: ["en"], lastUsed: [:]),
      .room)
check("language already holding a place (another variant) → nothing removed",
      TranscribePlaces.decide(held: full, max: 5, incoming: "en-US", protectedBases: [], lastUsed: [:]),
      .room)
check("Apple's underscore ids are the same languages",
      TranscribePlaces.decide(held: ["en_GB", "zh_CN"], max: 2, incoming: "vi_VN", protectedBases: ["en"], lastUsed: [:]),
      .remove("zh_CN"))
check("a smaller cap (low storage) still never takes a protected place",
      TranscribePlaces.decide(held: ["en-GB"], max: 1, incoming: "vi-VN", protectedBases: ["en"], lastUsed: [:]),
      .ask)
print(fail == 0 ? "transcribe places: all \(n) cases pass" : "transcribe places: \(fail) of \(n) FAILED")
exit(fail == 0 ? 0 : 1)
SWIFT
nice swiftc -O ios/VibeSDR/TranscribePlaces.swift "$T/main.swift" -o "$T/t"
"$T/t"
