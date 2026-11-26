#!/bin/bash
# test-apk-pack.sh — beta96: VibeApkPack.kt (the GPU map's packs read straight out of the APK) on a plain JVM.
# Compiles VibeApkPack.kt + scripts/test-apk-pack/ApkPackTest.kt with the Kotlin compiler gradle already cached
# (no Android SDK, no device, no network) and runs it. Exit 0 = all passed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
C="${GRADLE_USER_HOME:-$HOME/.gradle}/caches/modules-2/files-2.1"
jar() {   # newest jar of group/artifact in the gradle cache (not -sources)
  find "$C/$1/$2" -name "$2-*.jar" ! -name '*-sources.jar' 2>/dev/null | sort -V | tail -1
}
KV=2.1.20
CP="$(jar org.jetbrains.kotlin kotlin-compiler-embeddable | grep "$KV" || true)"
[ -n "$CP" ] || { echo "kotlin-compiler-embeddable $KV not in the gradle cache — run an Android build once"; exit 2; }
STD="$(find "$C/org.jetbrains.kotlin/kotlin-stdlib/$KV" -name "kotlin-stdlib-$KV.jar" | head -1)"
for a in "org.jetbrains.kotlin kotlin-daemon-embeddable" "org.jetbrains.kotlin kotlin-script-runtime" \
         "org.jetbrains.kotlin kotlin-reflect" "org.jetbrains.intellij.deps trove4j" \
         "org.jetbrains.kotlinx kotlinx-coroutines-core-jvm" "org.jetbrains annotations"; do
  set -- $a; CP="$CP:$(jar "$1" "$2")"
done
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
nice -n 10 java -Xmx768m -cp "$CP:$STD" org.jetbrains.kotlin.cli.jvm.K2JVMCompiler -no-stdlib -no-reflect \
  -classpath "$STD" -d "$OUT" -nowarn \
  "$ROOT/android/app/src/main/java/com/vibesdr/app/VibeApkPack.kt" "$ROOT/scripts/test-apk-pack/ApkPackTest.kt"
nice -n 10 java -Xmx256m -cp "$OUT:$STD" ApkPackTestKt
