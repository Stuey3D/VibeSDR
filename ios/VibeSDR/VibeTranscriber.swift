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
  private let q = DispatchQueue(label: "com.vibesdr.transcribe.feed", qos: .utility)
  private var backlog = 0
  private let lock = NSLock()

  func setSink(_ s: ((AVAudioPCMBuffer) -> Void)?) {
    lock.lock(); sink = s; armed = s != nil; lock.unlock()
  }

  /// `silent`: squelch is closed — the transcriber hears what the listener hears, i.e. nothing.
  @objc func feed(_ buf: AVAudioPCMBuffer, silent: Bool) {
    guard armed else { return }
    lock.lock(); let s = sink; let b = backlog; lock.unlock()
    guard let s else { return }
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
      s(mono)
      guard let self else { return }
      self.lock.lock(); self.backlog -= 1; self.lock.unlock()
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
      let supported = await SpeechEngines.supportedLocales()
      let installed = await SpeechEngines.installedIds()
      let reserved = Set(await AssetInventory.reservedLocales.map { $0.identifier(.bcp47) })
      let here = Locale.current
      let langs: [[String: Any]] = supported
        .map { loc -> [String: Any] in
          let id = loc.identifier(.bcp47)
          // ★ `reserved` = one of the places Apple gives this app (REMOVE frees it); `installed` = the model is here.
          return ["id": id, "name": here.localizedString(forIdentifier: id) ?? id, "installed": installed.contains(id),
                  "reserved": reserved.contains(id)]
        }
        .sorted { ($0["name"] as! String) < ($1["name"] as! String) }
      resolve(["available": true, "deviceLanguage": Locale.current.language.languageCode?.identifier ?? "en",
               "deviceLanguageName": here.localizedString(forLanguageCode: Locale.current.language.languageCode?.identifier ?? "en") ?? "",
               // ★ The phone's region, so 'English' means en-GB on a British phone, not the list's first (en-AU).
               "deviceRegion": Locale.current.region?.identifier ?? "",
               "maxLanguages": AssetInventory.maximumReservedLocales,   // Apple's cap on speech languages kept
               "languages": langs])
    }
  }

  /// language: "auto" or a BCP-47 id. translateTo: "" = off, else a language code ("en").
  @objc func start(_ language: String, translateTo: String) {
    guard #available(iOS 26.0, *) else { emit(["type": "error", "text": "Needs iOS 26 or later"]); return }
    (engineBox as? TranscribeEngine)?.stop()
    let eng = TranscribeEngine(owner: self, language: language, translateTo: translateTo)
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
      var roomNote = ""
      do {
        if await SpeechEngines.isPresent(loc, m) {
          await SpeechEngines.keepIfFree(loc)          // already here: runs without a place, removes nothing
        } else {
          switch await SpeechEngines.makePlace(for: loc, protecting: translateTo.isEmpty ? [] : [translateTo]) {
          case .full(let msg): resolve(["ok": false, "text": msg]); return
          case .made(let r): if let r { roomNote = ". Removed \(r) (least used) to make room for \(nm)" }
          }
          if let req = try await AssetInventory.assetInstallationRequest(supporting: [m]) {
            try await req.downloadAndInstall()
          }
        }
      } catch {
        resolve(["ok": false, "text": "Could not download \(nm): \(error.localizedDescription)"]); return
      }
      var text = "\(nm) is on this phone — AUTO can now pick it\(roomNote)"
      if !translateTo.isEmpty, code != translateTo {
        let dst = Locale.Language(identifier: translateTo)
        let dn = Locale.current.localizedString(forLanguageCode: translateTo) ?? translateTo
        switch await LanguageAvailability().status(from: loc.language, to: dst) {
        case .installed: text += ", translating to \(dn)"
        case .supported:
          let ok = await TranslationDownloadHost.request(source: loc.language, target: dst)
          text += ok ? ", translating to \(dn)" : " (translation pack not downloaded)"
        default: text += " (the phone cannot translate it to \(dn))"
        }
      }
      resolve(["ok": true, "text": text])
    }
  }

  /// ★ REMOVE in the language row (Stuart, 2026-10-15: "we need a remove button too as right now I try a download and
  /// get the 5 limit warning"). Gives back this app's place for the language; iOS deletes the model itself when it
  /// wants the space (Apple offers no "delete now"). Resolves { ok, text } for the box.
  @objc func remove(_ language: String,
                    resolver resolve: @escaping RCTPromiseResolveBlock, rejecter reject: @escaping RCTPromiseRejectBlock) {
    guard #available(iOS 26.0, *) else { resolve(["ok": false, "text": "Needs iOS 26 or later"]); return }
    Task {
      let base = Locale(identifier: language).language.languageCode?.identifier ?? language
      let nm = Locale.current.localizedString(forLanguageCode: base) ?? base
      var n = 0
      for l in await AssetInventory.reservedLocales where l.language.languageCode?.identifier == base {
        if await AssetInventory.release(reservedLocale: l) { n += 1 }
      }
      resolve(n > 0 ? ["ok": true, "text": "\(nm) removed — iOS deletes the model when it needs the space"]
                    : ["ok": false, "text": "\(nm) was not kept by this app"])
    }
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
  /* ★★★ APPLE KEEPS AT MOST `maximumReservedLocales` SPEECH LANGUAGES PER APP (5 on this Mac's SDK) — its limit, not
   *  ours. reserve() past it fails. A download the USER asked for (GET, the box's DOWNLOAD, choosing a language) makes
   *  room by giving back the LEAST USED place, and says which (Stuart, 2026-10-15: "auto remove the least used
   *  language"). AUTO itself never downloads, so it never evicts. Last use: UserDefaults id → time, set on use.
   *  ★★★ BUT NEVER A PROTECTED ONE (V12 467, 2026-10-16: an English iPhone lost ENGLISH to make room for Vietnamese —
   *  it is rarely HEARD on shortwave, so it looked least used). The rule is TranscribePlaces.decide (tested):
   *  the phone's languages, the translation target, what is being transcribed now and the incoming one keep their
   *  places; if every place is protected nothing is removed and the user is asked (REMOVE in the LANGUAGE list).
   *  ★★ A LANGUAGE ALREADY ON THE DEVICE RUNS WITHOUT A PLACE. Measured 2026-10-16 on macOS 27's Speech framework (the
   *  same one iOS 26/27 ships) with the app's exact SpeechTranscriber AND DictationTranscriber configurations: a binary
   *  holding NO reservation transcribed en-GB and nl-BE speech correctly. status(forModules:) said `.supported` for
   *  them — it reports THIS app's reservation, not whether the model is on the device; installedLocales does that.
   *  So the phone's own language, when the system already has it, never takes one of the places at all. */
  private static let lruKey = "vibeTxLangLastUsed"
  static func touch(_ loc: Locale) { touch(id: loc.identifier(.bcp47)) }
  /// Also takes a bare code ("en") — the translation target counts as use of its language.
  static func touch(id: String) {
    guard !id.isEmpty else { return }
    var m = UserDefaults.standard.dictionary(forKey: lruKey) as? [String: Double] ?? [:]
    m[id] = Date().timeIntervalSince1970
    UserDefaults.standard.set(m, forKey: lruKey)
  }
  /// The phone's own languages (Settings → Language order) as base codes — always protected.
  static func deviceBases() -> Set<String> {
    var out = Set(Locale.preferredLanguages.map { TranscribePlaces.base($0) })
    if let c = Locale.current.language.languageCode?.identifier { out.insert(c) }
    return out
  }
  /// Base codes being transcribed right now (set by the running engine) — protected while in use.
  private static let inUseLock = NSLock()
  private static var inUseStore = Set<String>()
  static var inUse: Set<String> {
    get { inUseLock.lock(); defer { inUseLock.unlock() }; return inUseStore }
    set { inUseLock.lock(); inUseStore = newValue; inUseLock.unlock() }
  }
  /// The model is on the device (any app or the system put it there) — or this app's own status says installed.
  static func isPresent(_ loc: Locale, _ m: any SpeechModule) async -> Bool {
    if await installedIds().contains(loc.identifier(.bcp47)) { return true }
    return await AssetInventory.status(forModules: [m]) == .installed
  }
  static func langName(_ id: String) -> String {
    let c = TranscribePlaces.base(id)
    return Locale.current.localizedString(forLanguageCode: c) ?? c
  }

  enum Place { case made(removed: String?); case full(message: String) }
  /// Makes sure `loc` holds a place before a download. `protecting`: more base codes to keep (translation target).
  static func makePlace(for loc: Locale, protecting extra: Set<String>) async -> Place {
    let id = loc.identifier(.bcp47)
    let held = await AssetInventory.reservedLocales
    let heldIds = held.map { $0.identifier(.bcp47) }
    let device = deviceBases()
    let prot = device.union(extra.map { TranscribePlaces.base($0) }).union(inUse)
    let used = UserDefaults.standard.dictionary(forKey: lruKey) as? [String: Double] ?? [:]
    var removed: String?
    switch TranscribePlaces.decide(held: heldIds, max: AssetInventory.maximumReservedLocales, incoming: id,
                                   protectedBases: prot, lastUsed: used) {
    case .room: break
    case .remove(let victimId):
      if let victim = held.first(where: { $0.identifier(.bcp47) == victimId }) {
        NSLog("[VibeTranscriber] releasing %@ to make room for %@", victim.identifier, id)
        if await AssetInventory.release(reservedLocale: victim) { removed = langName(victimId) }
      }
    case .ask:
      // ★ Never a silent removal of a language the user needs — say why each is kept and let them choose.
      let why: (String) -> String = { h in
        let b = TranscribePlaces.base(h)
        if device.contains(b) { return "\(langName(h)) (your phone\u{2019}s language)" }
        if extra.contains(where: { TranscribePlaces.base($0) == b }) { return "\(langName(h)) (translating into it)" }
        if inUse.contains(b) { return "\(langName(h)) (in use)" }
        return langName(h)
      }
      let kept = Array(Set(heldIds.map(why))).sorted().joined(separator: ", ")
      return .full(message: "iPhone keeps \(AssetInventory.maximumReservedLocales) speech languages for VibeSDR and all are needed: \(kept). REMOVE one in the LANGUAGE list to make room for \(langName(id)).")
    }
    _ = try? await AssetInventory.reserve(locale: loc)
    touch(loc)
    return .made(removed: removed)
  }
  /// A language already on the device that the user asked for: keep it (so iOS does not tidy it away) only when a
  /// place is FREE — never remove another for a model that runs without one — and never for the phone's language.
  static func keepIfFree(_ loc: Locale) async {
    touch(loc)
    if deviceBases().contains(TranscribePlaces.base(loc.identifier(.bcp47))) { return }
    let held = await AssetInventory.reservedLocales
    if held.contains(where: { TranscribePlaces.base($0.identifier(.bcp47)) == TranscribePlaces.base(loc.identifier(.bcp47)) }) { return }
    if held.count < AssetInventory.maximumReservedLocales { _ = try? await AssetInventory.reserve(locale: loc) }
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
   *    is too much for a phone, so AUTO tries the pool four at a time — EiBi's languages for this frequency, then the
   *    phone's, then every other language ON PHONE — and moves on when no lane in a round is convincing. After the
   *    last round it settles on the most confident language heard in ANY round. The pool is the user's to shape:
   *    GET in the language list adds a language to it. */
  private static let laneCap = 4
  private static let convincing = 0.55        // mean word confidence that ends the search early
  private var round = 0
  private var bestSeen: (locale: Locale, conf: Double)?
  private var rounds: Int { max(1, (candidates.count + Self.laneCap - 1) / Self.laneCap) }
  private var translator: Translator?

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

  init(owner: VibeTranscriber, language: String, translateTo: String) {
    self.owner = owner; self.translateTo = translateTo
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
    SpeechEngines.inUse = []
    q.async {
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
      // ★ EiBi's languages for this frequency first (fetched if missing — the box says so), then the phone's
      //   languages (in the user's order), then any other model already installed.
      var seen = Set<String>()
      var hinted = Set<String>()
      for code in hints {
        let want = code == "zh" ? "zh-CN" : code == "yue" ? "yue-CN" : code
        if let l = await SpeechEngines.supportedLocale(equivalentTo: Locale(identifier: want)),
           seen.insert(l.identifier(.bcp47)).inserted { cands.append(l); hinted.insert(l.identifier(.bcp47)) }
      }
      let phoneAt = cands.count
      for id in Locale.preferredLanguages {
        if let l = await SpeechEngines.supportedLocale(equivalentTo: Locale(identifier: id)),
           seen.insert(l.identifier(.bcp47)).inserted { cands.append(l) }
      }
      for l in await SpeechEngines.installedLocales() where seen.insert(l.identifier(.bcp47)).inserted {
        cands.append(l)
      }
      _ = phoneAt
      /* ★★ AUTO TESTS ONLY WHAT IS ALREADY ON THE PHONE (Stuart, 2026-10-15). A speech model is 300–450 MB, so probing
       *    by download could pull a gigabyte over mobile data and churn Apple's handful of places. A likely language
       *    that is not here is SAID instead — the user GETs it when and where it suits them. */
      //    (The box's own notice offers the DOWNLOAD — SDRScreen, from EiBi and the phone's list.)
      let installed = await SpeechEngines.installedIds()
      cands = cands.filter { installed.contains($0.identifier(.bcp47)) }
    }
    guard !cands.isEmpty else {
      owner?.emit(["type": "error", "text": "No speech language is on this phone yet — GET one in the LANGUAGE list"]); return
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
        // ★ Only a model that is not here needs a place — one already installed runs as it is, evicting nothing.
        if !probe, !isInstalled {
          switch await SpeechEngines.makePlace(for: loc, protecting: translateTo.isEmpty ? [] : [translateTo]) {
          case .full(let msg): owner?.emit(["type": "error", "text": msg]); return
          case .made(let r):
            if let r { owner?.emit(["type": "tried", "text": "Removed \(r) (least used) to make room for \(shortName(loc))"]) }
          }
        }
        if !isInstalled {
          status("Downloading the \(shortName(loc)) speech model (once, then it works offline)…")
          if let req = try await AssetInventory.assetInstallationRequest(supporting: [t]) {
            try await req.downloadAndInstall()
          }
        }
        guard let fmt = await SpeechAnalyzer.bestAvailableAudioFormat(compatibleWith: [t]) else { continue }
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
          ? "Listening — trying \(names) (round \(self.round + 1) of \(self.rounds); iPhone can only try 4 languages at a time, so this can take a while)…"
                                    : "Listening — detecting the language (\(names))…")
      } else {
        self.choose(made[0])
      }
      VibeTranscribeTap.shared.setSink { [weak self] buf in self?.q.async { self?.feed(buf) } }
    }
  }

  // q only
  private func feed(_ mono: AVAudioPCMBuffer) {
    guard !stopped else { return }
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
    SpeechEngines.touch(lane.locale)   // ★ "least used" means least used HERE
    SpeechEngines.touch(id: translateTo)   // ★ translating INTO a language is use of it too
    SpeechEngines.inUse = [TranscribePlaces.base(lane.locale.identifier(.bcp47))]
    let lang = lane.locale.language.languageCode?.identifier ?? ""
    owner?.emit(["type": "lang", "text": lane.locale.identifier(.bcp47), "name": shortName(lane.locale),
                 "detected": language == "auto"])
    status("Listening — \(shortName(lane.locale))\(language == "auto" ? " (detected)" : "")")
    translator = nil
    if !translateTo.isEmpty, lang != translateTo {
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
