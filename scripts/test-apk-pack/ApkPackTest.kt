// ApkPackTest.kt — beta96: the GPU map's packs read straight out of the APK (VibeApkPack.kt), on a plain JVM.
// Run: scripts/test-apk-pack.sh. A REAL zip (java.util.zip) with STORED and DEFLATED entries stands in for the APK, and
// a fake AssetManager.openFd answers like the real one: the entry's data offset and length for a stored entry, a
// FileNotFoundException for a compressed one ("This file can not be opened as a file descriptor; it is probably
// compressed").
import com.vibesdr.app.VibeApkPack
import java.io.File
import java.io.FileNotFoundException
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

var fails = 0
fun ok(c: Boolean, what: String) { println("  ${if (c) "ok  " else "FAIL"}  $what"); if (!c) fails++ }

fun stored(name: String, body: ByteArray) = ZipEntry(name).apply {
    method = ZipEntry.STORED; size = body.size.toLong(); compressedSize = body.size.toLong()
    crc = CRC32().apply { update(body) }.value
}

/** Data offset, size and method of each entry — from the CENTRAL DIRECTORY (a deflated entry's local header carries no
 *  sizes when the writer used a data descriptor), then its local header for the name/extra lengths: exactly the
 *  offset AssetManager.openFd reports as startOffset for a stored entry. */
fun entries(zip: File): Map<String, Triple<Long, Long, Int>> {
    val out = HashMap<String, Triple<Long, Long, Int>>()
    val z = zip.readBytes()
    fun u16(o: Int) = (z[o].toInt() and 255) or ((z[o + 1].toInt() and 255) shl 8)
    fun u32(o: Int) = u16(o).toLong() or (u16(o + 2).toLong() shl 16)
    var eocd = z.size - 22
    while (eocd >= 0 && u32(eocd) != 0x06054b50L) eocd--
    require(eocd >= 0) { "no end record" }
    var p = u32(eocd + 16).toInt()
    repeat(u16(eocd + 10)) {
        require(u32(p) == 0x02014b50L) { "bad central header" }
        val method = u16(p + 10); val usize = u32(p + 24)
        val nl = u16(p + 28); val xl = u16(p + 30); val cl = u16(p + 32)
        val lho = u32(p + 42).toInt()
        val name = String(z, p + 46, nl, Charsets.UTF_8)
        val data = lho + 30L + u16(lho + 26) + u16(lho + 28)
        out[name] = Triple(data, usize, method)
        p += 46 + nl + xl + cl
    }
    return out
}

