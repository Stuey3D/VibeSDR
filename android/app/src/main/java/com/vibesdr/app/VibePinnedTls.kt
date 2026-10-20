package com.vibesdr.app

import okhttp3.OkHttpClient
import okhttp3.internal.tls.OkHostnameVerifier
import okio.ByteString.Companion.toByteString
import java.net.InetSocketAddress
import java.net.Socket
import java.security.KeyStore
import java.security.MessageDigest
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.HostnameVerifier
import javax.net.ssl.SNIHostName
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLEngine
import javax.net.ssl.SSLSession
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.TrustManager
import javax.net.ssl.TrustManagerFactory
import javax.net.ssl.X509ExtendedTrustManager
import javax.net.ssl.X509TrustManager

/**
 * ★★★ PER-SERVER CERTIFICATE PINS — TRUST ON FIRST USE (GitHub #52).
 *
 * ★★★ THE FAULT. casm's OpenWebRX+ served https on 8073 with a self-signed certificate, and the app could not reach
 *     it at all: the platform refuses a certificate no recognised authority signed, and it is right to. A browser (or
 *     SSH) lets the person look at THAT certificate and trust it for THAT server. This does the same: the user is shown
 *     the SHA-256 fingerprint, and on "Trust this server" that exact leaf is pinned to that host:port and nothing else.
 *
 * ★★★ NEVER WEAKER THAN THE PLATFORM (VibeTls.kt's rule). The system's trust manager decides first, exactly as OkHttp's
 *     default would (the socket-aware call, so a network security config still applies); OkHttp's own host-name
 *     verifier decides first too. Only when the SYSTEM refuses is the pin consulted, only for the host:port the
 *     handshake is for, and only an exact leaf match that is in date passes. No pin → the system's own exception is
 *     thrown, unchanged. Nothing trusts all, skips host-name checks for anyone else, or touches the system store.
 *     ★ A certificate the system trusts is accepted even where a pin exists — that is no weaker than the platform, and
 *       it is what happens when an owner moves from the self-signed pair to Let's Encrypt.
 *     ★ An expired pinned certificate is refused: trust on first use replaces "who signed it", never "is it current".
 *
 * ★★ WHERE IT IS APPLIED (VibeTlsTrustModule.install, before React Native starts): React Native's own seams —
 *    WebSocketModule.setCustomClientBuilder and NetworkingModule.setCustomClientBuilder — so its sockets and fetch keep
 *    every other setting of their own; plus the native spectrum socket (VibeSpecSocketModule) and the native audio
 *    sockets (VibeStreamService). ✗ NOT OkHttpClientProvider.setOkHttpClientFactory: that would replace RN's clients
 *    wholesale (their cache, and the separate dispatchers that keep long-lived sockets out of fetch's per-host limit).
 *
 * ★ The Apple twin is modules/vibe-tls-trust/VibeTlsCore.m — the same key ("host:port", lower-case, no IPv6 brackets),
 *   the same fingerprint (SHA-256 of the leaf's DER, lower-case hex), the same reasons for the prompt.
 * ★ No android.* imports — scripts/test-tls-pin-android.sh compiles THIS file on a desktop JVM, with the real OkHttp,
 *   and runs it against self-signed servers on 127.0.0.1.
 */
object VibePinnedTls {

    data class Pin(val sha256: String, val subject: String, val at: Long)

    /** Where pins live between runs (SharedPreferences in the app, a map in the test). */
    interface Store {
        fun load(): Map<String, Pin>
        fun save(pins: Map<String, Pin>)
    }

    @Volatile private var store: Store? = null
    @Volatile private var pins: Map<String, Pin> = emptyMap()
    private val lock = Any()

    @JvmStatic fun init(s: Store) {
        synchronized(lock) {
            store = s
            pins = try { s.load().filterValues { it.sha256.length == 64 } } catch (_: Throwable) { emptyMap() }
        }
    }

    /** "host:port" — host lower-cased, IPv6 brackets and a trailing dot removed. src/utils/tlsPin.ts pinKey() is the same. */
    @JvmStatic fun key(host: String, port: Int): String {
        var h = host.trim().lowercase()
        if (h.startsWith("[") && h.endsWith("]")) h = h.substring(1, h.length - 1)
        h = h.trimEnd('.')
        return "$h:$port"
    }

