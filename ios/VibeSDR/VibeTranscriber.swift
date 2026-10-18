import Foundation
import AVFoundation
import Speech
import Translation
import NaturalLanguage
import SwiftUI

/* ★★★ ON-DEVICE TRANSCRIPTION + TRANSLATION (V12, experimental). Brief: briefs/BRIEF-live-transcription-translation.md.
 *
 *  It listens to WHAT WE PLAY — the 48 kHz buffers VibePowerModule hands the recorder — so it works on every
 *  backend (VibeServer, UberSDR, Kiwi, OWRX, FM-DX, local dongle, DAB) with no server help. That is the whole
 *  difference from UberSDR's STT key, which is the server's own whisper engine.
 *
 *  ★★ NOTHING LEAVES THE PHONE. SpeechAnalyzer/SpeechTranscriber (iOS 26) and the Translation framework run on
 *     the device; there is no fallback to a server recogniser, ever. Older iOS: the key is not drawn at all.
 *
 *  ★★ APPLE HAS NO SPOKEN-LANGUAGE IDENTIFICATION (checked in the iOS 27 SDK — Speech has a SpeechDetector for
 *     voice activity, nothing for language). So AUTO brute-forces it (rewritten 2026-10-17 night from the bake-off,
 *     docs/v12/LANGUAGE-ID-BAKEOFF.md): every language whose model is on the device hears the same buffered 10 s of
 *     speech, and rule H2 (LanguageID.swift — confidence × NaturalLanguage's reading of each transcript × the other
 *     models' vote) names it, or names nothing. A wrong-language model still writes words — often the RIGHT language's
 *     words, confidently, which is why confidence alone picked wrong on 60 % of English. Then ONE live transcriber runs
 *     in that language and a background check switches only on clear, repeated evidence.
 *     AUTO never downloads anything silently. Choosing a language explicitly downloads its model, and says so.
 *
 *  ★ Thread priority rule (Network > Audio > Spectrum > Decoders): feed() is called on audioQ and only copies
 *    and hands off; conversion runs on our own utility queue, and a backlog DROPS — playback never waits.
 */

// MARK: - Audio tap (called by VibePowerModule on audioQ)

@objc final class VibeTranscribeTap: NSObject {
  @objc static let shared = VibeTranscribeTap()

  /// Read on audioQ without a lock — the same pattern as VibePowerModule.recArmed: a stale read costs one buffer.
  @objc private(set) var armed = false
  private var sink: ((AVAudioPCMBuffer) -> Void)?
  /// When the SSB auto-tune's correction changes, on the tap's queue: the DIAL change in Hz (0 = none) and the
  /// estimator's stats [offsetHz, confidence, voicedSeconds, passes, estimates, corrections, audioShiftHz].
  private var onDial: ((Float, [Float]) -> Void)?
  private var lastDial: Float = 0          // q only
  private let q = DispatchQueue(label: "com.vibesdr.transcribe.feed", qos: .utility)
  private var backlog = 0
  private let lock = NSLock()
  /* ★★ THE RECOGNISER'S FEED IS SHARED C++ (vibedsp/ssb_offset — the same object Android's tap runs): any rate →
   *    16 kHz through an anti-aliased resampler, then SSB AUTO-TUNE in USB/LSB — the voice's own harmonics say how
   *    far the dial is off (measured: ±100 Hz cost the recogniser ~20 % of its words' agreement, ±200 Hz up to half)
   *    and the RECOGNISER'S copy is shifted back. Never what the listener hears: this is a copy of the played audio.
   *    Touched only on `q`, except set_tuning, which only stores atomics. */
  private let feedH: UnsafeMutableRawPointer? = vibe_rxfeed_create()
  private static let fmt16 = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 16000, channels: 1, interleaved: false)!

  func setSink(_ s: ((AVAudioPCMBuffer) -> Void)?, onDial d: ((Float, [Float]) -> Void)? = nil) {
    lock.lock(); sink = s; onDial = d; armed = s != nil; lock.unlock()
    q.async { self.lastDial = 0 }            // a new listener hears the current correction (if any) afresh
  }

  /// The dial: 'usb'/'lsb' arm the auto-tune, anything else switches it off; any change resets the estimate (the old
  /// offset belonged to another signal). Any thread.
  @objc func setTuning(_ mode: String, freqHz: Double) {
    let m = mode.lowercased()
    vibe_rxfeed_set_tuning(feedH, m == "usb" ? 1 : m == "lsb" ? -1 : 0, freqHz)
  }

  /// [offsetHz, confidence, voicedSeconds, passes, estimates, corrections, audioShiftHz] — for the debug log. q only.
  private func stats() -> [Float] {
    var o = [Float](repeating: 0, count: 7)
    _ = vibe_rxfeed_stats(feedH, &o)
    return o
  }

  /// ★★ V12 RECORD BUFFER → AUTO. Set by VibePowerModule: asked for up to `seconds` of the record buffer's most recent
  /// audio, it answers (on its own queue) with the chunks, oldest first, and their rate — [] when there is none to give.
  var primeSource: ((Double, @escaping ([RecordBufferRing.Chunk], Double) -> Void) -> Void)?

  /// AUTO is starting: hand it the record buffer's last `seconds`, mono at 16 kHz, so its first window need not wait
  /// for fresh speech. ★ A plain resample, NOT through feedH: the live audio is already flowing through that stateful
  /// feed, and pushing older audio into it would put a seam in the live lane's copy. So the prefix skips the SSB
  /// auto-tune's shift — fine for telling languages apart; the live audio that follows is shifted as ever.
  func requestPrime(seconds: Double, _ deliver: @escaping (AVAudioPCMBuffer) -> Void) {
    guard let src = primeSource else { return }
    src(seconds) { [weak self] chunks, rate in
      guard let self, !chunks.isEmpty else { return }
      self.q.async {
        let n = chunks.reduce(0) { $0 + $1.frames }
        guard n > 0,
              let inFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: rate, channels: 1, interleaved: false),
              let inBuf = AVAudioPCMBuffer(pcmFormat: inFmt, frameCapacity: AVAudioFrameCount(n)),
              let conv = AVAudioConverter(from: inFmt, to: Self.fmt16),
              let out = AVAudioPCMBuffer(pcmFormat: Self.fmt16, frameCapacity: AVAudioFrameCount(Double(n) * 16000 / rate + 64))
        else { return }
        /* ★ STEREO → ONE EAR, NEVER A MIX. A stereo stretch of the buffer is binaural (or WFM stereo). Binaural gives the
         *   far ear up to 0.6 ms of delay, so L+R would comb-filter the voice (notches every ~1.7 kHz) — exactly what the
         *   recogniser must not hear. Each ear alone is clean: the far one is only quieter and duller. So take the LOUDER
         *   ear — the near one, the brighter copy — chosen once over the whole prime, so it never hops between ears
         *   mid-word. WFM stereo loses nothing that matters for telling languages apart (speech sits in the middle). */
        var eL: Double = 0, eR: Double = 0
        for c in chunks where c.channels == 2 {
          c.samples.withUnsafeBufferPointer { sp in
            for i in 0..<c.frames { let l = Double(sp[i * 2]), r = Double(sp[i * 2 + 1]); eL += l * l; eR += r * r }
          }
        }
        let ear = eR > eL ? 1 : 0
        let d = inBuf.floatChannelData![0]
        var o = 0
        for c in chunks {
          c.samples.withUnsafeBufferPointer { sp in
            if c.channels == 2 { for i in 0..<c.frames { d[o + i] = Float(sp[i * 2 + ear]) / 32768 } }
            else { for i in 0..<c.frames { d[o + i] = Float(sp[i]) / 32768 } }
          }
          o += c.frames
        }
        inBuf.frameLength = AVAudioFrameCount(n)
        var fed = false
        var err: NSError?
        conv.convert(to: out, error: &err) { _, st in
          if fed { st.pointee = .endOfStream; return nil }
          fed = true; st.pointee = .haveData; return inBuf
        }
        if err == nil, out.frameLength > 0 { deliver(out) }
      }
    }
  }

  /// `silent`: squelch is closed — the transcriber hears what the listener hears, i.e. nothing.
  @objc func feed(_ buf: AVAudioPCMBuffer, silent: Bool) {
    guard armed else { return }
    lock.lock(); let s = sink; let b = backlog; lock.unlock()
    guard s != nil else { return }
    if b > 40 { return }                         // ~1 s of packets queued: drop, never block audio
    // Downmix to mono now (the buffer is the player's), a plain copy — cheap enough for audioQ.
    let n = Int(buf.frameLength)
    guard n > 0, let src = buf.floatChannelData,
          let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: buf.format.sampleRate,
                                  channels: 1, interleaved: false),
          let mono = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: AVAudioFrameCount(n)) else { return }
    mono.frameLength = AVAudioFrameCount(n)
    let dst = mono.floatChannelData![0]
    if silent {
      memset(dst, 0, n * MemoryLayout<Float>.size)
    } else if buf.format.channelCount >= 2 {
      let l = src[0], r = src[1]
      for i in 0..<n { dst[i] = (l[i] + r[i]) * 0.5 }
    } else {
      memcpy(dst, src[0], n * MemoryLayout<Float>.size)
    }
    lock.lock(); backlog += 1; lock.unlock()
    q.async { [weak self] in
      guard let self else { return }
      defer { self.lock.lock(); self.backlog -= 1; self.lock.unlock() }
      // ★ 16 kHz out: n·16000/rate + a little (the C side refuses a short buffer rather than overrun it).
      let rate = Int32(mono.format.sampleRate.rounded())
      let cap = Int32(Int64(n) * 16000 / Int64(max(1, rate)) + 8)
      guard let out = AVAudioPCMBuffer(pcmFormat: Self.fmt16, frameCapacity: AVAudioFrameCount(cap)) else { return }
      let m = vibe_rxfeed_process(self.feedH, mono.floatChannelData![0], Int32(n), rate, out.floatChannelData![0], cap)
      self.lock.lock(); let sk = self.sink; let od = self.onDial; self.lock.unlock()
      if m > 0 {
        out.frameLength = AVAudioFrameCount(m)
        sk?(out)
      }
      let d = vibe_rxfeed_dial_correction(self.feedH)
      if d != self.lastDial, let od { self.lastDial = d; od(d, self.stats()) }
    }
  }
}

