#!/usr/bin/env bash
# test-tls-pin-android.sh — per-server certificate pins (GitHub #52), Android side: compile the app's own
# VibePinnedTls.kt with the real OkHttp on a desktop JVM and run scripts/tls/PinnedTlsTest.kt against self-signed
# servers on 127.0.0.1 (scripts/tls/tls-test-server.mjs). Same toolchain trick as test-tls-roots.sh.
# ★ Missing java / Kotlin compiler / OkHttp in the gradle cache / openssl / node = exit 3, NOT RUN — never a pass.
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
# ★ The OkHttp React Native 0.86 ships (4.12.0) and the okio it was built against.
OKH="$(pick 'com.squareup.okhttp3/okhttp/4.12.0/*/okhttp-4.12.0.jar')"
OKIO="$(pick 'com.squareup.okio/okio-jvm/3.6.0/*/okio-jvm-3.6.0.jar')"
for t in java openssl node; do command -v $t >/dev/null || { echo "   not run — needs $t"; exit 3; }; done
if [ -z "$KC" ] || [ -z "$STD" ] || [ -z "$OKH" ] || [ -z "$OKIO" ]; then
  echo "   not run — needs the Kotlin compiler and OkHttp 4.12 in the gradle cache (build the Android app once)"; exit 3
fi
OUT="${TMPDIR:-/tmp}/vibe-tls-pin-android"
rm -rf "$OUT"; mkdir -p "$OUT"

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/ss.key" -out "$OUT/ss.pem" -days 3650 -subj "/CN=openwebrx" 2>/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/lh.key" -out "$OUT/lh.pem" -days 365 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" 2>/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/ex.key" -out "$OUT/ex.pem" -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost" -not_before 20200101000000Z -not_after 20210101000000Z 2>/dev/null
fp() { openssl x509 -in "$1" -noout -fingerprint -sha256 | sed 's/.*=//; s/://g' | tr 'A-F' 'a-f'; }

P1=$((20000 + RANDOM % 20000)); P2=$((P1 + 1)); P3=$((P1 + 2))
node scripts/tls/tls-test-server.mjs "$OUT/ss.pem" "$OUT/ss.key" $P1 "$OUT/lh.pem" "$OUT/lh.key" $P2 \
  "$OUT/ex.pem" "$OUT/ex.key" $P3 > "$OUT/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
for _ in $(seq 1 50); do [ "$(grep -c LISTEN "$OUT/server.log" || true)" -ge 3 ] && break; sleep 0.1; done

CP="$KC:$STD:$SCR:$REF:$DMN:$TRV:$COR:$ANN"
if ! nice java -cp "$CP" org.jetbrains.kotlin.cli.jvm.K2JVMCompiler -no-stdlib -no-reflect -classpath "$STD:$OKH:$OKIO" \
       -d "$OUT/classes" android/app/src/main/java/com/vibesdr/app/VibePinnedTls.kt scripts/tls/PinnedTlsTest.kt \
       > "$OUT/build.log" 2>&1; then
  echo "   did not build — $OUT/build.log"; tail -20 "$OUT/build.log"; exit 1
fi
java -cp "$OUT/classes:$STD:$OKH:$OKIO" PinnedTlsTestKt $P1 "$(fp "$OUT/ss.pem")" $P2 "$(fp "$OUT/lh.pem")" \
  $P3 "$(fp "$OUT/ex.pem")" "$OUT/server.log"
