#!/usr/bin/env bash
# test-tls-roots.sh — compile VibeTls.kt + scripts/tls/TlsRootsTest.kt on the desktop JVM and run it.
#
# ★★ WHY A DESKTOP JVM. The fault is Android 5-7's root store (B10: Kiko's Moto G could not fetch the
#    RIPE/APNIC registries). No emulator image that old runs on Apple silicon, so the test simulates the
#    old store and proves VibeTls — the real file the APKs compile — against the real chains.
# ★ Uses the Kotlin compiler already in the gradle cache (the Android build put it there). Missing
#   compiler, missing java, or no network = exit 3, NOT RUN — never a pass.
set -uo pipefail
cd "$(dirname "$0")/.."

G="$HOME/.gradle/caches/modules-2/files-2.1"
pick() { ls "$G"/$1 2>/dev/null | grep -v sources | sort | tail -1; }
KC="$(pick 'org.jetbrains.kotlin/kotlin-compiler-embeddable/2.*/*/kotlin-compiler-embeddable-2.*.jar')"
STD="$(pick 'org.jetbrains.kotlin/kotlin-stdlib/2.*/*/kotlin-stdlib-2.*.jar')"
SCR="$(pick 'org.jetbrains.kotlin/kotlin-script-runtime/2.*/*/kotlin-script-runtime-2.*.jar')"
REF="$(pick 'org.jetbrains.kotlin/kotlin-reflect/2.*/*/kotlin-reflect-2.*.jar')"
DMN="$(pick 'org.jetbrains.kotlin/kotlin-daemon-embeddable/2.*/*/kotlin-daemon-embeddable-2.*.jar')"
TRV="$(pick 'org.jetbrains.intellij.deps/trove4j/*/*/trove4j-*.jar')"
COR="$(pick 'org.jetbrains.kotlinx/kotlinx-coroutines-core-jvm/*/*/kotlinx-coroutines-core-jvm-*.jar')"
ANN="$(pick 'org.jetbrains/annotations/*/*/annotations-*.jar')"
if ! command -v java >/dev/null || [ -z "$KC" ] || [ -z "$STD" ]; then
  echo "   not run — needs java and the Kotlin compiler in the gradle cache (build the Android app once)"
  exit 3
fi
OUT="${TMPDIR:-/tmp}/vibe-tls-test"
rm -rf "$OUT"; mkdir -p "$OUT"
CP="$KC:$STD:$SCR:$REF:$DMN:$TRV:$COR:$ANN"
if ! java -cp "$CP" org.jetbrains.kotlin.cli.jvm.K2JVMCompiler -no-stdlib -no-reflect -classpath "$STD" \
       -d "$OUT" android/app/src/main/java/com/vibesdr/app/VibeTls.kt scripts/tls/TlsRootsTest.kt \
       > "$OUT/build.log" 2>&1; then
  echo "   did not build — $OUT/build.log"; cat "$OUT/build.log" | tail -20
  exit 1
fi
java -cp "$OUT:$STD" TlsRootsTestKt
