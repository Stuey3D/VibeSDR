#!/usr/bin/env bash
# test-tunnel-url-android.sh — the Android tunnel-address parser (VibeQuickTunnelUrl.kt, 2026-10-27): compile the app's
# own file on a desktop JVM and hold it to the same cloudflared lines as vibeserver/test-tunnel-url.cpp. Kiko's TV box
# read "https://api.trycloudflare.com" out of cloudflared's failure line and recycled the tunnel every 15 s.
# ★ Same toolchain trick as test-tls-pin-android.sh. Missing java / Kotlin compiler = exit 3, NOT RUN — never a pass.
set -uo pipefail
cd "$(dirname "$0")/.."

G="$HOME/.gradle/caches/modules-2/files-2.1"
pick() { ls "$G"/$1 2>/dev/null | grep -v sources | sort | tail -1; }
KC="$(pick 'org.jetbrains.kotlin/kotlin-compiler-embeddable/2.*/*/kotlin-compiler-embeddable-2.*.jar')"
KV="$(basename "$(dirname "$(dirname "$KC")")")"
STD="$(pick "org.jetbrains.kotlin/kotlin-stdlib/$KV/*/kotlin-stdlib-$KV.jar")"
SCR="$(pick "org.jetbrains.kotlin/kotlin-script-runtime/$KV/*/kotlin-script-runtime-$KV.jar")"
REF="$(pick "org.jetbrains.kotlin/kotlin-reflect/$KV/*/kotlin-reflect-$KV.jar")"
DMN="$(pick "org.jetbrains.kotlin/kotlin-daemon-embeddable/$KV/*/kotlin-daemon-embeddable-$KV.jar")"
TRV="$(pick 'org.jetbrains.intellij.deps/trove4j/*/*/trove4j-*.jar')"
COR="$(pick 'org.jetbrains.kotlinx/kotlinx-coroutines-core-jvm/*/*/kotlinx-coroutines-core-jvm-*.jar')"
ANN="$(pick 'org.jetbrains/annotations/*/*/annotations-*.jar')"
command -v java >/dev/null || { echo "   not run — needs java"; exit 3; }
if [ -z "$KC" ] || [ -z "$STD" ]; then
  echo "   not run — needs the Kotlin compiler in the gradle cache (build the Android app once)"; exit 3
fi
OUT="${TMPDIR:-/tmp}/vibe-tunnel-url-android"
rm -rf "$OUT"; mkdir -p "$OUT"

CP="$KC:$STD:$SCR:$REF:$DMN:$TRV:$COR:$ANN"
if ! nice java -Xmx512m -cp "$CP" org.jetbrains.kotlin.cli.jvm.K2JVMCompiler -no-stdlib -no-reflect -classpath "$STD" \
       -d "$OUT/classes" android/app/src/main/java/com/vibesdr/app/VibeQuickTunnelUrl.kt scripts/tunnel/QuickTunnelUrlTest.kt \
       > "$OUT/build.log" 2>&1; then
  echo "   did not build — $OUT/build.log"; tail -20 "$OUT/build.log"; exit 1
fi
java -Xmx128m -cp "$OUT/classes:$STD" QuickTunnelUrlTestKt
