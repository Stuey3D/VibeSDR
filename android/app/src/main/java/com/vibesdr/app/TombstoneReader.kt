package com.vibesdr.app

/**
 * Reads an Android tombstone (debuggerd's protobuf) into the text a person can read — the crashing
 * thread's backtrace, the signal, the abort message. Used by VibeExitInfo for the diagnostics report.
 *
 * ★★★ WHY (2026-10-05). Nick's Pixel (Android 16, Airspy HF+, the GitHub RC APK hosting VibeServer)
 *  crashed overnight and the Airspy was missing on restart — and there was NOTHING to read: the GitHub
 *  build is invisible to Play vitals, and Diagnostics only knew iOS crashes. Since API 31 the system
 *  keeps the tombstone of our last native crash and hands it back through
 *  ApplicationExitInfo.getTraceInputStream() — as the PROTOBUF, not the text logcat prints.
 *
 * ★★ HAND-ROLLED, NOT A PROTOBUF LIBRARY. We need ~15 fields of one message; a protobuf runtime for
 *  that is a dependency and an APK-size cost for a screen almost nobody opens. Field numbers are
 *  AOSP system/core/debuggerd/proto/tombstone.proto (checked against main, 2026-10-05):
 *    Tombstone: build_fingerprint 2, timestamp 4, pid 5, tid 6, command_line 9, signal_info 10,
 *               abort_message 14, causes 15, threads 16 (map<uint32,Thread>), process_uptime 20
 *    Signal:    number 1, name 2, code 3, code_name 4, has_sender 5, sender_uid 6, sender_pid 7,
 *               has_fault_address 8, fault_address 9
 *    Cause:     human_readable 1
 *    Thread:    id 1, name 2, current_backtrace 4
 *    BacktraceFrame: rel_pc 1, pc 2, sp 3, function_name 4, function_offset 5, file_name 6,
 *               file_map_offset 7, build_id 8
 *  ★ Proto3 is forward-compatible by construction: unknown fields are SKIPPED by wire type, so a
 *    newer Android that adds fields still decodes. Only a renumbering (which proto rules forbid)
 *    would break this.
 *
 * ★ PURE KOTLIN — no android.* imports — so it can be compiled and tested off-device.
 *
 * ★ PRIVACY: the tombstone also carries logcat (log_buffers, 18), open fds (19) and raw memory
 *  dumps. Fds and memory are never read. Logcat can hold anything the app ever logged — a URL with a
 *  PIN in it included — and this report is shown to the user and sent by them.
 *  ★★ (2026-10-05, Stuart approved) The LAST [MAX_LOG_LINES] log lines are read now ([logLines]), each
 *  passed through [redact] first: addresses, secrets after pin= / key= / token= / password=, emails,
 *  callsigns and long hex/base64 blobs become <placeholders>; the time, level, tag and the rest of the
 *  message stay — "what was the engine saying when it died" is the half of a crash a backtrace lacks.
 *    LogBuffer: name 1, logs 2 · LogMessage: timestamp 1, pid 2, tid 3, priority 4, tag 5, message 6
 *  Frames, thread names and the signal are code addresses, not user data.
 */
object TombstoneReader {
    const val MAX_FRAMES = 40
    const val MAX_OTHER_THREADS = 60
    const val MAX_LOG_LINES = 80
    private const val MAX_LOG_LINE_CHARS = 300

    private class Frame(
        var relPc: Long = 0, var functionName: String = "", var functionOffset: Long = 0,
        var fileName: String = "", var buildId: String = "",
    )
    private class Thread(var id: Long = 0, var name: String = "", val frames: MutableList<Frame> = ArrayList())

