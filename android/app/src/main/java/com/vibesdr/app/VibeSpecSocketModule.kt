package com.vibesdr.app

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.bridge.ReadableType
import com.facebook.react.modules.core.DeviceEventManagerModule
import com.facebook.react.modules.network.ForwardingCookieHandler
import com.facebook.react.modules.network.OkHttpClientProvider
import okhttp3.CookieJar
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import java.net.URI
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit

/**
 * ★★★ THE SPECTRUM SOCKET, NATIVE (docs/NATIVE-SPECTRUM-ENGINE.md, stage 2). React Native's WebSocket delivers every
 * message on the JS thread, so the waterfall could only be fed when JS got round to it — and on the XCover's DAB list
 * it "renders smoothly then crawls" (Stuart, 2026-10-12). This socket lands every message in the native sink
 * (cpp/spectrum/spec_socket.cpp) on OkHttp's thread: spectrum frames go into the waterfall engine AT ONCE, and every
 * message is queued, in order, for JS to drain (src/services/nativeSpecSocket.ts) — the client logic is unchanged.
 *
 * ★★ CONFIGURED EXACTLY AS React Native's WebSocketModule (node_modules/react-native/.../websocket/WebSocketModule.kt):
 *    OkHttpClientProvider's client, NO_COOKIES jar, connect/write 10 s, read 0, the Cookie header from the forwarding
 *    cookie handler, the caller's headers (our User-Agent), the default `origin`, a server close echoed back, failures
 *    reported with their message. Receivers that are not ours — UberSDR operators write rules against what we send —
 *    must see the same handshake as before. (VibeTls is deliberately NOT applied: RN's socket never had it. The
 *    per-server pins ARE — RN's socket has them through WebSocketModule.setCustomClientBuilder; see VibePinnedTls.)
 */
class VibeSpecSocketModule(private val ctx: ReactApplicationContext) : ReactContextBaseJavaModule(ctx) {
    override fun getName() = "VibeSpecSocket"

    private val sockets = ConcurrentHashMap<Int, WebSocket>()
    private val cookieHandler = ForwardingCookieHandler()

    private fun emit(id: Int, type: String, fill: (com.facebook.react.bridge.WritableMap) -> Unit = {}) {
        val m = Arguments.createMap()
        m.putInt("id", id); m.putString("type", type); fill(m)
        try {
            ctx.getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java).emit("vibeSpecSocket", m)
        } catch (_: Throwable) { /* JS gone (reload / teardown) */ }
    }

    @ReactMethod
    fun connect(idD: Double, url: String, headers: ReadableMap?, vibeServer: Boolean) {
        val id = idD.toInt()
        android.util.Log.i("VibeSpecSocket", "#$id native spectrum socket (${if (vibeServer) "VibeServer" else "UberSDR"})")
        try {
            VibeSpectrum.nativeSinkCreate(id, vibeServer)
        } catch (t: Throwable) {
            emit(id, "failed") { it.putString("message", "native spectrum socket unavailable: $t") }; return
        }
        val client = OkHttpClientProvider.getOkHttpClient()
            .newBuilder()
            .cookieJar(CookieJar.NO_COOKIES)
            .connectTimeout(10, TimeUnit.SECONDS)
            .writeTimeout(10, TimeUnit.SECONDS)
            .readTimeout(0, TimeUnit.MINUTES)
            .also { VibePinnedTls.applyTo(it) }   // ★ as RN's socket now does (WebSocketModule's custom builder, GitHub #52)
            .build()
        val b = Request.Builder().tag(id).url(url)
        cookieFor(url)?.let { b.addHeader("Cookie", it) }
        var hasOrigin = false
        if (headers != null) {
            val it = headers.keySetIterator()
            while (it.hasNextKey()) {
                val k = it.nextKey()
                if (headers.getType(k) == ReadableType.String) {
                    if (k.equals("origin", ignoreCase = true)) hasOrigin = true
                    b.addHeader(k, headers.getString(k)!!)
                }
            }
        }
        if (!hasOrigin) b.addHeader("origin", defaultOrigin(url))
        // ★ Registered at once (as RN does), so a close() while still connecting cancels the handshake.
        sockets[id] = client.newWebSocket(b.build(), object : WebSocketListener() {
            override fun onOpen(ws: WebSocket, response: Response) {
                emit(id, "open") { it.putString("protocol", response.header("Sec-WebSocket-Protocol", "")) }
            }
            override fun onMessage(ws: WebSocket, text: String) {
                if (VibeSpectrum.nativeSinkText(id, text.toByteArray(Charsets.UTF_8))) emit(id, "pending")
            }
            override fun onMessage(ws: WebSocket, bytes: ByteString) {
                if (VibeSpectrum.nativeSinkBinary(id, bytes.toByteArray())) emit(id, "pending")
            }
            override fun onClosing(ws: WebSocket, code: Int, reason: String) { ws.close(code, reason) }
            override fun onClosed(ws: WebSocket, code: Int, reason: String) {
                emit(id, "closed") { it.putInt("code", code); it.putString("reason", reason) }
            }
            override fun onFailure(ws: WebSocket, t: Throwable, response: Response?) {
                emit(id, "failed") { it.putString("message", t.message ?: "") }
            }
        })
    }

    @ReactMethod
    fun send(idD: Double, text: String) {
        val id = idD.toInt()
        val ws = sockets[id]
        if (ws == null) { emit(id, "failed") { it.putString("message", "client is null") }; return }
        try { ws.send(text) } catch (t: Throwable) { emit(id, "failed") { it.putString("message", t.message ?: "") } }
    }

    @ReactMethod
    fun close(idD: Double, code: Double, reason: String) {
        val id = idD.toInt()
        val ws = sockets.remove(id) ?: return
        try { ws.close(code.toInt(), reason) } catch (_: Throwable) { }
    }

    /** JS is done with this socket (after its close and the last drain): free the native sink. */
    @ReactMethod
    fun destroy(idD: Double) {
        val id = idD.toInt()
        sockets.remove(id)?.cancel()
        try { VibeSpectrum.nativeSinkDestroy(id) } catch (_: Throwable) { }
    }

    @ReactMethod fun addListener(eventName: String) {}
    @ReactMethod fun removeListeners(count: Double) {}

    private fun cookieFor(url: String): String? = try {
        val m = cookieHandler.get(URI(defaultOrigin(url)), HashMap<String, List<String>>())
        m["Cookie"]?.firstOrNull()
    } catch (_: Throwable) { null }

    private fun defaultOrigin(uri: String): String = try {
        val u = URI(uri)
        val scheme = when (u.scheme) { "wss" -> "https"; "ws" -> "http"; "http", "https" -> u.scheme; else -> "" }
        if (u.port != -1) "$scheme://${u.host}:${u.port}" else "$scheme://${u.host}"
    } catch (_: Throwable) { "" }
}