// MARK: - Bridge module

@objc(VibeTranscriber)
class VibeTranscriber: RCTEventEmitter {
  private var hasListeners = false
  private var engineBox: AnyObject?          // TranscribeEngine on iOS 26+

  override static func requiresMainQueueSetup() -> Bool { false }
  override func supportedEvents() -> [String]! { ["VibeTranscribe"] }
  override func startObserving() { hasListeners = true }
  override func stopObserving()  { hasListeners = false }

  fileprivate func emit(_ body: [String: Any]) {
    if hasListeners { sendEvent(withName: "VibeTranscribe", body: body) }
  }

  /// { available, reason, deviceLanguage, languages: [{ id, name, installed }] }
  @objc func capabilities(_ resolve: @escaping RCTPromiseResolveBlock, rejecter reject: @escaping RCTPromiseRejectBlock) {
    guard #available(iOS 26.0, *) else {
      resolve(["available": false, "reason": "Needs iOS 26 or later"]); return
    }
    Task {
      guard SpeechTranscriber.isAvailable else {
        resolve(["available": false, "reason": "On-device speech recognition is not available on this device"]); return
      }
      // ★ No releasing here any more (2026-10-17 night): the places the app KEEPS are put right by keep(), which JS
      //   calls with the ★ list once it has these capabilities (SpeechEngines.reconcileKept).
      let supported = await SpeechEngines.supportedLocales()
      let installed = await SpeechEngines.installedIds()
      let here = Locale.current
      let langs: [[String: Any]] = supported
        .map { loc -> [String: Any] in
          let id = loc.identifier(.bcp47)
          // ★ `installed` = the model is on the device (saved by this app, another app or the system).
          return ["id": id, "name": here.localizedString(forIdentifier: id) ?? id, "installed": installed.contains(id)]
        }
        .sorted { ($0["name"] as! String) < ($1["name"] as! String) }
      resolve(["available": true, "deviceLanguage": Locale.current.language.languageCode?.identifier ?? "en",
               "deviceLanguageName": here.localizedString(forLanguageCode: Locale.current.language.languageCode?.identifier ?? "en") ?? "",
               // ★ The phone's region, so 'English' means en-GB on a British phone, not the list's first (en-AU).
               "deviceRegion": Locale.current.region?.identifier ?? "",
               // ★ Translate INTO the phone's own variant where Apple's Translation has more than one (pt-BR, zh-Hant,
               //   en-GB) — the bare code would be Apple's default variant, not Kiko's Brazilian Portuguese.
               "deviceTranslateTo": await SpeechEngines.deviceTranslateTarget(),
               // ★ TRANSLATE TO is a list (Stuart, 2026-10-17: "that button for english needs to be a list too for
               //   people who are multilingual themselves") — every language Apple's Translation offers. JS folds it to
               //   one row per language (src/services/translateTargets.ts).
               "translateLanguages": await SpeechEngines.translateTargets(),
               "languages": langs])
    }
  }

  /// language: "auto" (or "auto:zh,pt-BR,…" — the prior order from JS) or a BCP-47 id. translateTo: "" = off, else a
  /// language id ("en", "pt-BR"). ★ No options any more: LANGUAGES I UNDERSTAND went on 2026-10-17 (Stuart: "not sure
  /// what the I understand thing is").
  @objc func start(_ language: String, translateTo: String) {
    guard #available(iOS 26.0, *) else { emit(["type": "error", "text": "Needs iOS 26 or later"]); return }
    (engineBox as? TranscribeEngine)?.stop()
    let eng = TranscribeEngine(owner: self, language: language, translateTo: translateTo)
    engineBox = eng
    eng.start()
  }

  /// ★ GET in the language row (Stuart, 2026-10-15: "an option to pop up the language downloader ourselves").
  /// Fetches a language's speech model without starting to listen — so AUTO can pick it from then on — and, when a
  /// translation target is set, puts up Apple's own translation-pack sheet. Resolves { ok, text } for the box.
  /// `keep`: a ★ language (or the phone's own) — it KEEPS its place; any other GET gives the place straight back.
  @objc func download(_ language: String, translateTo: String, keep: Bool,
                      resolver resolve: @escaping RCTPromiseResolveBlock, rejecter reject: @escaping RCTPromiseRejectBlock) {
    guard #available(iOS 26.0, *) else { resolve(["ok": false, "text": "Needs iOS 26 or later"]); return }
    Task {
      guard let loc = await SpeechEngines.supportedLocale(equivalentTo: Locale(identifier: language)) else {
        resolve(["ok": false, "text": "\(language) is not available for on-device speech"]); return
      }
      let code = loc.language.languageCode?.identifier ?? ""
      let nm = Locale.current.localizedString(forLanguageCode: code) ?? code
      let m = await SpeechEngines.module(for: loc)
      do {
        if !(await SpeechEngines.isPresent(loc, m)) { try await SpeechEngines.fetch(loc, m, keep: keep) }
        else if keep { await SpeechEngines.reserve(loc) }
      } catch {
        resolve(["ok": false, "text": "Could not download \(nm): \(error.localizedDescription)"]); return
      }
      var text = keep ? "\(nm) is kept on this device — AUTO can now pick it"
                      : "\(nm) is saved on this device — AUTO can now pick it"
      if !translateTo.isEmpty, code != TranscribePlaces.base(translateTo) {
        let dst = Locale.Language(identifier: translateTo)
        let dn = Locale.current.localizedString(forIdentifier: translateTo) ?? translateTo
        switch await LanguageAvailability().status(from: loc.language, to: dst) {
        case .installed: text += ", translating to \(dn)"
        case .supported:
          let ok = await TranslationDownloadHost.request(source: loc.language, target: dst)
          text += ok ? ", translating to \(dn)" : " (translation pack not downloaded)"
        default: text += " (this device cannot translate it to \(dn))"
        }
      }
      resolve(["ok": true, "text": text])
    }
  }

  /// ★★ THE 5 KEPT PLACES (Stuart, 2026-10-17 night): the phone's language + up to 4 ★ languages. `languages`: the ★
  /// list as locale ids, in order. Reserves the ★ ones that are on the device, keeps the phone's language if this app
  /// holds it, and gives back every other place this app holds. Resolves { kept: [ids] }. Called by JS at launch
  /// (once capabilities and the ★ list are known) and whenever a ★ changes.
  @objc func keep(_ languages: [String], resolver resolve: @escaping RCTPromiseResolveBlock,
                  rejecter reject: @escaping RCTPromiseRejectBlock) {
    guard #available(iOS 26.0, *) else { resolve(["kept": []]); return }
    Task { resolve(["kept": await SpeechEngines.reconcileKept(languages)]) }
  }

  /* ★ There is no REMOVE (2026-10-17). Apple offers no "delete this model now" — un-starring gives the place back and
   *   the system tidies the model away when it wants; languages are removed in the system settings. */

  /// The dial, for SSB auto-tune of the recogniser's feed ('usb'/'lsb' arm it; any change resets it).
  @objc func tuning(_ mode: String, freqHz: Double) {
    VibeTranscribeTap.shared.setTuning(mode, freqHz: freqHz)
  }

  @objc func stop() {
    guard #available(iOS 26.0, *) else { return }
    (engineBox as? TranscribeEngine)?.stop()
    engineBox = nil
  }
}