    /** A minimal protobuf wire reader over [b] from [start] to [end]. Throws on malformed input. */
    private class Pb(val b: ByteArray, var pos: Int, val end: Int) {
        fun more() = pos < end
        fun varint(): Long {
            var shift = 0; var r = 0L
            while (true) {
                if (pos >= end) throw IllegalStateException("varint past end")
                val x = b[pos++].toInt() and 0xff
                r = r or ((x and 0x7f).toLong() shl shift)
                if (x and 0x80 == 0) return r
                shift += 7
                if (shift > 63) throw IllegalStateException("varint too long")
            }
        }
        /** Returns [fieldNumber, wireType]. */
        fun tag(): IntArray { val t = varint(); return intArrayOf((t ushr 3).toInt(), (t and 7).toInt()) }
        fun lenRange(): IntRange {
            val n = varint()
            if (n < 0 || n > (end - pos).toLong()) throw IllegalStateException("length past end")
            val s = pos; pos += n.toInt(); return s until pos
        }
        fun str(r: IntRange) = String(b, r.first, r.last - r.first + 1, Charsets.UTF_8)
        fun sub(r: IntRange) = Pb(b, r.first, r.last + 1)
        fun skip(wire: Int) {
            when (wire) {
                0 -> varint()
                1 -> { if (end - pos < 8) throw IllegalStateException("fixed64 past end"); pos += 8 }
                2 -> lenRange()
                5 -> { if (end - pos < 4) throw IllegalStateException("fixed32 past end"); pos += 4 }
                else -> throw IllegalStateException("wire type $wire")    // groups (3/4) are not in this proto
            }
        }
    }

    /** The readable report, or — if the bytes will not decode — their printable strings. Never throws. */
    fun describe(bytes: ByteArray): String =
        try { decode(bytes) } catch (e: Throwable) {
            // ★ Redacted: the raw strings include the log buffers' text (2026-10-05 — they went out as-is before).
            "(tombstone did not decode: ${e.message}; printable strings follow)\n" +
                printableStrings(bytes).lineSequence().joinToString("\n") { redact(it) }
        }

    fun decode(bytes: ByteArray): String {
        val p = Pb(bytes, 0, bytes.size)
        var fingerprint = ""; var timestamp = ""; var pid = 0L; var tid = 0L; var uptime = -1L
        val cmd = ArrayList<String>(); val causes = ArrayList<String>()
        var abort = ""; var signal = ""
        val threads = ArrayList<Thread>()
        while (p.more()) {
            val (f, w) = p.tag().let { it[0] to it[1] }
            when {
                f == 2 && w == 2 -> fingerprint = p.str(p.lenRange())
                f == 4 && w == 2 -> timestamp = p.str(p.lenRange())
                f == 5 && w == 0 -> pid = p.varint()
                f == 6 && w == 0 -> tid = p.varint()
                f == 9 && w == 2 -> cmd += p.str(p.lenRange())
                f == 10 && w == 2 -> signal = signalLine(p.sub(p.lenRange()))
                f == 14 && w == 2 -> abort = p.str(p.lenRange())
                f == 15 && w == 2 -> causeText(p.sub(p.lenRange()))?.let { causes += it }
                f == 16 && w == 2 -> mapThread(p.sub(p.lenRange()))?.let { threads += it }
                f == 20 && w == 0 -> uptime = p.varint()
                else -> p.skip(w)
            }
        }
        if (threads.isEmpty() && signal.isEmpty() && pid == 0L) throw IllegalStateException("no tombstone fields")

        val out = StringBuilder()
        if (timestamp.isNotEmpty()) out.append("crashed at ").append(timestamp).append('\n')
        out.append("pid ").append(pid).append(", tid ").append(tid)
        if (cmd.isNotEmpty()) out.append(" — ").append(cmd.joinToString(" "))
        if (uptime >= 0) out.append(" (up ").append(uptime).append(" s)")
        out.append('\n')
        if (signal.isNotEmpty()) out.append(signal).append('\n')
        if (abort.isNotEmpty()) out.append("abort message: ").append(abort).append('\n')
        for (c in causes) out.append("cause: ").append(c).append('\n')
        if (fingerprint.isNotEmpty()) out.append("build: ").append(fingerprint).append('\n')

        val crashing = threads.firstOrNull { it.id == tid } ?: threads.firstOrNull()
        if (crashing != null) {
            out.append("\ncrashing thread ").append(crashing.id).append(" \"").append(crashing.name).append("\":\n")
            // ★ The same layout as a logcat tombstone ("#00 pc <rel_pc> <file> (<fn>+<off>) (BuildId: …)") so
            //   ndk-stack / addr2line read it as-is against the matching unstripped .so.
            for ((i, fr) in crashing.frames.take(MAX_FRAMES).withIndex()) {
                out.append("  #").append(i.toString().padStart(2, '0'))
                    .append(" pc ").append(java.lang.Long.toHexString(fr.relPc).padStart(16, '0'))
                    .append("  ").append(fr.fileName.ifEmpty { "<unknown>" })
                if (fr.functionName.isNotEmpty()) out.append(" (").append(fr.functionName).append('+').append(fr.functionOffset).append(')')
                if (fr.buildId.isNotEmpty()) out.append(" (BuildId: ").append(fr.buildId).append(')')
                out.append('\n')
            }
            if (crashing.frames.size > MAX_FRAMES) out.append("  … ").append(crashing.frames.size - MAX_FRAMES).append(" more frames\n")
        }
        val others = threads.filter { it !== crashing }.sortedBy { it.id }
        if (others.isNotEmpty()) {
            out.append("\nother threads (").append(others.size).append("):\n")
            // ★ Name + top frame only: enough to see whether the DSP / USB / server threads were alive and
            //   where they sat, without turning one report into thousands of lines.
            for (t in others.take(MAX_OTHER_THREADS)) {
                out.append("  ").append(t.id).append(' ').append(t.name)
                t.frames.firstOrNull()?.let { fr ->
                    out.append("  @ ").append(fr.fileName.substringAfterLast('/'))
                    if (fr.functionName.isNotEmpty()) out.append(" ").append(fr.functionName).append('+').append(fr.functionOffset)
                }
                out.append('\n')
            }
            if (others.size > MAX_OTHER_THREADS) out.append("  … ").append(others.size - MAX_OTHER_THREADS).append(" more\n")
        }
        return out.toString().trimEnd()
    }

