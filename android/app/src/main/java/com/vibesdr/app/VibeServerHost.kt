package com.vibesdr.app

import android.app.Service
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Binder
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Parcel
import android.util.Log
import org.json.JSONObject
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * ★★★ THE ONE DOOR BETWEEN THE APP'S UI AND ITS SERVER (beta88, 2026-11-20).
 *
 * The hosted VibeServer lives in "<package>:server" (see VibeProcess for why). Everything the UI used to do to the server
 * by calling VibeLocalSDR / VibeServerRestore / VibeTunnel directly — start, stop, status, the live setters, the public
 * listing, the Icom's audio — now goes through this bound service, which runs in the server's process and hands each
 * request to [VibeServerOps]. The UI never touches the server's native engine: it has its own copy of the library for
 * Local Hardware listening, and the two must not be confused.
 *
 * ★★ ONE METHOD, NOT AN AIDL INTERFACE PER CALL. A request is (method, JSON) and the answer is JSON — the same shape the
 *    HTTP config API uses, so the operations stay plain functions that are easy to read, log and test, and a new one is a
 *    `when` branch rather than a new interface + stub + proxy. Raw Binder.transact needs no AIDL build step.
 * ★★ NOT EXPORTED, AND CHECKED: android:exported="false" already confines binding to this app's uid; onTransact checks
 *    the calling uid as well, so a future manifest edit cannot quietly open the server's controls to other apps.
 * ★ The server PUSHES to the UI the same way: the UI registers a listener Binder (TX_LISTEN) and the server calls it
 *   one-way (EV_EVENT) — "the settings changed on the /setup page", so both views always show the current state.
 * ★ Works in ONE process too (Lite has no split): bindService hands back the very Binder object, and transact() calls
 *   onTransact() directly. One code path, two layouts.
 */
class VibeServerHost : Service() {

    private val binder = object : Binder() {
        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            if (code == INTERFACE_TRANSACTION) { reply?.writeString(DESCRIPTOR); return true }
            if (Binder.getCallingUid() != android.os.Process.myUid()) {
                Log.w(TAG, "refused a request from uid ${Binder.getCallingUid()}")
                return false
            }
            when (code) {
                TX_CALL -> {
                    data.enforceInterface(DESCRIPTOR)
                    val method = data.readString() ?: ""
                    val args = VibeServerLink.unwrap(applicationContext, data.readString())
                    val out = try {
                        VibeServerOps.call(applicationContext, method, args)
                    } catch (t: Throwable) {
                        // ★ A throw here would cross back as a RemoteException with no words; say what happened.
                        Log.e(TAG, "$method threw", t)
                        VibeServerOps.fail("internal", "${t.javaClass.simpleName}: ${t.message ?: ""}")
                    }
                    reply?.writeNoException()
                    reply?.writeString(VibeServerLink.wrap(applicationContext, out.toString()))
                    return true
                }
                TX_LISTEN -> {
                    data.enforceInterface(DESCRIPTOR)
                    val cb = data.readStrongBinder()
                    if (cb != null) VibeServerOps.addListener(cb)
                    reply?.writeNoException()
                    return true
                }
            }
            return super.onTransact(code, data, reply, flags)
        }
    }

    override fun onBind(intent: Intent?): IBinder = binder

    companion object {
        const val DESCRIPTOR = "com.vibesdr.app.VibeServerHost"
        const val TX_CALL = IBinder.FIRST_CALL_TRANSACTION
        const val TX_LISTEN = IBinder.FIRST_CALL_TRANSACTION + 1
        const val EV_EVENT = IBinder.FIRST_CALL_TRANSACTION + 2
        private const val TAG = "VibeServerHost"
    }
}

/**
 * ★★★ THE UI'S SIDE OF THE DOOR — bind on first use, call, and let go when idle.
 *
 * ★★ RAM: binding creates the server process if it is not running. So nothing binds at app start: a status poll for a
 *    server process that is not running answers "not running" without binding (VibeServerOps.status locally), and the
 *    binding is released 60 s after the last call so an idle :server process can be reclaimed by Android like any
 *    cached process. A RUNNING server keeps its process by its own foreground service, never by this binding.
 * ★★ NEVER ON THE MAIN THREAD. The answer to bindService arrives on the main looper; waiting for it there would wait for
 *    ever. Every caller (React methods run on the native-modules thread, receivers hop to a thread) is off it, and a
 *    main-thread call is refused with a reason rather than a hang.
 */
