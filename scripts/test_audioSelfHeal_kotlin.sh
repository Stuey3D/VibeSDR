#!/bin/bash
# test_audioSelfHeal_kotlin.sh — run the Kotlin port of the audio self-heal rules
# (android/app/src/main/java/com/vibesdr/app/AudioSelfHeal.kt) against the SAME scenarios as the
# TypeScript original (scripts/test_audioSelfHeal.ts --lines). No kotlinc is installed here, so this
# drives the Kotlin compiler Gradle already cached for the Android build, on the plain JVM.
set -euo pipefail
cd "$(dirname "$0")/.."
G="$HOME/.gradle/caches/modules-2/files-2.1"
jar() { find "$G/$1" -name "$2" ! -name '*sources*' | sort | tail -1; }
KC=$(jar org.jetbrains.kotlin/kotlin-compiler-embeddable 'kotlin-compiler-embeddable-2.1.20.jar')
STD=$(jar org.jetbrains.kotlin/kotlin-stdlib 'kotlin-stdlib-2.1.20.jar')
CP="$KC:$STD:$(jar org.jetbrains.kotlin/kotlin-script-runtime 'kotlin-script-runtime-2.1.20.jar'):$(jar org.jetbrains.kotlin/kotlin-reflect 'kotlin-reflect-2.1.0.jar'):$(jar org.jetbrains.intellij.deps/trove4j 'trove4j-1.0.20200330.jar'):$(jar org.jetbrains.kotlinx/kotlinx-coroutines-core-jvm 'kotlinx-coroutines-core-jvm-1.6.4.jar'):$(jar org.jetbrains/annotations 'annotations-13.0.jar'):$(jar org.jetbrains.kotlin/kotlin-daemon-embeddable 'kotlin-daemon-embeddable-2.1.20.jar')"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
npx tsx scripts/test_audioSelfHeal.ts --lines > "$T/scenarios.txt"
cat > "$T/Main.kt" <<'KT'
import com.vibesdr.app.AudioSelfHeal
import java.io.File
fun main(args: Array<String>) {
    var fail = 0; var n = 0
    var m: AudioSelfHeal? = null; var name = ""; val got = mutableListOf<String>()
    for (line in File(args[0]).readLines()) {
        val p = line.split(" ")
        when (p[0]) {
            "S" -> { name = p.drop(2).joinToString(" "); got.clear()
                     m = AudioSelfHeal(notRecvMs = if (p[1] == "-") null else p[1].toLong()) }
            "T" -> {
                // +1_000_000 ms: a real monotonic clock is never 0, and 0 means "never" inside.
                val now = p[1].toLong() + 1_000_000
                val hold = p[5].toLong()
                if (hold > 0) m!!.hold(now, hold)
                val d = m!!.tick(now, p[2].toLong(), p[3].toLong(), p[4] == "1")
                if (d.action != AudioSelfHeal.Action.NONE) got.add("${p[1]}:${d.action.wire}")
            }
            "W" -> { n++
                val want = p.drop(1).filter { it.isNotEmpty() }
                if (want == got) println("ok   $name") else { fail++; println("FAIL $name\n     want $want\n     got  $got") } }
        }
    }
    println(if (fail == 0) "kotlin port: all $n scenarios pass" else "kotlin port: $fail FAILED")
    kotlin.system.exitProcess(if (fail == 0) 0 else 1)
}
KT
java -cp "$CP" org.jetbrains.kotlin.cli.jvm.K2JVMCompiler -no-reflect -no-stdlib -cp "$STD" -d "$T/out" \
  android/app/src/main/java/com/vibesdr/app/AudioSelfHeal.kt "$T/Main.kt" 2>&1 | grep -v "^warning" || true
java -cp "$T/out:$STD" MainKt "$T/scenarios.txt"