// MARK: - Speech engines

/// ★ Apple has TWO on-device transcribers and they cover different languages. SpeechTranscriber (the newer, better
/// model) has Mandarin, Cantonese, the Indian languages, English, French, German, Spanish, Italian, Japanese, Korean,
/// Portuguese; DictationTranscriber adds Russian, Arabic, Turkish, Polish, Dutch, Ukrainian, Vietnamese, Thai and more.
/// A language uses SpeechTranscriber when it can, DictationTranscriber otherwise — one list to the user.
@available(iOS 26.0, *)
enum SpeechEngines {
  static func supportedLocales() async -> [Locale] {
    var seen = Set<String>(), out: [Locale] = []
    for l in await SpeechTranscriber.supportedLocales + DictationTranscriber.supportedLocales
      where seen.insert(l.identifier(.bcp47)).inserted { out.append(l) }
    return out
  }
  static func installedLocales() async -> [Locale] {
    var seen = Set<String>(), out: [Locale] = []
    for l in await SpeechTranscriber.installedLocales + DictationTranscriber.installedLocales
      where seen.insert(l.identifier(.bcp47)).inserted { out.append(l) }
    return out
  }
  static func installedIds() async -> Set<String> { Set(await installedLocales().map { $0.identifier(.bcp47) }) }
  static func supportedLocale(equivalentTo l: Locale) async -> Locale? {
    if let s = await SpeechTranscriber.supportedLocale(equivalentTo: l) { return s }
    return await DictationTranscriber.supportedLocale(equivalentTo: l)
  }
  /* ★★ A LANGUAGE ALREADY ON THE DEVICE RUNS WITHOUT A PLACE. Measured 2026-10-16 on macOS 27's Speech framework (the
   *  same one iOS 26/27 ships) with the app's exact SpeechTranscriber AND DictationTranscriber configurations: a binary
   *  holding NO reservation transcribed en-GB and nl-BE speech correctly. status(forModules:) said `.supported` for
   *  them — it reports THIS app's reservation, not whether the model is on the device; installedLocales does that. */
  /// The model is on the device (any app or the system put it there) — or this app's own status says installed.
  static func isPresent(_ loc: Locale, _ m: any SpeechModule) async -> Bool {
    if await installedIds().contains(loc.identifier(.bcp47)) { return true }
    return await AssetInventory.status(forModules: [m]) == .installed
  }
  static func langName(_ id: String) -> String {
    let c = TranscribePlaces.base(id)
    return Locale.current.localizedString(forLanguageCode: c) ?? c
  }