    private fun signalLine(p: Pb): String {
        var num = 0L; var name = ""; var code = 0L; var codeName = ""
        var hasFault = false; var fault = 0L; var hasSender = false; var senderPid = 0L; var senderUid = 0L
        while (p.more()) {
            val (f, w) = p.tag().let { it[0] to it[1] }
            when {
                f == 1 && w == 0 -> num = p.varint()
                f == 2 && w == 2 -> name = p.str(p.lenRange())
                f == 3 && w == 0 -> code = p.varint()
                f == 4 && w == 2 -> codeName = p.str(p.lenRange())
                f == 5 && w == 0 -> hasSender = p.varint() != 0L
                f == 6 && w == 0 -> senderUid = p.varint()
                f == 7 && w == 0 -> senderPid = p.varint()
                f == 8 && w == 0 -> hasFault = p.varint() != 0L
                f == 9 && w == 0 -> fault = p.varint()
                else -> p.skip(w)
            }
        }
        // ★ int32 negatives arrive as 10-byte varints; toInt() folds them back (SI_TKILL is -6).
        val sb = StringBuilder("signal ").append(num.toInt()).append(" (").append(name).append("), code ")
            .append(code.toInt()).append(" (").append(codeName).append(")")
        sb.append(", fault addr ").append(if (hasFault) "0x" + java.lang.Long.toHexString(fault) else "--------")
        if (hasSender) sb.append(", from pid ").append(senderPid.toInt()).append(" uid ").append(senderUid.toInt())
        return sb.toString()
    }

    private fun causeText(p: Pb): String? {
        while (p.more()) {
            val (f, w) = p.tag().let { it[0] to it[1] }
            if (f == 1 && w == 2) return p.str(p.lenRange()) else p.skip(w)
        }
        return null
    }

    /** One map<uint32, Thread> entry: key = 1, value = 2. */
    private fun mapThread(p: Pb): Thread? {
        var key = -1L; var t: Thread? = null
        while (p.more()) {
            val (f, w) = p.tag().let { it[0] to it[1] }
            when {
                f == 1 && w == 0 -> key = p.varint()
                f == 2 && w == 2 -> t = thread(p.sub(p.lenRange()))
                else -> p.skip(w)
            }
        }
        if (t != null && t.id == 0L && key >= 0) t.id = key
        return t
    }

    private fun thread(p: Pb): Thread {
        val t = Thread()
        while (p.more()) {
            val (f, w) = p.tag().let { it[0] to it[1] }
            when {
                f == 1 && w == 0 -> t.id = p.varint().toInt().toLong()
                f == 2 && w == 2 -> t.name = p.str(p.lenRange())
                // ★ Frames past the cap are COUNTED but not decoded — a deep recursion must not cost a big parse.
                f == 4 && w == 2 -> { val r = p.lenRange(); t.frames += if (t.frames.size < MAX_FRAMES) frame(p.sub(r)) else Frame() }
                else -> p.skip(w)
            }
        }
        return t
    }

