#!/bin/bash
# typecheck-buddy.sh — TYPE-CHECK Buddy's Swift before spending an Xcode Cloud build on it.
#
# ★★★ THE TWIN OF typecheck-jr.sh, and it exists for the same reason: parsing accepts code that
#     cannot compile, and a Cloud build is a slow way to learn about a four-second error.
#
# ★★★ -module-name IS NOT OPTIONAL. Without it this reports a wall of nonsense — "cannot find type
#     'VibeSDRWatch' in scope", EnvironmentObject wrapper errors, "unable to type-check this
#     expression in reasonable time" — none of it real. A tool that cries wolf is worse than no
#     tool: the first thing anyone does with it is learn to ignore its output.
set -o pipefail
cd "$(dirname "$0")/.."
TARGET=$(grep -m1 -o "WATCHOS_DEPLOYMENT_TARGET = [0-9.]*" ios/VibeSDR.xcodeproj/project.pbxproj | grep -o "[0-9.]*$")
OUT=$(xcrun swiftc -typecheck \
        -module-name VibeSDRWatch \
        -sdk "$(xcrun --sdk watchos --show-sdk-path)" \
        -target "arm64_32-apple-watchos${TARGET:-11.0}" \
        ios/VibeSDRWatch/*.swift 2>&1 | grep -E "^[^ ].*error:")
if [ -n "$OUT" ]; then
  echo "$OUT"
  echo
  echo "FAILED — fix these before triggering a build."
  exit 1
fi
echo "Buddy type-checks clean (watchOS ${TARGET:-11.0})"