  /* ★★★ KEPT, OR SAVED AND RELEASED (Stuart, 2026-10-17 night — the third answer that day, and the measured one).
   *  A model is downloaded under a reservation — Apple's API needs one, and `maximumReservedLocales` is 5 per app.
   *  Released straight after, it stays installed and runs with no place held (measured: Polish word-perfect) — BUT the
   *  system tidies released models away: of 13 locales downloaded and released, ru, ar, ro, id, uk, nl-NL and the Indian
   *  set were purged within minutes, with 12 GB free (memory speech_models_release_after_download). So the places are
   *  spent deliberately: THE PHONE'S LANGUAGE + UP TO 4 ★ LANGUAGES ARE KEPT; every other GET downloads and releases
   *  (it works until the system tidies it, then the list shows GET again). No REMOVE anywhere.
   *  ★ The assets must be fetched for the app's EXACT module (`module(for:)`): a `.longDictation` download left this
   *    configuration with no audio format — nothing ran. */
  /// Downloads `loc`'s model for `m` (the app's module). `keep`: hold the place; else give back the one it took.
  static func fetch(_ loc: Locale, _ m: any SpeechModule, keep: Bool) async throws {
    let before = Set(await AssetInventory.reservedLocales.map { $0.identifier(.bcp47) })
    let b = TranscribePlaces.base(loc.identifier(.bcp47))
    let held = before.contains { TranscribePlaces.base($0) == b }
    // ★ Every place kept (4 ★ + the phone's language): say so rather than take one from a kept language — a released
    //   language can be purged while the download runs.
    if !held && before.count >= AssetInventory.maximumReservedLocales { throw PlacesFull() }
    if let req = try await AssetInventory.assetInstallationRequest(supporting: [m]) {
      try await req.downloadAndInstall()
    }
    guard !keep else { return }
    // Give back only what this download took (Apple may file it under another spelling of the locale).
    for l in await AssetInventory.reservedLocales {
      let id = l.identifier(.bcp47)
      if !before.contains(id) && TranscribePlaces.base(id) == b { _ = await AssetInventory.release(reservedLocale: l) }
    }
  }
  struct PlacesFull: LocalizedError {
    var errorDescription: String? {
      "every place is kept for your \u{2605} languages and the phone\u{2019}s \u{2014} take a \u{2605} off one to GET another"
    }
  }
  /// Holds a place for a language already on the device (a ★ one). Quietly does nothing when full or refused.
  static func reserve(_ loc: Locale) async {
    let b = TranscribePlaces.base(loc.identifier(.bcp47))
    let now = await AssetInventory.reservedLocales
    if now.contains(where: { TranscribePlaces.base($0.identifier(.bcp47)) == b }) { return }
    guard now.count < AssetInventory.maximumReservedLocales else { return }
    _ = try? await AssetInventory.reserve(locale: loc)
  }
  /* ★★ RECONCILE (Stuart, 2026-10-17 night): kept = the phone's language + the ★ list. "Release anything else this app
   *  holds; reserve ★ ones that aren't."
   *  ★ THE PHONE'S LANGUAGE keeps a place only if THIS APP holds one for it — i.e. we downloaded it. A model the
   *    system provides (the usual case: the phone's dictation language) is installed with no reservation of ours and
   *    needs no place. (Unprovable from the API: a phone language an older build downloaded and released looks
   *    system-provided too, and so stays unreserved until it is GOT or starred again.)
   *  ★ A ★ language not on the device (never got, or tidied away) is left for GET — reserving nothing installs nothing. */
  static func reconcileKept(_ starIds: [String]) async -> [String] {
    let installed = await installedLocales()
    var held = await AssetInventory.reservedLocales
    var keepBases: [String] = []
    for id in starIds.prefix(4) where !keepBases.contains(TranscribePlaces.base(id)) { keepBases.append(TranscribePlaces.base(id)) }
    if let phone = await supportedLocale(equivalentTo: Locale.current) {
      let pb = TranscribePlaces.base(phone.identifier(.bcp47))
      if held.contains(where: { TranscribePlaces.base($0.identifier(.bcp47)) == pb }), !keepBases.contains(pb) { keepBases.append(pb) }
    }
    // Give back everything else — and a second spelling of a kept language (one place per language).
    var seen = Set<String>()
    for l in held {
      let b = TranscribePlaces.base(l.identifier(.bcp47))
      if !keepBases.contains(b) || !seen.insert(b).inserted { _ = await AssetInventory.release(reservedLocale: l) }
    }
    held = await AssetInventory.reservedLocales
    for id in starIds.prefix(4) {
      let b = TranscribePlaces.base(id)
      if held.contains(where: { TranscribePlaces.base($0.identifier(.bcp47)) == b }) { continue }
      let ins = installed.first(where: { $0.identifier(.bcp47) == id })
             ?? installed.first(where: { TranscribePlaces.base($0.identifier(.bcp47)) == b })
      guard let loc = ins, held.count < AssetInventory.maximumReservedLocales else { continue }
      if (try? await AssetInventory.reserve(locale: loc)) == true { held.append(loc) }
    }
    let kept = await AssetInventory.reservedLocales.map { $0.identifier(.bcp47) }
    NSLog("[VibeTranscriber] kept places: %@ (★ %@)", kept.joined(separator: ", "), starIds.joined(separator: ", "))
    return kept
  }
  /// TRANSLATE TO's list: every language Apple's Translation offers, as ids JS can store ('pt' = Brazilian, 'pt-PT',
  /// 'zh' / 'zh-TW'), named, and `installed` where the pack from the phone's language is already on the device.
  static func translateTargets() async -> [[String: Any]] {
    let avail = LanguageAvailability()
    let me = Locale.current.language
    var out: [[String: Any]] = [], seen = Set<String>()
    for l in await avail.supportedLanguages {
      let id = l.minimalIdentifier.replacingOccurrences(of: "_", with: "-")
      guard seen.insert(id).inserted else { continue }
      let same = l.languageCode?.identifier == me.languageCode?.identifier
      let st = same ? LanguageAvailability.Status.installed : await avail.status(from: me, to: l)
      out.append(["id": id, "name": Locale.current.localizedString(forIdentifier: id) ?? id, "installed": st == .installed])
    }
    return out.sorted { ($0["name"] as! String) < ($1["name"] as! String) }
  }
  /// The phone's language as a Translation target: a variant only when Apple's Translation offers several for the
  /// language and one matches the phone (region first, then script — zh-Hant for Taiwan/Hong Kong); else the bare code.
  static func deviceTranslateTarget() async -> String {
    let me = Locale.current.language
    guard let code = me.languageCode?.identifier else { return "en" }
    let variants = await LanguageAvailability().supportedLanguages.filter { $0.languageCode?.identifier == code }
    guard variants.count > 1 else { return code }
    if let r = me.region?.identifier,
       let v = variants.first(where: { $0.region?.identifier == r }) { return v.minimalIdentifier.replacingOccurrences(of: "_", with: "-") }
    // ★ The same id form as TRANSLATE TO's list (translateTargets: minimalIdentifier), so the phone's own variant
    //   is one row, not two.
    if let sc = (me.script ?? Locale.Language(identifier: me.maximalIdentifier).script)?.identifier,
       let v = variants.first(where: { ($0.script ?? Locale.Language(identifier: $0.maximalIdentifier).script)?.identifier == sc }) {
      return v.minimalIdentifier.replacingOccurrences(of: "_", with: "-")
    }
    return code
  }
  static func module(for loc: Locale) async -> any SpeechModule {
    if await SpeechTranscriber.supportedLocale(equivalentTo: loc) != nil {
      return SpeechTranscriber(locale: loc, transcriptionOptions: [], reportingOptions: [.volatileResults],
                               attributeOptions: [.transcriptionConfidence])
    }
    return DictationTranscriber(locale: loc, contentHints: [], transcriptionOptions: [.punctuation],
                                reportingOptions: [.volatileResults], attributeOptions: [.transcriptionConfidence])
  }
  /// One confidence per word (a run's confidence for each space-separated word in it) — the bake-off's count.
  static func confidences(_ rt: AttributedString) -> [Double] {
    var cs: [Double] = []
    for run in rt.runs {
      if let c = run[AttributeScopes.SpeechAttributes.ConfidenceAttribute.self] {
        let w = rt[run.range].characters.split(whereSeparator: { $0 == " " }).count
        for _ in 0..<max(1, w) { cs.append(c) }
      }
    }
    return cs
  }
}

// MARK: - Language scorer (AUTO's brute force)

/* ★★★ EVERY LANGUAGE ON THE DEVICE, ON THE SAME 10 s OF SPEECH (V12, 2026-10-17; docs/v12/LANGUAGE-ID-BAKEOFF.md).
 *  A fresh analyzer per language per window, at most 2 at a time (2-wide measured 1.64-1.83× faster; more is not
 *  worth it), each with the app's exact module. Keeping analyzers warm was measured to save nothing.
 *  ★★ THE API TRAP: finalize(through:) on an analyzer started with start(inputSequence:) HUNG FOREVER. So: one buffer
 *     through analyzeSequence(), then finalizeAndFinish(through:) the time it returned — and a 20 s guard that cancels
 *     a run that still does not finish, so one stuck model can never wedge AUTO.
 *  Then NaturalLanguage reads each transcript, and LanguageID.swift (pure, tested against the bake-off) scores it. */
@available(iOS 26.0, *)
enum LanguageScorer {
  struct Measured: Sendable {
    let heard: LanguageID.Heard
    let text: String
    let seconds: Double          // this language's analysis, wall (preparation + analysis)
  }

  static func score(_ samples: [Float], rate: Double, locales: [Locale], width: Int, priority: TaskPriority) async -> [Measured] {
    await withTaskGroup(of: Measured?.self) { g in
      var next = 0, out: [Measured] = []
      func add() {
        guard next < locales.count else { return }
        let l = locales[next]; next += 1
        g.addTask(priority: priority) { await one(l, samples, rate, priority) }
      }
      for _ in 0..<max(1, width) { add() }
      for await m in g { if let m { out.append(m) }; add() }
      return out
    }
  }

  static func one(_ loc: Locale, _ samples: [Float], _ rate: Double, _ priority: TaskPriority) async -> Measured? {
    let t0 = Date()
    let m = await SpeechEngines.module(for: loc)
    guard let fmt = await SpeechAnalyzer.bestAvailableAudioFormat(compatibleWith: [m]),
          let buf = buffer(samples, rate: rate, to: fmt) else { return nil }
    let an = SpeechAnalyzer(modules: [m], options: .init(priority: priority, modelRetention: .whileInUse))
    let collect = Task { () -> (String, [Double]) in
      var texts: [String] = [], cs: [Double] = []
      do {
        if let st = m as? SpeechTranscriber {
          for try await r in st.results where r.isFinal { texts.append(String(r.text.characters)); cs += SpeechEngines.confidences(r.text) }
        } else if let dt = m as? DictationTranscriber {
          for try await r in dt.results where r.isFinal { texts.append(String(r.text.characters)); cs += SpeechEngines.confidences(r.text) }
        }
      } catch {}
      return (texts.joined(separator: " ").trimmingCharacters(in: .whitespacesAndNewlines), cs)
    }
    let watchdog = Task {
      try? await Task.sleep(nanoseconds: 20_000_000_000)
      if !Task.isCancelled { await an.cancelAndFinishNow() }
    }
    do {
      try await an.prepareToAnalyze(in: fmt)
      let (stream, cont) = AsyncStream<AnalyzerInput>.makeStream()
      cont.yield(AnalyzerInput(buffer: buf)); cont.finish()
      if let last = try await an.analyzeSequence(stream) { try await an.finalizeAndFinish(through: last) }
      else { await an.cancelAndFinishNow() }
    } catch {
      await an.cancelAndFinishNow()
    }
    let (text, cs) = await collect.value
    watchdog.cancel()
    let secs = Double(samples.count) / max(1, rate)
    let (nl, top) = cs.isEmpty ? (0.0, "") : nlOwn(text, loc)
    let heard = LanguageID.Heard(id: loc.identifier(.bcp47), words: cs.count,
                                 meanConf: cs.isEmpty ? 0 : cs.reduce(0, +) / Double(cs.count),
                                 tokensPerSecond: Double(cs.count) / max(0.1, secs), nlOwn: nl, nlTop: top)
    return Measured(heard: heard, text: text, seconds: Date().timeIntervalSince(t0))
  }

