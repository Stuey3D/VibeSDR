#!/usr/bin/env bash
# check-xcode-sources.sh — every .swift on disk must be IN the Xcode project.
#
# ★★★ WHY. A .swift that is not in the .xcodeproj COMPILES TO SILENCE: no error, no warning, the
#     symbol simply does not exist. VibeSilentAudio.swift sat in ios/VibeSDR/ for months, never
#     referenced by the project — which is the real reason "nothing ever called it". The moment
#     something did, FOUR Xcode Cloud builds failed with "Cannot find 'vibeStartSilentAudio' in
#     scope", ~6 minutes each, and the cause was a file list rather than any code.
# ★★ AND IT COSTS A CLOUD BUILD TO FIND OUT, every time. This is a one-second check on the Mac.
# ★ Run it before triggering iOS. It is the same class of gate as build_ios.sh's link check: prove
#   the thing can build HERE rather than seven minutes away.
set -o pipefail
cd "$(dirname "$0")/.."
PROJ=ios/VibeSDR.xcodeproj/project.pbxproj
missing=0
for f in ios/VibeSDR/*.swift ios/VibeSDRWatch/*.swift; do
  b=$(basename "$f")
  grep -q "$b" "$PROJ" || { echo "!! not in the Xcode project: $f"; missing=1; }
done
if [ "$missing" = 1 ]; then
  echo "   Add it (PBXBuildFile + PBXFileReference + group + Sources phase) or the symbol will not exist."
  exit 1
fi
echo "ok   every .swift in ios/VibeSDR and ios/VibeSDRWatch is in the Xcode project"
