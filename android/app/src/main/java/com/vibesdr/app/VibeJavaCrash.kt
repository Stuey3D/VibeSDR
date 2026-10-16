package com.vibesdr.app

import android.content.Context
import android.os.Debug
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The Java/Kotlin crash Android does not keep. VibeExitInfo reads what the system records about how
 * our process died — and for a native crash or an ANR that includes the trace. For an ordinary JVM
 * crash (reason CRASH) Android keeps only the word "crash": no exception, no stack.
 *
 * ★★★ WHY (2026-10-15). NickB (Pixel 6, Android 17, RC41 569, serving his Airspy HF+): "CRASH, reason:
 *  crash, rss 1031 MB", and nothing else. An OutOfMemoryError, a RN JavascriptException in a release
 *  build, a bad cast in a module — all read the same. So we write it down ourselves, at the moment it
 *  happens: the exception chain, the thread, and the memory figures, to one file in our own storage.
 *  VibeExitInfo attaches it to the matching CRASH exit as its stack.
 *
 * ★ The previous handler ALWAYS runs afterwards — the app still crashes exactly as before (and Android
 *  still records the exit). This only takes notes on the way down.
 * ★ LOCAL ONLY, like the rest of the report: nothing leaves the phone unless the user sends it. Exception
 *  MESSAGES can carry an address or a key, so they go through TombstoneReader.redact; the "at …" frames
 *  are code locations only and are kept whole (redact would turn "Foo.kt:42" into <host>).
 */
object VibeJavaCrash {
    private const val FILE = "java-crash.txt"
    private const val MAX_FRAMES = 40
    private const val MAX_CAUSES = 6
    private val handling = AtomicBoolean(false)

    fun install(ctx: Context) {
        val file = File(ctx.filesDir, FILE)
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            // ★ Two threads can die together; the first one's note is the one that matters.
            if (handling.compareAndSet(false, true)) {
                try { file.writeText(describe(thread, error)) } catch (_: Throwable) {}
            }
            if (previous != null) previous.uncaughtException(thread, error)
            else { android.os.Process.killProcess(android.os.Process.myPid()); System.exit(10) }
        }
    }

    /** The note for a CRASH exit at [exitMs], or null if the one on disk belongs to another crash. */
    fun readFor(ctx: Context, exitMs: Long): String? = try {
        val f = File(ctx.filesDir, FILE)
        // ★ Written just before the process died; the system stamps the exit a moment later.
        if (f.exists() && f.lastModified() in (exitMs - 60_000)..(exitMs + 5_000)) f.readText() else null
    } catch (_: Throwable) { null }

    private fun describe(thread: Thread, error: Throwable): String {
        val rt = Runtime.getRuntime()
        val mb = 1024 * 1024
        val sb = StringBuilder()
        val now = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US)
            .apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date())
        sb.append("caught : ").append(now).append(" on thread \"").append(thread.name).append("\"\n")
        // ★ Java heap against ITS limit (the OOM line), and the native heap the C++ engine lives in.
        sb.append("heap   : java ").append((rt.totalMemory() - rt.freeMemory()) / mb)
            .append(" of ").append(rt.maxMemory() / mb).append(" MB, native ")
            .append(Debug.getNativeHeapAllocatedSize() / mb).append(" MB\n")
        var t: Throwable? = error
        var depth = 0
        val seen = HashSet<Throwable>()
        while (t != null && depth < MAX_CAUSES && seen.add(t)) {
            if (depth > 0) sb.append("Caused by: ")
            sb.append(t.javaClass.name)
            t.message?.let { sb.append(": ").append(TombstoneReader.redact(it.take(2000))) }
            sb.append('\n')
            val frames = t.stackTrace
            for (f in frames.take(MAX_FRAMES)) sb.append("\tat ").append(f.toString()).append('\n')
            if (frames.size > MAX_FRAMES) sb.append("\t... ").append(frames.size - MAX_FRAMES).append(" more\n")
            t = t.cause
            depth++
        }
        return sb.toString()
    }
}
