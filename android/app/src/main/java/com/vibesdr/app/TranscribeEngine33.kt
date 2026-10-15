package com.vibesdr.app

import android.content.Context
import android.content.Intent
import android.media.AudioFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.speech.RecognitionListener
import android.speech.RecognitionSupport
import android.speech.RecognitionSupportCallback
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import android.util.Log
import androidx.annotation.RequiresApi
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.google.mlkit.common.model.DownloadConditions
import com.google.mlkit.nl.translate.TranslateLanguage
import com.google.mlkit.nl.translate.Translation
import com.google.mlkit.nl.translate.Translator
import com.google.mlkit.nl.translate.TranslatorOptions
import java.io.IOException
import java.io.OutputStream
import java.util.Locale

/**
 * ★★ The API-33+ half of VibeTranscriberModule (kept out of the module class — see the note there).
 *
 *  One on-device SpeechRecognizer in a SEGMENTED session fed from a pipe: our 16 kHz mono PCM goes in the write end,
 *  results come back per segment (final) and as partials. The recogniser lives on the main looper (it must); the
 *  pipe is written on the tap thread.
 *
 *  ★ AUTO on Android 14+: the recogniser's own language detection, limited to the languages whose on-device model is
 *    installed, with language SWITCHING on — a shortwave station changing language mid-hour is followed. On 13 there
 *    is no detection API, so AUTO means the phone's language.
 *  ★ A language whose model is missing is DOWNLOADED BY THE SYSTEM on request (triggerModelDownload) and the box says
 *    so — never silently, never a cloud fallback.
 */
