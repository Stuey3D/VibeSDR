// TlsRootsTest.kt — VibeTls (android/app/src/main/java/com/vibesdr/app/VibeTls.kt) on a desktop JVM,
// against the REAL chains the server's downloads meet, with an OLD PHONE's root store simulated.
//
// ★★★ THE FAULT (B10): Kiko's Lite on Android 5.1 could not fetch the RIPE and APNIC registry files
//     ("Trust anchor for certification path not found"), so European visitors had no flag. No Android
//     5-7 emulator image runs on this Apple-silicon Mac, so the old store is SIMULATED: the JVM's own
//     roots with every root VibeTls bundles (and anything ISRG / Let's Encrypt) taken OUT. That store
//     must REPRODUCE the fault on its own, and VibeTls on top of it must cure it — while still refusing
//     an expired, self-signed, untrusted-root and wrong-host certificate.
// ★ Run by scripts/test-tls-roots.sh (compiles this and VibeTls.kt with the Kotlin compiler already in
//   the gradle cache). Needs the network; offline it reports NOT RUN.
import com.vibesdr.app.VibeTls
import java.net.URL
import java.security.KeyStore
import java.security.cert.X509Certificate
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.TrustManager
import javax.net.ssl.TrustManagerFactory
import javax.net.ssl.X509TrustManager
import kotlin.system.exitProcess

var pass = 0
var fail = 0
fun ok(c: Boolean, what: String) { if (c) pass++ else fail++; println("   ${if (c) "ok  " else "FAIL"} $what") }

fun factory(tm: X509TrustManager): SSLSocketFactory =
    SSLContext.getInstance("TLS").apply { init(null, arrayOf<TrustManager>(tm), null) }.socketFactory

/** Does an HTTPS request to `url` complete its handshake (and hostname check) with this trust? */
fun reaches(url: String, sf: SSLSocketFactory): Pair<Boolean, String> = try {
    val c = URL(url).openConnection() as HttpsURLConnection
    c.sslSocketFactory = sf
    c.connectTimeout = 15_000; c.readTimeout = 15_000
    c.instanceFollowRedirects = false
    c.requestMethod = "HEAD"
    val code = c.responseCode            // any HTTP status at all = TLS succeeded
    c.disconnect()
    true to "HTTP $code"
} catch (e: Exception) {
    false to "${e.javaClass.simpleName}: ${e.message?.take(90)}"
}

fun main() {
    // The JVM's own roots — a modern store.
    val jvm = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
        .apply { init(null as KeyStore?) }.trustManagers.filterIsInstance<X509TrustManager>().first()
    val bundled = VibeTls.bundledRoots()
    ok(bundled.size == 7, "VibeTls carries its 7 roots (${bundled.size})")
    val bundledSubjects = bundled.map { it.subjectX500Principal }.toSet()
    // ★ The OLD PHONE: every modern root we carry is missing, and so is everything ISRG.
    val oldRoots: List<X509Certificate> = jvm.acceptedIssuers.filter {
        it.subjectX500Principal !in bundledSubjects && !it.subjectX500Principal.name.contains("ISRG")
            && !it.subjectX500Principal.name.contains("Internet Security Research Group")
    }
    println("   (simulated old store: ${oldRoots.size} of the JVM's ${jvm.acceptedIssuers.size} roots)")
    val old = VibeTls.trustManagerFor(oldRoots)
    val oldOnly = factory(old)
    val fixed = factory(VibeTls.withAddedRoots(old))

    // Network check first: offline is NOT RUN, never a pass.
    if (!reaches("https://ftp.arin.net/", factory(jvm)).first) {
        println("   not run — no network (could not reach ftp.arin.net with the JVM's own roots)")
        exitProcess(3)
    }

    // ── The fault, reproduced on the old store alone ──
    for (u in listOf("https://ftp.ripe.net/", "https://ftp.apnic.net/",
                     "https://release-assets.githubusercontent.com/")) {
        val (r, why) = reaches(u, oldOnly)
        ok(!r, "OLD store alone REFUSES $u — the Moto G's fault ($why)")
    }

    // ── VibeTls cures it, for every host the server downloads from ──
    for (u in listOf(
        "https://ftp.ripe.net/pub/stats/ripencc/",          // country data: Europe / Middle East
        "https://ftp.apnic.net/stats/apnic/",                // country data: Asia-Pacific
        "https://ftp.arin.net/pub/stats/arin/",              // country data: North America
        "https://ftp.lacnic.net/pub/stats/lacnic/",          // country data: Latin America
        "https://ftp.afrinic.net/pub/stats/afrinic/",        // country data: Africa
        "https://iptoasn.com/",                              // the network (ASN) table
        "https://github.com/Stuey3D/VibeSDR/releases",       // benchmark clip + map pack, first hop
        "https://release-assets.githubusercontent.com/",     // …and where GitHub redirects them
        "https://objects.githubusercontent.com/",
        "https://vibeserver.vibesdr.net/",                   // the directory, speed test, tunnel listing
    )) {
        val (r, why) = reaches(u, fixed)
        ok(r, "OLD store + VibeTls reaches $u ($why)")
    }

    // ── And it is not weaker: bad certificates are still refused ──
    for ((u, what) in listOf(
        "https://expired.badssl.com/" to "an EXPIRED certificate",
        "https://self-signed.badssl.com/" to "a SELF-SIGNED certificate",
        "https://untrusted-root.badssl.com/" to "an UNTRUSTED ROOT",
        "https://wrong.host.badssl.com/" to "a certificate for the WRONG HOST",
    )) {
        val (r, why) = reaches(u, fixed)
        ok(!r, "OLD store + VibeTls still refuses $what ($why)")
        // ★ And so does a fully modern store — so the refusal is the certificate's, not our setup's.
        val (r2, _) = reaches(u, factory(VibeTls.withAddedRoots(jvm)))
        ok(!r2, "…and so does the JVM's full store + VibeTls")
    }

    println("   $pass passed, $fail failed")
    exitProcess(if (fail == 0) 0 else 1)
}
