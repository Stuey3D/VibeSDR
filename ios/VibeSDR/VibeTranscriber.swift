import Foundation
import AVFoundation
import Speech
import Translation
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
 *     voice activity, nothing for language). So AUTO is done the honest way: up to four transcribers, one per
 *     candidate language, listen to the SAME audio for the first few phrases, and the one that is most confident
 *     about what it heard wins. A wrong-language model still produces words — it just does not believe them.
 *     Candidates are only languages whose speech model is already on the phone plus the phone's own language:
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
      await SpeechEngines.releaseInstalled()
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
               //   people who are multilingual themselves") — every language Apple's Translation offers.
               "translateLanguages": await SpeechEngines.translateTargets(),
               "languages": langs])
    }
  }

  /// language: "auto" or a BCP-47 id. translateTo: "" = off, else a language code ("en"). options: JSON
  /// { understood: ["fr"] } — the languages the user reads without translation (TxOptions).
  @objc func start(_ language: String, translateTo: String, options: String) {
    guard #available(iOS 26.0, *) else { emit(["type": "error", "text": "Needs iOS 26 or later"]); return }
    (engineBox as? TranscribeEngine)?.stop()
    let eng = TranscribeEngine(owner: self, language: language, translateTo: translateTo, options: TxOptions(options))
    engineBox = eng
    eng.start()
  }

  /// ★ DOWNLOAD… in the language row (Stuart, 2026-10-15: "an option to pop up the language downloader ourselves").
  /// Fetches a language's speech model without starting to listen — so AUTO can pick it from then on — and, when a
  /// translation target is set, puts up Apple's own translation-pack sheet. Resolves { ok, text } for the box.
  @objc func download(_ language: String, translateTo: String,
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
        if !(await SpeechEngines.isPresent(loc, m)) { try await SpeechEngines.fetch(loc, m) }
      } catch {
        resolve(["ok": false, "text": "Could not download \(nm): \(error.localizedDescription)"]); return
      }
      var text = "\(nm) is saved on this device — AUTO can now pick it"
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

  /* ★ There is no REMOVE any more (2026-10-17). It existed to give back one of Apple's 5 places when a GET was
   *   refused; downloads no longer keep a place (SpeechEngines.fetch), so it would only ever release nothing — a dead
   *   control (AGENTS.md). Apple offers no "delete this model now"; iOS frees a released model's space itself. */

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

