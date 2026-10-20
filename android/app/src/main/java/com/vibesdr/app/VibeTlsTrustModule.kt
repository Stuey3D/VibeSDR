package com.vibesdr.app

import android.content.Context
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.modules.network.NetworkingModule
import com.facebook.react.modules.websocket.WebSocketModule
import org.json.JSONObject

/**
 * NativeModules.VibeTlsTrust — per-server certificate pins (GitHub #52). The rules are in VibePinnedTls; this is the
 * Android glue: SharedPreferences for the pins, React Native's client hooks, and the JS methods (inspect / listPins /
 * trust / forget) that src/services/tlsTrust.ts calls. iOS: modules/vibe-tls-trust/VibeTlsTrust.mm.
 */
class VibeTlsTrustModule(ctx: ReactApplicationContext) : ReactContextBaseJavaModule(ctx) {
    override fun getName() = "VibeTlsTrust"

    companion object {
        private const val PREFS = "vibe_tls_pins"
        @Volatile private var installed = false

        /**
         * ★★★ FROM Application.onCreate, BEFORE React Native starts — its client builders are read when each module
         *     makes its client, and the pins must be loaded before the first handshake (a saved default server
         *     auto-connects at launch). Idempotent.
         */
        @JvmStatic fun install(app: Context) {
            if (installed) return
            installed = true
            VibePinnedTls.init(PrefsStore(app.applicationContext))
            // ★ React Native's own seams: each adds our trust manager + host-name verifier to the builder RN made,
            //   and leaves everything else of RN's (cache, cookie jar, timeouts, dispatcher) as it was.
            WebSocketModule.setCustomClientBuilder { b -> VibePinnedTls.applyTo(b) }
            NetworkingModule.setCustomClientBuilder { b -> VibePinnedTls.applyTo(b) }
        }
    }

    /** One JSON string in SharedPreferences: { "host:port": { sha256, subject, at } }. */
    private class PrefsStore(private val ctx: Context) : VibePinnedTls.Store {
        override fun load(): Map<String, VibePinnedTls.Pin> {
            val raw = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("pins", null) ?: return emptyMap()
            val o = JSONObject(raw)
            val out = HashMap<String, VibePinnedTls.Pin>()
            for (k in o.keys()) {
                val p = o.optJSONObject(k) ?: continue
                out[k] = VibePinnedTls.Pin(p.optString("sha256"), p.optString("subject"), p.optLong("at"))
            }
            return out
        }
        override fun save(pins: Map<String, VibePinnedTls.Pin>) {
            val o = JSONObject()
            for ((k, p) in pins) o.put(k, JSONObject().put("sha256", p.sha256).put("subject", p.subject).put("at", p.at))
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString("pins", o.toString()).apply()
        }
    }

    /** ★ A handshake only, on a background thread — see VibePinnedTls.inspect. */
    @ReactMethod
    fun inspect(host: String, port: Double, promise: Promise) {
        Thread({
            try {
                val r = VibePinnedTls.inspect(host, port.toInt(), 5000)   // ★ short: it delays the error card when nothing answers
                val m = Arguments.createMap()
                m.putBoolean("systemTrusted", r["systemTrusted"] == true)
                m.putString("reason", r["reason"] as? String ?: "error")
                m.putString("sha256", r["sha256"] as? String ?: "")
                m.putString("der", r["der"] as? String ?: "")
                val pin = r["pinned"] as? String
                if (pin != null) m.putString("pinned", pin) else m.putNull("pinned")
                m.putBoolean("pinMatches", r["pinMatches"] == true)
                m.putString("message", r["message"] as? String ?: "")
                promise.resolve(m)
            } catch (t: Throwable) { promise.reject("inspect_failed", t.message ?: t.toString()) }
        }, "VibeTlsInspect").apply { isDaemon = true }.start()
    }

    @ReactMethod
    fun listPins(promise: Promise) {
        val m = Arguments.createMap()
        for ((k, p) in VibePinnedTls.all()) {
            val e = Arguments.createMap()
            e.putString("sha256", p.sha256); e.putString("subject", p.subject); e.putDouble("at", p.at.toDouble())
            m.putMap(k, e)
        }
        promise.resolve(m)
    }

    @ReactMethod
    fun trust(host: String, port: Double, sha256: String, subject: String?, promise: Promise) {
        try {
            VibePinnedTls.pin(host, port.toInt(), sha256, subject ?: "")
            android.util.Log.i("VibeTls", "trusted ${VibePinnedTls.key(host, port.toInt())} = $sha256")
            promise.resolve(true)
        } catch (t: Throwable) { promise.reject("bad_fingerprint", t.message ?: t.toString()) }
    }

    @ReactMethod
    fun forget(host: String, port: Double, promise: Promise) {
        val had = VibePinnedTls.forget(host, port.toInt())
        if (had) android.util.Log.i("VibeTls", "forgot ${VibePinnedTls.key(host, port.toInt())}")
        promise.resolve(had)
    }
}