    private fun frame(p: Pb): Frame {
        val fr = Frame()
        while (p.more()) {
            val (f, w) = p.tag().let { it[0] to it[1] }
            when {
                f == 1 && w == 0 -> fr.relPc = p.varint()
                f == 4 && w == 2 -> fr.functionName = p.str(p.lenRange())
                f == 5 && w == 0 -> fr.functionOffset = p.varint()
                f == 6 && w == 2 -> fr.fileName = p.str(p.lenRange())
                f == 8 && w == 2 -> fr.buildId = p.str(p.lenRange())
                else -> p.skip(w)
            }
        }
        return fr
    }

    /** Fallback: runs of ≥ 6 printable ASCII characters, de-duplicated, capped — function and library
     *  names survive in this form even when the structure does not. */
    fun printableStrings(bytes: ByteArray, maxChars: Int = 4000): String {
        val out = StringBuilder(); val seen = HashSet<String>(); val cur = StringBuilder()
        fun flush() {
            if (cur.length >= 6) { val s = cur.toString(); if (seen.add(s) && out.length < maxChars) out.append(s).append('\n') }
            cur.setLength(0)
        }
        for (x in bytes) { val c = x.toInt() and 0xff; if (c in 0x20..0x7e) cur.append(c.toChar()) else flush() }
        flush()
        return out.toString().take(maxChars).trimEnd()
    }

    // ── Log lines (log_buffers, 18) — REDACTED ─────────────────────────────────────────────────────

    private class LogLine(val time: String, val prio: Int, val tag: String, val msg: String)

    /**
     * The last [max] log lines in the tombstone, every buffer merged in time order, each REDACTED
     * ([redact]) and cut to [MAX_LOG_LINE_CHARS]: "10-05 03:40:12.345 E VibeServer: message".
     * "" when there are none or the bytes will not decode. Never throws.
     */
    fun logLines(bytes: ByteArray, max: Int = MAX_LOG_LINES): String = try {
        val all = ArrayList<LogLine>()
        val p = Pb(bytes, 0, bytes.size)
        while (p.more()) {
            val (f, w) = p.tag().let { it[0] to it[1] }
            if (f == 18 && w == 2) logBuffer(p.sub(p.lenRange()), all) else p.skip(w)
        }
        // ★ Stable sort on "MM-DD HH:MM:SS.mmm": buffers (main, system, crash…) arrive one after another.
        all.sortedBy { it.time }.takeLast(max).joinToString("\n") { l ->
            val line = "${l.time} ${prioLetter(l.prio)} ${redact(l.tag)}: ${redact(l.msg.trimEnd())}"
            if (line.length > MAX_LOG_LINE_CHARS) line.take(MAX_LOG_LINE_CHARS) + "…" else line
        }
    } catch (_: Throwable) { "" }

    private fun logBuffer(p: Pb, into: MutableList<LogLine>) {
        while (p.more()) {
            val (f, w) = p.tag().let { it[0] to it[1] }
            if (f == 2 && w == 2) {
                val m = p.sub(p.lenRange())
                var time = ""; var prio = 0; var tag = ""; var msg = ""
                while (m.more()) {
                    val (g, x) = m.tag().let { it[0] to it[1] }
                    when {
                        g == 1 && x == 2 -> time = m.str(m.lenRange())
                        g == 4 && x == 0 -> prio = m.varint().toInt()
                        g == 5 && x == 2 -> tag = m.str(m.lenRange())
                        g == 6 && x == 2 -> msg = m.str(m.lenRange())
                        else -> m.skip(x)       // pid 2, tid 3 — not needed
                    }
                }
                // ★ A multi-line message (a Java stack in one log call) is kept as one entry, its lines joined.
                into += LogLine(time, prio, tag, msg.replace('\n', ' '))
            } else p.skip(w)
        }
    }

    private fun prioLetter(p: Int) = when (p) { 2 -> 'V'; 3 -> 'D'; 4 -> 'I'; 5 -> 'W'; 6 -> 'E'; 7 -> 'F'; else -> '?' }