    @JvmStatic fun sha256(cert: X509Certificate): String =
        MessageDigest.getInstance("SHA-256").digest(cert.encoded).joinToString("") { "%02x".format(it) }

    @JvmStatic fun pinFor(host: String, port: Int): String? = pins[key(host, port)]?.sha256

    @JvmStatic fun all(): Map<String, Pin> = pins

    @JvmStatic fun pin(host: String, port: Int, sha256: String, subject: String) {
        val fp = sha256.lowercase()
        require(fp.length == 64 && fp.all { it in '0'..'9' || it in 'a'..'f' }) { "a SHA-256 fingerprint is 64 hex characters" }
        synchronized(lock) {
            pins = pins + (key(host, port) to Pin(fp, subject, System.currentTimeMillis()))
            store?.save(pins)
        }
    }

    @JvmStatic fun forget(host: String, port: Int): Boolean = synchronized(lock) {
        val k = key(host, port)
        if (k !in pins) return false
        pins = pins - k
        store?.save(pins)
        true
    }

    /** ★ The pin's whole test: this host:port has one, the leaf IS it, and the leaf is in date. */
    @JvmStatic fun pinnedAccepts(leaf: X509Certificate?, host: String?, port: Int): Boolean {
        if (leaf == null || host.isNullOrEmpty() || port <= 0) return false
        val pin = pinFor(host, port) ?: return false
        if (sha256(leaf) != pin) return false
        return try { leaf.checkValidity(); true } catch (_: Exception) { false }
    }

    // ── enforcement ──────────────────────────────────────────────────────────────────────────────────────────────

    @JvmStatic fun systemTrustManager(): X509TrustManager {
        val tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
        tmf.init(null as KeyStore?)
        return tmf.trustManagers.filterIsInstance<X509TrustManager>().first()
    }

    /** The system's trust manager, with this host:port's pin as the only second chance. */
    @JvmStatic val trustManager: X509ExtendedTrustManager by lazy { PinnedTrustManager(systemTrustManager()) }

    @JvmStatic val socketFactory: SSLSocketFactory by lazy {
        SSLContext.getInstance("TLS").apply { init(null, arrayOf<TrustManager>(trustManager), null) }.socketFactory
    }

    @JvmStatic val hostnameVerifier: HostnameVerifier by lazy { PinnedHostnameVerifier(OkHostnameVerifier) }

    /** Adds the pins to a client — and changes nothing else about it. */
    @JvmStatic fun applyTo(b: OkHttpClient.Builder): OkHttpClient.Builder =
        b.sslSocketFactory(socketFactory, trustManager).hostnameVerifier(hostnameVerifier)