  /// NaturalLanguage's probability that `text` is in `loc`'s language, and its top language (raw). Mandarin counts
  /// Simplified or Traditional, Cantonese Traditional (the bake-off's lidbench, exactly).
  static func nlOwn(_ text: String, _ loc: Locale) -> (Double, String) {
    let r = NLLanguageRecognizer()
    r.processString(text)
    let h = r.languageHypotheses(withMaximum: 5)
    let code = loc.language.languageCode?.identifier ?? ""
    let want: NLLanguage = code == "zh" ? .simplifiedChinese : code == "yue" ? .traditionalChinese : NLLanguage(rawValue: code)
    var p = h[want] ?? 0
    if code == "zh" { p = max(p, h[.traditionalChinese] ?? 0) }
    return (p, h.max { $0.value < $1.value }?.key.rawValue ?? "")
  }

  /// The window (16 kHz float mono) in the model's own format.
  static func buffer(_ s: [Float], rate: Double, to fmt: AVAudioFormat) -> AVAudioPCMBuffer? {
    guard !s.isEmpty,
          let src = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: rate, channels: 1, interleaved: false),
          let b = AVAudioPCMBuffer(pcmFormat: src, frameCapacity: AVAudioFrameCount(s.count)) else { return nil }
    b.frameLength = AVAudioFrameCount(s.count)
    s.withUnsafeBufferPointer { _ = memcpy(b.floatChannelData![0], $0.baseAddress!, s.count * MemoryLayout<Float>.size) }
    if fmt == src { return b }
    guard let conv = AVAudioConverter(from: src, to: fmt),
          let out = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: AVAudioFrameCount(Double(s.count) * fmt.sampleRate / rate) + 4096)
    else { return nil }
    var fed = false
    var err: NSError?
    conv.convert(to: out, error: &err) { _, st in
      if fed { st.pointee = .endOfStream; return nil }
      fed = true; st.pointee = .haveData; return b
    }
    return err == nil && out.frameLength > 0 ? out : nil
  }
}

// MARK: - Engine

@available(iOS 26.0, *)
final class TranscribeEngine {
  private weak var owner: VibeTranscriber?
  private let language: String
  /// AUTO's prior order from JS ('auto:zh,pt-BR,en'): EiBi's languages for this frequency, the receiver's main language,
  /// the ★ list, the receiver area's (src/services/transcribeOrder.ts). It orders the shortlist and breaks ties — it
  /// never decides (EiBi was wrong for 3 of the frequencies recorded for the bake-off).
  private let hints: [String]
  private let translateTo: String
  /// Everything below is touched only on `q`.
  private let q = DispatchQueue(label: "com.vibesdr.transcribe", qos: .utility)
  private var stopped = false
  /// ★ THE ONE LIVE LANE: the chosen language, or the one AUTO locked on. It alone feeds the box and the translator,
  ///   so translation gets whole sentences (Stuart: "translation works better once it locks on").
  private var live: Lane?
  private var liveWant = ""                    // the locale a lane is being opened for (a newer lock wins)
  private var translator: Translator?
  /* ★★★ AUTO (rewritten 2026-10-17 night, from the bake-off — Stuart's decision A). No rounds of four, no probing lanes,
   *    no challenger lane, no "best seen", no confidence-collapse re-probe: every language in the pool is scored on the
   *    same buffered 10 s of SPEECH (LanguageScorer), rule H2 decides, LanguageID.Lock locks fast and holds, and while
   *    locked the same check runs in the background and switches only on 2 consecutive wins by 0.10. */
  private var pool: [Locale] = []
  private var lock = LanguageID.Lock()
  private var costs = LanguageID.Costs()
  private var widen = LanguageID.Widen()
  private var budget = LanguageID.budgetSeconds
  private var critical = false
  private var scoring = false
  private var checkEvery = 1                   // while locked: background check every Nth speech window
  private var checkTick = 0
  private var unrecognised = 0                 // unconvincing speech windows since the last lock (for the box)
  private var window: [Float] = []             // speech-gated audio being gathered, at `windowRate`
  private var windowRate = 16000.0
  private var observers: [NSObjectProtocol] = []
  /* ★★ THE STALL WATCHDOG (V12 research, 2026-10-16). SpeechTranscriber was measured going quiet on medium SSB — words
   *    for 7-15 s, then nothing for the rest of the clip; a 12 s no-result watchdog that starts a FRESH analyzer took
   *    3702 kHz from 19 words to 85, and fired 0 times on clean VOLMET. So: audio FLOWING (over a low RMS floor — never
   *    squelch or mute silence) and no volatile or final result for 12 s → the live lane gets a fresh analyzer, same
   *    language; the translator and the box's text are kept. A restart that brings nothing doubles the wait (to 96 s),
   *    so a long stretch of band noise costs a few restarts, not one every 12 s. */
  private static let stallAfter: TimeInterval = 12
  private static let stallMax: TimeInterval = 96
  private static let stallAudible: TimeInterval = 10      // of the window, this much must have been audio
  private static let stallRmsFloor: Float = 0.002          // ≈ -54 dBFS — also AUTO's speech gate
  private var stallRestarts = 0

  final class Lane {
    let locale: Locale
    let transcriber: any SpeechModule
    let analyzer: SpeechAnalyzer
    let input: AsyncStream<AnalyzerInput>.Continuation
    let format: AVAudioFormat
    var converter: AVAudioConverter?
    var tasks: [Task<Void, Never>] = []
    // ── stall watchdog (q only) ──
    var lastResult = Date()
    var audible: TimeInterval = 0
    var stallAfter = TranscribeEngine.stallAfter
    var restarting = false
    init(locale: Locale, transcriber: any SpeechModule, analyzer: SpeechAnalyzer,
         input: AsyncStream<AnalyzerInput>.Continuation, format: AVAudioFormat) {
      self.locale = locale; self.transcriber = transcriber; self.analyzer = analyzer
      self.input = input; self.format = format
    }
    func shutdown() {
      input.finish()
      tasks.forEach { $0.cancel() }
      let a = analyzer
      Task { await a.cancelAndFinishNow() }
    }
  }

  init(owner: VibeTranscriber, language: String, translateTo: String) {
    self.owner = owner; self.translateTo = translateTo
    if language == "auto" || language.hasPrefix("auto:") {
      self.language = "auto"
      self.hints = language.hasPrefix("auto:")
        ? language.dropFirst(5).split(separator: ",").map(String.init).filter { !$0.isEmpty } : []
    } else {
      self.language = language; self.hints = []
    }
  }

  private func status(_ s: String) { owner?.emit(["type": "status", "text": s]) }
  private func name(_ l: Locale) -> String { Locale.current.localizedString(forIdentifier: l.identifier(.bcp47)) ?? l.identifier(.bcp47) }
  /// The short name the box shows ("Chinese", "Cantonese", "Russian") — the region is noise there.
  private func shortName(_ l: Locale) -> String {
    let c = l.language.languageCode?.identifier ?? ""
    return Locale.current.localizedString(forLanguageCode: c) ?? c.uppercased()
  }
  private func shortName(_ id: String) -> String { shortName(Locale(identifier: id)) }

