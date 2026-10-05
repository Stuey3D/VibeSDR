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
 *  dumps. NONE of those are read: logcat can hold anything the app ever logged — a URL with a PIN
 *  in it included — and this report is shown to the user and sent by them. Frames, thread names and
 *  the signal are code addresses, not user data.
 */
object TombstoneReader {
    const val MAX_FRAMES = 40
    const val MAX_OTHER_THREADS = 60

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
            "(tombstone did not decode: ${e.message}; printable strings follow)\n" + printableStrings(bytes)
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
}
