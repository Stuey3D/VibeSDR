package com.vibesdr.app

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.WritableMap
import com.facebook.react.modules.core.DeviceEventManagerModule

/**
 * mDNS/Bonjour discovery of networked RTL-TCP servers via Android's NsdManager.
 * Browses for `_rtl_tcp._tcp` services, resolves each to host:port, and emits
 * VibeMdnsFound / VibeMdnsLost to JS — mirroring the iOS path folded into
 * VibePowerModule. No subnet scanning: we only see servers that advertise the
 * service (e.g. an rtl_tcp host running an mDNS advertiser).
 */
class VibeMdnsModule(private val reactCtx: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactCtx) {

    companion object {
        private const val TAG = "VibeMDNS"
        private const val SERVICE_TYPE = "_rtl_tcp._tcp."
        // VibeServer advertises its own service type so a discovered server is
        // known to speak the compressed VibeServer protocol (and whether it needs
        // a PIN) before the client connects.
        private const val VIBE_SERVICE_TYPE = "_vibesdr._tcp."
        private const val RESOLVE_TIMEOUT_MS = 8000L

        private fun typeFor(proto: String) =
            if (proto == "vibeserver") VIBE_SERVICE_TYPE else SERVICE_TYPE
        private fun protoFor(serviceType: String) =
            if (serviceType.contains("vibesdr")) "vibeserver" else "rtltcp"
    }

    override fun getName() = "VibeMDNS"

    private val nsd: NsdManager? by lazy {
        reactCtx.getSystemService(Context.NSD_SERVICE) as? NsdManager
    }
    // One discovery listener per service type (NsdManager browses a single type
    // per listener), so we surface both RTL-TCP and VibeServer hosts at once.
    private val discoveryListeners = mutableListOf<NsdManager.DiscoveryListener>()

    // ★★★ A RESOLVE THAT NEVER ANSWERS MUST NOT STOP EVERY OTHER ONE (Stuart, 2026-10-20: "sometimes it wont find a
    //     server until I close and open again"). Resolves are serialised — older NsdManagers allow one at a time — and the
    //     queue moved on only when the in-flight resolve called back. Some never do (a responder that ignores the query,
    //     a network change mid-resolve, known NsdManager bugs before Android 14), and then NOTHING else was ever
    //     resolved: every server found after it stayed invisible until the app was killed. Now each resolve gets
    //     RESOLVE_TIMEOUT_MS; a timed-out or failed one is retried (twice, later) and the queue moves on at once. A late
    //     callback from a resolve we gave up on is recognised by its ticket and ignored. A discovery that fails to start
    //     is tried again once. All of this state is touched only under `lock` — the callbacks arrive on binder threads.
    private val lock = Any()
    private val handler = Handler(Looper.getMainLooper())
    private class Pending(val info: NsdServiceInfo, val attempt: Int)
    private val resolveQueue = ArrayDeque<Pending>()
    private var inFlight: Pending? = null
    private var inFlightListener: NsdManager.ResolveListener? = null
    private var ticket = 0          // bumped per resolve and on stop: a stale callback carries an old one
    private var generation = 0      // bumped on start/stop: a retry or restart scheduled before is dropped

    @ReactMethod
    fun startDiscovery() {
        val manager = nsd ?: return
        stopDiscovery()
        val gen = synchronized(lock) { ++generation }
        for (type in listOf(SERVICE_TYPE, VIBE_SERVICE_TYPE)) startOne(manager, type, gen, retryOnFail = true)
    }