  func start() {
    Task { await self.begin() }
  }

  func stop() {
    VibeTranscribeTap.shared.setSink(nil)
    q.async {
      if self.stallRestarts > 0 { NSLog("[VibeTranscriber] stall watchdog: %d restart(s) this session", self.stallRestarts) }
      self.stopped = true
      self.live?.shutdown(); self.live = nil
      self.observers.forEach { NotificationCenter.default.removeObserver($0) }
      self.observers.removeAll()
    }
  }

  /* ★★ AUTO TESTS ONLY WHAT IS ALREADY ON THE PHONE (Stuart, 2026-10-15). A speech model is 300–450 MB, so probing by
   *    download could pull a gigabyte over mobile data. A likely language that is not here is SAID instead — the box's
   *    own notice offers the DOWNLOAD (SDRScreen, from EiBi and the phone's list).
   *  ★★★ THE ORDER IS BUILT IN JS (src/services/transcribeOrder.ts, tested): EiBi's languages, the receiver's MAIN
   *    language (in its variant — 'pt-BR' — when that one is installed), the ★ list, the receiver area's others. Here
   *    the phone's own languages and everything else installed follow, and anything not installed is dropped.
   *  ★★ ONE ENTRY PER LANGUAGE, NOT PER VARIANT (Stuart on V12 479: "Trying English, English, Spanish, French"). Each
   *    language once: the variant JS named explicitly if installed, else the phone's own, else the hint's, else the
   *    first installed. Read again before every window — the system can tidy a model away mid-session. */
  private func buildPool() async -> [Locale] {
    let installedList = await SpeechEngines.installedLocales()
    let installed = Set(installedList.map { $0.identifier(.bcp47) })
    var order: [String] = []                          // base codes, in prior order
    var pick: [String: (loc: Locale, rank: Int)] = [:] // base → the variant to run (lower rank wins)
    func offer(_ l: Locale, rank: Int) {
      let id = l.identifier(.bcp47), b = TranscribePlaces.base(id)
      if !order.contains(b) { order.append(b) }
      guard installed.contains(id) else { return }
      if let cur = pick[b], cur.rank <= rank { return }
      pick[b] = (l, rank)
    }
    for code in hints {
      let want = code == "zh" ? "zh-CN" : code == "yue" ? "yue-CN" : code
      let explicit = code.contains("-") || code.contains("_")       // a variant JS chose on purpose
      if let l = await SpeechEngines.supportedLocale(equivalentTo: Locale(identifier: want)) { offer(l, rank: explicit ? 0 : 2) }
    }
    for id in Locale.preferredLanguages {
      if let l = await SpeechEngines.supportedLocale(equivalentTo: Locale(identifier: id)) { offer(l, rank: 1) }
    }
    // ★ Apple's "multiple languages of India" model is not a language to identify.
    for l in installedList where TranscribePlaces.base(l.identifier(.bcp47)) != "mul" { offer(l, rank: 3) }
    return order.compactMap { pick[$0]?.loc }
  }

  private func begin() async {
    status("Preparing on-device speech…")
    let tapDial: (Float, [Float]) -> Void = { [weak self] d, st in self?.q.async { self?.onDial(d, st) } }
    let sink: (AVAudioPCMBuffer) -> Void = { [weak self] buf in self?.q.async { self?.feed(buf) } }
    if language != "auto" {
      guard let loc = await SpeechEngines.supportedLocale(equivalentTo: Locale(identifier: language)) else {
        owner?.emit(["type": "error", "text": "\(language) is not supported for on-device speech"]); return
      }
      let t = await SpeechEngines.module(for: loc)
      do {
        // ★ On the device = runs (measured: no place needed). Choosing a language that is not here downloads it —
        //   JS asked first (size, Wi-Fi) — and gives the place back like any GET.
        if !(await SpeechEngines.isPresent(loc, t)) {
          status("Downloading the \(shortName(loc)) speech model (once, then it works offline)…")
          try await SpeechEngines.fetch(loc, t, keep: false)
        }
        guard let lane = try await makeLane(loc, module: t) else {
          owner?.emit(["type": "error", "text": "Could not start \(name(loc)) speech"]); return
        }
        q.async {
          guard !self.stopped else { lane.shutdown(); return }
          self.live = lane
          self.choose(lane)
          VibeTranscribeTap.shared.setSink(sink, onDial: tapDial)
        }
      } catch {
        owner?.emit(["type": "error", "text": "Could not start \(name(loc)) speech: \(error.localizedDescription)"])
      }
      return
    }
    let p = await buildPool()
    guard !p.isEmpty else {
      owner?.emit(["type": "error", "text": "No speech language is on this device yet — GET one in the LANGUAGE list"]); return
    }
    let names = p.map { shortName($0) }
    q.async {
      guard !self.stopped else { return }
      self.pool = p
      self.watchPower()
      // ★ The search shows in the BOX (Stuart, 10-15) — the status line alone flickers past. JS writes "Trying …".
      let shown = names.count > 8 ? names.prefix(8).joined(separator: ", ") + " and \(names.count - 8) more" : names.joined(separator: ", ")
      self.owner?.emit(["type": "trying", "text": shown])
      self.status("Listening — detecting the language from 10 s of speech…")
      VibeTranscribeTap.shared.setSink(sink, onDial: tapDial)
      // ★ V12: with the RECORD BUFFER on, the speech already heard counts towards the first window.
      VibeTranscribeTap.shared.requestPrime(seconds: LanguageID.windowSeconds) { [weak self] b in
        self?.q.async { self?.primeWindow(b) }
      }
    }
  }

  /// q only — the record buffer's recent audio (16 kHz mono, older than anything the live feed has delivered), gated
  /// like live audio (20 ms blocks over the speech floor) and put IN FRONT of what the window already holds.
  private func primeWindow(_ b: AVAudioPCMBuffer) {
    guard !stopped, language == "auto", lock.locked == nil, b.format.sampleRate == windowRate,
          let p = b.floatChannelData?[0] else { return }
    let n = Int(b.frameLength), blk = Int(windowRate / 50)
    var speech: [Float] = []
    speech.reserveCapacity(n)
    var i = 0
    while i < n {
      let m = min(blk, n - i)
      var rms: Float = 0
      vDSP_rmsqv(p + i, 1, &rms, vDSP_Length(m))
      if rms > Self.stallRmsFloor { speech.append(contentsOf: UnsafeBufferPointer(start: p + i, count: m)) }
      i += m
    }
    guard !speech.isEmpty else { return }
    window = speech + window
    let need = Int(LanguageID.windowSeconds * windowRate)
    NSLog("[VibeTranscriber] AUTO: primed with %.1f s of speech from the record buffer", Double(speech.count) / windowRate)
    if window.count >= need {
      let w = Array(window.suffix(need))      // the newest 10 s
      window.removeAll(keepingCapacity: true)
      windowReady(w)
    }
  }

  /// ★ The device budget follows the phone's state (bake-off: re-evaluate on thermal state and Low Power Mode). The
  ///   measured costs are thrown away too — a hot or slowed phone is measured afresh. q only.
  private func watchPower() {
    let nc = NotificationCenter.default
    let again: (Notification) -> Void = { [weak self] _ in self?.q.async { self?.reevaluate(log: true) } }
    observers.append(nc.addObserver(forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: nil, using: again))
    observers.append(nc.addObserver(forName: .NSProcessInfoPowerStateDidChange, object: nil, queue: nil, using: again))
    reevaluate(log: false)
  }
  private func reevaluate(log: Bool) {
    let pi = ProcessInfo.processInfo
    budget = LanguageID.budget(thermal: pi.thermalState.rawValue, lowPower: pi.isLowPowerModeEnabled)
    critical = pi.thermalState == .critical
    if log {
      costs = LanguageID.Costs()
      NSLog("[VibeTranscriber] AUTO budget %.1f s per window (thermal %d, low power %d)%@", budget,
            pi.thermalState.rawValue, pi.isLowPowerModeEnabled ? 1 : 0, critical ? " — background check suspended" : "")
    }
  }