    /**
     * ★ The handshake tells us the host and port through the socket (or engine) — the same route Android's own
     *   RootTrustManager uses to find a domain's network security config. The two-argument form has no host, so it
     *   gets the system's answer and nothing else.
     */
    class PinnedTrustManager(private val system: X509TrustManager) : X509ExtendedTrustManager() {
        override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String, socket: Socket?) {
            try {
                if (system is X509ExtendedTrustManager) system.checkServerTrusted(chain, authType, socket)
                else system.checkServerTrusted(chain, authType)
            } catch (e: CertificateException) {
                val s = (socket as? SSLSocket)?.handshakeSession
                if (pinnedAccepts(chain.firstOrNull(), s?.peerHost, s?.peerPort ?: -1)) return
                throw e
            }
        }
        override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String, engine: SSLEngine?) {
            try {
                if (system is X509ExtendedTrustManager) system.checkServerTrusted(chain, authType, engine)
                else system.checkServerTrusted(chain, authType)
            } catch (e: CertificateException) {
                if (pinnedAccepts(chain.firstOrNull(), engine?.peerHost, engine?.peerPort ?: -1)) return
                throw e
            }
        }
        override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) =
            system.checkServerTrusted(chain, authType)

        override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String, socket: Socket?) =
            if (system is X509ExtendedTrustManager) system.checkClientTrusted(chain, authType, socket)
            else system.checkClientTrusted(chain, authType)
        override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String, engine: SSLEngine?) =
            if (system is X509ExtendedTrustManager) system.checkClientTrusted(chain, authType, engine)
            else system.checkClientTrusted(chain, authType)
        override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) =
            system.checkClientTrusted(chain, authType)
        override fun getAcceptedIssuers(): Array<X509Certificate> = system.acceptedIssuers
    }

    /** OkHttp's own verifier first; a pinned leaf for this exact host:port second (a self-signed certificate rarely
     *  names the address it is reached by — the one OWRX+ owners make with `openssl req -x509` names "openwebrx"). */
    class PinnedHostnameVerifier(private val base: HostnameVerifier) : HostnameVerifier {
        override fun verify(hostname: String, session: SSLSession): Boolean {
            if (base.verify(hostname, session)) return true
            val leaf = try { session.peerCertificates.firstOrNull() as? X509Certificate } catch (_: Exception) { null }
            return pinnedAccepts(leaf, hostname, session.peerPort)
        }
    }

    // ── inspection, for the prompt only ──────────────────────────────────────────────────────────────────────────

    /**
     * ★ WHAT THE PLATFORM MAKES OF host:port's CERTIFICATE. A TLS handshake whose trust manager records the chain and
     *   then ABORTS — so no request, no cookie, nothing of ours ever reaches the server, whatever it presents. Never
     *   used to decide a live connection; only to fill the prompt.
     *   Returns: systemTrusted, reason (ok | untrusted | expired | hostname | unreachable), sha256, der (base64),
     *   pinned (the stored pin or null), pinMatches, message.
     */
    @JvmStatic fun inspect(host: String, port: Int, timeoutMs: Int = 8000): Map<String, Any?> {
        var chain: Array<X509Certificate>? = null
        var auth = "RSA"
        val capture = object : X509ExtendedTrustManager() {
            private fun grab(c: Array<X509Certificate>, a: String): Nothing {
                chain = c; auth = a; throw CertificateException("vibe-tls-probe: certificate captured, handshake aborted")
            }
            override fun checkServerTrusted(c: Array<X509Certificate>, a: String, s: Socket?) = grab(c, a)
            override fun checkServerTrusted(c: Array<X509Certificate>, a: String, e: SSLEngine?) = grab(c, a)
            override fun checkServerTrusted(c: Array<X509Certificate>, a: String) = grab(c, a)
            override fun checkClientTrusted(c: Array<X509Certificate>, a: String, s: Socket?) = throw CertificateException()
            override fun checkClientTrusted(c: Array<X509Certificate>, a: String, e: SSLEngine?) = throw CertificateException()
            override fun checkClientTrusted(c: Array<X509Certificate>, a: String) = throw CertificateException()
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
        }
        var message = ""
        try {
            val ctx = SSLContext.getInstance("TLS").apply { init(null, arrayOf<TrustManager>(capture), null) }
            val raw = Socket().apply { soTimeout = timeoutMs; connect(InetSocketAddress(host, port), timeoutMs) }
            (ctx.socketFactory.createSocket(raw, host, port, true) as SSLSocket).use { s ->
                if (!host.contains(':') && !host.all { it.isDigit() || it == '.' }) {
                    try { s.sslParameters = s.sslParameters.apply { serverNames = listOf(SNIHostName(host)) } } catch (_: Throwable) { }
                }
                s.startHandshake()
            }
        } catch (e: Exception) { message = e.message ?: e.toString() }
        val c = chain
        val leaf = c?.firstOrNull()
            ?: return mapOf("systemTrusted" to false, "reason" to "unreachable", "sha256" to "", "der" to "",
                            "pinned" to pinFor(host, port), "pinMatches" to false, "message" to message)
        val chainOk = try { systemTrustManager().checkServerTrusted(c, auth); true } catch (_: Exception) { false }
        val nameOk = OkHostnameVerifier.verify(host, leaf)
        val inDate = try { leaf.checkValidity(); true } catch (_: Exception) { false }
        val fp = sha256(leaf)
        val pin = pinFor(host, port)
        val reason = when {
            chainOk && nameOk -> "ok"
            !inDate -> "expired"
            chainOk -> "hostname"
            else -> "untrusted"
        }
        return mapOf(
            "systemTrusted" to (chainOk && nameOk), "reason" to reason, "sha256" to fp,
            "der" to leaf.encoded.toByteString().base64(),   // ★ okio: java.util.Base64 is API 26
            "pinned" to pin, "pinMatches" to (pin == fp), "message" to "",
        )
    }
}
