// PinnedTlsTest.kt — VibePinnedTls.kt (the file the APK compiles) with the REAL OkHttp against self-signed servers on
// 127.0.0.1 (tls-test-server.mjs). Run by scripts/test-tls-pin-android.sh. GitHub #52.
// Same cases as the Apple test (scripts/tls/VibeTlsPinTest.m), plus a WebSocket upgrade through the pinned client.
import com.vibesdr.app.VibePinnedTls
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

var bad = 0
fun check(cond: Boolean, what: String) { if (cond) println("  ok   $what") else { bad++; println("  FAIL $what") } }

class MemStore : VibePinnedTls.Store {
    var saved: Map<String, VibePinnedTls.Pin> = emptyMap()
    override fun load() = saved
    override fun save(pins: Map<String, VibePinnedTls.Pin>) { saved = pins }
}

fun client(): OkHttpClient = VibePinnedTls.applyTo(OkHttpClient.Builder().connectTimeout(5, TimeUnit.SECONDS)).build()

fun get(port: Int): String? = try {
    client().newCall(Request.Builder().url("https://localhost:$port/").build()).execute().use { r -> if (r.isSuccessful) r.body?.string() else null }
} catch (e: Exception) { null }

fun ws(port: Int): String? {
    val got = arrayOfNulls<String>(1)
    val done = CountDownLatch(1)
    client().newWebSocket(Request.Builder().url("wss://localhost:$port/ws/").build(), object : WebSocketListener() {
        override fun onMessage(webSocket: WebSocket, text: String) { got[0] = text; done.countDown(); webSocket.cancel() }
        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) { done.countDown() }
    })
    done.await(8, TimeUnit.SECONDS)
    return got[0]
}

fun main(args: Array<String>) {
    val (pSelf, pLh, pEx) = Triple(args[0].toInt(), args[2].toInt(), args[4].toInt())
    val (fpSelf, fpLh, fpEx) = Triple(args[1], args[3], args[5])
    val log = File(args[6])
    val store = MemStore()
    VibePinnedTls.init(store)

    println("keys")
    check(VibePinnedTls.key("SDR.Example.org.", 8073) == "sdr.example.org:8073", "host lower-cased, trailing dot dropped")
    check(VibePinnedTls.key("[fe80::1]", 443) == "fe80::1:443", "IPv6 brackets dropped")

    println("self-signed CN=openwebrx (OWRX+-style, 10 years, CA:TRUE), no pin")
    var r = VibePinnedTls.inspect("localhost", pSelf)
    check(r["reason"] == "untrusted", "inspect says untrusted")
    check(r["systemTrusted"] == false, "inspect: not system-trusted")
    check(r["sha256"] == fpSelf, "inspect fingerprint = openssl's")
    check(r["pinned"] == null, "inspect: no pin")
    check(get(pSelf) == null, "GET refused without a pin")
    check(ws(pSelf) == null, "WebSocket refused without a pin")
    check(!log.readText().contains("REQ $pSelf") && !log.readText().contains("UPGRADE $pSelf"),
          "the server never saw a request or an upgrade (inspect or refused connection)")

    println("pin it")
    VibePinnedTls.pin("localhost", pSelf, fpSelf, "openwebrx")
    check(get(pSelf)?.contains("openwebrx") == true, "GET accepted with the pin (name 'openwebrx' ≠ 'localhost': the pin covers it)")
    check(ws(pSelf)?.startsWith("CLIENT DE SERVER") == true, "WebSocket accepted with the pin, server hello received")
    check(VibePinnedTls.inspect("localhost", pSelf)["pinMatches"] == true, "inspect: pin matches")

    println("the same pin does not cover another port")
    check(get(pLh) == null, "other port refused (a pin is host:port, not host)")

    println("certificate changed")
    VibePinnedTls.pin("localhost", pLh, fpSelf, "openwebrx")
    check(get(pLh) == null, "a different certificate is refused")
    r = VibePinnedTls.inspect("localhost", pLh)
    check(r["pinMatches"] == false && r["sha256"] == fpLh, "inspect: new fingerprint, pin does not match")

    println("expired, even when pinned")
    VibePinnedTls.pin("localhost", pEx, fpEx, "localhost")
    check(get(pEx) == null, "an expired pinned certificate is refused")
    check(VibePinnedTls.inspect("localhost", pEx)["reason"] == "expired", "inspect says expired")

    println("restart")
    VibePinnedTls.init(MemStore().also { it.saved = store.saved })
    check(VibePinnedTls.pinFor("LOCALHOST", pSelf) == fpSelf, "pin survives a reload")
    check(get(pSelf) != null, "and still connects")

    println("forget")
    check(VibePinnedTls.forget("localhost", pSelf), "forget returns true")
    check(get(pSelf) == null, "refused again after forget")

    println("nothing listening")
    check(VibePinnedTls.inspect("localhost", 1, 2000)["reason"] == "unreachable", "inspect: unreachable")

    r = VibePinnedTls.inspect("vibesdr.net", 443)
    if (r["reason"] == "unreachable") println("  (offline — public-CA cases not run)")
    else {
        println("public CA (network)")
        check(r["systemTrusted"] == true && r["reason"] == "ok", "vibesdr.net: system-trusted")
        check(client().newCall(Request.Builder().url("https://vibesdr.net/").build()).execute().use { it.code in 200..499 },
              "vibesdr.net: a real request through the pinned client still works")
        val w = VibePinnedTls.inspect("wrong.host.badssl.com", 443)
        if (w["reason"] != "unreachable") check(w["reason"] == "hostname", "wrong.host.badssl.com: hostname mismatch, not offered")
    }
    println(if (bad > 0) "\n$bad FAILED" else "\nall passed")
    kotlin.system.exitProcess(if (bad > 0) 1 else 0)
}