  /// One analyzer for `loc` on `module`, started, with its results task: the live lane (and a stalled lane's fresh one).
  private func makeLane(_ loc: Locale, module t: any SpeechModule) async throws -> Lane? {
    guard let fmt = await SpeechAnalyzer.bestAvailableAudioFormat(compatibleWith: [t]) else { return nil }
    let (stream, cont) = AsyncStream<AnalyzerInput>.makeStream(bufferingPolicy: .bufferingNewest(64))
    let a = SpeechAnalyzer(modules: [t], options: .init(priority: .utility, modelRetention: .whileInUse))
    try await a.prepareToAnalyze(in: fmt)
    try await a.start(inputSequence: stream)
    let lane = Lane(locale: loc, transcriber: t, analyzer: a, input: cont, format: fmt)
    lane.tasks.append(Task { [weak self] in
      do {
        if let st = t as? SpeechTranscriber {
          for try await r in st.results { self?.q.async { self?.onResult(lane, r.text, r.isFinal) } }
        } else if let dt = t as? DictationTranscriber {
          for try await r in dt.results { self?.q.async { self?.onResult(lane, r.text, r.isFinal) } }
        }
      } catch {
        self?.q.async { self?.onLaneError(lane, error) }
      }
    })
    return lane
  }

  /// q only — AUTO locked or switched: the live lane moves to `loc`. The old lane keeps writing until the new one is
  /// ready, so a switch drops nothing; `lead` (the deciding window's transcript) is written first.
  private func goLive(_ loc: Locale, lead: String?) {
    let id = loc.identifier(.bcp47)
    liveWant = id
    Task { [weak self] in
      guard let self else { return }
      let lane = try? await self.makeLane(loc, module: await SpeechEngines.module(for: loc))
      self.q.async {
        guard let lane else {
          if self.liveWant == id { self.owner?.emit(["type": "error", "text": "Could not start \(self.name(loc)) speech"]) }
          return
        }
        guard !self.stopped, self.liveWant == id else { lane.shutdown(); return }
        self.live?.shutdown()
        self.live = lane
        self.choose(lane)
        if let lead, !lead.isEmpty { self.deliver(lead, locale: loc) }   // nothing heard while deciding is lost
      }
    }
  }

  // q only — the stalled live lane gets a fresh analyzer for the same language, then the old one is shut down.
  private func restartLane(_ old: Lane, quiet: TimeInterval) {
    old.restarting = true
    stallRestarts += 1
    let next = min(old.stallAfter * 2, Self.stallMax)
    NSLog("[VibeTranscriber] stall watchdog: %@ — no result for %.0f s with %.0f s of audio; fresh analyzer (#%d this session)",
          old.locale.identifier(.bcp47), quiet, old.audible, stallRestarts)
    let loc = old.locale
    Task { [weak self] in
      guard let self else { return }
      let fresh = try? await self.makeLane(loc, module: await SpeechEngines.module(for: loc))
      self.q.async {
        guard let fresh else {
          old.restarting = false; old.lastResult = Date(); old.audible = 0; old.stallAfter = next
          return
        }
        guard !self.stopped, self.live === old else { fresh.shutdown(); return }
        fresh.stallAfter = next
        self.live = fresh
        old.shutdown()
      }
    }
  }

  // q only — SSB auto-tune moved the recogniser's copy: the box's header says so, quietly (only on a change).
  private func onDial(_ d: Float, _ st: [Float]) {
    guard !stopped else { return }
    NSLog("[VibeTranscriber] ssb auto-tune: dial correction %.0f Hz (estimate %.1f Hz, confidence %.2f, %.0f s voiced, %d estimates, %d corrections)",
          d, st.count == 7 ? st[0] : 0, st.count == 7 ? st[1] : 0, st.count == 7 ? st[2] : 0,
          st.count == 7 ? Int(st[4]) : 0, st.count == 7 ? Int(st[5]) : 0)
    owner?.emit(["type": "tune", "hz": Int(d.rounded())])
  }

  // q only
  private func feed(_ mono: AVAudioPCMBuffer) {
    guard !stopped else { return }
    var rms: Float = 0
    if let p = mono.floatChannelData?[0], mono.frameLength > 0 { vDSP_rmsqv(p, 1, &rms, vDSP_Length(mono.frameLength)) }
    let dur = Double(mono.frameLength) / mono.format.sampleRate
    if let lane = live, !lane.restarting {
      if rms > Self.stallRmsFloor { lane.audible += dur }
      let quiet = Date().timeIntervalSince(lane.lastResult)
      if quiet >= lane.stallAfter, lane.audible >= Self.stallAudible { restartLane(lane, quiet: quiet) }
    }
    if let lane = live {
      if lane.converter == nil || lane.converter!.inputFormat != mono.format {
        lane.converter = AVAudioConverter(from: mono.format, to: lane.format)
      }
      if let conv = lane.converter {
        let ratio = lane.format.sampleRate / mono.format.sampleRate
        let cap = AVAudioFrameCount(Double(mono.frameLength) * ratio + 64)
        if let out = AVAudioPCMBuffer(pcmFormat: lane.format, frameCapacity: cap) {
          var fed = false
          var err: NSError?
          conv.convert(to: out, error: &err) { _, st in
            if fed { st.pointee = .noDataNow; return nil }
            fed = true; st.pointee = .haveData; return mono
          }
          if err == nil, out.frameLength > 0 { lane.input.yield(AnalyzerInput(buffer: out)) }
        }
      }
    }
    /* ★★ AUTO'S WINDOW: only audio over the floor is gathered — squelch (the tap sends zeros), mute and dead air are
     *    never scored — and every 10 s of it is one window. The live lane above is untouched by any of this. */
    guard language == "auto", rms > Self.stallRmsFloor, let p = mono.floatChannelData?[0], mono.frameLength > 0 else { return }
    if mono.format.sampleRate != windowRate { window.removeAll(); windowRate = mono.format.sampleRate }
    window.append(contentsOf: UnsafeBufferPointer(start: p, count: Int(mono.frameLength)))
    if Double(window.count) >= LanguageID.windowSeconds * windowRate {
      let w = window
      window.removeAll(keepingCapacity: true)
      windowReady(w)
    }
  }

  // q only — 10 s of speech gathered: score it, unless the last one is still being scored (then it is dropped —
  // decoders never wait) or, while locked, the background check is not due / the phone is too hot.
  private func windowReady(_ w: [Float]) {
    guard !scoring else { NSLog("[VibeTranscriber] AUTO: window dropped (still scoring the last)"); return }
    let background = lock.locked != nil
    if background {
      if critical { return }
      checkTick += 1
      if checkTick < checkEvery { return }
      checkTick = 0
    }
    scoring = true
    let plan = LanguageID.plan(costs, budget: budget)
    let step = widen.step, locked = lock.locked?.id, rate = windowRate
    // ★ Background priority while locked: the check must never slow the live lane, the box or the translator.
    Task.detached(priority: background ? .background : .utility) { [weak self] in
      guard let self else { return }
      let p = await self.buildPool()
      let ids = p.map { $0.identifier(.bcp47) }
      let picks = LanguageID.pick(pool: ids, locked: locked, perWindow: plan.perWindow, step: step)
      let locs = picks.compactMap { id in p.first { $0.identifier(.bcp47) == id } }
      let t0 = Date()
      let got = await LanguageScorer.score(w, rate: rate, locales: locs, width: plan.twoWide ? 2 : 1,
                                           priority: background ? .background : .utility)
      let wall = Date().timeIntervalSince(t0)
      self.q.async { self.onScored(got, wall: wall, plan: plan, pool: p) }
    }
  }

