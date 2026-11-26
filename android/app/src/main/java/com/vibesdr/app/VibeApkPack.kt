package com.vibesdr.app

import java.io.File
import java.io.FileNotFoundException
import java.io.RandomAccessFile

/**
 * ★★★ beta96: THE GPU MAP'S BIG PACKS READ STRAIGHT OUT OF THE APK — the plain-JVM half of VibeMapGL, kept free of
 *  android.* so it is tested off a phone (scripts/test-apk-pack.sh: a real zip with stored and compressed entries,
 *  and a fake AssetManager). Shared by the main app and Lite (Lite's SHARED list), like VibeMapGL.
 *
 *  The APK stores the .pmtiles uncompressed (mapgl-assets.gradle noCompress), so each one is a single run of bytes
 *  inside the APK file. AssetManager.openFd hands back exactly that run (the APK's descriptor, startOffset, length) —
 *  and THROWS FileNotFoundException for a compressed entry, which is how a pack that cannot be read in place is
 *  noticed: it is then unpacked, as before beta96.
 */
object VibeApkPack {
    /** `length` bytes from `offset` of `path` (the APK). */
    class Slice(val path: String, val offset: Long, val length: Long)

    /** AssetManager.openFd, reduced to what we need: the file behind the descriptor, the offset, the length.
     *  Throws FileNotFoundException for a compressed (or missing) entry. */
    fun interface Opener { fun open(asset: String): Slice }

    /**
     * Which of `names` (assets under `assetDir`) can be read in place. Each must open as a descriptor (stored, not
     * compressed), be the length the manifest says (`manifestBytes`, when it names it), lie inside its file, and start
     * with the 7-byte PMTiles magic. Anything else is left out, with `log` saying why — the caller unpacks it.
     */
    fun locate(assetDir: String, names: List<String>, manifestBytes: Map<String, Long>, opener: Opener,
               log: (String) -> Unit): Map<String, Slice> {
        val out = LinkedHashMap<String, Slice>()
        for (name in names) {
            try {
                val sl = try { opener.open("$assetDir/$name") } catch (e: FileNotFoundException) {
                    log("$name is COMPRESSED in the APK (or missing) — unpacking it instead: ${e.message}"); continue
                }
                val want = manifestBytes[name]
                if (want != null && want != sl.length) {
                    log("$name: APK entry is ${sl.length} B, the manifest says $want — unpacking it instead"); continue
                }
                val magic = ByteArray(7)
                RandomAccessFile(sl.path, "r").use { f ->
                    if (sl.offset < 0 || sl.length < 7 || sl.offset + sl.length > f.length())
                        throw IllegalStateException("entry @${sl.offset}+${sl.length} runs outside ${sl.path}")
                    f.seek(sl.offset); f.readFully(magic)
                }
                if (String(magic, Charsets.US_ASCII) != "PMTiles") {
                    log("$name: no PMTiles magic at ${sl.path}@${sl.offset} — unpacking it instead"); continue
                }
                out[name] = sl
            } catch (t: Throwable) {
                log("$name: cannot be read from the APK (${t.message}) — unpacking it instead")
            }
        }
        return out
    }

    /** Ranged reads, with the APK kept open between them (one RandomAccessFile per path — normally just base.apk). */
    class Reader {
        private val files = HashMap<String, RandomAccessFile>()

        /** Up to `length` bytes from `offset` of the slice (fewer at its end); null past the end or on a failure. */
        fun read(sl: Slice, offset: Long, length: Int): ByteArray? {
            if (offset < 0 || length <= 0 || offset >= sl.length) return null
            val n = minOf(length.toLong(), sl.length - offset).toInt()
            val buf = ByteArray(n)
            return synchronized(files) {
                try {
                    val f = files.getOrPut(sl.path) { RandomAccessFile(sl.path, "r") }
                    f.seek(sl.offset + offset); f.readFully(buf)
                    buf
                } catch (t: Throwable) {
                    closeLocked()   // ★ a stale handle (the APK replaced under us) is reopened on the next read
                    null
                }
            }
        }

        fun close() = synchronized(files) { closeLocked() }
        private fun closeLocked() {
            files.values.forEach { try { it.close() } catch (_: Throwable) {} }
            files.clear()
        }
    }

    /** Up to `length` bytes from `offset` of a plain file (an unpacked pack — the fallback). */
    fun readFile(file: File, offset: Long, length: Int): ByteArray? {
        if (offset < 0 || length <= 0) return null
        return try {
            RandomAccessFile(file, "r").use { f ->
                if (offset >= f.length()) return null
                val buf = ByteArray(minOf(length.toLong(), f.length() - offset).toInt())
                f.seek(offset); f.readFully(buf)
                buf
            }
        } catch (t: Throwable) { null }
    }
}