object VibeServerLink {
    private const val TAG = "VibeServerLink"
    private const val IDLE_UNBIND_MS = 60_000L
    /** ★ Binder transactions are capped at 1 MB for the whole process; a station list can be larger. Bigger payloads
     *  go through a file in the app's private cache, named in the transaction. */
    private const val INLINE_MAX = 200 * 1024
    private const val FILE_MARK = "@vibe-ipc-file:"

    private val main = Handler(Looper.getMainLooper())
    @Volatile private var service: IBinder? = null
    @Volatile private var bound = false
    @Volatile private var connecting: CountDownLatch? = null
    @Volatile private var lastUse = 0L
    private var appCtx: Context? = null

    private val listeners = java.util.concurrent.CopyOnWriteArrayList<(String, JSONObject) -> Unit>()

    /** ★ The UI's listener Binder: the server calls it one-way with an event name and its JSON. */
    private val listenerBinder = object : Binder() {
        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            if (code != VibeServerHost.EV_EVENT) return super.onTransact(code, data, reply, flags)
            if (Binder.getCallingUid() != android.os.Process.myUid()) return false
            val type = data.readString() ?: return true
            val json = try { JSONObject(data.readString() ?: "{}") } catch (_: Throwable) { JSONObject() }
            for (l in listeners) try { l(type, json) } catch (t: Throwable) { Log.w(TAG, "listener: $t") }
            return true
        }
    }

    /** Hear the server's events (settings changed remotely, …). Registered again on every (re)bind. */
    fun addListener(l: (String, JSONObject) -> Unit) {
        listeners.add(l)
        val b = service ?: return
        worker.execute { registerListener(b) }
    }

    private val conn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, b: IBinder?) {
            service = b
            registerListener(b)
            connecting?.countDown()
        }
        override fun onServiceDisconnected(name: ComponentName?) {
            // ★ The server process died. Android rebinds on its own when it comes back; until then calls rebind.
            Log.w(TAG, "the server process went away")
            service = null
        }
        override fun onBindingDied(name: ComponentName?) {
            service = null
            unbindNow()
        }
    }

    private fun registerListener(b: IBinder?) {
        if (b == null || listeners.isEmpty()) return
        val d = Parcel.obtain(); val r = Parcel.obtain()
        try {
            d.writeInterfaceToken(VibeServerHost.DESCRIPTOR)
            d.writeStrongBinder(listenerBinder)
            b.transact(VibeServerHost.TX_LISTEN, d, r, 0)
            r.readException()
        } catch (t: Throwable) { Log.w(TAG, "could not register for server events: $t") }
        finally { d.recycle(); r.recycle() }
    }

    @Synchronized private fun binder(ctx: Context, waitMs: Long): IBinder? {
        service?.let { if (it.isBinderAlive) return it }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            // ★ Start the bind for the next caller, but do not wait for it here.
            if (!bound) bindNow(ctx)
            return null
        }
        if (!bound) bindNow(ctx)
        val l = connecting ?: CountDownLatch(1).also { connecting = it }
        try { l.await(waitMs, TimeUnit.MILLISECONDS) } catch (_: InterruptedException) {}
        if (service == null && l.count == 0L) connecting = null
        return service
    }

    private fun bindNow(ctx: Context) {
        val app = ctx.applicationContext
        appCtx = app
        connecting = CountDownLatch(1)
        bound = try {
            app.bindService(Intent(app, VibeServerHost::class.java), conn, Context.BIND_AUTO_CREATE)
        } catch (t: Throwable) { Log.w(TAG, "bindService failed: $t"); false }
        scheduleIdleUnbind()
    }

    private fun unbindNow() {
        val app = appCtx ?: return
        if (bound) try { app.unbindService(conn) } catch (_: Throwable) {}
        bound = false; service = null; connecting = null
    }

    private val idleCheck = object : Runnable {
        override fun run() {
            if (!bound) return
            val idle = android.os.SystemClock.elapsedRealtime() - lastUse
            if (idle >= IDLE_UNBIND_MS) { synchronized(this@VibeServerLink) { unbindNow() } }
            else main.postDelayed(this, IDLE_UNBIND_MS - idle + 50)
        }
    }
    private fun scheduleIdleUnbind() { main.removeCallbacks(idleCheck); main.postDelayed(idleCheck, IDLE_UNBIND_MS) }

    /**
     * Ask the server process to do `method` with `args`; the answer is VibeServerOps' JSON ({ok:true,…} or
     * {ok:false,code,message}). Blocks the calling thread — never call it on the main thread.
     */
    fun call(ctx: Context, method: String, args: JSONObject = JSONObject(), waitMs: Long = 8000): JSONObject {
        lastUse = android.os.SystemClock.elapsedRealtime()
        val b = binder(ctx, waitMs)
            ?: return VibeServerOps.fail("no_server_process",
                if (Looper.myLooper() == Looper.getMainLooper()) "the server cannot be asked from the main thread"
                else "the server process did not answer")
        val d = Parcel.obtain(); val r = Parcel.obtain()
        try {
            d.writeInterfaceToken(VibeServerHost.DESCRIPTOR)
            d.writeString(method)
            d.writeString(wrap(ctx, args.toString()))
            b.transact(VibeServerHost.TX_CALL, d, r, 0)
            r.readException()
            val s = unwrap(ctx, r.readString())
            return s
        } catch (t: Throwable) {
            Log.w(TAG, "$method: the call to the server process failed: $t")
            service = null
            return VibeServerOps.fail("ipc_failed", "the server process did not answer ($t)")
        } finally { d.recycle(); r.recycle(); lastUse = android.os.SystemClock.elapsedRealtime() }
    }

    /** Fire and forget, on a worker thread (safe from anywhere, including the main thread). */
    fun callAsync(ctx: Context, method: String, args: JSONObject = JSONObject(), then: ((JSONObject) -> Unit)? = null) {
        val app = ctx.applicationContext
        worker.execute {
            val r = call(app, method, args)
            if (r.optBoolean("ok", false) == false) Log.w(TAG, "$method: ${r.optString("message")}")
            try { then?.invoke(r) } catch (t: Throwable) { Log.w(TAG, "$method callback: $t") }
        }
    }
    private val worker = java.util.concurrent.Executors.newSingleThreadExecutor { r ->
        Thread(r, "vibe-server-link").apply { isDaemon = true }
    }

    /**
     * ★★ A RADIO WAS ATTACHED AND ANDROID OPENED THE APP FOR IT (MainActivity, in the main app and in Lite). The server's
     * half — resume after a blip, or hand a running server its radio back — is the server process's
     * (VibeServerRestore.onRadioAttached, through VibeServerOps). An RSP's driver adopts it in THIS process too: Local
     * Hardware runs its own driver here, and VibeRsp.onAttached does nothing where no RSP session is open.
     */
    fun radioAttached(ctx: Context, dev: android.hardware.usb.UsbDevice?, why: String) {
        if (dev != null && VibeRsp.isRsp(dev) && serverIsElsewhere(ctx)) try { VibeRsp.onAttached(ctx, dev) } catch (_: Throwable) {}
        val args = JSONObject().put("why", why)
        if (dev != null) args.put("vid", dev.vendorId).put("pid", dev.productId).put("deviceName", dev.deviceName)
        callAsync(ctx, "radioAttached", args)
    }
    private fun serverIsElsewhere(ctx: Context) = VibeProcess.serverIsSeparate(ctx) && !VibeProcess.isServerProcess(ctx)

    // ── Payloads bigger than a Binder transaction may carry ─────────────────────────────────────────────────────

    internal fun wrap(ctx: Context, s: String): String {
        if (s.length < INLINE_MAX) return s
        return try {
            val dir = java.io.File(ctx.cacheDir, "ipc").apply { mkdirs() }
            val f = java.io.File.createTempFile("x", ".json", dir)
            f.writeText(s)
            FILE_MARK + f.absolutePath
        } catch (t: Throwable) { Log.w(TAG, "could not stage a large payload: $t"); s }
    }

    internal fun unwrap(ctx: Context, s: String?): JSONObject {
        val raw = s ?: return JSONObject()
        val text = if (raw.startsWith(FILE_MARK)) {
            val f = java.io.File(raw.removePrefix(FILE_MARK))
            // ★ Only our own cache folder: a path from anywhere else is not one we wrote.
            val ok = try { f.canonicalPath.startsWith(java.io.File(ctx.cacheDir, "ipc").canonicalPath) } catch (_: Throwable) { false }
            if (!ok) return JSONObject()
            try { f.readText() } catch (_: Throwable) { "{}" } finally { f.delete() }
        } else raw
        return try { JSONObject(text) } catch (_: Throwable) { JSONObject() }
    }
}
