package com.vibesdr.app

import android.content.Context
import android.content.Intent
import android.media.AudioFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
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
 * ★★ The API-31+ half of VibeTranscriberModule (kept out of the module class — see the note there).
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
 *
 *  ★★★ ANDROID 12 (API 31-32 — Kiko's Galaxy S10, whose last update is 12). createOnDeviceSpeechRecognizer is there
 *    (31), but checkRecognitionSupport and triggerModelDownload are 33: the app can neither LIST the phone's offline
 *    speech languages nor fetch one. So on 31-32 (`chooseOnly` in the caps): no AUTO, no GET — the user chooses the
 *    language from a fixed common list (the phone's own first), and a language that is not installed for offline use
 *    says how to add it in the phone's settings, and stops (no retry loop).
 *    ★ The pipe (EXTRA_AUDIO_SOURCE) is a 33 FRAMEWORK constant but it is read by the recogniser SERVICE, which is
 *      updated on its own (Google's comes from Play). If the 12 phone's service ignores it, it would listen to the
 *      MICROPHONE — captions of the room, not the radio. So on 31-32 our end of the pipe is NON-BLOCKING and if the
 *      recogniser has not read our audio for PIPE_DEAF_MS we stop and say so, rather than caption the wrong thing.
 *    ★ Not yet run on a 31/32 device (2026-10-16) — the first real test says which 12 phones take the pipe.
 *    ★ getVoiceDetailsIntent / ACTION_GET_LANGUAGE_DETAILS was considered for the list: it asks the DEFAULT (cloud)
 *      recogniser for every language it supports, not what is installed offline — a list that would promise languages
 *      the on-device recogniser cannot do. Not used.
 */
@RequiresApi(31)
class TranscribeEngine(
    private val ctx: Context,
    private val mod: VibeTranscriberModule,
    languageArg: String,
    private val translateTo: String,
    /** start()'s options JSON from JS: { understood: ["fr"] } — languages shown untranslated (multilingual users). */
    optionsJson: String = "",
) {
    /** ★★ LANGUAGES I UNDERSTAND (Stuart, 2026-10-17: "for people who are multilingual themselves") — base codes. */
    private val understood: Set<String> = try {
        val a = org.json.JSONObject(optionsJson.ifEmpty { "{}" }).optJSONArray("understood")
        (0 until (a?.length() ?: 0)).map { a!!.getString(it).substringBefore('-').lowercase(Locale.ROOT) }.toSet()
    } catch (_: Throwable) { emptySet() }
    /** 'auto:zh,tr' from JS = AUTO with EiBi's languages for this frequency weighed first. */
    private val language = if (languageArg.startsWith("auto:")) "auto" else languageArg
    private val hints = if (languageArg.startsWith("auto:"))
        languageArg.substring(5).split(',').map { it.trim() }.filter { it.isNotEmpty() } else emptyList()
    private var supported: List<String> = emptyList()
    private var announced = false   // the box's "Trying …" line, once per start (open() runs per segment)
    companion object {
        private const val TAG = "VibeTranscribe"
        /** 31-32: the recogniser has not taken our audio for this long while it flows → it is not reading the pipe. */
        private const val PIPE_DEAF_MS = 5000L
        /* ★★ THE STALL WATCHDOG (V12 research, 2026-10-16). Apple's SpeechTranscriber was measured going quiet on medium
         *    SSB — words for 7-15 s, then nothing for the rest of the clip (3702 kHz: 19 words → 85 with a 12 s watchdog
         *    that starts a fresh analyzer; 0 restarts on clean VOLMET). The same guard runs here: audio FLOWING (over a
         *    low RMS floor — never on squelch/mute silence) and no partial or final for STALL_MS → a fresh recogniser,
         *    same language and translation, the box's text untouched. A restart that brings nothing waits twice as long
         *    before the next (to STALL_MAX_MS), so a long stretch of band noise costs a few restarts, not one every 12 s. */
        private const val STALL_MS = 12_000L
        private const val STALL_MAX_MS = 96_000L
        private const val STALL_AUDIBLE_MS = 10_000L     // of the window, this much must have been audio, not silence
        private const val STALL_RMS_FLOOR = 0.002f      // ≈ -54 dBFS: squelch shut / muted is far below, band noise above
        /** 31-32: the languages offered when the phone cannot be asked (the phone's own goes first). One tag each. */
        private val COMMON = listOf("en-US", "es-ES", "fr-FR", "de-DE", "it-IT", "pt-BR", "nl-NL", "ru-RU", "uk-UA",
            "pl-PL", "tr-TR", "ar-EG", "zh-CN", "ja-JP", "ko-KR", "hi-IN", "vi-VN", "th-TH", "id-ID", "cs-CZ", "sv-SE")

        /** Android 12: the user's own setting is the only way to add an offline speech language — say where, plainly. */
        fun notInstalledText(nm: String) =
            "$nm is not installed for offline speech on this phone. Add it in the phone's Settings (search Settings for " +
            "\"offline speech\" — Google's recogniser keeps them under Speech Services by Google), then try again."

        fun capabilities(ctx: Context, p: Promise) {
            if (Build.VERSION.SDK_INT < 33) capabilities31(ctx, p) else capabilities33(ctx, p)
        }

        @RequiresApi(33)
        private fun capabilities33(ctx: Context, p: Promise) {
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
                m.putString("deviceRegion", dev.country)   // ★ picks en-GB over en-AU (pickLocale)
                m.putArray("translateLanguages", translateTargets(dev))
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

        /** 31-32: no list and no download — the phone's language and the common ones, nothing marked ON PHONE. */
        private fun capabilities31(ctx: Context, p: Promise) {
            Handler(Looper.getMainLooper()).post {
                val m = Arguments.createMap()
                if (!SpeechRecognizer.isOnDeviceRecognitionAvailable(ctx)) {
                    m.putBoolean("available", false)
                    m.putString("reason", "This phone has no on-device speech recogniser")
                    p.resolve(m); return@post
                }
                val dev = Locale.getDefault()
                m.putBoolean("available", true)
                m.putBoolean("chooseOnly", true)
                m.putString("deviceLanguage", dev.language)
                m.putString("deviceLanguageName", dev.getDisplayLanguage(dev))
                m.putString("deviceRegion", dev.country)
                m.putArray("translateLanguages", translateTargets(dev))
                m.putBoolean("nativeLanguageDetection", false)
                val tags = (listOf(dev.toLanguageTag()) + COMMON)
                    .distinctBy { Locale.forLanguageTag(it).language }
                val arr = Arguments.createArray()
                for (id in tags) {
                    val o = Arguments.createMap()
                    o.putString("id", id)
                    o.putString("name", Locale.forLanguageTag(id).getDisplayName(dev))
                    o.putBoolean("installed", false)   // unknown on 12 — never claimed
                    arr.pushMap(o)
                }
                m.putArray("languages", arr)
                p.resolve(m)
            }
        }

        /** ★ TRANSLATE TO is a list (Stuart, 2026-10-17): every language ML Kit translates, named in the phone's language.
         *  Bare codes — ML Kit has no regional variants. Which packs are downloaded is not marked (ML Kit only says so
         *  asynchronously; a follow-up). */
        private fun translateTargets(dev: Locale) = Arguments.createArray().also { arr ->
            for (code in TranslateLanguage.getAllLanguages().sortedBy { Locale.forLanguageTag(it).getDisplayLanguage(dev) }) {
                val o = Arguments.createMap()
                o.putString("id", code)
                o.putString("name", Locale.forLanguageTag(code).getDisplayLanguage(dev))
                arr.pushMap(o)
            }
        }

        /** ★ DOWNLOAD… — the SYSTEM fetches the speech model (it shows its own progress); the translation pack follows
         *  on Wi-Fi only, as when listening. Resolves at once with what was started, never waits on the system. */
        fun download(ctx: Context, language: String, translateTo: String, p: Promise) {
            if (Build.VERSION.SDK_INT < 33) {   // no GET is drawn on 12; answer plainly if one is ever asked for
                val res = Arguments.createMap()
                res.putBoolean("ok", false)
                res.putString("text", notInstalledText(Locale.forLanguageTag(language).getDisplayLanguage(Locale.getDefault())))
                p.resolve(res); return
            }
            download33(ctx, language, translateTo, p)
        }

        @RequiresApi(33)
        private fun download33(ctx: Context, language: String, translateTo: String, p: Promise) {
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
                val dst = if (translateTo.isEmpty()) null else TranslateLanguage.fromLanguageTag(translateTo.substringBefore('-'))
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
    // ── stall watchdog (see STALL_MS) — results on main, audio on the tap thread ──
    @Volatile private var lastResultAt = 0L
    private val audibleMs = java.util.concurrent.atomic.AtomicLong(0)
    private var stallAfter = STALL_MS
    private var stallRestarts = 0
    private var lastDialCorr = 0f
    private val watchdog = object : Runnable {
        override fun run() {
            if (stopped) return
            val quiet = SystemClock.elapsedRealtime() - lastResultAt
            if (sr != null && quiet >= stallAfter && audibleMs.get() >= STALL_AUDIBLE_MS) {
                stallRestarts++
                Log.i(TAG, "stall watchdog: no result for ${quiet / 1000} s with ${audibleMs.get() / 1000} s of audio — " +
                    "fresh recogniser (#$stallRestarts this session)")
                stallAfter = minOf(stallAfter * 2, STALL_MAX_MS)
                markResult(false)
                reopen(0)
            }
            main.postDelayed(this, 2000)
        }
    }
    /** A partial or final arrived (or a fresh recogniser starts): the stall clock starts again. */
    private fun markResult(heard: Boolean) {
        lastResultAt = SystemClock.elapsedRealtime()
        audibleMs.set(0)
        if (heard) stallAfter = STALL_MS
    }

    private fun status(s: String) = mod.emit("status", s)
    private fun display(tag: String) = Locale.forLanguageTag(tag).let { it.getDisplayName(Locale.getDefault()) }

    fun start() {
        VibeTranscribeTap.onBlock = { rms, samples, dial ->
            if (rms > STALL_RMS_FLOOR) audibleMs.addAndGet(samples * 1000L / 16000)
            // ★ SSB auto-tune moved the recogniser's copy: say so, quietly, in the box's header (only on a change).
            if (dial != lastDialCorr) {
                lastDialCorr = dial
                VibeTranscribeTap.stats()?.let { st ->
                    Log.d(TAG, "ssb auto-tune: dial correction ${dial} Hz (estimate ${st[0]} Hz, confidence ${st[1]}, " +
                        "${st[2]} s voiced, ${st[4].toInt()} estimates, ${st[5].toInt()} corrections)")
                }
                main.post { if (!stopped) mod.emit("tune", "", mapOf("hz" to Math.round(dial).toString())) }
            }
        }
        main.post {
            markResult(false)
            main.postDelayed(watchdog, 2000)
            status("Preparing on-device speech…")
            if (Build.VERSION.SDK_INT < 33) open() else start33()   // 12: nothing to ask — the chosen language
        }
    }

    @RequiresApi(33)
    private fun start33() {
        run {
            val probe = try { SpeechRecognizer.createOnDeviceSpeechRecognizer(ctx) } catch (e: Throwable) { null }
            if (probe == null) { mod.emit("error", "Could not start the on-device recogniser"); return }
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
        VibeTranscribeTap.onBlock = null
        if (stallRestarts > 0) Log.i(TAG, "stall watchdog: $stallRestarts restart(s) this session")
        try { out?.close() } catch (_: IOException) {}
        out = null
        main.post {
            main.removeCallbacks(watchdog)
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
        markResult(false)
        if (Build.VERSION.SDK_INT < 33) {
            // ★ 12: our end never blocks; a pipe left full for PIPE_DEAF_MS = the recogniser is not reading it.
            val fd = pipe[1].fileDescriptor
            try { Os.fcntlInt(fd, OsConstants.F_SETFL, OsConstants.O_NONBLOCK) } catch (_: ErrnoException) {}
            var fullSince = 0L
            VibeTranscribeTap.sink = sink@{ bytes ->
                if (out !== os || stopped) return@sink
                val n = try { Os.write(fd, bytes, 0, bytes.size) } catch (e: ErrnoException) {
                    if (e.errno != OsConstants.EAGAIN) return@sink   // closed: the session ended; reopen() takes over
                    0
                } catch (_: IOException) { return@sink }
                if (n > 0) { fullSince = 0L; return@sink }
                val now = SystemClock.elapsedRealtime()
                if (fullSince == 0L) fullSince = now
                else if (now - fullSince > PIPE_DEAF_MS) {
                    fullSince = Long.MAX_VALUE / 2   // once
                    main.post { deaf() }
                }
            }
        } else {
            VibeTranscribeTap.sink = { bytes ->
                val o = out
                if (o != null && !stopped) {
                    try { o.write(bytes) } catch (e: IOException) { /* the session ended; reopen() takes over */ }
                }
            }
        }
        if (detected.isEmpty()) setDetected(prim, announce = false)
        status(if (language == "auto" && Build.VERSION.SDK_INT >= 34 && installed.size > 1)
            "Listening — detecting the language…" else "Listening — ${display(prim)}")
    }

    @RequiresApi(33)
    private fun fetchModel(intent: Intent) { try { sr?.triggerModelDownload(intent) } catch (_: Throwable) {} }

    /** 12: the recogniser never read our audio — it is (at best) listening to the microphone. Stop, and say so. */
    private fun deaf() {
        if (stopped) return
        stop()
        mod.emit("error", "This phone's speech recogniser does not take the radio's audio on Android ${Build.VERSION.RELEASE} " +
            "(it listens only to the microphone), so TRANSCRIBE cannot work here. It needs Android 13 on this phone.")
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
            markResult(true)
            if (t.isNotEmpty()) mod.emit("partial", t)
        }
        override fun onSegmentResults(b: Bundle) { markResult(true); deliver(b) }
        override fun onResults(b: Bundle?) {
            markResult(true)
            if (b != null) deliver(b)
            // ★ 12's recogniser may not keep a segmented session: one utterance, then results — listen again.
            if (Build.VERSION.SDK_INT < 33) reopen(100)
        }
        override fun onEndOfSegmentedSession() { reopen(200) }

        override fun onLanguageDetection(b: Bundle) {
            val lang = b.getString(SpeechRecognizer.DETECTED_LANGUAGE) ?: return
            if (!lang.equals(detected, true)) setDetected(lang, announce = true)
        }

        override fun onError(error: Int) {
            if (stopped) return
            when (error) {
                SpeechRecognizer.ERROR_LANGUAGE_UNAVAILABLE, SpeechRecognizer.ERROR_LANGUAGE_NOT_SUPPORTED -> {
                    if (Build.VERSION.SDK_INT < 33) {
                        // ★ 12: the app cannot fetch it — tell the user where to add it, and stop (no retry loop).
                        val nm = Locale.forLanguageTag(primary()).getDisplayLanguage(Locale.getDefault())
                        stop()
                        mod.emit("error", notInstalledText(nm))
                        return
                    }
                    // ★ Ask the SYSTEM to fetch the model (it shows its own progress); say so here too.
                    status("Downloading the ${display(primary())} speech model — it works offline once installed…")
                    fetchModel(intent)
                    reopen(15000)
                }
                SpeechRecognizer.ERROR_NO_MATCH, SpeechRecognizer.ERROR_SPEECH_TIMEOUT -> reopen(100)
                SpeechRecognizer.ERROR_RECOGNIZER_BUSY -> reopen(1000)
                // ★ 12: the recogniser wanted the microphone, not our pipe — say so rather than retry.
                SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS ->
                    if (Build.VERSION.SDK_INT < 33) deaf() else {
                        restarts++
                        if (restarts > 5) mod.emit("error", "Speech recogniser stopped (error $error)") else reopen(500L * restarts)
                    }
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
        val dst = if (translateTo.isEmpty()) null else TranslateLanguage.fromLanguageTag(translateTo.substringBefore('-'))
        val key = "$src>$dst"
        if (key == translatorFor) return
        translator?.close(); translator = null; translatorReady = false; translatorFor = key
        if (dst != null && understood.contains(lang.substringBefore('-').lowercase(Locale.ROOT))) {
            status("Listening — ${display(lang)} (you understand it — not translated)")
            return
        }
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