@RequiresApi(33)
class TranscribeEngine33(
    private val ctx: Context,
    private val mod: VibeTranscriberModule,
    languageArg: String,
    private val translateTo: String,
) {
    /** 'auto:zh,tr' from JS = AUTO with EiBi's languages for this frequency weighed first. */
    private val language = if (languageArg.startsWith("auto:")) "auto" else languageArg
    private val hints = if (languageArg.startsWith("auto:"))
        languageArg.substring(5).split(',').map { it.trim() }.filter { it.isNotEmpty() } else emptyList()
    private var supported: List<String> = emptyList()
    private var announced = false   // the box's "Trying …" line, once per start (open() runs per segment)
    companion object {
        private const val TAG = "VibeTranscribe"

        fun capabilities(ctx: Context, p: Promise) {
            Handler(Looper.getMainLooper()).post {
                val m = Arguments.createMap()
                if (!SpeechRecognizer.isOnDeviceRecognitionAvailable(ctx)) {
                    m.putBoolean("available", false)
                    m.putString("reason", "This phone has no on-device speech recogniser")
                    p.resolve(m); return@post
                }
                val dev = Locale.getDefault()
                m.putBoolean("available", true)
                m.putString("deviceLanguage", dev.language)
                m.putString("deviceLanguageName", dev.getDisplayLanguage(dev))
                m.putBoolean("nativeLanguageDetection", Build.VERSION.SDK_INT >= 34)
                val sr = try { SpeechRecognizer.createOnDeviceSpeechRecognizer(ctx) } catch (e: Throwable) { null }
                if (sr == null) { p.resolve(m); return@post }
                var done = false
                val finish = { langs: List<String>, installed: Set<String> ->
                    if (!done) {
                        done = true
                        val arr = Arguments.createArray()
                        for (id in langs.distinct().sorted()) {
                            val o = Arguments.createMap()
                            val loc = Locale.forLanguageTag(id)
                            o.putString("id", id)
                            o.putString("name", loc.getDisplayName(dev))
                            o.putBoolean("installed", installed.contains(id))
                            arr.pushMap(o)
                        }
                        m.putArray("languages", arr)
                        p.resolve(m)
                        try { sr.destroy() } catch (_: Throwable) {}
                    }
                }
                try {
                    sr.checkRecognitionSupport(Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH), ctx.mainExecutor,
                        object : RecognitionSupportCallback {
                            override fun onSupportResult(s: RecognitionSupport) {
                                finish(s.supportedOnDeviceLanguages + s.installedOnDeviceLanguages,
                                       s.installedOnDeviceLanguages.toSet())
                            }
                            override fun onError(error: Int) { finish(emptyList(), emptySet()) }
                        })
                } catch (e: Throwable) { finish(emptyList(), emptySet()) }
                // Some recognisers never answer — don't hang the JS promise.
                Handler(Looper.getMainLooper()).postDelayed({ finish(emptyList(), emptySet()) }, 4000)
            }
        }

        /** ★ DOWNLOAD… — the SYSTEM fetches the speech model (it shows its own progress); the translation pack follows
         *  on Wi-Fi only, as when listening. Resolves at once with what was started, never waits on the system. */
        fun download(ctx: Context, language: String, translateTo: String, p: Promise) {
            Handler(Looper.getMainLooper()).post {
                val dev = Locale.getDefault()
                val nm = Locale.forLanguageTag(language).getDisplayLanguage(dev)
                val res = Arguments.createMap()
                val sr = try { SpeechRecognizer.createOnDeviceSpeechRecognizer(ctx) } catch (e: Throwable) { null }
                if (sr == null) {
                    res.putBoolean("ok", false); res.putString("text", "This phone has no on-device speech recogniser")
                    p.resolve(res); return@post
                }
                try {
                    sr.triggerModelDownload(Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH)
                        .putExtra(RecognizerIntent.EXTRA_LANGUAGE, language))
                } catch (e: Throwable) {
                    Log.w(TAG, "triggerModelDownload $language", e)
                }
                Handler(Looper.getMainLooper()).postDelayed({ try { sr.destroy() } catch (_: Throwable) {} }, 2000)
                var text = "Downloading $nm speech — Android shows its progress"
                val src = TranslateLanguage.fromLanguageTag(language.substringBefore('-'))
                val dst = if (translateTo.isEmpty()) null else TranslateLanguage.fromLanguageTag(translateTo)
                if (src != null && dst != null && src != dst) {
                    val tr = Translation.getClient(TranslatorOptions.Builder().setSourceLanguage(src).setTargetLanguage(dst).build())
                    tr.downloadModelIfNeeded(DownloadConditions.Builder().requireWifi().build())
                        .addOnCompleteListener { tr.close() }
                    text += "; the translation pack follows on Wi-Fi"
                }
                res.putBoolean("ok", true); res.putString("text", text)
                p.resolve(res)
            }
        }
    }

    private val main = Handler(Looper.getMainLooper())
    private var sr: SpeechRecognizer? = null
    @Volatile private var out: OutputStream? = null
    @Volatile private var stopped = false
    private var installed: List<String> = emptyList()
    private var detected: String = ""          // BCP-47 of the language being heard
    private var translator: Translator? = null
    private var translatorFor = ""
    private var translatorReady = false
    private var restarts = 0

    private fun status(s: String) = mod.emit("status", s)
    private fun display(tag: String) = Locale.forLanguageTag(tag).let { it.getDisplayName(Locale.getDefault()) }

    fun start() {
        main.post {
            status("Preparing on-device speech…")
            val probe = try { SpeechRecognizer.createOnDeviceSpeechRecognizer(ctx) } catch (e: Throwable) { null }
            if (probe == null) { mod.emit("error", "Could not start the on-device recogniser"); return@post }
            try {
                probe.checkRecognitionSupport(Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH), ctx.mainExecutor,
                    object : RecognitionSupportCallback {
                        override fun onSupportResult(s: RecognitionSupport) {
                            installed = s.installedOnDeviceLanguages
                            supported = s.supportedOnDeviceLanguages + s.installedOnDeviceLanguages
                            try { probe.destroy() } catch (_: Throwable) {}
                            open()
                        }
                        override fun onError(error: Int) {
                            try { probe.destroy() } catch (_: Throwable) {}
                            open()
                        }
                    })
            } catch (e: Throwable) { try { probe.destroy() } catch (_: Throwable) {}; open() }
        }
    }

    fun stop() {
        stopped = true
        VibeTranscribeTap.sink = null
        try { out?.close() } catch (_: IOException) {}
        out = null
        main.post {
            try { sr?.cancel() } catch (_: Throwable) {}
            try { sr?.destroy() } catch (_: Throwable) {}
            sr = null
            translator?.close(); translator = null
        }
    }

    /** The recogniser's primary language: the chosen one, else the phone's (AUTO detects among the installed). */
    /** The recogniser's tag for an EiBi hint ('zh' → zh-CN, 'yue' → a Cantonese tag), installed ones first. */
    private fun hintTag(h: String): String? {
        val pool = installed + supported
        // ★ Mandarin may be listed as 'cmn-Hans-CN' rather than 'zh-CN'.
        val base = pool.filter { val b = it.substringBefore('-'); b.equals(h, true) || (h == "zh" && b.equals("cmn", true)) }
        return when (h) {
            "zh" -> base.firstOrNull { it.contains("CN", true) || it.contains("Hans", true) } ?: base.firstOrNull()
            else -> base.firstOrNull()
        }
    }

    private fun primary(): String {
        if (language != "auto") return language
        // ★ EiBi says what is on this frequency now — listen for that first (fetched by the system if missing).
        hints.firstNotNullOfOrNull { hintTag(it) }?.let { return it }
        val dev = Locale.getDefault().toLanguageTag()
        return installed.firstOrNull { it.equals(dev, true) }
            ?: installed.firstOrNull { it.substringBefore('-').equals(Locale.getDefault().language, true) }
            ?: dev
    }

    private fun open() {
        if (stopped) return
        val pipe = try { ParcelFileDescriptor.createPipe() } catch (e: IOException) {
            mod.emit("error", "Could not open the audio pipe: ${e.message}"); return
        }
        val prim = primary()
        val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
            putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
            putExtra(RecognizerIntent.EXTRA_LANGUAGE, prim)
            putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
            putExtra(RecognizerIntent.EXTRA_PREFER_OFFLINE, true)
            putExtra(RecognizerIntent.EXTRA_AUDIO_SOURCE, pipe[0])
            putExtra(RecognizerIntent.EXTRA_AUDIO_SOURCE_ENCODING, AudioFormat.ENCODING_PCM_16BIT)
            putExtra(RecognizerIntent.EXTRA_AUDIO_SOURCE_CHANNEL_COUNT, 1)
            putExtra(RecognizerIntent.EXTRA_AUDIO_SOURCE_SAMPLING_RATE, 16000)
            // A segmented session runs until the audio source closes, reporting each segment — continuous captions.
            putExtra(RecognizerIntent.EXTRA_SEGMENTED_SESSION, RecognizerIntent.EXTRA_AUDIO_SOURCE)
            if (language == "auto" && !announced) {
                announced = true
                val names = (listOf(prim) + if (Build.VERSION.SDK_INT >= 34) installed else emptyList())
                    .map { Locale.forLanguageTag(it).getDisplayLanguage(Locale.getDefault()) }.distinct().take(6)
                mod.emit("trying", names.joinToString(", "))
            }
            if (language == "auto" && Build.VERSION.SDK_INT >= 34 && installed.size > 1) {
                val allowed = ArrayList((hints.mapNotNull { hintTag(it) }.filter { installed.contains(it) } + installed).distinct())
                putExtra(RecognizerIntent.EXTRA_ENABLE_LANGUAGE_DETECTION, true)
                putExtra(RecognizerIntent.EXTRA_LANGUAGE_DETECTION_ALLOWED_LANGUAGES, allowed)
                putExtra(RecognizerIntent.EXTRA_ENABLE_LANGUAGE_SWITCH, RecognizerIntent.LANGUAGE_SWITCH_BALANCED)
                putExtra(RecognizerIntent.EXTRA_LANGUAGE_SWITCH_ALLOWED_LANGUAGES, allowed)
            }
        }
        val r = try { SpeechRecognizer.createOnDeviceSpeechRecognizer(ctx) } catch (e: Throwable) {
            mod.emit("error", "Could not start the on-device recogniser"); return
        }
        sr = r
        r.setRecognitionListener(listener(intent))
        try { r.startListening(intent) } catch (e: Throwable) {
            mod.emit("error", "Recogniser refused to start: ${e.message}"); return
        }
        try { pipe[0].close() } catch (_: IOException) {}      // the recogniser holds its own dup
        val os = ParcelFileDescriptor.AutoCloseOutputStream(pipe[1])
        out = os
        VibeTranscribeTap.reset()
        VibeTranscribeTap.sink = { bytes ->
            val o = out
            if (o != null && !stopped) {
                try { o.write(bytes) } catch (e: IOException) { /* the session ended; reopen() takes over */ }
            }
        }
        if (detected.isEmpty()) setDetected(prim, announce = false)
        status(if (language == "auto" && Build.VERSION.SDK_INT >= 34 && installed.size > 1)
            "Listening — detecting the language…" else "Listening — ${display(prim)}")
    }

    private fun reopen(delayMs: Long) {
        if (stopped) return
        VibeTranscribeTap.sink = null
        try { out?.close() } catch (_: IOException) {}
        out = null
        main.postDelayed({
            if (stopped) return@postDelayed
            try { sr?.destroy() } catch (_: Throwable) {}
            sr = null
            open()
        }, delayMs)
    }

    private fun listener(intent: Intent) = object : RecognitionListener {
        override fun onReadyForSpeech(params: Bundle?) { restarts = 0 }
        override fun onBeginningOfSpeech() {}
        override fun onRmsChanged(rmsdB: Float) {}
        override fun onBufferReceived(buffer: ByteArray?) {}
        override fun onEndOfSpeech() {}
        override fun onEvent(eventType: Int, params: Bundle?) {}

        override fun onPartialResults(b: Bundle?) {
            val t = b?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)?.firstOrNull()?.trim().orEmpty()
            if (t.isNotEmpty()) mod.emit("partial", t)
        }
        override fun onSegmentResults(b: Bundle) { deliver(b) }
        override fun onResults(b: Bundle?) { if (b != null) deliver(b) }
        override fun onEndOfSegmentedSession() { reopen(200) }

        override fun onLanguageDetection(b: Bundle) {
            val lang = b.getString(SpeechRecognizer.DETECTED_LANGUAGE) ?: return
            if (!lang.equals(detected, true)) setDetected(lang, announce = true)
        }

        override fun onError(error: Int) {
            if (stopped) return
            when (error) {
                SpeechRecognizer.ERROR_LANGUAGE_UNAVAILABLE, SpeechRecognizer.ERROR_LANGUAGE_NOT_SUPPORTED -> {
                    // ★ Ask the SYSTEM to fetch the model (it shows its own progress); say so here too.
                    status("Downloading the ${display(primary())} speech model — it works offline once installed…")
                    try { sr?.triggerModelDownload(intent) } catch (_: Throwable) {}
                    reopen(15000)
                }
                SpeechRecognizer.ERROR_NO_MATCH, SpeechRecognizer.ERROR_SPEECH_TIMEOUT -> reopen(100)
                SpeechRecognizer.ERROR_RECOGNIZER_BUSY -> reopen(1000)
                else -> {
                    restarts++
                    if (restarts > 5) { mod.emit("error", "Speech recogniser stopped (error $error)"); return }
                    reopen(500L * restarts)
                }
            }
        }
    }

    private fun setDetected(lang: String, announce: Boolean) {
        detected = lang
        mod.emit("lang", lang, mapOf("name" to display(lang)))
        if (announce) status("Listening — ${display(lang)} (detected)")
        prepareTranslator(lang)
    }

    private fun deliver(b: Bundle) {
        val t = b.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)?.firstOrNull()?.trim().orEmpty()
        if (t.isEmpty()) return
        val code = detected.substringBefore('-').uppercase(Locale.ROOT)
        val tr = translator
        if (tr == null || !translatorReady) {
            mod.emit("final", t, mapOf("lang" to code)); return
        }
        tr.translate(t)
            .addOnSuccessListener { out -> mod.emit("final", t, mapOf("lang" to code, "translated" to out)) }
            .addOnFailureListener { mod.emit("final", t, mapOf("lang" to code)) }
    }

    private fun prepareTranslator(lang: String) {
        val src = TranslateLanguage.fromLanguageTag(lang.substringBefore('-'))
        val dst = if (translateTo.isEmpty()) null else TranslateLanguage.fromLanguageTag(translateTo)
        val key = "$src>$dst"
        if (key == translatorFor) return
        translator?.close(); translator = null; translatorReady = false; translatorFor = key
        if (dst == null || src == null || src == dst) {
            if (dst != null && src == null) status("Translation from ${display(lang)} is not available on this device")
            return
        }
        val tr = Translation.getClient(TranslatorOptions.Builder().setSourceLanguage(src).setTargetLanguage(dst).build())
        translator = tr
        // ★ Language packs (~30 MB) download on Wi-Fi only, and the box says so — never a silent cellular download.
        status("Getting the ${display(lang)} → ${display(translateTo)} translation pack (Wi-Fi)…")
        tr.downloadModelIfNeeded(DownloadConditions.Builder().requireWifi().build())
            .addOnSuccessListener {
                if (translator === tr) {
                    translatorReady = true
                    status("Listening — ${display(lang)}, translating to ${display(translateTo)}")
                }
            }
            .addOnFailureListener { e ->
                Log.w(TAG, "translation model: ${e.message}")
                if (translator === tr) status("Translation pack not available (needs Wi-Fi once) — showing the original")
            }
    }
}
