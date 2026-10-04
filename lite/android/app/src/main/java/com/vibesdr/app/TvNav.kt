package com.vibesdr.app

import android.app.Activity
import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.drawable.Drawable
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.InputMethodManager
import android.widget.CompoundButton
import android.widget.EditText
import android.widget.ScrollView
import android.widget.SeekBar
import com.facebook.react.views.textinput.ReactEditText
import java.lang.ref.WeakReference

/**
 * ★★ ANDROID TV REMOTE NAVIGATION (Stuart, 2026-09-19, on a Sony XE85): "Navigation needs to be large green
 * highlight bar and then left and right moves the smaller highlight over the control with the enter button
 * selecting it."
 *
 * Android's own focus search is geometric and wandered on this page (LEFT from the LSB chip landed on the gain
 * slider BELOW it). So on a TV this owns the D-pad instead, with a model a remote can drive:
 *  - the page's controls are grouped into ROWS by their position on screen (every control on one horizontal line);
 *  - UP/DOWN move a full-width green BAR from row to row and scroll it into view;
 *  - LEFT/RIGHT move a smaller highlight along the row — or, on a lone slider, move the slider;
 *  - OK presses it: click a button, flip a switch, open the keyboard on a text field.
 * ★★ AND ON ANY DEVICE THE MOMENT A REMOTE, KEYBOARD OR CONTROLLER IS USED (Stuart, 2026-10-04: "put it in remote
 * mode if ANYTHING other than a touch screen interacts"). MainActivity builds this everywhere: the first arrow /
 * D-pad / controller key shows the bar, the next real touch hides it (hide()). On a phone that is never touched by
 * a key, nothing ever appears. While a text field is being TYPED in, LEFT / RIGHT / ENTER stay with the text (cursor,
 * submit) and only UP / DOWN leave it — the original trap, without stealing the keyboard.
 */
class TvNav(private val act: Activity) {
    private var cur: WeakReference<View>? = null
    private var row: List<WeakReference<View>> = emptyList()
    private var attached = false

