package com.vibesdr.app

import android.app.Activity
import android.content.Context
import android.hardware.input.InputManager
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.modules.core.DeviceEventManagerModule
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * ★★★ GAME CONTROLLERS IN THE APP (V12) — Android.
 *
 * This only REPORTS: the whole pad, as a 'VibePad' event, whenever something on it changes — exactly what
 * VibeGamepad.swift sends on Apple. Buttons go by POSITION (south / east / west / north: Android's BUTTON_A is the
 * bottom face button, B the right, X the left, Y the top), sticks as −1…1 with y DOWN. src/utils/padActions.ts turns
 * that into the keyboard's own keys by Stuart's scheme and src/utils/hardwareKeys.ts feeds them into the same bus as
 * the hardware keyboard — so a pad drives the app exactly like the keyboard, with the mapping in ONE place for the app
 * and the web page. The bit numbers are PAD_BIT there; keep the three in step.
 *
 * ★ Fed from MainActivity.dispatchKeyEvent / dispatchGenericMotionEvent. Nothing runs without a pad: a key or motion
 *   that is not from a game controller returns false at once and goes on as before (a keyboard, a TV remote's D-pad,
 *   a touch screen are none of ours).
 * ★★ A pad's events are CONSUMED. Left alone, Android does two things of its own with them: an unhandled BUTTON_B
 *   becomes BACK and BUTTON_A becomes a click on whatever view has focus, and an unhandled stick or hat movement is
 *   turned into D-pad KEYS (ViewRootImpl's synthetic joystick stage) — so the left stick would ALSO move the highlight
 *   while it tuned. One owner for the pad: us.
 * ★ The D-pad arrives as KEYCODE_DPAD_* keys on some pads and as the AXIS_HAT_X / HAT_Y hat on others (some send
 *   both); the two are kept apart and OR'd, so a pad that sends both still presses once.
 * ★ `typing`: while a text field has the cursor the JS side lets only enter and back through (the keyboard's rule),
 *   and back lets go of the cursor — leaveTextField.
 */
object VibeGamepad {
    // PAD_BIT (src/utils/padActions.ts)
    private const val UP = 1; private const val DOWN = 2; private const val LEFT = 4; private const val RIGHT = 8
    private const val SOUTH = 16; private const val EAST = 32; private const val WEST = 64; private const val NORTH = 128
    private const val L1 = 256; private const val R1 = 512; private const val START = 1024

    @Volatile var ctx: ReactApplicationContext? = null

    private class Pad {
        var keys = 0                      // from key events
        var hat = 0                       // from the hat axes
        var lx = 0f; var ly = 0f; var rx = 0f; var ry = 0f
        var sent: IntArray? = null        // the last report: b, lx, ly, rx, ry (quantised)
    }
    private val pads = HashMap<Int, Pad>()
    private var listening = false

    private fun isPadSource(source: Int): Boolean =
        (source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
        (source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

    private fun isPadDevice(dev: InputDevice?): Boolean = dev != null && isPadSource(dev.sources)

    private fun bitFor(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_DPAD_UP -> UP
        KeyEvent.KEYCODE_DPAD_DOWN -> DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> RIGHT
        KeyEvent.KEYCODE_BUTTON_A -> SOUTH
        KeyEvent.KEYCODE_BUTTON_B -> EAST
        KeyEvent.KEYCODE_BUTTON_X -> WEST
        KeyEvent.KEYCODE_BUTTON_Y -> NORTH
        KeyEvent.KEYCODE_BUTTON_L1 -> L1
        KeyEvent.KEYCODE_BUTTON_R1 -> R1
        KeyEvent.KEYCODE_BUTTON_START -> START
        else -> 0
    }

    /** From MainActivity.dispatchKeyEvent. True = ours, consumed. */
    fun onKey(act: Activity, e: KeyEvent): Boolean {
        val bit = bitFor(e.keyCode)
        if (bit == 0) return false
        // ★ The D-pad keys also come from keyboards and TV remotes — ours only when the DEVICE is a game controller.
        if (!isPadSource(e.source) && !isPadDevice(e.device)) return false
        if (ctx?.hasActiveReactInstance() != true) return false
        val p = pads.getOrPut(e.deviceId) { Pad() }
        when (e.action) {
            KeyEvent.ACTION_DOWN -> { if (e.repeatCount > 0) return true; p.keys = p.keys or bit }   // JS repeats a held key
            KeyEvent.ACTION_UP -> p.keys = p.keys and bit.inv()
            else -> return true
        }
        report(act, e.deviceId, p, e.device)
        return true
    }

    /** From MainActivity.dispatchGenericMotionEvent. True = ours, consumed. */
    fun onMotion(act: Activity, e: MotionEvent): Boolean {
        if ((e.source and InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) return false
        if (e.actionMasked != MotionEvent.ACTION_MOVE) return false
        if (ctx?.hasActiveReactInstance() != true) return false
        val dev = e.device
        val p = pads.getOrPut(e.deviceId) { Pad() }
        val hx = e.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y)
        p.hat = (if (hx < -0.5f) LEFT else 0) or (if (hx > 0.5f) RIGHT else 0) or
                (if (hy < -0.5f) UP else 0) or (if (hy > 0.5f) DOWN else 0)
        p.lx = axis(e, dev, MotionEvent.AXIS_X)
        p.ly = axis(e, dev, MotionEvent.AXIS_Y)
        val zr = rightStickAxes(dev)
        p.rx = if (zr != null) axis(e, dev, zr.first) else 0f
        p.ry = if (zr != null) axis(e, dev, zr.second) else 0f
        report(act, e.deviceId, p, dev)
        return true
    }

    /** The right stick: Z / RZ on most pads (Xbox, DualShock), RX / RY on some others; null if it has none. */
    private fun rightStickAxes(dev: InputDevice?): Pair<Int, Int>? {
        if (dev == null) return null
        val src = InputDevice.SOURCE_JOYSTICK
        if (dev.getMotionRange(MotionEvent.AXIS_Z, src) != null && dev.getMotionRange(MotionEvent.AXIS_RZ, src) != null)
            return Pair(MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ)
        if (dev.getMotionRange(MotionEvent.AXIS_RX, src) != null && dev.getMotionRange(MotionEvent.AXIS_RY, src) != null)
            return Pair(MotionEvent.AXIS_RX, MotionEvent.AXIS_RY)
        return null
    }

    private fun hasSticks(dev: InputDevice?): Boolean {
        if (dev == null) return false
        val src = InputDevice.SOURCE_JOYSTICK
        return dev.getMotionRange(MotionEvent.AXIS_X, src) != null && dev.getMotionRange(MotionEvent.AXIS_Y, src) != null &&
               rightStickAxes(dev) != null
    }

    /** Dead zone (the device's own "flat", at least 0.12), so a resting stick's jitter sends nothing. */
    private fun axis(e: MotionEvent, dev: InputDevice?, a: Int): Float {
        val v = e.getAxisValue(a)
        val flat = dev?.getMotionRange(a, e.source)?.flat ?: 0f
        if (!v.isFinite() || abs(v) < max(flat, 0.12f)) return 0f
        return max(-1f, min(1f, v))
    }

    private fun q(v: Float): Int = (v * 32f).roundToInt()     // quantised to 1/32 of travel, as on Apple

    private fun report(act: Activity, id: Int, p: Pad, dev: InputDevice?) {
        val b = p.keys or p.hat
        val now = intArrayOf(b, q(p.lx), q(p.ly), q(p.rx), q(p.ry))
        if (p.sent?.contentEquals(now) == true) return
        p.sent = now
        listenForRemoval(act)
        val m = Arguments.createMap()
        m.putString("id", "a$id")
        m.putBoolean("sticks", hasSticks(dev))
        m.putInt("b", b)
        m.putDouble("lx", now[1] / 32.0); m.putDouble("ly", now[2] / 32.0)
        m.putDouble("rx", now[3] / 32.0); m.putDouble("ry", now[4] / 32.0)
        m.putBoolean("typing", act.currentFocus is EditText)
        emit(m)
    }

    private fun emit(m: com.facebook.react.bridge.WritableMap) {
        val c = ctx ?: return
        if (!c.hasActiveReactInstance()) return
        try { c.getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java).emit("VibePad", m) }
        catch (t: Throwable) { android.util.Log.w("VibeGamepad", "emit: $t") }
    }

    /** A pad switched off mid-press must not leave a tune sweep running: 'gone' makes JS release what is held. */
    private fun listenForRemoval(act: Activity) {
        if (listening) return
        listening = true
        val im = act.applicationContext.getSystemService(Context.INPUT_SERVICE) as? InputManager ?: return
        im.registerInputDeviceListener(object : InputManager.InputDeviceListener {
            override fun onInputDeviceAdded(deviceId: Int) {}
            override fun onInputDeviceChanged(deviceId: Int) {}
            override fun onInputDeviceRemoved(deviceId: Int) {
                if (pads.remove(deviceId) == null) return
                val m = Arguments.createMap()
                m.putString("id", "a$deviceId")
                m.putBoolean("gone", true)
                emit(m)
            }
        }, android.os.Handler(android.os.Looper.getMainLooper()))
    }

    /** ★ Back on a pad while a field has the cursor leaves the field — one layer, as Escape does on a keyboard. */
    fun leaveTextField(act: Activity) {
        act.runOnUiThread {
            val v = act.currentFocus as? EditText ?: return@runOnUiThread
            v.clearFocus()
            (act.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager)
                ?.hideSoftInputFromWindow(v.windowToken, 0)
        }
    }
}