    private fun startOne(manager: NsdManager, type: String, gen: Int, retryOnFail: Boolean) {
        val listener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {}
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                Log.w(TAG, "start discovery failed ($type): $errorCode")
                synchronized(lock) { discoveryListeners.remove(this) }
                if (retryOnFail) handler.postDelayed({
                    if (synchronized(lock) { generation == gen }) startOne(manager, type, gen, retryOnFail = false)
                }, 2000)
            }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            override fun onServiceFound(info: NsdServiceInfo) {
                enqueueResolve(Pending(info, 0))
            }
            override fun onServiceLost(info: NsdServiceInfo) {
                emit("VibeMdnsLost") { it.putString("name", info.serviceName) }
            }
        }
        try {
            synchronized(lock) { discoveryListeners.add(listener) }
            manager.discoverServices(type, NsdManager.PROTOCOL_DNS_SD, listener)
        } catch (e: Exception) {
            synchronized(lock) { discoveryListeners.remove(listener) }
            Log.w(TAG, "discoverServices($type) threw: ${e.message}")
        }
    }

    @ReactMethod
    fun stopDiscovery() {
        val listeners: List<NsdManager.DiscoveryListener>
        val resolving: NsdManager.ResolveListener?
        synchronized(lock) {
            generation++
            ticket++
            listeners = discoveryListeners.toList()
            discoveryListeners.clear()
            resolveQueue.clear()
            inFlight = null
            resolving = inFlightListener
            inFlightListener = null
        }
        handler.removeCallbacksAndMessages(null)
        for (listener in listeners) {
            try { nsd?.stopServiceDiscovery(listener) } catch (_: Exception) {}
        }
        if (resolving != null && Build.VERSION.SDK_INT >= 34) {
            try { nsd?.stopServiceResolution(resolving) } catch (_: Exception) {}
        }
    }

    private fun enqueueResolve(p: Pending) {
        synchronized(lock) { resolveQueue.addLast(p) }
        pumpResolve()
    }

    /** The resolve with `myTicket` is over (answered, failed or timed out); retry it if asked, then start the next. */
    private fun settle(myTicket: Int, retry: Boolean) {
        val again: Pending?
        val gen: Int
        val stuck: NsdManager.ResolveListener?
        synchronized(lock) {
            if (myTicket != ticket || inFlight == null) return      // already settled, or stopped
            again = if (retry && inFlight!!.attempt < 2) Pending(inFlight!!.info, inFlight!!.attempt + 1) else null
            stuck = if (retry) inFlightListener else null
            inFlight = null
            inFlightListener = null
            ticket++
            gen = generation
        }
        if (stuck != null && Build.VERSION.SDK_INT >= 34) {
            try { nsd?.stopServiceResolution(stuck) } catch (_: Exception) {}
        }
        if (again != null) handler.postDelayed({
            if (synchronized(lock) { generation == gen }) enqueueResolve(again)
        }, if (again.attempt == 1) 3000L else 10000L)
        pumpResolve()
    }

    private fun pumpResolve() {
        val manager = nsd ?: return
        val next: Pending
        val myTicket: Int
        synchronized(lock) {
            if (inFlight != null) return
            next = resolveQueue.removeFirstOrNull() ?: return
            inFlight = next
            myTicket = ++ticket
        }
        val listener = object : NsdManager.ResolveListener {
            override fun onResolveFailed(info: NsdServiceInfo, errorCode: Int) {
                settle(myTicket, retry = true)
            }
            override fun onServiceResolved(info: NsdServiceInfo) {
                val stale = synchronized(lock) { myTicket != ticket }
                val host = info.host?.hostAddress
                val port = info.port
                if (!stale && host != null && port > 0) {
                    emit("VibeMdnsFound") {
                        it.putString("name", friendlyName(info))
                        it.putString("host", host)
                        it.putInt("port", port)
                        it.putString("proto", protoFor(info.serviceType ?: ""))
                        it.putBoolean("pin", txt(info, "pin") == "1")
                    }
                }
                settle(myTicket, retry = host == null || port <= 0)
            }
        }
        synchronized(lock) { if (myTicket == ticket) inFlightListener = listener }
        handler.postDelayed({ settle(myTicket, retry = true) }, RESOLVE_TIMEOUT_MS)
        try {
            manager.resolveService(next.info, listener)
        } catch (e: Exception) {
            Log.w(TAG, "resolveService threw: ${e.message}")
            settle(myTicket, retry = true)
        }
    }

    /** Prefer the `name` TXT attribute; fall back to the service name. */
    private fun friendlyName(info: NsdServiceInfo): String {
        val n = txt(info, "name")
        return if (!n.isNullOrEmpty()) n else info.serviceName
    }

    /** Read a TXT attribute as UTF-8, or null. */
    private fun txt(info: NsdServiceInfo, key: String): String? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) return null
        val raw = info.attributes?.get(key) ?: return null
        return try { String(raw, Charsets.UTF_8) } catch (_: Exception) { null }
    }

    // ── Advertise (register) THIS device's RTL-TCP server via mDNS ───────────
    private var registrationListener: NsdManager.RegistrationListener? = null

    /** Advertise an `_rtl_tcp._tcp` service so clients auto-discover this server.
     *  TXT `name` carries the friendly label. Re-registers if already advertising
     *  (e.g. the user edited the name). */
    @ReactMethod
    fun advertise(name: String, port: Int, proto: String, pinRequired: Boolean, promise: Promise) {
        val manager = nsd ?: run { promise.reject("no_nsd", "NSD unavailable"); return }
        stopAdvertiseInternal()
        val info = NsdServiceInfo().apply {
            serviceName = name
            serviceType = typeFor(proto)
            this.port = port
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                setAttribute("name", name)
                setAttribute("proto", proto)
                setAttribute("pin", if (pinRequired) "1" else "0")
            }
        }
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {}
            override fun onRegistrationFailed(info: NsdServiceInfo, errorCode: Int) {
                Log.w(TAG, "advertise failed: $errorCode")
            }
            override fun onServiceUnregistered(info: NsdServiceInfo) {}
            override fun onUnregistrationFailed(info: NsdServiceInfo, errorCode: Int) {}
        }
        registrationListener = listener
        try {
            manager.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
            promise.resolve(null)
        } catch (e: Exception) {
            registrationListener = null
            promise.reject("advertise_threw", e.message)
        }
    }

    @ReactMethod
    fun stopAdvertise() { stopAdvertiseInternal() }

    private fun stopAdvertiseInternal() {
        val listener = registrationListener ?: return
        registrationListener = null
        try { nsd?.unregisterService(listener) } catch (_: Exception) {}
    }

    private fun emit(name: String, fill: (WritableMap) -> Unit) {
        try {
            val map = Arguments.createMap()
            fill(map)
            reactCtx.getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java)
                .emit(name, map)
        } catch (e: Exception) {
            Log.w(TAG, "emit $name failed: ${e.message}")
        }
    }

    // NativeEventEmitter housekeeping (events arrive via RCTDeviceEventEmitter)
    @ReactMethod fun addListener(eventName: String) {}
    @ReactMethod fun removeListeners(count: Double) {}
}