/// start() options from JS: { understood: ["fr"] } (JSON; absent or bad = none).
struct TxOptions {
  /// Base codes the user reads without translation (Stuart, 2026-10-17: multilingual users). The target always is.
  var understood: Set<String> = []
  init(_ json: String) {
    guard let d = json.data(using: .utf8),
          let o = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { return }
    understood = Set((o["understood"] as? [String] ?? []).map { TranscribePlaces.base($0) })
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
   *  them — it reports THIS app's reservation, not whether the model is on the device; installedLocales does that.
   *  ★★★ And since 2026-10-17 a language this app downloads is the same: see `fetch` below. (Before, each download
   *  KEPT one of Apple's 5 places and a full set removed the least used — gone, with TranscribePlaces.decide.) */
  /// The model is on the device (any app or the system put it there) — or this app's own status says installed.
  static func isPresent(_ loc: Locale, _ m: any SpeechModule) async -> Bool {
    if await installedIds().contains(loc.identifier(.bcp47)) { return true }
    return await AssetInventory.status(forModules: [m]) == .installed
  }
  static func langName(_ id: String) -> String {
    let c = TranscribePlaces.base(id)
    return Locale.current.localizedString(forLanguageCode: c) ?? c
  }

  /* ★★★ SAVED TO THE DEVICE, NOT KEPT IN A PLACE (measured 2026-10-17 on macOS 27, memory
   *  speech_models_release_after_download). A model is downloaded under a reservation — Apple's API needs one — and the
   *  reservation is RELEASED straight after: the model stays installed and a process holding NO reservation transcribed
   *  Polish word-perfect. Stuart: "that way we can offer the button to save it to the system no 5 slot weirdness".
   *  So the 5-place cap only limits downloads IN FLIGHT, and nothing is ever removed to make room.
   *  ★ The assets must be fetched for the app's EXACT module (`module(for:)`): a `.longDictation` download left this
   *    configuration with no audio format — nothing ran.
   *  ★★ AND THE SYSTEM DOES TIDY RELEASED MODELS AWAY (measured later on 2026-10-17: of 13 locales downloaded and
   *    released, ru, ar, ro, id, uk, nl-NL and the Indian set were purged almost at once; pl-PL, released and then
   *    used, survived). Kept anyway — Stuart's decision, 2026-10-17: "if we are abusing the system level downloads to
   *    get around the 5 then we don't need the remove button. We do however need to inform users that they will take
   *    space, they will need to be removed in the system settings, and recently used ones may need to wait before
   *    removal." So no REMOVE, no promise that a language stays: the list reads installedLocales every time and a
   *    tidied-away language shows GET again; the GET confirm and the help say the rest. */
  /// Downloads `loc`'s model for `m` (the app's module) and gives the place straight back.
  static func fetch(_ loc: Locale, _ m: any SpeechModule) async throws {
    await releaseInstalled()                     // a place must be free for the download itself
    if let req = try await AssetInventory.assetInstallationRequest(supporting: [m]) {
      try await req.downloadAndInstall()
    }
    _ = await AssetInventory.release(reservedLocale: loc)
    await releaseInstalled()                     // ★ and any place Apple filed under another spelling of the locale
  }
  /// Gives back every place whose model is installed — those run without one. This is also how places held by
  /// builds before 2026-10-17 (which KEPT a place per language) are handed back: the first list read frees them.
  static func releaseInstalled() async {
    let have = await installedIds()
    for l in await AssetInventory.reservedLocales where have.contains(l.identifier(.bcp47)) {
      _ = await AssetInventory.release(reservedLocale: l)
    }
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
}

// MARK: - Engine

@available(iOS 26.0, *)
final class TranscribeEngine {
  private weak var owner: VibeTranscriber?
  private let language: String
  /// AUTO's EiBi hint ('auto:zh,tr' from JS): what the schedule says is on this frequency now — weighed first.
  private let hints: [String]
  private let translateTo: String
  private let options: TxOptions
  /// Everything below is touched only on `q`.
  private let q = DispatchQueue(label: "com.vibesdr.transcribe", qos: .utility)
  private var lanes: [Lane] = []
  private var chosen: Lane?
  private var probing = false
  private var probeStart = Date()
  private var stopped = false
  /* ★★ THE RUNNER-UP KEEPS LISTENING (Stuart, 2026-10-15: an SSB contact in another language whose call signs were
   *    said in English was settled as English, and a French station as German). Detection decides on a few phrases,
   *    and a few phrases can mislead. So the second-best language of the deciding round stays on beside the chosen
   *    one, and if it is clearly the more confident over the last 40 words it takes over — the language follows the
   *    audio instead of being fixed by the first sentence. Costs one extra speech model while AUTO runs. */
  private var challenger: Lane?
  private static let overtake = 0.08          // the challenger's lead (mean confidence) that makes it the language
  private var candidates: [Locale] = []
  /* ★★ ROUNDS OF FOUR (Stuart, 2026-10-15). Apple has no language identifier and running more than ~4 models at once
   *    is too much for a phone, so AUTO tries the pool four at a time — EiBi's languages for this frequency, the
   *    receiver's main language, the user's ★ preferred ones, the receiver area's, the phone's, then every other
   *    language ON PHONE (order: begin()) — and moves on when no lane in a round is convincing. After the
   *    last round it settles on the most confident language heard in ANY round. The pool is the user's to shape:
   *    GET in the language list adds a language to it. */
  private static let laneCap = 4
  private static let convincing = 0.55        // mean word confidence that ends the search early
  private var round = 0
  private var bestSeen: (locale: Locale, conf: Double)?
  private var rounds: Int { max(1, (candidates.count + Self.laneCap - 1) / Self.laneCap) }
  private var translator: Translator?
  /* ★★ THE STALL WATCHDOG (V12 research, 2026-10-16). SpeechTranscriber was measured going quiet on medium SSB — words
   *    for 7-15 s, then nothing for the rest of the clip; a 12 s no-result watchdog that starts a FRESH analyzer took
   *    3702 kHz from 19 words to 85, and fired 0 times on clean VOLMET. So: audio FLOWING (over a low RMS floor — never
   *    squelch or mute silence) and no volatile or final result for 12 s → that lane gets a fresh analyzer, same
   *    language; the translator, the box's text and the lane's confidence history are kept. A restart that brings
   *    nothing doubles the wait (to 96 s), so a long stretch of band noise costs a few restarts, not one every 12 s. */
  private static let stallAfter: TimeInterval = 12
  private static let stallMax: TimeInterval = 96
  private static let stallAudible: TimeInterval = 10      // of the window, this much must have been audio
  private static let stallRmsFloor: Float = 0.002          // ≈ -54 dBFS
  private var stallRestarts = 0

  final class Lane {
    let locale: Locale
    let transcriber: any SpeechModule
    let analyzer: SpeechAnalyzer
    let input: AsyncStream<AnalyzerInput>.Continuation
    let format: AVAudioFormat
    var converter: AVAudioConverter?
    var tasks: [Task<Void, Never>] = []
    var confSum = 0.0, words = 0
    var finals: [String] = []
    /// The last 40 words' confidence — what the chosen language and its challenger are compared on.
    var recent: [Double] = []
    var recentMean: Double { recent.isEmpty ? 0 : recent.reduce(0, +) / Double(recent.count) }
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
    var meanConf: Double { words > 0 ? confSum / Double(words) : 0 }
    func shutdown() {
      input.finish()
      tasks.forEach { $0.cancel() }
      let a = analyzer
      Task { await a.cancelAndFinishNow() }
    }
  }

  init(owner: VibeTranscriber, language: String, translateTo: String, options: TxOptions) {
    self.owner = owner; self.translateTo = translateTo; self.options = options
    if language.hasPrefix("auto:") {
      self.language = "auto"
      self.hints = language.dropFirst(5).split(separator: ",").map(String.init).filter { !$0.isEmpty }
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

  func start() {
    Task { await self.begin() }
  }

  func stop() {
    VibeTranscribeTap.shared.setSink(nil)
    q.async {
      if self.stallRestarts > 0 { NSLog("[VibeTranscriber] stall watchdog: %d restart(s) this session", self.stallRestarts) }
      self.stopped = true
      self.lanes.forEach { $0.shutdown() }
      self.lanes.removeAll(); self.chosen = nil; self.challenger = nil
    }
  }

  private func begin() async {
    status("Preparing on-device speech…")
    var cands: [Locale] = []
    if language != "auto" {
      guard let l = await SpeechEngines.supportedLocale(equivalentTo: Locale(identifier: language)) else {
        owner?.emit(["type": "error", "text": "\(language) is not supported for on-device speech"]); return
      }
      cands = [l]
    } else {
      /* ★★ AUTO TESTS ONLY WHAT IS ALREADY ON THE PHONE (Stuart, 2026-10-15). A speech model is 300–450 MB, so probing
       *    by download could pull a gigabyte over mobile data. A likely language
       *    that is not here is SAID instead — the user GETs it when and where it suits them. */
      //    (The box's own notice offers the DOWNLOAD — SDRScreen, from EiBi and the phone's list.)
      /* ★★★ THE ORDER IS BUILT IN JS (src/services/transcribeOrder.ts, tested; 2026-10-17): `hints` arrives as EiBi's
       *    languages for the frequency, the receiver's MAIN language (a reserved round-1 lane, in the receiver's
       *    variant — 'pt-BR' — when that one is installed), the user's ★ PREFERRED languages in their order, then the
       *    receiver area's others. Here the phone's own languages and everything else installed follow, and anything
       *    not installed is dropped — so no entry costs a lane unless its model is really on the device. */
      /* ★★ ONE LANE PER LANGUAGE, NOT PER VARIANT (Stuart on V12 479, 2026-10-17: the box read "Trying English, Dutch,
       *    French, German (round 1 of 7)… Trying English, English, Spanish, French (round 2 of 7)"). A Mac has many
       *    system models — en-GB, en-US, en-AU, en-IN… — and each took a lane of its own, so English was tried over
       *    and over and AUTO ran to seven rounds. Telling languages apart is the job here; the accent is not. Each
       *    language is tried ONCE: in the variant JS named explicitly (the receiver's, 'pt-BR') if installed, else the
       *    one the phone itself uses (Settings → Language order), else the hint's, else the first installed. */
      let installedList = await SpeechEngines.installedLocales()
      let installed = Set(installedList.map { $0.identifier(.bcp47) })
      var order: [String] = []                          // base codes, in the order they are tried
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
      for l in installedList { offer(l, rank: 3) }
      cands = order.compactMap { pick[$0]?.loc }
    }
    guard !cands.isEmpty else {
      owner?.emit(["type": "error", "text": "No speech language is on this device yet — GET one in the LANGUAGE list"]); return
    }
    candidates = cands
    round = 0; bestSeen = nil
    await openRound()
  }

  /// The current round's slice of the pool, probed side by side (a single explicit language is just chosen).
  private func openRound() async {
    let lo = round * Self.laneCap, hi = min(candidates.count, lo + Self.laneCap)
    guard lo < hi else { return }
    await openLanes(Array(candidates[lo..<hi]), probe: candidates.count > 1)
  }

  private func openLanes(_ cands: [Locale], probe: Bool) async {
    var made: [Lane] = []
    for loc in cands {
      let t = await SpeechEngines.module(for: loc)
      do {
        // ★ On the device = runs (measured: no place needed). status() alone said `.supported` for a model the
        //   system already had, which made AUTO skip it.
        let isInstalled = await SpeechEngines.isPresent(loc, t)
        // ★ AUTO never downloads (Stuart, 10-15 — 300–450 MB each), so it never takes a place either.
        if probe && !isInstalled { continue }
        if !isInstalled {
          status("Downloading the \(shortName(loc)) speech model (once, then it works offline)…")
          try await SpeechEngines.fetch(loc, t)
        }
        guard let lane = try await makeLane(loc, module: t) else { continue }
        made.append(lane)
      } catch {
        NSLog("[VibeTranscriber] lane %@ failed: %@", loc.identifier, error.localizedDescription)
        if cands.count == 1 {
          owner?.emit(["type": "error", "text": "Could not start \(name(loc)) speech: \(error.localizedDescription)"])
          return
        }
      }
    }
    guard !made.isEmpty else { owner?.emit(["type": "error", "text": "Could not start on-device speech"]); return }
    let tapDial: (Float, [Float]) -> Void = { [weak self] d, st in self?.q.async { self?.onDial(d, st) } }
    q.async {
      guard !self.stopped else { made.forEach { $0.shutdown() }; return }
      self.lanes = made
      self.probing = probe
      self.probeStart = Date()
      if self.probing {
        self.chosen = nil
        let names = made.map { self.shortName($0.locale) }.joined(separator: ", ")
        // ★ The search shows in the BOX, line by line (Stuart, 10-15) — the status line alone flickers past.
        self.owner?.emit(["type": "trying",
                          "text": names + (self.rounds > 1 ? " (round \(self.round + 1) of \(self.rounds))" : "")])
        // ★ Said while it searches, so a slow AUTO explains itself (Stuart, 10-15: "warn users of how it works").
        self.status(self.rounds > 1
          ? "Listening — trying \(names) (round \(self.round + 1) of \(self.rounds); only 4 languages can be tried at a time, so this can take a while)…"
                                    : "Listening — detecting the language (\(names))…")
      } else {
        self.choose(made[0])
      }
      VibeTranscribeTap.shared.setSink({ [weak self] buf in self?.q.async { self?.feed(buf) } }, onDial: tapDial)
    }
  }

  /// One analyzer for `loc` on `module`, started, with its results task. Used to open a lane and to give a stalled
  /// lane a fresh analyzer.
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

  // q only — the stalled lane gets a fresh analyzer for the same language; it takes the old lane's place (chosen,
  // challenger, its confidence history and anything heard during detection), then the old one is shut down.
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
        guard !self.stopped, let i = self.lanes.firstIndex(where: { $0 === old }) else { fresh.shutdown(); return }
        fresh.confSum = old.confSum; fresh.words = old.words; fresh.finals = old.finals; fresh.recent = old.recent
        fresh.stallAfter = next
        self.lanes[i] = fresh
        if self.chosen === old { self.chosen = fresh }
        if self.challenger === old { self.challenger = fresh }
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
    let now = Date()
    for lane in lanes where !lane.restarting {
      if rms > Self.stallRmsFloor { lane.audible += dur }
      let quiet = now.timeIntervalSince(lane.lastResult)
      if quiet >= lane.stallAfter, lane.audible >= Self.stallAudible { restartLane(lane, quiet: quiet) }
    }
    for lane in lanes {
      if lane.converter == nil || lane.converter!.inputFormat != mono.format {
        lane.converter = AVAudioConverter(from: mono.format, to: lane.format)
      }
      guard let conv = lane.converter else { continue }
      let ratio = lane.format.sampleRate / mono.format.sampleRate
      let cap = AVAudioFrameCount(Double(mono.frameLength) * ratio + 64)
      guard let out = AVAudioPCMBuffer(pcmFormat: lane.format, frameCapacity: cap) else { continue }
      var fed = false
      var err: NSError?
      conv.convert(to: out, error: &err) { _, st in
        if fed { st.pointee = .noDataNow; return nil }
        fed = true; st.pointee = .haveData; return mono
      }
      if err == nil, out.frameLength > 0 { lane.input.yield(AnalyzerInput(buffer: out)) }
    }
  }

  // q only
  private func onResult(_ lane: Lane, _ rt: AttributedString, _ isFinal: Bool) {
    guard !stopped, lanes.contains(where: { $0 === lane }) else { return }
    // ★ Any result, volatile or final, is life: the stall clock starts again and the next wait is the short one.
    lane.lastResult = Date(); lane.audible = 0; lane.stallAfter = Self.stallAfter
    let text = String(rt.characters).trimmingCharacters(in: .whitespacesAndNewlines)
    if !isFinal {
      if chosen === lane, !text.isEmpty { owner?.emit(["type": "partial", "text": text]) }
      return
    }
    guard !text.isEmpty else { return }
    var cs: [Double] = []
    for run in rt.runs {
      if let c = run[AttributeScopes.SpeechAttributes.ConfidenceAttribute.self] {
        let w = rt[run.range].characters.split(whereSeparator: { $0 == " " }).count
        for _ in 0..<max(1, w) { cs.append(c) }
      }
    }
    lane.confSum += cs.reduce(0, +); lane.words += cs.count
    if probing {
      lane.finals.append(text)
      decideIfReady()
      return
    }
    lane.recent.append(contentsOf: cs)
    if lane.recent.count > 40 { lane.recent.removeFirst(lane.recent.count - 40) }
    guard let cur = chosen else { return }
    if lane === challenger {
      // ★ Both have heard enough of the same audio, and the challenger believes it clearly more: switch.
      if lane.recent.count >= 15, cur.recent.count >= 15, lane.recentMean > cur.recentMean + Self.overtake {
        owner?.emit(["type": "tried", "text": "Sounds more like \(shortName(lane.locale)) than \(shortName(cur.locale)) — switching"])
        challenger = cur
        cur.recent.removeAll(); lane.recent.removeAll()
        choose(lane)
      }
      return
    }
    guard cur === lane else { return }
    deliver(text, lane: lane)
    // ★ The station changed language (SW broadcasts do, hourly): confidence collapses. Re-detect, AUTO only.
    if language == "auto", candidates.count > 1, lane.recent.count >= 25, lane.recentMean < 0.35 {
      lane.recent.removeAll()
      status("The language seems to have changed — re-detecting…")
      reprobe()
    }
  }

  // q only
  private func decideIfReady() {
    let elapsed = Date().timeIntervalSince(probeStart)
    // ★ More evidence than the first build asked for (6 words, 5 s): French settled as German on that (10-15).
    let ready = lanes.filter { $0.words >= 10 }
    guard !ready.isEmpty, elapsed >= 8 || ready.count == lanes.count else { return }
    // Enough heard by at least one model: the most confident wins — unless the top two are too close to call, when
    // it listens on, for up to 20 s.
    let ranked = lanes.filter { $0.words >= 4 }.sorted { $0.meanConf > $1.meanConf }
    guard let best = ranked.first else { return }
    let second = ranked.count > 1 ? ranked[1] : nil
    if let s2 = second, best.meanConf - s2.meanConf < 0.05, elapsed < 20 { return }
    if bestSeen == nil || best.meanConf > bestSeen!.conf { bestSeen = (best.locale, best.meanConf) }
    // ★ Not convincing and languages left to try: the next four.
    if best.meanConf < Self.convincing, round + 1 < rounds {
      let tried = lanes.map { shortName($0.locale) }.joined(separator: ", ")
      emitVerdicts()
      shutdownLanes()
      round += 1
      status("Not \(tried) — trying the next languages…")
      Task { await self.openRound() }
      return
    }
    // ★ Every round tried and none convincing: the best heard anywhere, if it was not in this round.
    if best.meanConf < Self.convincing, let b = bestSeen, b.locale.identifier(.bcp47) != best.locale.identifier(.bcp47) {
      emitVerdicts()
      owner?.emit(["type": "tried", "text": "None was a clear match — going back to \(shortName(b.locale)), the best heard"])
      shutdownLanes()
      Task { await self.openLanes([b.locale], probe: false) }
      return
    }
    // ★ The runner-up stays on as the challenger (see `challenger`) — only if it heard something at all.
    let runnerUp = (second?.words ?? 0) > 0 ? second : nil
    for l in lanes where l !== best && l !== runnerUp { l.shutdown() }
    lanes = [best] + (runnerUp.map { [$0] } ?? [])
    challenger = runnerUp
    best.recent.removeAll(); runnerUp?.recent.removeAll()
    probing = false
    choose(best)
    for t in best.finals { deliver(t, lane: best) }   // nothing heard during detection is lost
    best.finals.removeAll()
  }

  // q only
  private func choose(_ lane: Lane) {
    chosen = lane
    let lang = lane.locale.language.languageCode?.identifier ?? ""
    owner?.emit(["type": "lang", "text": lane.locale.identifier(.bcp47), "name": shortName(lane.locale),
                 "detected": language == "auto"])
    status("Listening — \(shortName(lane.locale))\(language == "auto" ? " (detected)" : "")")
    translator = nil
    /* ★★ LANGUAGES I UNDERSTAND (Stuart, 2026-10-17: "for people who are multilingual themselves") — speech in one of
     *    them is shown as it is, no translation line; everything else goes to the target. */
    if options.understood.contains(TranscribePlaces.base(lang)) {
      if !translateTo.isEmpty { status("Listening — \(shortName(lane.locale)) (you understand it — not translated)") }
    } else if !translateTo.isEmpty, lang != TranscribePlaces.base(translateTo) {
      let tr = Translator(source: lane.locale.language, target: Locale.Language(identifier: translateTo)) { [weak self] s in
        self?.status(s)
      }
      translator = tr
      Task { await tr.prepare() }
    }
  }

  // q only
  private func reprobe() {
    shutdownLanes()
    round = 0; bestSeen = nil
    Task { await self.openRound() }
  }

  // q only — one line for the box: what each language in this round made of the audio.
  private func emitVerdicts() {
    let v = lanes.map { l -> String in
      l.words == 0 ? "\(shortName(l.locale)) — nothing recognised"
                   : "\(shortName(l.locale)) — weak match (\(Int((l.meanConf * 100).rounded())) %)"
    }.joined(separator: " · ")
    owner?.emit(["type": "tried", "text": v])
  }

  // q only
  private func shutdownLanes() {
    for l in lanes { l.shutdown() }
    lanes.removeAll(); chosen = nil; challenger = nil; probing = false
    VibeTranscribeTap.shared.setSink(nil)
  }

  // q only
  private func deliver(_ text: String, lane: Lane) {
    let lang = shortName(lane.locale)
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
    guard !stopped, chosen === lane else { return }
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
