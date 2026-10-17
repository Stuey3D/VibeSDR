#!/bin/bash
# test_languageID_swift.sh — TRANSCRIBE AUTO's language ID (ios/VibeSDR/LanguageID.swift, the file the app compiles)
# replayed against the language-ID bake-off's recorded 10 s windows (test/fixtures/langid/h2-10s.json.gz, built by
# scripts/build_langid_fixture.py from ~/VibeSDR-test-audio/lang/tools/results). It must reproduce
# docs/v12/LANGUAGE-ID-BAKEOFF.md: H2 per window (184 speech windows: 155 right, 2 wrong, 27 no answer; 0 false on 234)
# and the lock simulation (28/35 first locks right, 0 false switches, 0/25 false locks, switch tests 651/142/0).
# A scorer or lock that decides differently on the same numbers is a bug in the port.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
gunzip -c test/fixtures/langid/h2-10s.json.gz > "$T/fixture.json"
nice xcrun swiftc -O -o "$T/t" ios/VibeSDR/LanguageID.swift scripts/langid/main.swift
"$T/t" "$T/fixture.json"