    private val hl = object : Drawable() {
        private val barFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x2622C55E }
        private val barEdge = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF22C55E.toInt(); style = Paint.Style.STROKE; strokeWidth = 4f }
        private val ctlFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x4D4ADE80 }
        private val ctlEdge = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFB9F6CA.toInt(); style = Paint.Style.STROKE; strokeWidth = 6f }
        override fun draw(c: Canvas) {
            val v = cur?.get() ?: return
            if (!v.isAttachedToWindow || !v.isShown) return
            var top = Int.MAX_VALUE; var bottom = Int.MIN_VALUE
            for (w in row) { val r = w.get()?.let { rectOf(it) } ?: continue; top = minOf(top, r.top); bottom = maxOf(bottom, r.bottom) }
            if (top == Int.MAX_VALUE) return
            val bar = RectF(12f, top - 16f, bounds.width() - 12f, bottom + 16f)
            c.drawRoundRect(bar, 18f, 18f, barFill); c.drawRoundRect(bar, 18f, 18f, barEdge)
            val r = rectOf(v)
            val ctl = RectF(r.left - 8f, r.top - 8f, r.right + 8f, r.bottom + 8f)
            c.drawRoundRect(ctl, 14f, 14f, ctlFill); c.drawRoundRect(ctl, 14f, 14f, ctlEdge)
        }
        override fun setAlpha(a: Int) {}
        override fun setColorFilter(f: ColorFilter?) {}
        @Suppress("OVERRIDE_DEPRECATION") override fun getOpacity() = PixelFormat.TRANSLUCENT
    }

    private fun root() = act.window.decorView as ViewGroup

    private fun attach() {
        if (attached) return
        val r = root()
        hl.setBounds(0, 0, r.width, r.height)
        r.overlay.add(hl)
        // Scrolling and re-layout move the controls under a still highlight — repaint on both.
        r.viewTreeObserver.addOnScrollChangedListener { hl.invalidateSelf() }
        r.viewTreeObserver.addOnGlobalLayoutListener { hl.setBounds(0, 0, r.width, r.height); hl.invalidateSelf() }
        attached = true
    }

    private fun rectOf(v: View): Rect {
        val p = IntArray(2); v.getLocationInWindow(p)
        return Rect(p[0], p[1], p[0] + v.width, p[1] + v.height)
    }

    /** Every control a remote can press, in page order. A pressable container is taken whole — its text is not a stop. */
    private fun collect(v: View, out: MutableList<View>) {
        if (v.visibility != View.VISIBLE || v.alpha == 0f) return
        val control = v is EditText || v is SeekBar || v is CompoundButton ||
            (v.isClickable && v.isFocusable && v !is ScrollView)
        if (control) { if (v.width > 0 && v.height > 0 && v.isEnabled) out.add(v); return }
        if (v is ViewGroup) for (i in 0 until v.childCount) collect(v.getChildAt(i), out)
    }

    /** Controls grouped into rows: one row = every control whose middle sits within the row's height. */
    private fun rows(): List<List<View>> {
        val all = mutableListOf<View>(); collect(root(), all)
        val withR = all.map { it to rectOf(it) }.sortedBy { it.second.top }
        val out = mutableListOf<MutableList<Pair<View, Rect>>>()
        var top = 0; var bottom = 0
        for (p in withR) {
            val cy = p.second.centerY()
            if (out.isNotEmpty() && cy in top..bottom) { out.last().add(p); bottom = maxOf(bottom, p.second.bottom) }
            else { out.add(mutableListOf(p)); top = p.second.top; bottom = p.second.bottom }
        }
        return out.map { r -> r.sortedBy { it.second.left }.map { it.first } }
    }

    private fun select(v: View, inRow: List<View>) {
        val prev = cur?.get()
        if (prev is EditText && prev !== v && prev.isFocused) {
            prev.clearFocus()
            (act.getSystemService(Activity.INPUT_METHOD_SERVICE) as InputMethodManager).hideSoftInputFromWindow(prev.windowToken, 0)
        }
        cur = WeakReference(v); row = inRow.map { WeakReference(it) }
        // Bring the row into view with room above for its label.
        v.requestRectangleOnScreen(Rect(0, -140, v.width, v.height + 60), false)
        hl.invalidateSelf()
    }

    /** A touch: back to touch mode — the bar and highlight go until the next key. */
    fun hide() {
        if (cur == null) return
        cur = null; row = emptyList(); hl.invalidateSelf()
    }

    fun handle(e: KeyEvent): Boolean {
        val kc = e.keyCode
        val up = kc == KeyEvent.KEYCODE_DPAD_UP; val down = kc == KeyEvent.KEYCODE_DPAD_DOWN
        val left = kc == KeyEvent.KEYCODE_DPAD_LEFT; val right = kc == KeyEvent.KEYCODE_DPAD_RIGHT
        // ★ A controller's A is OK, as on every Android TV launcher.
        val ok = kc == KeyEvent.KEYCODE_DPAD_CENTER || kc == KeyEvent.KEYCODE_ENTER || kc == KeyEvent.KEYCODE_NUMPAD_ENTER
            || kc == KeyEvent.KEYCODE_BUTTON_A
        if (!(up || down || left || right || ok)) return false
        // ★ Typing in a field: the cursor keys and Enter are the TEXT's (a keyboard user editing); UP / DOWN leave.
        val typing = act.currentFocus.let { it is EditText && it.isFocused }
        if (typing && (left || right || kc == KeyEvent.KEYCODE_ENTER || kc == KeyEvent.KEYCODE_NUMPAD_ENTER)) return false
        attach()
        if (e.action != KeyEvent.ACTION_DOWN) return true
        val rows = rows()
        if (rows.isEmpty()) return false
        val v = cur?.get()
        val ri = if (v != null && v.isAttachedToWindow && v.isShown) rows.indexOfFirst { v in it } else -1
        if (ri < 0) {
            // First press (or the control went away): start at the first row on screen.
            val h = root().height
            val r = rows.firstOrNull { rectOf(it[0]).let { x -> x.top >= 0 && x.top < h } } ?: rows[0]
            select(r[0], r); return true
        }
        val r = rows[ri]; val ci = r.indexOf(v!!)
        when {
            up || down -> {
                val ni = ri + if (up) -1 else 1
                if (ni in rows.indices) {
                    val cx = rectOf(v).centerX()
                    val nr = rows[ni]
                    select(nr.minByOrNull { Math.abs(rectOf(it).centerX() - cx) }!!, nr)
                }
            }
            (left || right) && v is SeekBar && r.size == 1 -> v.onKeyDown(kc, e)   // the slider moves itself
            left -> if (ci > 0) select(r[ci - 1], r)
            right -> if (ci < r.size - 1) select(r[ci + 1], r)
            ok -> when (v) {
                is ReactEditText -> v.requestFocusFromJS()      // focus + on-screen keyboard
                is EditText -> { v.requestFocus(); (act.getSystemService(Activity.INPUT_METHOD_SERVICE) as InputMethodManager).showSoftInput(v, 0) }
                else -> v.performClick()                        // RN touchables and switches answer a click
            }
        }
        return true
    }
}