    // ★★ WHAT COUNTS AS PRIVATE (Stuart, 2026-10-05): where the user connects (URLs, host:port, IP
    //  addresses), what unlocks it (PINs, keys, tokens, passwords, admin codes), who they are (email,
    //  callsign) and anything opaque enough to be one of those (long hex / base64). ORDER MATTERS: a URL
    //  goes whole before its host or its ?pin= could be half-matched.
    private val URL = Regex("""\b(?:https?|wss?|ftp|rtsp|rtmp|file|content)://[^\s"'<>]*""", RegexOption.IGNORE_CASE)
    private val SECRET_JSON = Regex(""""(pin|key|token|password|passwd|pwd|admin|secret|auth|apikey|api_key)"\s*:\s*"[^"]*"""", RegexOption.IGNORE_CASE)
    private val SECRET = Regex("""\b(pin|key|token|password|passwd|pwd|admin|secret|auth|apikey|api_key)(\s*[=:]\s*)[^\s&,;"']+""", RegexOption.IGNORE_CASE)
    private val EMAIL = Regex("""[\w.+-]+@[\w-]+(?:\.[\w-]+)+""")
    private val IPV4 = Regex("""\b(?:\d{1,3}\.){3}\d{1,3}(?::\d{1,5})?\b""")
    private val IPV6 = Regex("""\[?\b[0-9A-Fa-f]{0,4}(?::[0-9A-Fa-f]{0,4}){2,7}\b\]?(?::\d{1,5})?""")
    private val HOST_PORT = Regex("""\b[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*:\d{2,5}\b""")
    private val DOMAIN = Regex("""\b[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.(?:com|net|org|io|co|uk|de|nl|fr|eu|me|info|dev|tv|xyz|online|local|lan|home|arpa|ddns|duckdns|freemyip)\b""", RegexOption.IGNORE_CASE)
    private val CODE_PREFIX = Regex("""^(?:com|org|net|android|androidx|java|javax|kotlin|kotlinx|dalvik|io)\.""", RegexOption.IGNORE_CASE)
    private val CALLSIGN = Regex("""\b(?:[A-Z]{1,2}|\d[A-Z])\d{1,2}[A-Z]{1,4}(?:/[A-Z0-9]{1,4})?\b""")
    private val HEX = Regex("""\b(?:0x)?[0-9A-Fa-f]{16,}\b""")
    private val B64 = Regex("""[A-Za-z0-9+/_-]{24,}={0,2}""")

    /** One log string with the private parts replaced by <placeholders>. Public for the test harness. */
    fun redact(s: String): String {
        var r = URL.replace(s, "<url>")
        r = SECRET_JSON.replace(r) { "\"${it.groupValues[1]}\":\"<redacted>\"" }
        r = SECRET.replace(r) { "${it.groupValues[1]}${it.groupValues[2]}<redacted>" }
        r = EMAIL.replace(r, "<email>")
        r = IPV4.replace(r, "<ip>")
        // ★ "12:34:56" is three hex groups too: an address needs "::", six or more groups (a MAC
        //   included), or a hex LETTER — a time of day has none of those.
        r = IPV6.replace(r) { m ->
            val v = m.value; val groups = v.trim('[', ']').split(':').size
            if (v.contains("::") || groups >= 6 || v.any { it in 'a'..'f' || it in 'A'..'F' }) "<ip>" else v
        }
        // ★ host:port needs a dotted name (or localhost): "rate:48000" is a log field, not an address.
        r = HOST_PORT.replace(r) { m ->
            val h = m.value.substringBefore(':')
            if (h.any { it.isLetter() } && (h.contains('.') || h.equals("localhost", true))) "<host>" else m.value
        }
        // ★ A bare domain (a server's name) — not a Java/Android package (com.vibesdr.app ends in ".app").
        r = DOMAIN.replace(r) { m -> if (CODE_PREFIX.containsMatchIn(m.value)) m.value else "<host>" }
        r = CALLSIGN.replace(r, "<call>")
        r = HEX.replace(r, "<hex>")
        // ★ A blob needs a digit AND a letter: long identifiers (VibeStreamModuleListener) are not secrets.
        //   A path (/vendor/lib64/libairspyhf) is judged per segment, so only a blob-like segment hides it.
        r = B64.replace(r) { m ->
            fun blob(x: String) = x.length >= 16 && x.any { it.isDigit() } && x.any { it.isLetter() }
            val v = m.value
            if (if ('/' in v) v.split('/').any { blob(it) } else blob(v)) "<blob>" else v
        }
        return r
    }
}
