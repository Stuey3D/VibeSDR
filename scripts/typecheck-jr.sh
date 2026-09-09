#!/bin/bash
# typecheck-jr.sh — TYPE-CHECK Jr's Swift before spending an Xcode Cloud build on it.
#
# ★★★ WHY. Build 41 failed on "use of local variable 'secure' before its declaration" — a
#     four-second error that cost a full Cloud build and a round trip, because the check I had
#     been running was `swiftc -parse`, which only validates SYNTAX. Parsing accepts code that
#     cannot compile. This is the difference between "it is well-formed" and "it is correct".
#
# ★ The opus_* / OPUS_OK errors are EXPECTED here: those C symbols come from the bridging header
#   Xcode supplies, which a bare swiftc invocation has no knowledge of. Everything else is real.
set -o pipefail
cd "$(dirname "$0")/.."
TARGET=$(grep -m1 -o "WATCHOS_DEPLOYMENT_TARGET = [0-9.]*" spike/WristSDR/WristSDR.xcodeproj/project.pbxproj | grep -o "[0-9.]*$")
# ★★★ WITH THE BRIDGING HEADER, OR THE CHECK IS HOLLOW. Without it the opus_* symbols are
#     unresolved, and once a module has those errors the compiler does not go on to check every
#     body: Jr 83 and 84 (2026-09-09) failed in Cloud on "cannot find 'baseURL' in scope" in
#     UberClient.swift while this script said CLEAN — twice, on the code that failed. With the
#     header and the opus include path the same command reports exactly the Cloud error.
OUT=$(xcrun swiftc -typecheck \
        -sdk "$(xcrun --sdk watchos --show-sdk-path)" \
        -target "arm64_32-apple-watchos${TARGET}" \
        -import-objc-header spike/WristSDR/WristSDR/WristSDR-Bridging-Header.h \
        -Xcc -Ispike/WristSDR/WristSDR/opus/include -Xcc -Ispike/WristSDR/WristSDR/opus/include/opus \
        spike/WristSDR/WristSDR/*.swift 2>&1 | grep -E "^[^ ].*error:")
if [ -n "$OUT" ]; then
  echo "$OUT"
  echo
  echo "FAILED — fix these before triggering a build."
  exit 1
fi
echo "Jr type-checks clean (watchOS ${TARGET}, with the bridging header)"
