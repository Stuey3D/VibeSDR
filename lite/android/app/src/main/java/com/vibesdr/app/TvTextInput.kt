package com.vibesdr.app

import android.app.UiModeManager
import android.content.Context
import android.content.res.Configuration
import android.graphics.Rect
import android.view.KeyEvent
import android.view.ViewGroup
import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.views.textinput.ReactEditText
import com.facebook.react.views.textinput.ReactTextInputManager

/**
 * ★★ ANDROID TV: A REMOTE COULD NOT GET PAST A TEXT FIELD (Sony XE85, 2026-09-19 — "i can go down but not up").
 * RN 0.73's ReactEditText.requestFocus(direction, rect) is literally `return isFocused()` (javap), so a D-pad
 * focus search that lands on a text field is refused and the selection stops dead. ServerModeScreen has 20 of
 * them, so on a TV most of the page was unreachable.
 *
 * This replaces RN's "AndroidTextInput" manager (LitePackage is listed after MainReactPackage, so its manager
 * wins) and ON A TV ONLY hands out a field that accepts D-pad focus WITHOUT the keyboard, and opens the keyboard
 * when OK is pressed on it. A phone or tablet gets RN's own ReactEditText, unchanged.
 */
class TvTextInputManager : ReactTextInputManager() {
    override fun createViewInstance(ctx: ThemedReactContext): ReactEditText {
        val v = if (isTv(ctx)) TvEditText(ctx) else ReactEditText(ctx)
        // The same three steps as ReactTextInputManager.createViewInstance (0.73.11, javap).
        v.inputType = v.inputType and 0x20000.inv()   // ~TYPE_TEXT_FLAG_MULTI_LINE
        v.setReturnKeyType("done")
        v.layoutParams = ViewGroup.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        return v
    }

    companion object {
        /** ★ The shared rule (VibeLocalSdrModule.kt remoteDriven): it was the TV UI mode alone, which a cheap box
         *  running a phone launcher never reports — so TvNav (row bar + highlight) never started there. */
        fun isTv(ctx: Context): Boolean = remoteDriven(ctx)
    }
}

class TvEditText(ctx: Context) : ReactEditText(ctx) {
    /** D-pad focus: take it, but do not throw the keyboard over the screen on every pass through the page. */
    override fun requestFocus(direction: Int, previouslyFocusedRect: Rect?): Boolean {
        if (isFocused) return true
        val soft = showSoftInputOnFocus
        showSoftInputOnFocus = false
        requestFocusFromJS()
        showSoftInputOnFocus = soft
        return isFocused
    }

    /** OK on the remote opens the on-screen keyboard. */
    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_DPAD_CENTER && isFocused) {
            showSoftKeyboard()
            return true
        }
        return super.onKeyUp(keyCode, event)
    }
}