fun main() {
    val dir = kotlin.io.path.createTempDirectory("test-apk-pack").toFile()
    try {
        val apk = File(dir, "base.apk")
        val basic = ByteArray(300_000) { (it % 251).toByte() }.also { "PMTiles".toByteArray().copyInto(it) }
        val runways = ByteArray(5_000) { (it % 13).toByte() }.also { "PMTiles".toByteArray().copyInto(it) }
        val notPack = ByteArray(1_000) { 7 }
        ZipOutputStream(FileOutputStream(apk)).use { z ->
            z.putNextEntry(ZipEntry("AndroidManifest.xml")); z.write(ByteArray(4000)); z.closeEntry()   // deflated
            z.putNextEntry(stored("assets/mapgl/vibemap-basic.pmtiles", basic)); z.write(basic); z.closeEntry()
            val relief = ByteArray(10_000).also { "PMTiles".toByteArray().copyInto(it) }
            z.putNextEntry(ZipEntry("assets/mapgl/vibemap-relief.pmtiles")); z.write(relief); z.closeEntry()   // DEFLATED
            z.putNextEntry(stored("assets/mapgl/vibemap-runways.pmtiles", runways)); z.write(runways); z.closeEntry()
            z.putNextEntry(stored("assets/mapgl/other.pmtiles", notPack)); z.write(notPack); z.closeEntry()
        }
        val ents = entries(apk)
        // The fake AssetManager.openFd.
        val opener = VibeApkPack.Opener { asset ->
            val e = ents[asset] ?: throw FileNotFoundException(asset)
            if (e.third != 0) throw FileNotFoundException("This file can not be opened as a file descriptor; it is probably compressed")
            VibeApkPack.Slice(apk.path, e.first, e.second)
        }
        val logs = ArrayList<String>()
        val names = listOf("vibemap-basic.pmtiles", "vibemap-relief.pmtiles", "vibemap-runways.pmtiles")

        println("locate:")
        val got = VibeApkPack.locate("assets/mapgl", names, mapOf("vibemap-basic.pmtiles" to basic.size.toLong()), opener) { logs += it }
        ok(got.keys == setOf("vibemap-basic.pmtiles", "vibemap-runways.pmtiles"), "the two STORED packs are read in place")
        ok("vibemap-relief.pmtiles" !in got && logs.any { it.contains("relief") && it.contains("COMPRESSED") },
           "★★ a COMPRESSED pack is left to the unpacker, and the log says so")
        val bad = VibeApkPack.locate("assets/mapgl", names, mapOf("vibemap-basic.pmtiles" to 12345L), opener) { logs += it }
        ok("vibemap-basic.pmtiles" !in bad && logs.any { it.contains("manifest says 12345") }, "a length that disagrees with the manifest is refused")
        val noMagic = VibeApkPack.locate("assets/mapgl", listOf("other.pmtiles"), emptyMap(), opener) { logs += it }
        ok(noMagic.isEmpty() && logs.any { it.contains("no PMTiles magic") }, "a stored entry without the PMTiles magic is refused")
        val off = VibeApkPack.locate("assets/mapgl", listOf("vibemap-basic.pmtiles"), emptyMap(),
            { VibeApkPack.Slice(apk.path, apk.length() - 10, 300_000) }) { logs += it }
        ok(off.isEmpty(), "an entry that runs past the end of the APK is refused")
        val missing = VibeApkPack.locate("assets/mapgl", listOf("vibemap-basic.pmtiles"), emptyMap(),
            { VibeApkPack.Slice(File(dir, "gone.apk").path, 0, 10) }) { logs += it }
        ok(missing.isEmpty(), "an APK path that cannot be opened is refused (no throw)")

        println("Reader:")
        val r = VibeApkPack.Reader()
        val sb = got.getValue("vibemap-basic.pmtiles")
        ok(r.read(sb, 0, 7)?.let { String(it, Charsets.US_ASCII) } == "PMTiles", "the magic at offset 0 of the entry")
        ok(r.read(sb, 123_456, 544)?.contentEquals(basic.copyOfRange(123_456, 124_000)) == true, "a range in the middle is the entry's own bytes")
        ok(r.read(sb, basic.size - 10L, 100)?.contentEquals(basic.copyOfRange(basic.size - 10, basic.size)) == true,
           "a read across the end is cut at the ENTRY's end, never runs into the next entry")
        ok(r.read(sb, basic.size.toLong(), 10) == null, "past the end: null")
        ok(r.read(sb, -1, 10) == null && r.read(sb, 0, 0) == null, "negative offset / empty length: null")
        val sr = got.getValue("vibemap-runways.pmtiles")
        ok(r.read(sr, 0, runways.size)?.contentEquals(runways) == true, "a second pack through the same open APK")
        r.close()
        ok(r.read(sb, 7, 3)?.contentEquals(basic.copyOfRange(7, 10)) == true, "reopened after close()")
        apk.delete()
        r.close()
        ok(r.read(sb, 0, 7) == null, "the APK gone: null, not a throw")

        println("readFile (the unpacked fallback):")
        val plain = File(dir, "vibemap-relief.pmtiles").apply { writeBytes(basic) }
        ok(VibeApkPack.readFile(plain, 1000, 24)?.contentEquals(basic.copyOfRange(1000, 1024)) == true, "a range of an unpacked pack")
        ok(VibeApkPack.readFile(plain, basic.size.toLong(), 1) == null, "past its end: null")
        ok(VibeApkPack.readFile(File(dir, "nope"), 0, 1) == null, "missing: null")
    } finally {
        dir.deleteRecursively()
    }
    println(if (fails == 0) "\nall passed" else "\n$fails FAILED")
    kotlin.system.exitProcess(if (fails == 0) 0 else 1)
}
