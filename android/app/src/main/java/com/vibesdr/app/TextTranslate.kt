package com.vibesdr.app

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.google.mlkit.common.model.DownloadConditions
import com.google.mlkit.nl.translate.TranslateLanguage
import com.google.mlkit.nl.translate.Translation
import com.google.mlkit.nl.translate.Translator
import com.google.mlkit.nl.translate.TranslatorOptions
import java.util.Locale

/*
 * ★★ V12 GALLERY TRANSLATE (2026-10-23) — a TEXT through ML Kit's on-device translator, the same one TRANSCRIBE's
 *  captions use (TranscribeEngine.prepareTranslator). JS sends one line at a time (src/utils/galleryTranslate.ts), so a
 *  NAVTEX message keeps its layout. Nothing leaves the phone.
 *  ★ Language packs (~30 MB) download on Wi-Fi only, as for the captions — never a silent cellular download; the
 *    reason comes back to the gallery, which says it.
 *  ★ ONE translator at a time (a pack is the memory — 1 GB phones run this app): a new pair closes the last.
 *  ★ No android.* type newer than minSdk here: ML Kit translation runs from API 21, so the gallery can translate on
 *    phones too old for TRANSCRIBE.
 */
object TextTranslate {
    private var key = ""
    private var tr: Translator? = null

    private fun name(code: String) = Locale.forLanguageTag(code).getDisplayLanguage(Locale.getDefault())

    fun translate(text: String, source: String, target: String, p: Promise) {
        val src = TranslateLanguage.fromLanguageTag(source.substringBefore('-'))
        val dst = TranslateLanguage.fromLanguageTag(target.substringBefore('-'))
        val res = Arguments.createMap()
        if (src == null || dst == null) {
            res.putBoolean("ok", false)
            res.putString("reason", "This phone cannot translate ${name(source)} into ${name(target)}")
            p.resolve(res); return
        }
        if (src == dst) { res.putBoolean("ok", true); res.putString("text", text); p.resolve(res); return }
        val t: Translator = synchronized(this) {
            val k = "$src>$dst"
            if (k != key || tr == null) {
                tr?.close()
                tr = Translation.getClient(TranslatorOptions.Builder().setSourceLanguage(src).setTargetLanguage(dst).build())
                key = k
            }
            tr!!
        }
        t.downloadModelIfNeeded(DownloadConditions.Builder().requireWifi().build())
            .addOnSuccessListener {
                t.translate(text)
                    .addOnSuccessListener { out ->
                        val m = Arguments.createMap(); m.putBoolean("ok", true); m.putString("text", out); p.resolve(m)
                    }
                    .addOnFailureListener { e ->
                        val m = Arguments.createMap(); m.putBoolean("ok", false); m.putString("reason", e.message ?: "translation failed"); p.resolve(m)
                    }
            }
            .addOnFailureListener {
                val m = Arguments.createMap(); m.putBoolean("ok", false)
                m.putString("reason", "the ${name(source)} → ${name(target)} translation pack is not on this phone yet — it downloads once, on Wi-Fi")
                p.resolve(m)
            }
    }
}
