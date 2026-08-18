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
OUT=$(xcrun swiftc -typecheck \
        -sdk "$(xcrun --sdk watchos --show-sdk-path)" \
        -target "arm64_32-apple-watchos${TARGET}" \
        spike/WristSDR/WristSDR/*.swift 2>&1 | grep -E "^[^ ].*error:" | grep -v "opus_\|OPUS_")
if [ -n "$OUT" ]; then
  echo "$OUT"
  echo
  echo "FAILED — fix these before triggering a build."
  exit 1
fi
echo "Jr type-checks clean (watchOS ${TARGET}; opus_* symbols excluded — they come from the bridging header)"