  // q only — one window scored.
  private func onScored(_ got: [LanguageScorer.Measured], wall: TimeInterval, plan: LanguageID.Plan, pool p: [Locale]) {
    scoring = false
    guard !stopped else { return }
    pool = p
    if plan.twoWide && got.count >= 2 { costs.addTwoWide(languages: got.count, wall: wall) }
    else { for g in got { costs.addSequential(g.heard.id, g.seconds) } }
    checkEvery = p.count <= plan.perWindow ? 1 : 3      // a shortlist device checks every 30 s, not every 10 s
    guard !got.isEmpty else { return }
    let ranked = LanguageID.rank(got.map { $0.heard }, prior: p.map { $0.identifier(.bcp47) })
    let convinced = LanguageID.convincing(ranked)
    widen.after(convincing: convinced, slices: LanguageID.slices(pool: p.count, perWindow: plan.perWindow))
    let summary = ranked.prefix(4).map { "\(shortName($0.id)) \(Int(($0.score * 100).rounded())) %" }.joined(separator: " · ")
    NSLog("[VibeTranscriber] AUTO window: %d languages in %.2f s (%@): %@", got.count, wall, plan.twoWide ? "2-wide" : "1-wide", summary)
    switch lock.observe(ranked) {
    case .locked(let id):
      unrecognised = 0
      guard let loc = p.first(where: { $0.identifier(.bcp47) == id }) else { return }
      owner?.emit(["type": "tried", "text": "Heard: \(summary)"])
      goLive(loc, lead: got.first(where: { $0.heard.id == id })?.text)
    case .switched(let from, let to):
      guard let loc = p.first(where: { $0.identifier(.bcp47) == to }) else { return }
      // ★ Never disturbs the live lane until the new language is ready (goLive); the box says why.
      owner?.emit(["type": "tried", "text": "Sounds more like \(shortName(to)) than \(shortName(from)) — switching"])
      goLive(loc, lead: nil)
    case .challenging(let id, let n):
      NSLog("[VibeTranscriber] AUTO: %@ ahead of %@ (%d of %d)", id, lock.locked?.id ?? "-", n, LanguageID.switchWins)
    case .nothing:
      // ★ REJECTION (bake-off: 0 false answers on 234 noise, data, music and uninstalled-language windows): nothing
      //   reached 0.40 → say so, never guess. Once in the box, then the status line carries it.
      unrecognised += 1
      if unrecognised == 1 {
        owner?.emit(["type": "tried", "text": "No language recognised yet (closest: \(summary)) — listening on"])
      }
      status(p.count > plan.perWindow
        ? "Listening — no language recognised yet (\(min(plan.perWindow, p.count)) of \(p.count) languages per 10 s on this device)"
        : "Listening — no language recognised yet")
    case .held:
      break
    }
  }

  // q only
  private func onResult(_ lane: Lane, _ rt: AttributedString, _ isFinal: Bool) {
    guard !stopped, lane === live else { return }
    // ★ Any result, volatile or final, is life: the stall clock starts again and the next wait is the short one.
    lane.lastResult = Date(); lane.audible = 0; lane.stallAfter = Self.stallAfter
    let text = String(rt.characters).trimmingCharacters(in: .whitespacesAndNewlines)
    guard !text.isEmpty else { return }
    if !isFinal { owner?.emit(["type": "partial", "text": text]); return }
    deliver(text, locale: lane.locale)
  }

  // q only
  private func choose(_ lane: Lane) {
    let lang = lane.locale.language.languageCode?.identifier ?? ""
    owner?.emit(["type": "lang", "text": lane.locale.identifier(.bcp47), "name": shortName(lane.locale),
                 "detected": language == "auto"])
    status("Listening — \(shortName(lane.locale))\(language == "auto" ? " (detected)" : "")")
    translator = nil
    // ★ Speech already in the target language is never translated.
    if !translateTo.isEmpty, lang != TranscribePlaces.base(translateTo) {
      let tr = Translator(source: lane.locale.language, target: Locale.Language(identifier: translateTo)) { [weak self] s in
        self?.status(s)
      }
      translator = tr
      Task { await tr.prepare() }
    }
  }

  // q only
  private func deliver(_ text: String, locale: Locale) {
    let lang = shortName(locale)
    guard let tr = translator else {
      owner?.emit(["type": "final", "text": text, "lang": lang]); return
    }
    Task { [weak self] in
      let out = await tr.translate(text)
      self?.owner?.emit(["type": "final", "text": text, "lang": lang, "translated": out ?? ""])
    }
  }

  // q only
  private func onLaneError(_ lane: Lane, _ e: Error) {
    guard !stopped, live === lane else { return }
    owner?.emit(["type": "error", "text": "Speech stopped: \(e.localizedDescription)"])
  }
}

// MARK: - Translation (on-device language packs)

@available(iOS 26.0, *)
final actor Translator {
  private let source: Locale.Language
  private let target: Locale.Language
  private let say: (String) -> Void
  private var session: TranslationSession?
  private var failed = false

  init(source: Locale.Language, target: Locale.Language, say: @escaping (String) -> Void) {
    self.source = source; self.target = target; self.say = say
  }

  private func nm(_ l: Locale.Language) -> String {
    Locale.current.localizedString(forLanguageCode: l.languageCode?.identifier ?? "") ?? (l.languageCode?.identifier ?? "?")
  }

  func prepare() async {
    let avail = LanguageAvailability()
    switch await avail.status(from: source, to: target) {
    case .installed:
      session = TranslationSession(installedSource: source, target: target)
    case .supported:
      // ★ A missing language pack needs Apple's own download sheet — never downloaded silently.
      say("Translation \(nm(source)) → \(nm(target)) needs a language pack — confirm the download")
      let ok = await TranslationDownloadHost.request(source: source, target: target)
      if ok, await avail.status(from: source, to: target) == .installed {
        session = TranslationSession(installedSource: source, target: target)
        say("Translating \(nm(source)) → \(nm(target)) on the device")
      } else {
        failed = true
        say("Translation pack not downloaded — showing the original only")
      }
    default:
      failed = true
      say("Translation \(nm(source)) → \(nm(target)) is not available on this device")
    }
  }

  func translate(_ text: String) async -> String? {
    guard !failed, let s = session else { return nil }
    return try? await s.translate(text).targetText
  }
}

/// Presents Apple's language-pack download sheet via a throwaway SwiftUI host, then dismisses.
@available(iOS 26.0, *)
enum TranslationDownloadHost {
  @MainActor static func requestOnMain(source: Locale.Language, target: Locale.Language,
                                       done: @escaping (Bool) -> Void) {
    guard let root = UIApplication.shared.connectedScenes
      .compactMap({ ($0 as? UIWindowScene)?.keyWindow?.rootViewController }).first else { done(false); return }
    var top = root
    while let p = top.presentedViewController { top = p }
    var host: UIHostingController<DownloadView>?
    let view = DownloadView(config: .init(source: source, target: target)) { ok in
      host?.dismiss(animated: false)
      host = nil
      done(ok)
    }
    let h = UIHostingController(rootView: view)
    h.view.backgroundColor = .clear
    h.modalPresentationStyle = .overFullScreen
    host = h
    top.present(h, animated: false)
  }

  static func request(source: Locale.Language, target: Locale.Language) async -> Bool {
    await withCheckedContinuation { c in
      DispatchQueue.main.async {
        requestOnMain(source: source, target: target) { c.resume(returning: $0) }
      }
    }
  }

  struct DownloadView: View {
    let config: TranslationSession.Configuration
    let done: (Bool) -> Void
    var body: some View {
      Color.clear
        .translationTask(config) { session in
          do { try await session.prepareTranslation(); done(true) }
          catch { done(false) }
        }
    }
  }
}
