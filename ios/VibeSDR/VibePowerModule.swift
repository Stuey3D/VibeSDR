import Foundation
import AVFoundation
import CoreLocation
import MediaPlayer
import UIKit
import AppIntents
import Network

// Classic RCT bridge module — accessible via NativeModules.VibePowerModule.
// Owns the audio WebSocket natively so audio survives JS suspension (background).
//
// Packet layout (version=2, always 21-byte header):
//   [0:8]   uint64 LE  timestamp
//   [8:12]  uint32 LE  sample rate
//   [12]    uint8      channels
//   [13:17] float32 LE baseband power
//   [17:21] float32 LE noise density
//   [21:]   Opus payload

@objc(VibePowerModule)
class VibePowerModule: RCTEventEmitter, CLLocationManagerDelegate {

  // MARK: - CLLocationManagerDelegate (one-shot location for the picker)

  func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
    guard locResolve != nil else { return }
    switch manager.authorizationStatus {
    case .authorizedWhenInUse, .authorizedAlways:
      manager.requestLocation()
    case .denied, .restricted:
      locResolve?(nil)
      locResolve = nil
    default: break  // .notDetermined — prompt still showing
    }
  }

  func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
    guard let loc = locations.last else { return }
    locResolve?(["lat": loc.coordinate.latitude, "lon": loc.coordinate.longitude])
    locResolve = nil
  }

  func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
    locResolve?(nil)
    locResolve = nil
  }

  // MARK: - RCTEventEmitter

  override func supportedEvents() -> [String]! {
    return ["VibeTuned", "VibeMuted", "VibeWsText", "VibeSkip", "VibeCarConnected", "VibeCarTune",
            "VibeDataSaverDisconnect", "VibeDataSaverResume", "VibeSignal",
            "VibeVoiceQuery", "VibeVoiceTune", "VibeMdnsFound", "VibeMdnsLost",
            "VibeNetworkPathChanged", "VibeVolume",
            // Hardware keyboard (see VibeKeyWindow in AppDelegate.swift). Key DOWN and UP
            // are separate events on purpose: the arrows reuse the tuner keys' press/hold
            // semantics, and a sweep needs to know when the key was released.
            "VibeKeyDown", "VibeKeyUp", "VibeScroll",
            // ★ The local audio pump's byte tally. The link meter has to include AUDIO or it is
            //   worse than useless — it once read 12 KB/s while the link carried 198, and hid the
            //   real problem for months. Now the socket is native, only native can count it.
            "VibeLocalAudioBytes",
            // ★★★ THE AUDIO PATH'S OWN VOICE. The diagnostics report could describe the LOCAL
            //     player's socket and had NOTHING to say about this one — so a report from the
            //     UberSDR path printed "the socket was never opened" as a DEFAULT, with no
            //     instrumentation behind it, and we read a guess as a finding (issue #20).
            //     Native is the only layer that knows whether the engine started, whether the
            //     socket reached .ready, and whether a packet was refused after arriving.
            "VibeAudioPath",
            // ★★★ THE WATCHDOG ASKING FOR HELP. Native can reopen a socket; it cannot re-register a
            //     session, because that is an HTTP POST the JS client owns. Without this event the
            //     one recovery that works was unreachable from the only layer that knew it was
            //     needed.
            "VibeAudioStuck",
            // ★ What the local-audio socket is DOING. It reported bytes once flowing and nothing
            //   at all before that, so a socket that never connected was indistinguishable from
            //   one that was never started — and the server cannot see either. A whole night of
            //   diagnosis was built on that silence (2026-08-16).
            "VibeLocalAudioState"]
  }

  override static func requiresMainQueueSetup() -> Bool { return false }

  /// The live instance, so the key window (which is not part of the bridge) can emit.
  /// Weak: the bridge owns the module's lifetime and may recreate it on reload.
  static weak var shared: VibePowerModule?

  /// Called from VibeKeyWindow. No-ops harmlessly before JS has subscribed.
  /// ★ `plain` is false when the name was SYNTHESISED — an arrow that came from the `< > - +`
  /// aliases, or one that only arrived because a modifier was held. The Full Keyboard Access
  /// detector needs that distinction: those are precisely the WORKAROUNDS for FKA, so counting
  /// them as evidence that the arrows work would make the feature hide its own diagnosis.
  static func emitKey(_ name: String, _ key: String, _ plain: Bool = true) {
    shared?.sendEvent(withName: name, body: ["key": key, "plain": plain])
  }

  /// Pointer scroll deltas (mouse wheel / trackpad), in points.
  static func emitScroll(_ dx: Double, _ dy: Double, _ x: Double, _ y: Double) {
    shared?.sendEvent(withName: "VibeScroll", body: ["dx": dx, "dy": dy, "x": x, "y": y])
  }

  override init() {
    super.init()
    VibePowerModule.shared = self
    startVoiceObserver()
    VibeCrashLog.install()      // record uncaught ObjC exceptions — see VibeCrashLog
    VibeCrashLog.announce()     // ...and SAY what the last one was, or recording it achieves nothing
  }

  // MARK: - State

  // mDNS/Bonjour RTL-TCP discovery
  private var mdnsBrowsers:  [NWBrowser] = []
  private var mdnsResolvers: [NWConnection] = []
  private let mdnsQueue      = DispatchQueue(label: "com.vibesdr.mdns", qos: .utility)

  private let audioQ       = DispatchQueue(label: "com.vibesdr.audio", qos: .userInteractive)
  private var opusDecoder:       OpaquePointer?
  private var decoderSampleRate: Int32 = 0
  private var decoderChannels:   Int32 = 0
  private var audioEngine:       AVAudioEngine?
  private var playerNode:        AVAudioPlayerNode?
  private var audioFormat:       AVAudioFormat?

  // FIXED-FORMAT ENGINE (half-speed FM bug 2026-06-12): the server flips
  // sample rate per mode (linear 12k, FM 24k). The old design rebuilt the
  // engine on main.async while the Opus decoder swapped synchronously on
  // audioQ — packets decoded at the new rate were scheduled into buffers of
  // the old format during the window = half-speed stretched audio. Now the
  // engine runs at 48 kHz stereo FOREVER and an AVAudioConverter (audioQ-only,
  // rebuilt when the packet format changes) resamples each packet. No engine
  // rebuilds, no race.
  private let ENGINE_RATE: Double = 48_000
  private let ENGINE_CH:   AVAudioChannelCount = 2
  private var converter:      AVAudioConverter?
  private var converterInFmt: AVAudioFormat?

  // Recorder — taps the uniform post-converter 48 kHz feed, so mode/rate
  // flips mid-recording are invisible to the file (same reason the skin
  // recorder behaved: Web Audio resampled upstream). AVAudioFile encodes
  // AAC .m4a in hardware. recFile is audioQ-only; recArmed is read on the
  // WS callback thread (same benign cross-thread pattern as isMuted).
  private var recFile:  AVAudioFile?
  private var recPath:  String = ""
  private var recArmed  = false

  // Client noise DSP (VibeDSP.swift — verbatim skin ports) applied to the
  // MONO packet-rate feed before stereo duplication + 48k conversion, the
  // same point the skin's Web Audio graph ran them. All state audioQ-only;
  // engines are lazy and rebuilt whenever the stream sample rate flips.
  private var nrModeStr     = "off"   // "off" | "nr" | "nr2"
  private var nbOn          = false
  private var dspRate: Int32 = 0
  private var nbEngine:      NoiseBlankerEngine?
  private var nr2Engine:     NR2Engine?
  private var nr2Chunker:    BlockChunker?
  private var websdrEngine:  WebSDRNREngine?
  private var websdrChunker: BlockChunker?
  private var nrBandwidthHz: Double = 2700

  private var wsTask:       URLSessionWebSocketTask?
  private var wsSession:    URLSession?
  // ── Native audio WebSocket transport ────────────────────────────────────
  // iOS 27 beta regressed Foundation's URLSessionWebSocketTask for this stream:
  // the task reports `.running` but stops delivering frames (server keeps
  // streaming at the right bitrate — confirmed) → audio dies, media card sticks
  // paused. Only UberSDR hit it (the one path on URLSession; Kiwi/OWRX use the RN
  // socket). Fix = run the native audio WS on Network.framework (NWConnection +
  // NWProtocolWebSocket), a lower-level Apple transport that doesn't share the
  // regression, while staying off the JS thread so background audio survives.
  // Toggle kept so the proven iOS-26 URLSession path can be restored in one line
  // until NWConnection is device-confirmed on both 26 and 27.
  private static let useNWConnectionAudioWs = true
  private var wsConn:  NWConnection?
  private var wsReady = false
  private var wsGen   = 0   // generation — ignore callbacks from a superseded socket
  private let wsQueue = DispatchQueue(label: "com.vibesdr.ws", qos: .userInteractive)
  // Set on every WS (re)open; the first received packet triggers a tune
  // re-assert so the server session always matches app state — sends during
  // the handshake window can be lost, which left the session on the URL's
  // freq/mode while the UI showed the restored tune.
  private var wsNeedsTuneAssert = false
  // SERVER BUG WORKAROUND (FM half-speed, root-caused 2026-06-12): ubersdr
  // creates its opus encoder ONCE per WS at the then-current sample rate;
  // a mode change flips radiod to a new rate but keeps the old encoder, so
  // the audio is time-stretched INSIDE the opus stream (tape-with-dying-
  // batteries FM). Cycling the WS makes the server build a fresh encoder.
  private var wsBaseSr: Int32 = 0
  private var srFlipCount = 0
  private var lastSrCycleAt = Date(timeIntervalSince1970: 0)
  private var isRunning     = false
  private var isMuted       = false
  // Client-side auto notch for NETWORK backends (UberSDR/OWRX/Kiwi). Local/RTL-TCP
  // are notched in the shim, so this stays off for them (the JS toggle routes
  // local → LocalHw.setNotch, network → here). One filter per output channel.
  private var notchOn       = false
  private var notch: [AutoNotch] = []
  // Client-side audio squelch gate (network backends, e.g. Kiwi). JS drives it
  // from the S-meter dBm vs the threshold; when closed we output silence (engine
  // keeps running — distinct from pause/isMuted). Defaults open.
  private var squelchOpen   = true
  // External-audio pause behaviour (matches Android): "release" (OWRX/Kiwi —
  // pause drops the card), "resume" (local/RTL-TCP — pause mutes in place, keeps
  // the card, play resumes; no server to disconnect).
  private var externalPauseMode = "release"
  // Pause disconnects the SDR (the server drops it almost instantly on suspend
  // anyway) and Play reconnects; these track the two non-playing media-card
  // states: cleanly disconnected vs a reconnect that failed (server full /
  // rate-limited) and needs the user to open the app.
  private var dataSaverDisconnected = false
  private var reconnectFailed = false
  private var lastSignalEmit: TimeInterval = 0   // throttle the SNR (VibeSignal) event
  private var currentFreq:  Int    = 14_074_000
  private var currentMode:  String = "usb"
  private var currentBase:  String = ""
  private var currentStep:  Int    = 1_000
  private var currentUuid:  String = ""
  /// ★★★ THE OWNER'S ADMIN CREDENTIAL, FOR THIS SOCKET TOO. The audio WS is opened HERE, not by
  /// the JS client, and it carried only the bypass PIN — so on a busy receiver it was refused
  /// ("audio WS refused — server busy … no credential") while the spectrum socket, which does carry
  /// one, evicted the occupant and took the slot. The owner ended up holding their own radio in
  /// silence (Stuart, 2026-08-15, after a whole evening of client-side workarounds).
  /// ★★ THE BROWSER AND JR BOTH PUT IT ON BOTH SOCKETS; the phone could not, because this engine's
  ///    entire interface was (baseUrl, frequency, mode, uuid, password). That asymmetry — not the
  ///    server, and not the takeover logic — is what made the phone the only client that could not
  ///    take a receiver back.
  /// ★ Arrives ready to append: "&vs_admin_ticket=…" or "&vs_admin_nonce=…&vs_admin_auth=…".
  private var adminSuffix = ""
  // Bypass password (rate-limit/ban bypass) — appended to the audio WS URL
  private var bypassPassword: String = ""
  private var packetCount   = 0
  private let FRAME_SIZE: Int32 = 5760
  private var instanceName: String = ""

  // Zombie-socket watchdog. Packets flow ~50/s, so staleness is a reliable
  // death signal. After background suspension (e.g. user switches to another
  // app and iOS freezes us once audio is interrupted), the server reaps the
  // session but our socket never errors — receiveLoop waits forever and audio
  // + spectrum stay dead until app relaunch (bug 2026-06-11).
  private var lastPacketAt  = Date()
  private var healthTimer: Timer?

  // Playback live-edge control (laggy-tuning bug 2026-06-11): scheduleBuffer
  // with no accounting let the queue grow after any delivery burst, so
  // playback ran seconds behind live FOREVER — tuning sounded delayed because
  // you kept hearing the backlog. queuedSeconds is mutated on audioQ only.
  private var queuedSeconds: Double = 0
  /** ★ The cushion that absorbs a source gap — see scheduleOut. An RTL gain change costs ~40 ms of
   *  broken audio at the hardware, so this has to be several times that to be worth having, and
   *  small enough that the dial still feels immediate. */
  private let kPreRollSeconds: Double = 0.22
  private var preRoll: Double = 0
  private var preRollBufs: [AVAudioPCMBuffer] = []
  // Now-playing overrides — JS computes a VTS-aware title/artist (station or
  // band name, user's frequency unit, tune step) and pushes them here. A
  // native lock-screen skip clears them until JS catches up (VibeTuned →
  // setNowPlaying round-trip, sub-second; background audio keeps JS alive).
  private var npTitleOverride:  String?
  private var npArtistOverride: String?
  // Composited album art (app icon + server-type logo inset), cached per type
  private var npArtwork: MPMediaItemArtwork?
  private var npArtworkType = ""
  // FM-DX: the resolved station logo (radio-browser favicon), inlaid left of the
  // FM-DX mark on the lock-screen artwork. Downloaded from the URL JS resolves.
  private var stationLogoImg: UIImage?
  private var stationLogoUrl = ""
  // Media skip routing: "step" = native tune±step; "bookmark" = emit
  // VibeSkip and let JS jump bookmarks (it owns the VTS station list)
  private var skipMode = "step"

  // Tune coalescing: the velocity drum can emit 20+ steps/s; one WS tune per
  // step thrashes radiod. Leading send + 80ms trailing timer.
  private var pendingTune: (freq: Int, mode: String)?
  private var lastTuneSentAt = Date(timeIntervalSince1970: 0)
  private var tuneTimer: Timer?

  // MARK: - Exported methods

  /* ★★★ KEEP THE APP ALIVE LONG ENOUGH TO FINISH A CONNECT THE WATCH ASKED FOR.
   *
   *  iOS wakes us for a WatchConnectivity message and then suspends us again within seconds. A
   *  watch-driven connect is not a few seconds' work: resolve the address, probe the auth, mount a
   *  screen, open two sockets, start decoding. From COLD — nothing playing — the app was routinely
   *  suspended part-way, which is why the phone "never woke up": it woke, started, and died.
   *  ★★★ UIBackgroundModes=audio IS NOT ENOUGH ON ITS OWN. AppDelegate makes the session active at
   *     launch so we appear in Now Playing, and an ACTIVE SESSION WITH NO AUDIO PLAYING GRANTS
   *     NOTHING — iOS keeps an audio app alive while it is actually producing audio. Stuart's own
   *     Now Playing widget showed TOOL, not VibeSDR: nothing of ours was playing, so nothing of
   *     ours was running.
   *  ★★ vibeStartSilentAudio() has existed for this the whole time and NOTHING EVER CALLED IT —
   *     written and never read. It plays an inaudible loop at 1% so the system counts us as an
   *     audio app; the real audio replaces it moments later.
   *  ★ Stopped by the caller once the session is up (or has failed), so we never hold the audio
   *    focus longer than the job needs — holding it indefinitely would be a battery cost and would
   *    park us in Now Playing over whatever the user was actually listening to. */
  @objc func keepAliveForConnect(_ on: Bool) {
    /* ★★★ ON MAIN, because this starts an AVAudioEngine and a bridge method does not run there.
     *   `requiresMainQueueSetup` is false, so React Native hands this module its own serial
     *   background queue — the hazard the long note below already describes for the engine
     *   lifecycle, and I walked straight into it. Started off-main the engine simply does not come
     *   up: no exception reaches JS, no audio session is taken, and Now Playing never changes.
     *   Stuart, 2026-08-29: "your background audio keep alive isnt working as ... the now playing
     *   still shows my music not VibeSDR." That is what a silent failure looks like from outside.
     * ★★ AND IT EXPLAINS THE REST OF WHAT HE SAW. With no keep-alive the phone is suspended
     *   mid-connect exactly as before, so the first attempt dies and the second works because the
     *   app is warm by then — "back out to the servers, repeat, and it connects".
     * ★ Inline when already on main, like the engine sequence below, so a caller that is already
     *   there is not deferred behind whatever else is queued. */
    /* ★★★ A REASON, NOT THE RAW START/STOP. The wake in VibeWatchModule now holds this loop too
     *   (it has to — see vibeHoldSilentAudio), and calling vibeStopSilentAudio() here would tear
     *   down a loop the wake is still holding: JS releases at the END of a connect, which is
     *   inside the window the wake exists to protect. vibeHoldSilentAudio does the main-thread
     *   hop itself, so the hop that used to live here is gone with it. */
    vibeHoldSilentAudio("js", on)
  }

  /// Set (or clear) the admin credential carried on the audio socket. Takes effect on the NEXT
  /// connection, so callers set it before starting the engine — or set it and reconnect.
  @objc func setAdminAuth(_ q: String) {
    adminSuffix = q.hasPrefix("&") || q.isEmpty ? q : "&" + q
  }

  /// ★★★ A BRIDGE METHOD DOES NOT RUN ON MAIN — and the engine lifecycle only runs there.
  ///     `requiresMainQueueSetup` is false, so React Native gives this module its own serial
  ///     background queue and every @objc method below arrives on it. When stopEngine() and
  ///     startEngine() learned to hop to main by themselves (build 135, the AVFAudio abort), the
  ///     three START methods stopped being sequences: `stopEngine()` was DEFERRED while the lines
  ///     after it ran immediately, so the deferred stop landed last and undid them —
  ///     `externalAudio = false`, `isRunning = false`, `closeAudioWs()` over a socket that had
  ///     just been opened. The engine ran and played silence, because every feed path checks the
  ///     flags the stop had just cleared. NO SOUND AT ALL, on every server (issue #20).
  /// ★★ So hop ONCE, for the WHOLE sequence, rather than per call. The per-call guards in
  ///    stop/startEngine stay as the net; this is the rule. Inline when already on main, so the
  ///    local audio pump (which hops first) keeps its ordering too.
  private func onMain(_ work: @escaping () -> Void) {
    if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
  }

  @objc func startAudioEngine(_ baseUrl: String, frequency: Int, mode: String, uuid: String, password: String) {
    onMain { self.startAudioEngineOnMain(baseUrl, frequency: frequency, mode: mode, uuid: uuid, password: password) }
  }

  private func startAudioEngineOnMain(_ baseUrl: String, frequency: Int, mode: String, uuid: String, password: String) {
    NSLog("[VibePowerModule] startAudioEngine %@ %d %@", baseUrl, frequency, mode)
    notePath("startAudioEngine \(baseUrl) \(frequency) \(mode)")
    stopEngine()
    currentBase  = baseUrl
    currentFreq  = frequency
    currentMode  = mode
    currentUuid  = uuid
    bypassPassword = password
    isRunning    = true
    isMuted      = false
    // Fresh session — clear the disconnected / reconnect-failed card state.
    dataSaverDisconnected = false
    reconnectFailed = false
    lastArtworkKey = ""
    packetCount  = 0
    lastPacketAt = Date()
    startHealthTimer()
    startPathMonitor()
    configureAVSession()
    startEngine()
    openAudioWs(baseUrl: baseUrl, frequency: frequency, mode: mode, uuid: uuid)
    DispatchQueue.main.async {
      self.setupRemoteCommands()
      self.updateNowPlaying()
      // AFTER configureAVSession: the session must be active for outputVolume to be
      // meaningful. Both are idempotent, so a re-start of the engine is harmless.
      self.startVolumeObserver()
      self.installVolumeView()
    }
  }

  @objc func stopAudioEngine() {
    onMain { self.stopAudioEngineOnMain() }
  }

  private func stopAudioEngineOnMain() {
    NSLog("[VibePowerModule] stopAudioEngine")
    notePath("stopAudioEngine")
    stopEngine()
  }

  // ── External PCM path (OWRX/Kiwi: the WS + decode live in JS; the native
  //    engine just plays the PCM JS pushes — foreground-first, no WS here) ──
  private var externalAudio = false
  // FM-DX: a single shared hardware tuner served as native MP3-over-WS. Like
  // externalAudio it owns its OWN reconnect (openFmdxWs/scheduleFmdxReconnect),
  // so the UberSDR watchdog must not touch it; it also disables lock-screen skip
  // (shared tuner — a skip would retune for everyone) and shows its own card.
  private var fmdxAudio = false
  private var externalRate: Double = 12000

  @objc func startExternalAudio(_ sampleRate: NSNumber, pauseMode: String) {
    onMain { self.startExternalAudioOnMain(sampleRate, pauseMode: pauseMode) }
  }

  private func startExternalAudioOnMain(_ sampleRate: NSNumber, pauseMode: String) {
    NSLog("[VibePowerModule] startExternalAudio %@ pauseMode=%@", sampleRate, pauseMode)
    stopEngine()
    externalAudio = true
    externalPauseMode = pauseMode.isEmpty ? "release" : pauseMode
    externalRate  = max(8000, sampleRate.doubleValue)
    isRunning     = true
    isMuted       = false
    dataSaverDisconnected = false
    reconnectFailed = false
    packetCount   = 0
    lastPacketAt  = Date()
    configureAVSession()
    startEngine()
    DispatchQueue.main.async {
      self.setupRemoteCommands()
      self.updateNowPlaying()
      // System volume is the SYSTEM's — it applies to OWRX/Kiwi audio exactly as it
      // does to UberSDR's, so the wrist must be able to see and turn it here too.
      self.startVolumeObserver()
      self.installVolumeView()
    }
  }

  /// base64 of little-endian Int16 mono PCM at `sampleRate` → resample → play.
  /// Per-frame rate so type-2 (12 kHz) and type-4 HD/WFM (48 kHz) both work.
  /// ★ The base64 hop exists ONLY because JS cannot hand raw bytes across the bridge. Anything
  ///   already native should call feedExternalPcm directly — see the local audio pump below.
  @objc func pushExternalPcm(_ base64: String, sampleRate: NSNumber, channels: NSNumber) {
    guard let data = Data(base64Encoded: base64) else { return }
    feedExternalPcm(data, sampleRate: sampleRate.doubleValue, stereo: channels.intValue == 2)
  }

  /// ★★★ AUDIO ARRIVING AT A PATH THAT REFUSES IT IS THE SILENT FAILURE. Both feed guards drop
  ///     the packet and return, so a cleared `externalAudio` produces a perfect stream, a running
  ///     engine, a moving byte meter — and no sound, with nothing anywhere saying why (issue #20).
  ///     Say it, once every two seconds, naming the flag that refused. A user can read this back
  ///     to us; a boolean nobody prints cannot be read by anyone.
  /// ★ Rate-limited rather than per packet: fifty of these a second would bury the log that is
  ///   meant to explain the fault.
  /// Report a step of the audio path to JS, where audioPathLog keeps it for the diagnostics
  /// report. NSLog alone is unreachable: it needs a Mac, a cable and Console.app, and the person
  /// who can reproduce the fault is holding a phone.
  private func notePath(_ what: String) {
    NSLog("[VibePowerModule] path: %@", what)
    sendEvent(withName: "VibeAudioPath", body: ["what": what])
  }

  private var lastRefusedLogAt = Date.distantPast
  private func noteFeedRefused(_ what: String) {
    guard Date().timeIntervalSince(lastRefusedLogAt) > 2 else { return }
    lastRefusedLogAt = Date()
    notePath("\(what) DROPPED — externalAudio=\(externalAudio) isMuted=\(isMuted) isRunning=\(isRunning)")
    NSLog("[VibePowerModule] %@ DROPPED — externalAudio=%@ isMuted=%@ isRunning=%@",
          what, externalAudio ? "true" : "false", isMuted ? "true" : "false",
          isRunning ? "true" : "false")
  }

  func feedExternalPcm(_ data: Data, sampleRate: Double, stereo: Bool) {
    guard externalAudio, !isMuted, data.count >= 2 else { noteFeedRefused("pcm"); return }
    let rate = max(8000, sampleRate)
    let ch2  = stereo                   // interleaved L,R (local WFM stereo)
    audioQ.async { [weak self] in
      guard let self else { return }
      let total = data.count / 2
      let n = ch2 ? total / 2 : total
      // Stereo frames get a real 2-channel planar buffer. This used to downmix L+R
      // to mono, which silently threw away WFM stereo on EVERY local path (USB,
      // RTL-TCP, SpyServer) — the pilot locked, the stereo icon lit, and the audio
      // was mono. Mono frames still arrive as 1 channel and convertTo48k upmixes.
      guard n > 0,
            let inFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: rate,
                                      channels: ch2 ? self.ENGINE_CH : 1, interleaved: false),
            let inBuf = AVAudioPCMBuffer(pcmFormat: inFmt, frameCapacity: AVAudioFrameCount(n)) else { return }
      inBuf.frameLength = AVAudioFrameCount(n)
      data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
        let s16 = raw.bindMemory(to: Int16.self)
        self.fillExternalBuffer(inBuf, from: s16.baseAddress!, frames: n, ch2: ch2)
      }
      self.playExternalBuffer(inBuf)
    }
  }

  /// Interleaved little-endian Int16 → the buffer's float channels. Shared by the
  /// RAW and OPUS external paths so both handle stereo identically.
  private func fillExternalBuffer(_ inBuf: AVAudioPCMBuffer,
                                  from s16: UnsafePointer<Int16>, frames n: Int, ch2: Bool) {
    if ch2 {
      let left  = inBuf.floatChannelData![0]
      let right = inBuf.floatChannelData![1]
      for i in 0..<n {
        left[i]  = Float(Int16(littleEndian: s16[i*2]))   / 32768.0
        right[i] = Float(Int16(littleEndian: s16[i*2+1])) / 32768.0
      }
    } else {
      let ch = inBuf.floatChannelData![0]
      for i in 0..<n { ch[i] = Float(Int16(littleEndian: s16[i])) / 32768.0 }
    }
  }

  private func playExternalBuffer(_ inBuf: AVAudioPCMBuffer) {
    packetCount += 1
    lastPacketAt = Date()
    if let out = convertTo48k(inBuf) {
      if recArmed { writeRecording(out) }   // OWRX/external audio recording
      scheduleOut(out)
    }
  }

  // ── VibeServer COMPRESSED audio (Opus) on the external path ────────────────
  //
  // ★★ WHY THIS EXISTS. The phone opened /ws/audio with NO `codec` parameter, so
  // VibeServer fell back to raw PCM — and 48 kHz × 2 ch × 2 bytes is 187 KB/s,
  // measured on the wire as 186. The spectrum beside it was 8 KB/s. Worse, the
  // shim sends both sockets under ONE blocking send mutex, so that torrent
  // starved the spectrum emitter and the server delivered ~58% of its configured
  // frame rate — which the phone's link controller then correctly read as a bad
  // link and throttled itself over. An entire afternoon of "the rate controller
  // is broken" was this.
  //
  // ★ The decoder was already here (the UberSDR path uses one), and the JS side
  // already crosses the bridge once per audio frame with base64 PCM — so sending
  // base64 OPUS instead is roughly 20x LESS bridge traffic, not more.
  //
  // Its own decoder instance, deliberately: the UberSDR path's decoder carries
  // its own rate/channel state and that path stays exactly as-is.
  private var extOpusDec:  OpaquePointer?
  private var extOpusRate: Int32 = 0
  private var extOpusCh:   Int32 = 0
  /// Consecutive opus_decode failures — drives the rebuild + thinned logging above.
  private var extOpusFails: Int = 0

  @objc func pushExternalOpus(_ base64: String, sampleRate: NSNumber, channels: NSNumber) {
    guard let pkt = Data(base64Encoded: base64) else { return }
    feedExternalOpus(pkt, sampleRate: sampleRate.intValue, channels: channels.intValue)
  }

  func feedExternalOpus(_ pkt: Data, sampleRate: Int, channels: Int) {
    guard externalAudio, !isMuted, pkt.count >= 3 else { noteFeedRefused("opus"); return }
    let sr = Int32(max(8000, sampleRate))
    let ch = Int32(channels == 2 ? 2 : 1)
    audioQ.async { [weak self] in
      guard let self else { return }
      if self.extOpusDec == nil || self.extOpusRate != sr || self.extOpusCh != ch {
        if let d = self.extOpusDec { opus_decoder_destroy(d) }
        var err: Int32 = 0
        self.extOpusDec = opus_decoder_create(sr, ch, &err)
        self.extOpusRate = sr; self.extOpusCh = ch
        NSLog("[VibePowerModule] ext opus decoder sr=%d ch=%d err=%d", sr, ch, err)
      }
      guard let dec = self.extOpusDec else { return }
      var pcm16 = [Int16](repeating: 0, count: Int(self.FRAME_SIZE) * Int(ch))
      let frames = pkt.withUnsafeBytes { raw -> Int32 in
        opus_decode(dec, raw.baseAddress!.assumingMemoryBound(to: UInt8.self),
                    Int32(pkt.count), &pcm16, self.FRAME_SIZE, 0)
      }

      // ★★★ A DECODE FAILURE WAS SILENT AND PERMANENT. opus_decode returns a NEGATIVE error
      //   code, and `guard frames > 0 ... else { return }` swallowed it — no log, no reset, no
      //   fallback. Every later packet then took the same path, so one bad frame killed the
      //   audio for the rest of the session while the spectrum, RDS and signal meter carried
      //   on perfectly (they are separate sockets). The cure was a retune, because a retune
      //   changes the channel count and THAT is the only thing that ever rebuilt the decoder.
      //   Measured on the Pi 2026-08-01: at 95.9 MHz — noise between two stations, where the
      //   pilot lock flaps and the server's channel count flips with it — the server sent a
      //   flawless 250 audio frames per 5 s for five solid minutes while the app sat silent.
      //   The audio never stopped arriving. We stopped decoding it.
      // ★★ REBUILD, DO NOT JUST COUNT. An Opus decoder carries state across packets; once it
      //   is unhappy the next packet usually is too, so the recovery has to be a new decoder
      //   rather than a retry. Rebuilding on the NEXT packet (nil here) keeps this off the
      //   hot path when nothing is wrong.
      if frames <= 0 {
        self.extOpusFails += 1
        // Log the first, then thin out — a wedged stream must not spam the device log.
        if self.extOpusFails == 1 || self.extOpusFails % 50 == 0 {
          NSLog("[VibePowerModule] opus decode failed (%d) x%d — rebuilding the decoder",
                frames, self.extOpusFails)
        }
        opus_decoder_destroy(dec)
        self.extOpusDec = nil          // recreated by the block above on the next packet
        self.extOpusRate = 0; self.extOpusCh = 0
        return
      }
      self.extOpusFails = 0

      guard frames > 0,
            let inFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: Double(sr),
                                      channels: ch == 2 ? self.ENGINE_CH : 1, interleaved: false),
            let inBuf = AVAudioPCMBuffer(pcmFormat: inFmt,
                                         frameCapacity: AVAudioFrameCount(frames)) else { return }
      inBuf.frameLength = AVAudioFrameCount(frames)
      pcm16.withUnsafeBufferPointer { bp in
        self.fillExternalBuffer(inBuf, from: bp.baseAddress!, frames: Int(frames), ch2: ch == 2)
      }
      self.playExternalBuffer(inBuf)
    }
  }

  @objc func stopExternalAudio() {
    onMain { self.stopExternalAudioOnMain() }
  }

  private func stopExternalAudioOnMain() {
    NSLog("[VibePowerModule] stopExternalAudio")
    externalAudio = false
    stopEngine()
  }

  // ── FM-DX Webserver audio (v7 spike): native MP3-over-WS. The server owns
  //    demod + RDS; we just decode 3LAS MP3 frames and play them. Separate WS +
  //    decoder from the UberSDR opus path (that path stays exactly as-is). ──
  private var fmdxConn: NWConnection?
  private var fmdxGen = 0
  private var fmdxDecoder: FmdxMp3Decoder?
  private var fmdxBase = ""

  /// baseUrl = the FM-DX server root (http(s)://host[:port]); we open `<host>/audio`.
  @objc func startFmdxAudio(_ baseUrl: String) {
    onMain { self.startFmdxAudioOnMain(baseUrl) }
  }

  private func startFmdxAudioOnMain(_ baseUrl: String) {
    NSLog("[VibePowerModule] startFmdxAudio %@", baseUrl)
    stopEngine()
    fmdxBase   = baseUrl
    isRunning  = true
    isMuted    = false
    externalAudio = false            // native-WS path, not JS-push
    fmdxAudio  = true                // own reconnect + no skip + own card
    dataSaverDisconnected = false
    reconnectFailed = false
    packetCount = 0
    lastPacketAt = Date()
    // Clear any UberSDR state so it can't bleed into the FM-DX lock-screen card
    // or let a stray watchdog revive reach a live UberSDR endpoint.
    npTitleOverride  = nil
    npArtistOverride = nil
    npArtworkType    = "fmdx"
    lastArtworkKey   = ""
    currentBase      = ""
    currentUuid      = ""
    configureAVSession()
    startEngine()
    openFmdxWs()
    DispatchQueue.main.async {
      self.setupRemoteCommands()
      self.updateNowPlaying()
      self.startVolumeObserver()
      self.installVolumeView()
    }
  }

  @objc func stopFmdxAudio() {
    onMain { self.stopFmdxAudioOnMain() }
  }

  private func stopFmdxAudioOnMain() {
    NSLog("[VibePowerModule] stopFmdxAudio")
    stopEngine()                     // closeFmdxWs() runs inside stopEngine
  }

  private func fmdxWsURL(_ base: String) -> URL? {
    var s = base.trimmingCharacters(in: .whitespaces)
    if s.hasPrefix("https://") { s = "wss://" + s.dropFirst(8) }
    else if s.hasPrefix("http://") { s = "ws://" + s.dropFirst(7) }
    s = s.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
    return URL(string: s + "/audio")
  }

  private func closeFmdxWs() {
    fmdxGen &+= 1                     // supersede any in-flight receive/reconnect
    fmdxConn?.cancel(); fmdxConn = nil
    fmdxDecoder = nil
  }

  private func openFmdxWs() {
    guard let url = fmdxWsURL(fmdxBase) else {
      NSLog("[VibePowerModule] bad FM-DX audio URL from base: %@", fmdxBase); return
    }
    NSLog("[VibePowerModule] opening FM-DX audio WS: %@", url.absoluteString)
    fmdxGen &+= 1
    let gen = fmdxGen

    let dec = FmdxMp3Decoder()
    dec.onPcm = { [weak self] pcm, ch, rate in
      // Decoder runs on audioQ (feed is dispatched there), so play inline.
      self?.playFmdxPcm(pcm, channels: ch, rate: rate)
    }
    fmdxDecoder = dec

    let secure = (url.scheme == "wss")
    let params: NWParameters = secure ? .tls : .tcp
    let wsOpts = NWProtocolWebSocket.Options()
    wsOpts.autoReplyPing = true
    params.defaultProtocolStack.applicationProtocols.insert(wsOpts, at: 0)

    let conn = NWConnection(to: .url(url), using: params)
    fmdxConn = conn
    conn.stateUpdateHandler = { [weak self] state in
      guard let self, self.isRunning, self.fmdxGen == gen else { return }
      switch state {
      case .ready:
        NSLog("[VibePowerModule] FM-DX audio WS ready")
        // 3LAS handshake: request the MP3 fallback stream.
        self.sendFmdxText(#"{"type":"fallback","data":"mp3"}"#, conn: conn)
        self.fmdxReceive(conn, gen: gen)
      case .waiting(let err):
        NSLog("[VibePowerModule] FM-DX audio WS waiting: %@", "\(err)")
      case .failed(let err):
        NSLog("[VibePowerModule] FM-DX audio WS failed: %@ — reconnecting in 2s", "\(err)")
        self.scheduleFmdxReconnect(gen: gen)
      case .cancelled:
        break
      default:
        break
      }
    }
    conn.start(queue: wsQueue)
  }

  private func fmdxReceive(_ conn: NWConnection, gen: Int) {
    conn.receiveMessage { [weak self] (data, context, _, error) in
      guard let self, self.isRunning, self.fmdxGen == gen, self.fmdxConn === conn else { return }
      if let error {
        NSLog("[VibePowerModule] FM-DX audio WS receive error: %@ — reconnecting", "\(error)")
        self.scheduleFmdxReconnect(gen: gen); return
      }
      let op = (context?.protocolMetadata(definition: NWProtocolWebSocket.definition)
                as? NWProtocolWebSocket.Metadata)?.opcode
      switch op {
      case .binary:
        if let data, !data.isEmpty {
          self.packetCount += 1
          self.lastPacketAt = Date()
          if self.packetCount <= 3 {
            NSLog("[VibePowerModule] FM-DX audio pkt#%d len=%d", self.packetCount, data.count)
          }
          self.audioQ.async { self.fmdxDecoder?.feed(data) }
        }
      case .close:
        NSLog("[VibePowerModule] FM-DX audio WS closed by peer — reconnecting")
        self.scheduleFmdxReconnect(gen: gen); return
      default:
        break   // text handshake acks / ping-pong
      }
      self.fmdxReceive(conn, gen: gen)   // re-arm
    }
  }

  private func scheduleFmdxReconnect(gen: Int) {
    DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
      guard let self, self.isRunning, self.fmdxGen == gen else { return }
      self.openFmdxWs()
    }
  }

  private func sendFmdxText(_ text: String, conn: NWConnection) {
    guard let data = text.data(using: .utf8) else { return }
    let md  = NWProtocolWebSocket.Metadata(opcode: .text)
    let ctx = NWConnection.ContentContext(identifier: "fmdxSend", metadata: [md])
    conn.send(content: data, contentContext: ctx, isComplete: true,
              completion: .contentProcessed { err in
                if let err { NSLog("[VibePowerModule] FM-DX ws send error: %@", "\(err)") }
              })
  }

  /// interleaved LE Int16 PCM → planar float stereo → 48k → scheduleOut.
  /// Mirrors the tail of handlePacket / pushExternalPcm.
  private func playFmdxPcm(_ data: Data, channels: Int, rate: Double) {
    guard isRunning, !isMuted, data.count >= 2 else { return }
    let total  = data.count / 2
    let frames = channels == 2 ? total / 2 : total
    guard frames > 0,
          let inFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: rate,
                                    channels: ENGINE_CH, interleaved: false),
          let inBuf = AVAudioPCMBuffer(pcmFormat: inFmt, frameCapacity: AVAudioFrameCount(frames)) else { return }
    inBuf.frameLength = AVAudioFrameCount(frames)
    let left  = inBuf.floatChannelData![0]
    let right = inBuf.floatChannelData![1]
    data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
      let s16 = raw.bindMemory(to: Int16.self)
      if channels == 2 {
        for i in 0..<frames {
          left[i]  = Float(Int16(littleEndian: s16[i*2]))   / 32768.0
          right[i] = Float(Int16(littleEndian: s16[i*2+1])) / 32768.0
        }
      } else {
        for i in 0..<frames {
          let v = Float(Int16(littleEndian: s16[i])) / 32768.0
          left[i] = v; right[i] = v
        }
      }
    }
    guard let out = convertTo48k(inBuf) else { return }
    if recArmed { writeRecording(out) }
    scheduleOut(out)
  }

  /** Called from JS on app-foreground: instant zombie check instead of
   *  waiting for the next watchdog tick. */
  @objc func revive() {
    DispatchQueue.main.async { [weak self] in
      self?.reviveIfDead(staleAfter: 3)
    }
  }

  /** Seconds since the last audio packet — or −1 when the native engine does not
   *  own this backend's audio and therefore cannot vouch for the session.
   *
   *  JS uses this to sequence spectrum reopens AFTER audio has confirmed the
   *  session is alive (UberSDRClient._awaitAudioAlive). connect() has always given
   *  audio a 1s head start for exactly this reason; the background recovery paths
   *  did not, and would reopen the spectrum into a session the server had reaped —
   *  a socket that connects, receives nothing, and never retries. */
  @objc func audioStaleness(_ resolve: @escaping RCTPromiseResolveBlock,
                            reject: @escaping RCTPromiseRejectBlock) {
    // Same exclusions as reviveIfDead: OWRX/Kiwi push external PCM and a shared
    // FM-DX tuner runs its own WS, so there is no native liveness to report.
    guard isRunning, !externalAudio, !fmdxAudio, !dataSaverDisconnected else {
      resolve(-1.0)
      return
    }
    resolve(Date().timeIntervalSince(lastPacketAt))
  }

  // MARK: - Watchdog

  private func startHealthTimer() {
    healthTimer?.invalidate()
    let t = Timer(timeInterval: 4.0, repeats: true) { [weak self] _ in
      self?.reviveIfDead(staleAfter: 8)
    }
    RunLoop.main.add(t, forMode: .common)
    healthTimer = t
  }

  // MARK: - System volume
  //
  // THE WATCH CONTROLS EXACTLY ONE KNOB: the iPhone's SYSTEM volume.
  //
  // The previous attempt drove an app-level gain (an AVAudioEngine scalar). Delivered
  // loudness is `appGain × systemVolume` — two independent knobs — and the watch could
  // only see and turn one of them. With the phone at 50% system volume the wrist meter
  // read FULL while delivering half loudness, and cranking it did nothing. The missing
  // piece was never control; it was READBACK of the knob that actually matters.
  //
  // So: app gain is not the watch's to touch, and the volume the wrist displays is the
  // volume the phone is really at — including changes it did not make.

  private var volumeObs: NSKeyValueObservation?
  /// Kept in the hierarchy for the life of the engine. Its slider is not settable
  /// until ~100ms after insertion, so it CANNOT be created on demand.
  private var volumeView: MPVolumeView?

  /// Observe the real system volume and forward every change to JS.
  ///
  /// KVO on `outputVolume` catches ALL of them — the hardware buttons, Control Centre,
  /// a Bluetooth headset's own volume rocker — not just the ones the watch asked for.
  /// That is the whole point: the wrist meter can then never disagree with the phone.
  ///
  /// Registered once and left alone. reviveIfDead() restarts the audio ENGINE, but the
  /// AVAudioSession singleton it observes outlives that, so re-registering per restart
  /// would only risk a double-observation.
  private func startVolumeObserver() {
    guard volumeObs == nil else { return }
    let session = AVAudioSession.sharedInstance()
    volumeObs = session.observe(\.outputVolume, options: [.new]) { [weak self] _, change in
      guard let self, let v = change.newValue else { return }
      self.sendEvent(withName: "VibeVolume", body: ["volume": Double(v)])
    }
    // Seed JS with the CURRENT volume — KVO only fires on change, so without this the
    // wrist would show a default until the user happened to touch something.
    sendEvent(withName: "VibeVolume", body: ["volume": Double(session.outputVolume)])
  }

  /// iOS has no public setter for system volume. The sanctioned route is MPVolumeView's
  /// own UISlider: setting it maps through to the active output route (including a
  /// Bluetooth headset's absolute volume) and, unlike other approaches, does NOT raise
  /// the system volume HUD over whatever the user is looking at.
  ///
  /// It must be IN the view hierarchy before first use and stay there — the slider is
  /// not settable for ~100ms after insertion, so an add-adjust-remove cycle per detent
  /// would silently drop the first change of every gesture.
  private func installVolumeView() {
    guard volumeView == nil else { return }
    let scene = UIApplication.shared.connectedScenes
      .compactMap { $0 as? UIWindowScene }
      .first(where: { $0.activationState == .foregroundActive })
      ?? UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
    guard let window = scene?.windows.first(where: { $0.isKeyWindow }) ?? scene?.windows.first else {
      return
    }
    // Offscreen rather than hidden: an `isHidden` MPVolumeView stops working.
    let v = MPVolumeView(frame: CGRect(x: -4000, y: -4000, width: 100, height: 30))
    v.alpha = 0.01
    v.isUserInteractionEnabled = false
    window.addSubview(v)
    volumeView = v
  }

  /// Set the iPhone's system volume (0…1).
  ///
  /// iOS quantises to 1/16 steps, so the value that lands is the SNAPPED one — and the
  /// KVO observer above then fires with that snapped truth, which is what gets echoed to
  /// the wrist. The watch predicts while the crown turns and adopts this on settle, so
  /// the two can differ for a moment and always converge on the phone's answer.
  @objc func setSystemVolume(_ v: NSNumber) {
    let target = Float(min(1, max(0, v.doubleValue)))
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.installVolumeView()   // no-op after the first call
      guard let slider = self.volumeView?.subviews.compactMap({ $0 as? UISlider }).first else {
        NSLog("[VibePowerModule] setSystemVolume: no slider — MPVolumeView not ready")
        return
      }
      slider.value = target
      // Some routes ignore a bare `.value` assignment without the control event.
      slider.sendActions(for: .valueChanged)
    }
  }

  /// The phone's current system volume (0…1) — so the watch's first detent can be
  /// applied as a DELTA against the truth rather than against a guess.
  @objc func getSystemVolume(_ resolve: @escaping RCTPromiseResolveBlock,
                             reject: @escaping RCTPromiseRejectBlock) {
    resolve(Double(AVAudioSession.sharedInstance().outputVolume))
  }

  // MARK: - Network path monitoring
  //
  // The 8s staleness window is the right patience for a bumpy link, but it is
  // pure waiting when the OS already KNOWS the path changed underneath us — a
  // WiFi→cellular handover, or a cellular IP change on cell handover. Neither
  // produces a FIN or an RST: the old flow is simply gone, and every socket on it
  // is a zombie that will sit OPEN forever. NWPathMonitor turns that into an
  // event, which makes recovery near-instant instead of near-12-seconds.
  //
  // Cheap: a system callback, not a poll. Nothing here runs per-frame.
  private var pathMonitor: NWPathMonitor?
  private let pathQueue = DispatchQueue(label: "com.vibesdr.path", qos: .utility)
  private var sawFirstPath = false
  private var lastPathIface: NWInterface.InterfaceType?

  private func startPathMonitor() {
    guard pathMonitor == nil else { return }
    sawFirstPath  = false
    lastPathIface = nil
    let m = NWPathMonitor()
    m.pathUpdateHandler = { [weak self] path in
      guard let self, self.isRunning else { return }

      let iface = path.availableInterfaces.first(where: { path.usesInterfaceType($0.type) })?.type

      // The monitor always fires once on start with the CURRENT path. That is a
      // report, not a change — acting on it would revive a healthy socket at the
      // start of every session.
      guard self.sawFirstPath else {
        self.sawFirstPath  = true
        self.lastPathIface = iface
        return
      }

      let satisfied = path.status == .satisfied
      let changed   = iface != self.lastPathIface
      self.lastPathIface = iface

      // An unsatisfied path means there is nothing to reconnect TO yet — wait for
      // it to come back rather than burning a recovery attempt on dead air.
      guard satisfied, changed else { return }

      NSLog("[VibePowerModule] network path changed → %@ — treating sockets as suspect",
            String(describing: iface))
      DispatchQueue.main.async {
        // staleAfter: 0 — the path is gone, so don't spend the 8s window proving it.
        self.reviveIfDead(staleAfter: 0)
        // JS owns the spectrum WS and has the same zombie on the same dead flow.
        self.sendEvent(withName: "VibeNetworkPathChanged", body: nil)
      }
    }
    m.start(queue: pathQueue)
    pathMonitor = m
  }

  private func stopPathMonitor() {
    pathMonitor?.cancel()
    pathMonitor = nil
  }

  /// ★★★ HOW MANY TIMES THE WATCHDOG HAS REOPENED THIS SOCKET WITHOUT A SINGLE PACKET COMING BACK.
  ///     Reset by the first packet. An UberSDR server drops an audio socket whose session it has
  ///     never registered (POST /connection) — accepted at the handshake, closed in the same
  ///     breath — and reopening it can NEVER fix that: only re-registering can, and only JS can do
  ///     that. So the watchdog used to retry every 4 seconds for ever, silently, while the user sat
  ///     in front of a perfect waterfall with no sound (issue #20, the UberSDR-over-LAN case).
  /// ★★ THE RETRY THAT CANNOT WORK MUST ASK FOR HELP. Two dead reopens is the same threshold JS
  ///    already uses for the spectrum socket (REOPENS_BEFORE_RECHECK) — reached here in ~8s.
  private var deadRevives = 0
  private var lastStuckAt = Date.distantPast

  private func reviveIfDead(staleAfter: TimeInterval) {
    // externalAudio (OWRX/Kiwi) has NO native WS to revive — the socket + decode
    // live in JS, which drives its own resume. Reviving here would (re)open a
    // UberSDR audio WS to the stale currentBase = UberSDR audio under the OWRX
    // stream. Only the native Opus engine (startAudioEngine) is the watchdog's.
    guard isRunning, !externalAudio, !fmdxAudio, !dataSaverDisconnected else { return }  // FM-DX / data saver own their own WS
    let stale  = Date().timeIntervalSince(lastPacketAt)
    // Packet staleness stays the PRIMARY zombie detector — the regression is
    // "state says alive, frames stop", so wsReady/.running is only a secondary cue.
    let wsDead = wsUsingNW ? !wsReady : (wsTask?.state != .running)
    guard stale > staleAfter || wsDead else { return }
    NSLog("[VibePowerModule] watchdog: stale=%.1fs wsDead=%d — reviving audio WS",
          stale, wsDead ? 1 : 0)
    lastPacketAt = Date() // debounce — one revive attempt per window
    // ★ Counted BEFORE the reopen, so the first attempt is 1 and the ask for help lands on the
    //   second failure rather than the third.
    deadRevives += 1
    // ★★★ ASK ON THE FIRST FAILED REOPEN, NOT THE SECOND. A socket the server accepts and drops in
    //     the same breath is not a flaky link — it is a refusal, and no number of reopens will
    //     change the server's mind. At two, the listener sat in silence for sixteen seconds before
    //     anything tried the one cure that works (measured on the device, 19:03:15 → 19:03:31).
    // ★★ Re-registering is cheap and idempotent — one small POST — so the cost of being early is
    //    nothing, and the cost of being late is the whole point of the feature.
    // ★ The 15 s throttle still stops a genuinely unreachable server from being asked repeatedly.
    if deadRevives >= 1 && Date().timeIntervalSince(lastStuckAt) > 15 {
      lastStuckAt = Date()
      notePath("audio STUCK — \(deadRevives) reopens, no packets")
      NSLog("[VibePowerModule] audio stuck after %d reopens — asking JS to re-register", deadRevives)
      sendEvent(withName: "VibeAudioStuck", body: ["reopens": deadRevives])
    }
    closeAudioWs()
    if let engine = audioEngine, !engine.isRunning {
      try? AVAudioSession.sharedInstance().setActive(true)
      try? engine.start()
      if !isMuted { playerNode?.play() }
    }
    // SAME uuid — decoders + spectrum WS are keyed to it server-side.
    openAudioWs(baseUrl: currentBase, frequency: currentFreq,
                mode: currentMode, uuid: currentUuid)
  }

  @objc func sendTuneCommand(_ frequency: Int, mode: String) {
    currentFreq = frequency
    currentMode = mode
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.pendingTune = (frequency, mode)
      let since = Date().timeIntervalSince(self.lastTuneSentAt)
      if since >= 0.08 {
        self.flushPendingTune()
      } else if self.tuneTimer == nil {
        let t = Timer(timeInterval: 0.08 - since, repeats: false) { [weak self] _ in
          self?.tuneTimer = nil
          self?.flushPendingTune()
        }
        RunLoop.main.add(t, forMode: .common)
        self.tuneTimer = t
      }
      self.updateNowPlaying()
    }
  }

  private func flushPendingTune() {
    guard let t = pendingTune else { return }
    pendingTune = nil
    lastTuneSentAt = Date()
    sendWsJson(["type": "tune", "frequency": t.freq, "mode": t.mode])
    // Drop queued (pre-tune) audio so what you HEAR snaps to the new
    // frequency — fine-tuning SSB through a stale backlog is impossible.
    audioQ.async { [weak self] in
      guard let self else { return }
      /* ★★★ ONLY FLUSH A BACKLOG THAT IS ACTUALLY STALE. This fired at 0.15 s, and the queue sits
       *     comfortably above that in normal running (the upper bound is 0.4 s) — so a stop/play,
       *     which DISCARDS SCHEDULED BUFFERS and is audible, happened on essentially EVERY TUNE,
       *     and on every step of a sweep. Stuart: "the stutter seems to happen around a tuning
       *     event", and "the app sweep tunes slower than the web browser too".
       *  ★★ THE FLUSH ITSELF IS RIGHT and stays — "fine-tuning SSB through a stale backlog is
       *     impossible" is exactly true, and it is why this exists. What was wrong is the
       *     threshold: it has to mean "you would hear the OLD frequency for a noticeable moment",
       *     not "there is a normal amount of audio queued".
       *  ★ 0.35 s is under the 0.4 s bound, so a genuine pile-up after a delivery burst is still
       *    cut, while an ordinary queue is left alone and the tune stays seamless. */
      if self.queuedSeconds > 0.35 {
        self.queuedSeconds = 0
        // ★ Re-arm the cushion. A flush leaves playback hard against the live edge, which is
        //   exactly the state the pre-roll exists to avoid — without this, the first gap after
        //   any tune would underrun again. See scheduleOut.
        self.preRoll = 0
        self.preRollBufs.removeAll(keepingCapacity: true)
        let player = self.playerNode
        DispatchQueue.main.async {
          player?.stop()  // discards scheduled buffers
          if !self.isMuted { player?.play() }
        }
      }
    }
  }

  @objc func sendBandwidth(_ low: Int, high: Int) {
    sendWsJson(["type": "tune", "bandwidthLow": low, "bandwidthHigh": high])
    audioQ.async { [weak self] in
      guard let self else { return }
      // Audio occupies 0..max-edge Hz regardless of sideband
      self.nrBandwidthHz = Double(max(abs(low), abs(high)))
      self.websdrEngine?.syncBins(bandwidthHz: self.nrBandwidthHz,
                                  sampleRate: Double(self.dspRate > 0 ? self.dspRate : 12_000))
    }
  }

  // MARK: - Client noise DSP control

  @objc func setNrMode(_ mode: String) {
    audioQ.async { [weak self] in
      guard let self else { return }
      self.nrModeStr = mode
      // Fresh start on every (re)engage — matches the skin's reset-on-toggle
      self.nr2Engine?.reset();    self.nr2Chunker?.reset()
      self.websdrEngine?.reset(); self.websdrChunker?.reset()
    }
  }

  @objc func setNoiseBlanker(_ on: Bool) {
    audioQ.async { [weak self] in
      guard let self else { return }
      self.nbOn = on
      self.nbEngine?.reset()
    }
  }

  /** Auto notch on/off for network backends (routed here by JS for
   *  UberSDR/OWRX/Kiwi; local/RTL-TCP are handled by the shim). */
  @objc func setNotch(_ on: Bool) {
    audioQ.async { [weak self] in
      guard let self else { return }
      self.notchOn = on
      if !on { for nf in self.notch { nf.reset() } }
    }
  }

  /** Client-side audio squelch gate (network, e.g. Kiwi). JS opens/closes it
   *  from the S-meter level vs the threshold. open=true is the default. */
  @objc func setSquelchOpen(_ open: Bool) {
    audioQ.async { [weak self] in self?.squelchOpen = open }
  }

  /** Forward a raw JSON command over the native audio WS (set_dsp,
   *  set_dsp_params, get_dsp_filters, set_audio_gate, set_squelch — these
   *  are AUDIO-WS message types; the spectrum WS doesn't know them). */
  @objc func sendAudioCommand(_ json: String) {
    sendWsText(json)
  }

  /** audioQ-only. Applies NB → NR/NR2 to the mono packet-rate feed. */
  private func dspProcessMono(_ samples: inout [Float], sr: Int32) {
    if dspRate != sr {
      dspRate = sr
      nbEngine = nil
      nr2Engine = nil; nr2Chunker = nil
      websdrEngine = nil; websdrChunker = nil
    }
    if nbOn {
      if nbEngine == nil { nbEngine = NoiseBlankerEngine(sampleRate: Double(sr)) }
      nbEngine?.process(&samples)
    }
    switch nrModeStr {
    case "nr2":
      if nr2Engine == nil {
        let eng = NR2Engine()
        nr2Engine  = eng
        nr2Chunker = BlockChunker(block: 512) { blk in eng.processHop(blk) }
      }
      if let c = nr2Chunker { samples = c.run(samples) }
    case "nr":
      if websdrEngine == nil {
        let eng = WebSDRNREngine()
        eng.syncBins(bandwidthHz: nrBandwidthHz, sampleRate: Double(sr))
        websdrEngine  = eng
        websdrChunker = BlockChunker(block: WebSDRNREngine.BLOCK) { blk in eng.processWithDelay(blk) }
      }
      if let c = websdrChunker { samples = c.run(samples) }
    default: break
    }
  }

  @objc func setStep(_ hz: Int) {
    currentStep = hz
  }

  @objc func setMediaSkipMode(_ mode: String) {
    skipMode = mode
  }

  /// Car browse tree (bookmarks + band plan) pushed from JS. Cached here for the
  /// CarPlay scene (CPListTemplate) — inert until the CarPlay-audio entitlement
  /// and App Store/TestFlight distribution are in place. See VibeCarPlay.swift.
  @objc func setBrowseItems(_ json: String) {
    VibeCarPlayData.shared.payloadJSON = json
    VibeVoice.setBrowse(json)   // persist for Siri entity resolution (incl. cold)
  }

  // ── Siri voice control ───────────────────────────────────────────────────

  /// JS pushes the default-instance name ('' = none) so the Siri intent can
  /// auto-connect (or tell the user to set a default). Persisted in VibeVoice.
  @objc func setDefaultInstance(_ name: String) { VibeVoice.setDefaultInstance(name) }

  /// JS pushes whether the SDR is connected (app open + live), so the intent can
  /// emit the command now vs stash it for a cold launch.
  @objc func setVoiceConnected(_ connected: Bool) { VibeVoice.setConnected(connected) }

  /// Cold-launch: JS reads the spoken query the intent stashed, once connected.
  @objc func getPendingVoiceQuery(_ resolve: @escaping RCTPromiseResolveBlock,
                                  rejecter _: @escaping RCTPromiseRejectBlock) {
    resolve(VibeVoice.takePending())
  }

  /// The last uncaught ObjC exception, or nil. Local only — see VibeCrashLog.
  @objc func getNativeCrash(_ resolve: @escaping RCTPromiseResolveBlock,
                            rejecter _: @escaping RCTPromiseRejectBlock) {
    resolve(VibeCrashLog.take())
  }

  @objc func clearNativeCrash() { VibeCrashLog.clear() }

  /// Device identifiers for the diagnostics report. No name, no account, no location.
  @objc func getDeviceInfo(_ resolve: @escaping RCTPromiseResolveBlock,
                           rejecter _: @escaping RCTPromiseRejectBlock) {
    resolve([
      "model": VibeCrashLog.hardwareModel(),
      "os": UIDevice.current.systemVersion,
      "systemName": UIDevice.current.systemName,
    ])
  }

  /// Bridge the Siri intent (which runs separately) to JS via a notification.
  private func startVoiceObserver() {
    NotificationCenter.default.addObserver(
      forName: VibeVoice.note, object: nil, queue: .main
    ) { [weak self] n in
      guard let event = n.userInfo?["event"] as? String,
            let body = n.userInfo?["body"] as? [String: Any] else { return }
      self?.sendEvent(withName: event, body: body)
    }
  }

  @objc func setInstanceName(_ name: String) {
    instanceName = name
    DispatchQueue.main.async { self.updateNowPlaying() }
  }

  @objc func setMuted(_ muted: Bool) {
    isMuted = muted
    sendEvent(withName: "VibeMuted", body: ["muted": muted])
    // FM-DX power-saving pause: STOP the MP3 audio stream + release the audio
    // route so it isn't draining battery/network in the background (AirPods out /
    // Bluetooth off). ▶ reopens FM-DX's OWN audio WS + engine — the earlier bug
    // was that pause routed through the UberSDR data-saver path whose resume event
    // only the SDR screen handles, so FM-DX audio never came back. The lightweight
    // /text control WS (JS) is left alone (it self-suspends in the background).
    if fmdxAudio {
      DispatchQueue.main.async {
        if muted {
          self.closeFmdxWs()
          self.playerNode?.stop(); self.audioEngine?.stop()
        } else {
          try? AVAudioSession.sharedInstance().setActive(true)
          self.startEngine()
          self.openFmdxWs()
        }
        self.updateNowPlaying()
      }
      return
    }
    // OWRX/Kiwi (external): tuning + WS live in JS, and an OWRX reconnect RESETS
    // the server to its default profile — so we deliberately do NOT offer a
    // play-to-reconnect card here. PAUSE fully releases the media controls
    // (stopExternalAudio → stopEngine clears the now-playing card); JS closes its
    // WS on the VibeMuted event and shows an in-app reconnect prompt instead, so
    // reconnection (and the profile reset) is always a deliberate user action.
    if externalAudio {
      switch externalPauseMode {
      case "resume":
        // Local / RTL-TCP: no server to disconnect. The shim keeps running and
        // pushExternalPcm drops samples while muted, so mute in place and keep the
        // media card (▶ resumes). We must actually pause()/play() the player node:
        // if we only gate the buffers, iOS still sees a "playing" node and springs
        // the lock-screen button back to ▶. updateNowPlaying() reads isMuted so the
        // card reflects the paused state.
        DispatchQueue.main.async {
          if muted { self.playerNode?.pause() }
          else     { try? self.audioEngine?.start(); self.playerNode?.play() }
          self.updateNowPlaying()
        }
      case "reconnect":
        // Kiwi: behave like UberSDR — pause disconnects but keeps the card, play
        // reconnects. JS closes/reopens the Kiwi WS off the VibeDataSaver* events.
        DispatchQueue.main.async {
          if muted { self.disconnectForPause() }
          else if self.dataSaverDisconnected { self.resumeFromDataSaver() }
        }
      default:  // "release" — OWRX: pause drops the card; reconnect is manual in-app.
        if muted { DispatchQueue.main.async { self.stopExternalAudio() } }
      }
      return
    }
    // Pause = disconnect, Play = reconnect. The server drops the session almost
    // immediately on suspend anyway, and reconnecting is near-instant, so there's
    // no point keeping a muted-but-streaming state — we just disconnect cleanly
    // and show a "Disconnected" card with a working ▶ button.
    DispatchQueue.main.async {
      if muted {
        self.disconnectForPause()
      } else if self.dataSaverDisconnected {
        self.resumeFromDataSaver()
      }
    }
  }

  private func disconnectForPause() {
    guard !dataSaverDisconnected else { return }
    dataSaverDisconnected = true
    healthTimer?.invalidate(); healthTimer = nil
    closeAudioWs()
    isRunning = false
    playerNode?.stop(); audioEngine?.stop()   // releases the audio route (AirPods)
    // KEEP the media session + remote commands so ▶ reconnects; show it as a
    // clearly "Disconnected" card (handled in updateNowPlaying / refreshArtwork).
    DispatchQueue.main.async {
      self.updateNowPlaying()
      MPNowPlayingInfoCenter.default().playbackState = .paused
    }
    sendEvent(withName: "VibeDataSaverDisconnect", body: [:])
  }

  private func resumeFromDataSaver() {
    guard dataSaverDisconnected else { return }
    // Don't reopen the old session here — a partial reopen lands in a broken
    // half-state (frozen waterfall/zoom, no audio). Hand off to JS, which does a
    // full from-scratch reconnect (new uuid → fresh startAudioEngine, which
    // clears our data-saver state). The flag stays set until then so the
    // watchdog won't revive the stale socket.
    sendEvent(withName: "VibeDataSaverResume", body: [:])
  }

  /// JS calls this when a reconnect attempt fails (server full / rate-limited) so
  /// the lock-screen card tells the user to open the app.
  @objc func setReconnectFailed(_ failed: Bool) {
    reconnectFailed = failed
    if failed { dataSaverDisconnected = false }
    DispatchQueue.main.async { self.updateNowPlaying() }
  }

  @objc func setVolume(_ volume: Double) {
    playerNode?.volume = Float(max(0, min(1, volume)))
  }

  @objc func getDebugInfoSync() -> String {
    let eng = audioEngine != nil ? "yes" : "no"
    let dec = opusDecoder != nil ? "yes" : "no"
    let ws  = (wsUsingNW ? wsReady : (wsTask?.state == .running)) ? "open" : "closed"
    return "run=\(isRunning) pkts=\(packetCount) eng=\(eng) dec=\(dec) sr=\(decoderSampleRate) ws=\(ws) rec=\(recArmed)"
  }

  // MARK: - Recording

  @objc func startRecording(_ frequency: NSNumber, mode: NSString,
                            resolver resolve: @escaping RCTPromiseResolveBlock,
                            rejecter reject: @escaping RCTPromiseRejectBlock) {
    guard isRunning else {
      reject("not_running", "Audio engine is not running", nil); return
    }
    // JS passes the LIVE freq/mode — the native currentFreq/currentMode are only
    // tracked on the UberSDR audio-WS path, so for OWRX (external audio) they're
    // stale; fall back to them only if JS didn't supply a value.
    let freqHz = frequency.intValue > 0 ? frequency.intValue : currentFreq
    let modeStr = (mode as String).isEmpty ? currentMode : (mode as String)
    let df = DateFormatter()
    df.dateFormat = "yyyy-MM-dd'T'HH-mm-ss"
    let mhz  = String(format: "%.4fMHz", Double(freqHz) / 1e6)
    let name = "VibeSDR_\(df.string(from: Date()))_\(mhz)_\(modeStr.uppercased()).m4a"
    let url  = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
      .appendingPathComponent(name)
    audioQ.async { [weak self] in
      guard let self else { reject("gone", "module deallocated", nil); return }
      do {
        // AAC straight from the hardware encoder; the file format is fixed
        // 48 kHz so tune/mode/BW changes mid-recording need no handling.
        let settings: [String: Any] = [
          AVFormatIDKey:         kAudioFormatMPEG4AAC,
          AVSampleRateKey:       self.ENGINE_RATE,
          AVNumberOfChannelsKey: Int(self.ENGINE_CH),
          AVEncoderBitRateKey:   128_000,
        ]
        self.recFile = try AVAudioFile(forWriting: url, settings: settings,
                                       commonFormat: .pcmFormatFloat32, interleaved: false)
        self.recPath  = url.path
        self.recArmed = true
        NSLog("[VibePowerModule] recording → %@", name)
        resolve(url.path)
      } catch {
        reject("rec_open", error.localizedDescription, error)
      }
    }
  }

  @objc func stopRecording(_ resolve: @escaping RCTPromiseResolveBlock,
                           rejecter reject: @escaping RCTPromiseRejectBlock) {
    recArmed = false
    audioQ.async { [weak self] in
      guard let self else { resolve(nil); return }
      let path = self.recPath
      self.recFile = nil  // closing the AVAudioFile finalises the .m4a
      self.recPath = ""
      NSLog("[VibePowerModule] recording stopped: %@", path)
      resolve(path.isEmpty ? nil : path)
    }
  }

  /** audioQ-only — called from handlePacket with the converted 48 kHz buffer. */
  private func writeRecording(_ buf: AVAudioPCMBuffer) {
    guard let f = recFile else { return }
    do { try f.write(from: buf) }
    catch {
      NSLog("[VibePowerModule] rec write error: %@", error.localizedDescription)
      recArmed = false
      recFile  = nil
    }
  }

  // MARK: - Location (one-shot, for nearest-first instance sorting)
  // navigator.geolocation doesn't exist in React Native — the JS picker
  // never prompted and the directory fell back to IP geolocation (wildly
  // wrong on cellular). NSLocationWhenInUseUsageDescription is in Info.plist.

  private var locManager: CLLocationManager?
  private var locResolve: RCTPromiseResolveBlock?

  @objc func getLocation(_ resolve: @escaping RCTPromiseResolveBlock,
                         rejecter reject: @escaping RCTPromiseRejectBlock) {
    DispatchQueue.main.async {
      self.locResolve?(nil)  // settle any stale request
      self.locResolve = resolve
      let mgr = self.locManager ?? CLLocationManager()
      self.locManager = mgr
      mgr.delegate = self
      mgr.desiredAccuracy = kCLLocationAccuracyKilometer
      switch mgr.authorizationStatus {
      case .notDetermined:
        mgr.requestWhenInUseAuthorization()  // continues in the delegate
      case .authorizedWhenInUse, .authorizedAlways:
        mgr.requestLocation()
      default:
        self.locResolve = nil
        resolve(nil)  // denied — picker falls back to unsorted/IP order
      }
    }
  }

  /** Half-height iOS share sheet (medium detent, grabber to expand) — skin
   *  parity with the web navigator.share card. Unlike the skin, no save-mode
   *  workaround is needed: the audio WS is native, so presenting the sheet
   *  can't defocus/kill the connection. */
  @objc func shareRecording(_ path: String) {
    DispatchQueue.main.async {
      let url = URL(fileURLWithPath: path)
      guard FileManager.default.fileExists(atPath: path) else {
        NSLog("[VibePowerModule] shareRecording: missing file %@", path); return
      }
      guard let scene = UIApplication.shared.connectedScenes
              .compactMap({ $0 as? UIWindowScene })
              .first(where: { $0.activationState == .foregroundActive })
              ?? UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first,
            let root = (scene.windows.first(where: { $0.isKeyWindow }) ?? scene.windows.first)?
              .rootViewController else {
        NSLog("[VibePowerModule] shareRecording: no root VC"); return
      }
      var top = root
      while let presented = top.presentedViewController { top = presented }
      let avc = UIActivityViewController(activityItems: [url], applicationActivities: nil)
      if let sheet = avc.sheetPresentationController {
        sheet.detents = [.medium(), .large()]
        sheet.selectedDetentIdentifier = .medium
        sheet.prefersGrabberVisible = true
      }
      // ★★★ ANCHOR IT, OR IT CRASHES ON iPad. A UIActivityViewController is ALWAYS
      // UIModalPresentationPopover on a regular-width device, and presenting a popover
      // with no sourceView/sourceRect and no barButtonItem raises an uncatchable
      // ObjC exception inside -[UIPopoverPresentationController
      // presentationTransitionWillBegin]. The sheetPresentationController block above
      // only ever applies on iPhone, so iPad had NOTHING set.
      // ★ Real crash, TestFlight 9.0.1 (8) on an iPad17,1 running iOS 26.5.2, reported
      //   as "crashed after stopping a 30 second recording" — which is exactly when the
      //   share sheet is offered. Present from the centre with no arrow: we are called
      //   from JS and have no sensible source rect to point at.
      if let pop = avc.popoverPresentationController {
        pop.sourceView = top.view
        pop.sourceRect = CGRect(x: top.view.bounds.midX, y: top.view.bounds.midY, width: 1, height: 1)
        pop.permittedArrowDirections = []
      }
      top.present(avc, animated: true)
    }
  }

  // MARK: - Native WebSocket

  /// Open the native audio WS. NWConnection first (see the toggle), with a per-server fallback to
  /// URLSession when it cannot reach the host at all — see wsForceURLSession.
  private func openAudioWs(baseUrl: String, frequency: Int, mode: String, uuid: String) {
    if Self.useNWConnectionAudioWs {
      openAudioWsNW(baseUrl: baseUrl, frequency: frequency, mode: mode, uuid: uuid)
    } else {
      openAudioWsURLSession(baseUrl: baseUrl, frequency: frequency, mode: mode, uuid: uuid)
    }
  }

  /// Tear down whichever transport is live (both calls are no-ops if nil).
  private func closeAudioWs() {
    wsTask?.cancel(with: .goingAway, reason: nil); wsTask = nil; wsSession = nil
    wsConn?.cancel(); wsConn = nil; wsReady = false
  }

  // ── Shared per-packet handling (identical semantics for both transports) ──
  // Returns true if the socket was cycled (sr-flip) so the caller stops re-arming
  // the now-superseded receive loop.
  private func onAudioData(_ data: Data) -> Bool {
    packetCount += 1
    lastPacketAt = Date()
    // ★ A packet is the only proof the session is real. Everything else — a 101, a .ready — is
    //   equally true of a socket the server is about to drop.
    deadRevives = 0
    if wsNeedsTuneAssert {
      wsNeedsTuneAssert = false
      sendWsJson(["type": "tune", "frequency": currentFreq, "mode": currentMode])
    }
    // Header sample-rate flip → server's per-WS opus encoder is now mismatched
    // (see wsBaseSr note) — cycle the socket for a fresh encoder. 3-packet
    // confirmation + 4s cooldown so stragglers around the flip can't storm.
    if data.count > 21 {
      let b = [UInt8](data.prefix(12))
      let sr = Int32(b[8]) | Int32(b[9]) << 8 | Int32(b[10]) << 16 | Int32(b[11]) << 24
      if sr >= 8000 && sr <= 96000 {
        if wsBaseSr == 0 {
          wsBaseSr = sr
        } else if sr != wsBaseSr {
          srFlipCount += 1
          if srFlipCount >= 3, Date().timeIntervalSince(lastSrCycleAt) > 4 {
            NSLog("[VibePowerModule] sample rate %d→%d — cycling WS for a fresh server encoder", wsBaseSr, sr)
            lastSrCycleAt = Date()
            closeAudioWs()                  // bumps to no live socket
            openAudioWs(baseUrl: currentBase, frequency: currentFreq,
                        mode: currentMode, uuid: currentUuid)  // NW: bumps wsGen
            return true                     // new receive loop owns the socket now
          }
        } else {
          srFlipCount = 0
        }
      }
    }
    if packetCount <= 3 {
      NSLog("[VibePowerModule] ws pkt#%d len=%d", packetCount, data.count)
      // ★ The FIRST packet is the one that matters: it separates "the socket never delivered" from
      //   "audio arrived and something downstream ate it", and those have opposite cures.
      if packetCount == 1 { notePath("first audio packet (\(data.count) B)") }
    }
    // Recording must keep decoding through mutes (file taps the converter feed);
    // playback gating happens after conversion.
    if !isMuted || recArmed {
      audioQ.async { self.handlePacket(data) }
    }
    return false
  }

  /// ★★★ WHICH TRANSPORT THIS BASE URL HAS EARNED. Neither is right everywhere:
  ///
  ///  - URLSessionWebSocketTask regressed on iOS 27 beta for the TUNNEL stream — task reports
  ///    `.running`, frames stop, audio dies (c9fd37e8, 26 June). That is why NWConnection became
  ///    the default.
  ///  - NWConnection, on this device, cannot reach a plain `ws://` on a PRIVATE address: the
  ///    connection is aborted at connect (POSIX 53) and never reaches the handshake, while a
  ///    `fetch` to the SAME host and port, at the same moment, succeeds — which is proven every
  ///    time, because the preflight that opens the audio gate is exactly that fetch. Symptom:
  ///    UberSDR over a LAN address has no sound while the tunnel is perfect, OWRX-over-LAN is fine
  ///    (its audio is a JS socket) and the waterfall never falters (Stuart, 2026-08-18).
  ///
  /// ★★ SO CHOOSE PER CONNECTION, AND LET IT PROVE ITSELF. The first attempt uses NWConnection; a
  ///    connect-time abort falls back to URLSession for THIS base URL and remembers it, so a
  ///    reconnect does not pay the timeout again. A static toggle could only ever be wrong for one
  ///    of the two cases — and it was, for eight weeks.
  private var wsForceURLSession = Set<String>()
  /// ★★★ WHICH TRANSPORT IS LIVE RIGHT NOW — not which one is compiled in. Three places asked the
  ///     STATIC toggle instead: the watchdog's "is it dead" test, the debug line, and the TEXT SEND
  ///     that carries every tune and DSP command. On a URLSession socket they would all have read
  ///     the NWConnection's state: `wsReady` is never set there, so the watchdog would have
  ///     reopened a perfectly healthy socket every four seconds and no tune would ever have been
  ///     sent — "audio works but tuning does nothing", a fault we have chased before.
  /// ★★ A fallback is not finished when the connection succeeds. Everything that ASKS about the
  ///    connection has to move with it.
  private var wsUsingNW = true

  // ── Transport A: Network.framework (iOS 27-safe, default) ────────────────
  private func openAudioWsNW(baseUrl: String, frequency: Int, mode: String, uuid: String) {
    // ★ Already learned that this server needs the other transport — go straight there.
    if wsForceURLSession.contains(baseUrl) {
      notePath("using URLSession transport (learned) for \(baseUrl)")
      openAudioWsURLSession(baseUrl: baseUrl, frequency: frequency, mode: mode, uuid: uuid)
      return
    }
    guard let url = audioWsURL(baseUrl: baseUrl, frequency: frequency, mode: mode, uuid: uuid) else {
      NSLog("[VibePowerModule] bad WS URL from base: %@", baseUrl); return
    }
    NSLog("[VibePowerModule] opening audio WS (NWConnection): %@", url.absoluteString)
    wsUsingNW = true
    wsNeedsTuneAssert = true
    wsBaseSr = 0
    srFlipCount = 0
    wsReady = false
    wsGen &+= 1
    let gen = wsGen

    let secure = (url.scheme == "wss")
    let params: NWParameters = secure ? .tls : .tcp   // tunnel cert valid → default trust
    let wsOpts = NWProtocolWebSocket.Options()
    wsOpts.autoReplyPing = true                        // answer server pings natively
    params.defaultProtocolStack.applicationProtocols.insert(wsOpts, at: 0)

    let conn = NWConnection(to: .url(url), using: params)
    wsConn = conn
    conn.stateUpdateHandler = { [weak self] state in
      guard let self, self.isRunning, self.wsGen == gen else { return }
      switch state {
      case .ready:
        self.wsReady = true
        NSLog("[VibePowerModule] audio WS ready")
        self.notePath("audio WS ready")
        self.wsReceive(conn, gen: gen)
      case .waiting(let err):
        // Path not satisfiable yet (e.g. just after airplane-mode off). NWConnection
        // auto-retries toward .ready; don't reconnect here — the watchdog covers a
        // stuck wait via packet staleness.
        NSLog("[VibePowerModule] audio WS waiting: %@", "\(err)")
        self.notePath("audio WS waiting: \(err)")
        // ★★★ .waiting IS NOT ALWAYS "NOT YET". For a plain ws:// on a private address this state
        //     is terminal in practice: NWConnection retries for ever and never connects, so the
        //     watchdog reopened it every four seconds and the listener simply had no sound. Take
        //     the abort as the answer it is, and try the transport that CAN reach this server —
        //     the same one the preflight fetch just used successfully.
        // ★ Only for this base, only once: the flag makes every later reconnect go straight there.
        if !self.wsForceURLSession.contains(baseUrl) {
          self.wsForceURLSession.insert(baseUrl)
          self.notePath("NWConnection aborted — falling back to URLSession for \(baseUrl)")
          NSLog("[VibePowerModule] NWConnection cannot reach %@ — falling back to URLSession", baseUrl)
          conn.cancel()
          DispatchQueue.main.async {
            self.openAudioWsURLSession(baseUrl: baseUrl, frequency: frequency, mode: mode, uuid: uuid)
          }
        }
      case .failed(let err):
        self.wsReady = false
        NSLog("[VibePowerModule] audio WS failed: %@ — reconnecting in 2s", "\(err)")
        self.notePath("audio WS FAILED \(err)")
        self.scheduleAudioWsReconnect(gen: gen)
      case .cancelled:
        self.wsReady = false
      default:
        break
      }
    }
    conn.start(queue: wsQueue)
  }

  private func wsReceive(_ conn: NWConnection, gen: Int) {
    conn.receiveMessage { [weak self] (data, context, _, error) in
      guard let self, self.isRunning, self.wsGen == gen, self.wsConn === conn else { return }
      if let error {
        NSLog("[VibePowerModule] audio WS receive error: %@ — reconnecting", "\(error)")
        self.scheduleAudioWsReconnect(gen: gen); return
      }
      let op = (context?.protocolMetadata(definition: NWProtocolWebSocket.definition)
                as? NWProtocolWebSocket.Metadata)?.opcode
      switch op {
      case .binary:
        if let data, self.onAudioData(data) { return }  // cycled → superseded loop
        if self.wsGen != gen { return }
      case .text:
        if let data {
          self.sendEvent(withName: "VibeWsText",
                         body: ["text": String(decoding: data, as: UTF8.self)])
        }
      case .close:
        NSLog("[VibePowerModule] audio WS closed by peer — reconnecting")
        self.scheduleAudioWsReconnect(gen: gen); return
      default:
        break   // ping/pong auto-handled; continuation frames coalesced
      }
      self.wsReceive(conn, gen: gen)   // re-arm
    }
  }

  /// Idempotent 2s reconnect (a receive error, .failed and a peer .close can all
  /// route here; the wsGen guard makes it fire at most once per generation).
  private func scheduleAudioWsReconnect(gen: Int) {
    DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
      guard let self, self.isRunning, self.wsGen == gen else { return }  // superseded → drop
      // SAME uuid — decoders + spectrum WS are keyed to it server-side.
      self.openAudioWs(baseUrl: self.currentBase, frequency: self.currentFreq,
                       mode: self.currentMode, uuid: self.currentUuid)
    }
  }

  // ══ LOCAL AUDIO PUMP (VibeServer / local hardware /ws/audio) ═══════════════
  //
  // ★★★ WHY THIS EXISTS: iOS used to read this socket in JAVASCRIPT. Every packet arrived in JS,
  // was base64-encoded there and pushed across the bridge — about fifty crossings a second at Opus
  // rates, and roughly twenty-three times the bytes with uncompressed audio (~187 KB/s against
  // ~8). Android has never done that: VibeStreamService opens the socket in Kotlin and hands PCM
  // straight to the engine. This is that, for iOS.
  // ★★ It matters most where there is least headroom. Stuart: "every time we have any real issues
  // it always seems to be JS being starved… gotta think of our users on ancient low end devices."
  // ★ Same NWConnection + NWProtocolWebSocket transport as the UberSDR audio socket above, for the
  //   same reason it was chosen there: URLSessionWebSocketTask regressed on the iOS 27 beta.
  // ★ Frame layout, identical to Android's parser (they must not drift):
  //     [0] channels · [1] format · [2..5] rate, LE · then payload
  //     format 0 = raw int16 PCM · 1 = IMA-ADPCM mono · 2 = ADPCM mid/side · 3 = Opus
  //   ★★ EVERY format is decoded, including ADPCM. Dropping the ones we do not expect would be
  //   silent audio loss on an older server — and as the Android note puts it, silence is a bug you
  //   go and look for, a buzz is one you blame on the radio.

  private var laConn: NWConnection?
  private var laGen  = 0
  private var laTune: String = ""
  private var laBytes = 0            // since the last report
  /// ★ An engine start has been asked for on main and has not landed yet. Without it every frame
  ///   arriving in that window queues another start — see onLocalAudioFrame.
  private var laEngineStarting = false
  private var laBytesAt = Date.distantPast
  private static let adpcmStep: [Int] = [
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,
    88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,
    544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,
    2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
    10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767]
  private static let adpcmIndex: [Int] = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8]

  @objc func startLocalAudio(_ host: String, port: NSNumber, initialTune: String, authSuffix: String,
                             wsBase: String) {
    let p = port.intValue
    if p <= 0 { return }
    let h = host.isEmpty ? "127.0.0.1" : host
    // authSuffix arrives as "&a=b&c=d" (built to append to an existing query); /ws/audio has no
    // query of its own, so the leading & becomes a ?.
    var q = ""
    if !authSuffix.isEmpty {
      q = "?" + (authSuffix.hasPrefix("&") ? String(authSuffix.dropFirst()) : authSuffix)
    }
    // ★★★ A FULL ws BASE WHEN WE HAVE ONE, because host+port CANNOT EXPRESS what this must now
    //     reach. A multi-radio VibeServer puts every radio behind `/r/<id>/…`, and a public one is
    //     served over https (so wss). Rebuilding "ws://host:port" here dropped BOTH: the audio
    //     socket went to the front door, which owns no radio, so the waterfall was perfect and
    //     there was no sound whatsoever (Stuart, 2026-08-12). The host:port form stays for local
    //     hardware on loopback, which has neither a prefix nor TLS.
    let base = wsBase.isEmpty ? "ws://\(h):\(p)"
                             : (wsBase.hasSuffix("/") ? String(wsBase.dropLast()) : wsBase)
    guard let url = URL(string: "\(base)/ws/audio\(q)") else { return }
    laTune = initialTune
    // ★★ THE ENGINE STARTS ON THE FIRST PACKET, NOT HERE — see onLocalAudioFrame. Starting it at
    //    connect left it running and pulling with nothing arriving yet, and that window is audible:
    //    a stutter on connect that settles once the stream catches up (Stuart, build 58).
    //    ★ This is also what iOS did BEFORE this pump existed — the JS reader called
    //      startExternalAudio on its first packet — so the AV session, remote commands, Now Playing
    //      and the volume observer are all set up at exactly the moment they always were. Deferring
    //      restores the arrangement background media controls were verified against; it does not
    //      invent a new one. (Android starts it upfront, which is why I copied that first.)
    stopLocalAudioConn()
    laGen &+= 1
    let gen = laGen
    // ★★★ TLS WHEN THE URL SAYS wss, AND THIS WAS HARDCODED `.tcp`. A VibeServer reached through
    //     its Cloudflare tunnel is wss on 443, so a plain-TCP NWConnection opened the socket and
    //     then spoke unencrypted WebSocket at a TLS endpoint: no handshake ever completed, the
    //     connection sat in `.waiting`, and NOTHING reached the server's log because nothing
    //     arrived. To the listener that is "no audio" — and no TUNING either, because tuning and
    //     every control ride this same socket — while the waterfall plays perfectly, since the
    //     spectrum socket is opened in JavaScript by React Native, which has always done TLS.
    //     Exactly the report: "still playing audio but not tuning", and on a fresh connect no
    //     audio at all (Stuart, 2026-08-23, on TestFlight against the XCover).
    //  ★★ THE OTHER TWO NWConnection SITES IN THIS FILE ALREADY DO THIS — line ~646 (FM-DX) and
    //     ~1558 (UberSDR audio), both `secure ? .tls : .tcp`. This one was written for the LOCAL
    //     shim on loopback, where plain ws is all there is, and never revisited when the same call
    //     started being pointed at a public tunnelled server. One place updated, the others quietly
    //     left behind — the same shape as the audio-socket bug this pump was built to fix.
    //  ★ Android was unaffected throughout: OkHttp reads the scheme from the URL, so there was
    //    never a decision to forget.
    let secure = (url.scheme == "wss")
    let params: NWParameters = secure ? .tls : .tcp
    let wsOpts = NWProtocolWebSocket.Options()
    wsOpts.autoReplyPing = true
    params.defaultProtocolStack.applicationProtocols.insert(wsOpts, at: 0)
    let conn = NWConnection(to: .url(url), using: params)
    laConn = conn
    conn.stateUpdateHandler = { [weak self] state in
      guard let self, self.laGen == gen else { return }
      switch state {
      case .ready:
        NSLog("[VibePowerModule] local audio WS ready: %@", url.absoluteString)
        self.sendEvent(withName: "VibeLocalAudioState", body: ["state": "ready"])
        if !self.laTune.isEmpty { self.sendLocalTune(self.laTune) }
        self.laReceive(conn, gen: gen)
      case .failed(let err):
        NSLog("[VibePowerModule] local audio WS failed: %@", "\(err)")
        self.sendEvent(withName: "VibeLocalAudioState", body: ["state": "failed: \(err)"])
      // ★★ WAITING IS THE ONE THAT WAS INVISIBLE. NWConnection sits in .waiting on a refused or
      //    unreachable endpoint and RETRIES QUIETLY — no failure, no bytes, nothing in the server's
      //    log because nothing ever completed a handshake. It presents to the user as "no audio"
      //    with everything else working, which is exactly the report.
      case .waiting(let err):
        NSLog("[VibePowerModule] local audio WS waiting: %@", "\(err)")
        self.sendEvent(withName: "VibeLocalAudioState", body: ["state": "waiting: \(err)"])
      case .cancelled:
        self.sendEvent(withName: "VibeLocalAudioState", body: ["state": "cancelled"])
      default:
        break
      }
    }
    conn.start(queue: wsQueue)
  }

  @objc func sendLocalTune(_ json: String) {
    laTune = json
    guard let conn = laConn, let data = json.data(using: .utf8) else { return }
    let meta = NWProtocolWebSocket.Metadata(opcode: .text)
    let ctx  = NWConnection.ContentContext(identifier: "tune", metadata: [meta])
    conn.send(content: data, contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in })
  }

  @objc func stopLocalAudio() {
    // ★ A fresh session starts with a fresh cushion — see scheduleOut's pre-roll.
    audioQ.async { [weak self] in
      self?.preRoll = 0
      self?.preRollBufs.removeAll(keepingCapacity: true)
      self?.queuedSeconds = 0
    }
    stopLocalAudioConn()
    stopExternalAudio()
  }

  private func stopLocalAudioConn() {
    laGen &+= 1              // supersede every in-flight callback
    laConn?.cancel()
    laConn = nil
  }

  private func laReceive(_ conn: NWConnection, gen: Int) {
    conn.receiveMessage { [weak self] (data, context, _, error) in
      guard let self, self.laGen == gen, self.laConn === conn else { return }
      if error != nil { return }
      let op = (context?.protocolMetadata(definition: NWProtocolWebSocket.definition)
                as? NWProtocolWebSocket.Metadata)?.opcode
      if op == .binary, let data {
        // ★ COUNT EVERY BYTE THAT CROSSED THE LINK, before anything can drop the frame — the JS
        //   reader this replaces counted at exactly this point, and for the same reason.
        //   Reported at ~2 Hz rather than per packet: the meter updates about once a second, and
        //   fifty bridge events a second to feed it would reintroduce what the pump just removed.
        self.laBytes += data.count
        let now = Date()
        if now.timeIntervalSince(self.laBytesAt) >= 0.5 {
          let n = self.laBytes
          self.laBytes = 0
          self.laBytesAt = now
          self.sendEvent(withName: "VibeLocalAudioBytes", body: ["bytes": n])
        }
        self.onLocalAudioFrame(data)
      }
      if op == .close { return }
      self.laReceive(conn, gen: gen)
    }
  }

  private func onLocalAudioFrame(_ raw: Data) {
    // ★★ REBASE TO A 0-INDEXED BUFFER FIRST. A Data may be a SLICE whose startIndex is not zero,
    //    and subscripting it as if it were reads the wrong bytes — which here would not crash, it
    //    would just produce noise, and noise gets blamed on the radio. Copying a ~2-8 KB frame is
    //    nothing beside the base64 round trip this whole pump exists to delete.
    let b = [UInt8](raw)
    guard b.count > 6 else { return }
    let channels = Int(b[0]) == 2 ? 2 : 1
    let format   = Int(b[1])
    let rate = Int(b[2]) | (Int(b[3]) << 8) | (Int(b[4]) << 16) | (Int(b[5]) << 24)
    guard rate > 0 else { return }

    // ★ First audio starts the engine, at the rate the STREAM is actually using rather than an
    //   assumed 48k. feedExternalPcm/Opus both refuse while externalAudio is false, so this must
    //   happen before the feed below, not after it.
    // ★★★ ON MAIN, NEVER ON THIS QUEUE — THIS CRASHED THE APP (Stuart, 2026-08-16, build 134).
    //     `startExternalAudio` begins with `stopEngine()`, which sends `stop()` to the player node
    //     and the engine. This runs on the WS queue, while every other engine lifecycle call in
    //     this file hops to main — the mute path, the route-change observer, the config-change
    //     restart. So two threads tore at one AVAudioEngine, and AVFAudio aborted inside its own
    //     attach/engine lock:
    //         std::__1::recursive_mutex::lock()  →  __throw_system_error  →  abort
    //         AVAudioNodeImplBase::GetAttachAndEngineLock()  ←  -[AVAudioPlayerNode stop]
    //     ★★ AND IT IS UNCATCHABLE. That is a C++ exception thrown through Objective-C frames, so
    //        it terminates rather than surfacing anywhere Swift could `try`. Exactly the property
    //        that makes the sibling `play()`-on-a-stopped-engine crash unfixable after the fact:
    //        the ONLY defence is never to make the call from two threads.
    // ★ The frame that discovers the engine is missing is dropped rather than fed into one that
    //   does not exist yet. It is a few milliseconds at the very start of a stream, and inaudible;
    //   `laEngineStarting` stops the next thousand frames each asking for their own engine while
    //   the first request is still in flight.
    if !externalAudio {
      if !laEngineStarting {
        laEngineStarting = true
        let r = rate
        DispatchQueue.main.async {
          self.startExternalAudio(NSNumber(value: r), pauseMode: "resume")
          self.laEngineStarting = false
        }
      }
      return
    }

    switch format {
    case 3:
      feedExternalOpus(Data(b[6...]), sampleRate: rate, channels: channels)
    case 0:
      feedExternalPcm(Data(b[6...]), sampleRate: Double(rate), stereo: channels == 2)
    case 1, 2:
      guard b.count > 8 else { return }
      let count = Int(b[6]) | (Int(b[7]) << 8)
      guard count > 0 else { return }
      let nb = (count + 1) / 2
      let mid = adpcmDecode(b, offset: 8, count: count)
      var pcm: [Int16]
      if format == 2 && b.count >= 8 + 4 + nb + 4 + nb {
        let side = adpcmDecode(b, offset: 8 + 4 + nb, count: count)
        pcm = [Int16](repeating: 0, count: count * 2)
        for i in 0..<count {
          let m = Int(mid[i]), sd = Int(side[i])
          pcm[i*2]   = Int16(max(-32768, min(32767, m + sd)))
          pcm[i*2+1] = Int16(max(-32768, min(32767, m - sd)))
        }
      } else {
        pcm = mid
      }
      pcm.withUnsafeBufferPointer { buf in
        feedExternalPcm(Data(buffer: buf), sampleRate: Double(rate), stereo: format == 2)
      }
    default:
      break   // an unknown format is dropped, never guessed at — see the note above
    }
  }

  /// IMA-ADPCM, ported from VibeStreamService.adpcmDecodeBlock. The two MUST stay identical.
  private func adpcmDecode(_ b: [UInt8], offset off: Int, count: Int) -> [Int16] {
    guard b.count >= off + 4 else { return [] }
    var predictor = Int(Int16(truncatingIfNeeded: Int(b[off]) | (Int(b[off+1]) << 8)))
    var index = max(0, min(88, Int(b[off+2])))
    var out = [Int16](repeating: 0, count: count)
    var bi = off + 4
    var s = 0
    while s < count && bi < b.count {
      let byte = Int(b[bi]); bi += 1
      var half = 0
      while half < 2 && s < count {
        let nib = (half == 0) ? (byte & 0x0F) : ((byte >> 4) & 0x0F)
        let step = VibePowerModule.adpcmStep[index]
        var diff = step >> 3
        if nib & 1 != 0 { diff += step >> 2 }
        if nib & 2 != 0 { diff += step >> 1 }
        if nib & 4 != 0 { diff += step }
        if nib & 8 != 0 { diff = -diff }
        predictor = max(-32768, min(32767, predictor + diff))
        index = max(0, min(88, index + VibePowerModule.adpcmIndex[nib]))
        out[s] = Int16(predictor); s += 1
        half += 1
      }
    }
    return out
  }

  // ── Transport B: URLSessionWebSocketTask (legacy, iOS 26 path) ───────────
  private func openAudioWsURLSession(baseUrl: String, frequency: Int, mode: String, uuid: String) {
    guard let url = audioWsURL(baseUrl: baseUrl, frequency: frequency, mode: mode, uuid: uuid) else {
      NSLog("[VibePowerModule] bad WS URL from base: %@", baseUrl); return
    }
    NSLog("[VibePowerModule] opening audio WS (URLSession): %@", url.absoluteString)
    wsUsingNW = false
    let session = URLSession(configuration: .default)
    wsSession = session
    let task = session.webSocketTask(with: url)
    wsTask = task
    wsNeedsTuneAssert = true
    wsBaseSr = 0
    srFlipCount = 0
    task.resume()
    receiveLoopURLSession(task: task)
  }

  private func audioWsURL(baseUrl: String, frequency: Int, mode: String, uuid: String) -> URL? {
    var s = baseUrl.trimmingCharacters(in: .whitespaces)
    if s.hasPrefix("https://") { s = "wss://" + s.dropFirst(8) }
    else if s.hasPrefix("http://") { s = "ws://" + s.dropFirst(7) }
    s = s.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
    var path = "/ws?user_session_id=\(uuid)&frequency=\(frequency)&mode=\(mode)&format=opus&version=2"
    // ★★★ NAME OURSELVES HERE TOO, BECAUSE THIS SOCKET USUALLY ARRIVES FIRST. The JS client delays
    //     the spectrum socket a second to let the session register, so it is THIS one that claims
    //     the occupant slot — and it was anonymous, so the server stamped an empty agent and the
    //     owner's admin page listed a VibeSDR session as "browser / unknown" while the connection
    //     log, written from the spectrum socket, named it correctly (Stuart, 2026-08-14). A query
    //     parameter is the only thing a WebSocket client can always control; the header is not ours
    //     to set (see UberSDRClient's CLIENT_Q).
    // ★ Same shape as the JS USER_AGENT — operators write filter rules against this string, so only
    //   the version may move.
    let ver = (Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String) ?? "10"
    if let c = "VibeSDR/\(ver) (+https://vibesdr.net)"
        .addingPercentEncoding(withAllowedCharacters: .alphanumerics) {
      path += "&client=\(c)"
    }
    if !bypassPassword.isEmpty,
       let pw = bypassPassword.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) {
      path += "&password=\(pw)"
    }
    // ★ Already percent-safe and already carrying its leading '&' — it is the same string the
    //   spectrum socket appends, built once in JS so the two cannot disagree about the credential.
    path += adminSuffix
    return URL(string: s + path)
  }

  private func receiveLoopURLSession(task: URLSessionWebSocketTask) {
    task.receive { [weak self] result in
      guard let self, self.isRunning, self.wsTask === task else { return }
      switch result {
      case .success(let msg):
        switch msg {
        case .data(let data):
          if self.onAudioData(data) { return }   // cycled → new receive loop owns
        case .string(let text):
          // dsp_filters / dsp_status / dsp_error etc. — JS owns the server-NR
          // UI, so forward every text message up as an event.
          self.sendEvent(withName: "VibeWsText", body: ["text": text])
        @unknown default: break
        }
        self.receiveLoopURLSession(task: task)

      case .failure(let err):
        NSLog("[VibePowerModule] ws error: %@ — reconnecting in 2s", err.localizedDescription)
        guard self.isRunning else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
          // ★ `wsUsingNW`, not the compile-time toggle: this reconnect belongs to whichever
          //   transport is LIVE. Asking the static flag disabled it entirely for a socket that had
          //   fallen back to URLSession — the fallback would have worked once and never recovered.
          guard self.isRunning, !self.wsUsingNW else { return }
          // MUST reconnect with the SAME session uuid — audio extensions
          // (decoders) and the spectrum WS are keyed to it server-side. A
          // fresh UUID here silently orphans them ("no active audio session").
          self.openAudioWs(
            baseUrl: self.currentBase,
            frequency: self.currentFreq,
            mode: self.currentMode,
            uuid: self.currentUuid
          )
        }
      }
    }
  }

  /// Transport-agnostic text send (tune asserts, DSP commands). No-op unless the
  /// active socket is connected.
  private func sendWsText(_ text: String) {
    if wsUsingNW {
      guard let conn = wsConn, wsReady, let data = text.data(using: .utf8) else { return }
      let md  = NWProtocolWebSocket.Metadata(opcode: .text)
      let ctx = NWConnection.ContentContext(identifier: "send", metadata: [md])
      conn.send(content: data, contentContext: ctx, isComplete: true,
                completion: .contentProcessed { err in
                  if let err { NSLog("[VibePowerModule] ws send error: %@", "\(err)") }
                })
    } else {
      guard let task = wsTask, task.state == .running else { return }
      task.send(.string(text)) { err in
        if let err { NSLog("[VibePowerModule] ws send error: %@", err.localizedDescription) }
      }
    }
  }

  private func sendWsJson(_ obj: [String: Any]) {
    guard let data = try? JSONSerialization.data(withJSONObject: obj),
          let str  = String(data: data, encoding: .utf8) else { return }
    sendWsText(str)
  }

  // MARK: - Engine

  /// ★★★ ENGINE LIFECYCLE IS MAIN-THREAD ONLY, and this is the net rather than the rule. A
  ///     background caller reaching `stop()` while main is rebuilding the engine made AVFAudio
  ///     abort inside its own attach lock — a C++ exception through ObjC frames, so nothing could
  ///     catch it and the app simply died (build 134). One stray call site was enough.
  /// ★★ Rerouted rather than asserted: an assertion would trade a crash for a crash. Every real
  ///    caller is already on main, so this costs nothing and closes the door on the next one.
  private func stopEngine() {
    if !Thread.isMainThread { DispatchQueue.main.async { self.stopEngine() }; return }
    isRunning = false
    // ★ The real audio has gone — whatever was holding the keep-alive may need it again.
    //   See vibeSetRealAudioPlaying: two AVAudioEngines on one session is the fault this avoids.
    vibeSetRealAudioPlaying(false)
    // Reset external mode so switching OWRX→UberSDR doesn't leave us in external
    // mode (which made UberSDR pause take the external release path).
    // startExternalAudio re-sets it true right after its stopEngine() call.
    externalAudio = false
    fmdxAudio = false
    externalPauseMode = "release"
    healthTimer?.invalidate()
    healthTimer = nil
    stopPathMonitor()
    closeAudioWs()
    closeFmdxWs()
    destroyDecoder()
    recArmed = false
    audioQ.async { [weak self] in
      guard let self else { return }
      self.recFile   = nil  // closing the AVAudioFile finalises the .m4a
      self.converter = nil
      self.converterInFmt = nil
      self.dspRate = 0      // force DSP engine rebuild on next session
    }
    playerNode?.stop()
    audioEngine?.stop()
    playerNode  = nil
    audioEngine = nil
    audioFormat = nil
    MPNowPlayingInfoCenter.default().nowPlayingInfo = nil
  }

  private func configureAVSession() {
    do {
      let s = AVAudioSession.sharedInstance()
      try s.setCategory(.playback, mode: .default)
      try s.setActive(true)
      NSLog("[VibePowerModule] AVAudioSession active")
    } catch {
      NSLog("[VibePowerModule] AVAudioSession error: %@", error.localizedDescription)
    }
    NotificationCenter.default.addObserver(
      forName: AVAudioSession.interruptionNotification, object: nil, queue: nil
    ) { [weak self] note in
      guard let self else { return }
      let type = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
      if type == AVAudioSession.InterruptionType.began.rawValue {
        // Something grabbed the audio session — could be transient (Siri in the
        // car, a phone call) or persistent (a Mac took the shared AirPods). iOS
        // pauses us; sync our state so the UI shows muted. NB this calls
        // disconnectForPause() → isRunning=false, so .ended must NOT gate on it.
        guard self.isRunning else { return }
        DispatchQueue.main.async { if !self.isMuted { self.setMuted(true) } }
      } else if type == AVAudioSession.InterruptionType.ended.rawValue {
        // Only auto-resume when iOS says the interruption was transient
        // (.shouldResume) — e.g. Siri voice tuning in CarPlay. Without this the
        // Siri interruption disconnected VibeSDR and it sat dead until a manual
        // Play. A persistent takeover (no .shouldResume) still waits for Play.
        let opts = note.userInfo?[AVAudioSessionInterruptionOptionKey] as? UInt ?? 0
        let shouldResume = (opts & AVAudioSession.InterruptionOptions.shouldResume.rawValue) != 0
        guard shouldResume else { return }
        try? AVAudioSession.sharedInstance().setActive(true)
        DispatchQueue.main.async {
          if self.dataSaverDisconnected {
            self.setMuted(false)   // full reconnect, identical to pressing ▶
          } else if self.fmdxAudio && self.isMuted {
            self.setMuted(false)   // FM-DX mutes in place on interruption → unmute to resume
          } else if !self.isMuted {
            try? self.audioEngine?.start(); self.playerNode?.play()
          }
        }
      }
    }
    // Car-audio route → gates band-aware auto mode/step on iPhone. Works through
    // the normal media route (CarPlay or car Bluetooth) with no CarPlay
    // entitlement; emits VibeCarConnected on every route change.
    NotificationCenter.default.addObserver(
      forName: AVAudioSession.routeChangeNotification, object: nil, queue: nil
    ) { [weak self] _ in self?.emitCarConnected() }
    emitCarConnected()
  }

  private func isCarAudioRoute() -> Bool {
    return AVAudioSession.sharedInstance().currentRoute.outputs.contains {
      $0.portType == .carAudio
    }
  }

  private var lastCarConnected: Bool?
  private func emitCarConnected() {
    let connected = isCarAudioRoute()
    if connected == lastCarConnected { return }   // de-dupe route-change spam
    lastCarConnected = connected
    sendEvent(withName: "VibeCarConnected", body: ["connected": connected])
  }

  private func startEngine() {
    // ★ Same rule as stopEngine — attaching a node touches the same lock that stopping one does.
    if !Thread.isMainThread { DispatchQueue.main.async { self.startEngine() }; return }
    /* ★★★ AND THE SILENT KEEP-ALIVE LOOP STANDS DOWN. It exists only to make iOS count us as an
     *   audio app in the seconds before this engine exists; from here on THIS engine does that.
     *   Left running it is a SECOND AVAudioEngine on the same session — see the note in
     *   VibeSilentAudio — which is exactly the sort of contention that produces stutter and
     *   dropouts, and it never stopped while Buddy was on the wrist. */
    vibeSetRealAudioPlaying(true)
    let engine = AVAudioEngine()
    let player = AVAudioPlayerNode()
    engine.attach(player)
    guard let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                  sampleRate: ENGINE_RATE,
                                  channels: ENGINE_CH,
                                  interleaved: false) else {
      NSLog("[VibePowerModule] AVAudioFormat init failed"); return
    }
    engine.connect(player, to: engine.mainMixerNode, format: fmt)
    do {
      try engine.start()
      player.play()
      NSLog("[VibePowerModule] engine started %.0fHz %dch", ENGINE_RATE, Int(ENGINE_CH))
      notePath("engine started")
    } catch {
      NSLog("[VibePowerModule] engine start error: %@", error.localizedDescription)
      notePath("engine START FAILED \(error.localizedDescription)"); return
    }
    audioEngine = engine
    playerNode  = player
    audioFormat = fmt

    // Route changes (AirPods connect etc.) still need an engine restart, but
    // the format is fixed so a restart can never disagree with the decoder.
    NotificationCenter.default.addObserver(
      forName: .AVAudioEngineConfigurationChange, object: engine, queue: nil
    ) { [weak self] _ in
      guard let self, self.isRunning else { return }
      NSLog("[VibePowerModule] config change — restarting")
      DispatchQueue.main.async {
        self.playerNode?.stop(); self.audioEngine?.stop()
        self.playerNode = nil;   self.audioEngine = nil; self.audioFormat = nil
        self.startEngine()
      }
    }
  }

  // MARK: - Packet parsing

  private func handlePacket(_ data: Data) {
    let headerLen = 21
    guard data.count > headerLen else { return }
    let bytes = [UInt8](data)
    let sr = Int32(bytes[8]) | Int32(bytes[9]) << 8 | Int32(bytes[10]) << 16 | Int32(bytes[11]) << 24
    let ch = Int32(bytes[12])
    guard sr >= 8000, sr <= 96000, ch == 1 || ch == 2 else { return }

    // SNR meter = radiod channel SNR = basebandPower − noiseDensity (both dBFS),
    // carried per-packet. This is the demodulator's own channel measurement, so
    // it's independent of the spectrum/zoom — same as UberSDR's meter.
    let bb = Float(bitPattern: UInt32(bytes[13]) | UInt32(bytes[14]) << 8 | UInt32(bytes[15]) << 16 | UInt32(bytes[16]) << 24)
    let nd = Float(bitPattern: UInt32(bytes[17]) | UInt32(bytes[18]) << 8 | UInt32(bytes[19]) << 16 | UInt32(bytes[20]) << 24)
    if bb > -900, nd > -900 {
      let now = ProcessInfo.processInfo.systemUptime
      if now - lastSignalEmit > 0.2 {
        lastSignalEmit = now
        sendEvent(withName: "VibeSignal", body: ["snr": Double(bb - nd), "dbfs": Double(bb)])
      }
    }

    let opusLen = data.count - headerLen
    if opusLen < 3 { return }

    if opusDecoder == nil || decoderSampleRate != sr || decoderChannels != ch {
      initDecoder(sr: sr, ch: ch)
    }
    guard let decoder = opusDecoder else { return }

    let maxSamples = Int(FRAME_SIZE) * Int(ch)
    var pcm16 = [Int16](repeating: 0, count: maxSamples)
    let decoded = data.withUnsafeBytes { raw -> Int32 in
      let ptr = raw.baseAddress!.advanced(by: headerLen).assumingMemoryBound(to: UInt8.self)
      return opus_decode(decoder, ptr, Int32(opusLen), &pcm16, FRAME_SIZE, 0)
    }
    guard decoded > 0 else { return }

    let total = Int(decoded) * Int(ch)
    var pcmF  = [Float](repeating: 0, count: total)
    vDSP_vflt16(&pcm16, 1, &pcmF, 1, vDSP_Length(total))
    var scale: Float = 1.0 / 32768.0
    var pcmFScaled = [Float](repeating: 0, count: total)
    vDSP_vsmul(&pcmF, 1, &scale, &pcmFScaled, 1, vDSP_Length(total))

    // Client noise DSP (NB / NR / NR2) on the mono feed at the packet rate —
    // the recorder downstream captures the processed audio, like the skin.
    if ch == 1 && (nbOn || nrModeStr != "off") {
      dspProcessMono(&pcmFScaled, sr: sr)
    }

    // Packet-rate stereo buffer (mono duplicated to both channels) so the
    // converter only ever resamples 2ch→2ch — no channel-map ambiguity.
    let frameCount = Int(decoded)
    guard let inFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                    sampleRate: Double(sr),
                                    channels: ENGINE_CH,
                                    interleaved: false),
          let inBuf = AVAudioPCMBuffer(pcmFormat: inFmt,
                                       frameCapacity: AVAudioFrameCount(frameCount))
    else { return }
    inBuf.frameLength = AVAudioFrameCount(frameCount)
    let left  = inBuf.floatChannelData![0]
    let right = inBuf.floatChannelData![1]
    if ch == 1 {
      for i in 0..<frameCount { let v = pcmFScaled[i]; left[i] = v; right[i] = v }
    } else {
      for i in 0..<frameCount { left[i] = pcmFScaled[i*2]; right[i] = pcmFScaled[i*2+1] }
    }

    guard let outBuf = convertTo48k(inBuf) else { return }
    if recArmed { writeRecording(outBuf) }
    if !isMuted { scheduleOut(outBuf) }
  }

  /** Resample a packet-rate buffer to the fixed engine format. Runs on
   *  audioQ, same thread as the decoder swap — a rate flip rebuilds the
   *  converter and the very next packet is already correct. */
  private func convertTo48k(_ inBuf: AVAudioPCMBuffer) -> AVAudioPCMBuffer? {
    guard let fmt = audioFormat else { return nil }
    // Only skip conversion when BOTH rate and channel count match — a 48 kHz MONO
    // input (OWRX WFM HD audio) has the engine's rate but not its stereo layout;
    // returning it unconverted scheduled a mono buffer on the stereo node → crash.
    if inBuf.format.sampleRate == fmt.sampleRate && inBuf.format.channelCount == fmt.channelCount { return inBuf }
    if converter == nil || converterInFmt != inBuf.format {
      converter      = AVAudioConverter(from: inBuf.format, to: fmt)
      converterInFmt = inBuf.format
      NSLog("[VibePowerModule] converter %.0f→%.0fHz", inBuf.format.sampleRate, fmt.sampleRate)
    }
    guard let conv = converter else { return nil }
    let ratio  = fmt.sampleRate / inBuf.format.sampleRate
    let outCap = AVAudioFrameCount(Double(inBuf.frameLength) * ratio) + 64
    guard let outBuf = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: outCap) else { return nil }
    var fed = false
    var err: NSError?
    let status = conv.convert(to: outBuf, error: &err) { _, outStatus in
      if fed { outStatus.pointee = .noDataNow; return nil }
      fed = true
      outStatus.pointee = .haveData
      return inBuf
    }
    if status == .error {
      NSLog("[VibePowerModule] convert error: %@", err?.localizedDescription ?? "?")
      return nil
    }
    return outBuf.frameLength > 0 ? outBuf : nil
  }

  // Last time ensureRendering() forced a session/engine/player recovery — debounce
  // so a burst of buffers can't thrash setActive.
  private var lastRenderKick: TimeInterval = 0

  /** Make sure the player is ACTUALLY rendering before we schedule real audio.
   *  UberSDR opens its WS ~1s AFTER startEngine, so startEngine's play() runs on
   *  an empty player; on iOS 27 a play()-before-any-buffer leaves the node idle
   *  and it never re-engages when buffers finally arrive (UberSDR-only: OWRX/Kiwi
   *  push PCM immediately so they never hit the gap). The system audio session can
   *  also wedge (observed nominal sample rate 0) and survive an app force-quit —
   *  only a full session rebuild (a phone call, or Kiwi warming it first) cured it.
   *  This applies that rebuild proactively the moment we have a buffer to play. */
  private func ensureRendering() {
    guard isRunning else { return }
    let engineDown = !(audioEngine?.isRunning ?? false)
    let playerDown = !(playerNode?.isPlaying ?? false)
    guard engineDown || playerDown else { return }
    let now = ProcessInfo.processInfo.systemUptime
    guard now - lastRenderKick > 2 else { return }   // one rebuild per 2s
    lastRenderKick = now
    NSLog("[VibePowerModule] ensureRendering: engineDown=%d playerDown=%d — rebuilding session",
          engineDown, playerDown)
    DispatchQueue.main.async { [weak self] in
      guard let self, self.isRunning else { return }
      let s = AVAudioSession.sharedInstance()
      // Deactivate→reactivate clears a wedged session (sample-rate 0) that a bare
      // setActive(true) won't — this is what the phone-call teardown did.
      try? s.setActive(false, options: .notifyOthersOnDeactivation)
      try? s.setCategory(.playback, mode: .default)
      try? s.setActive(true)
      if !(self.audioEngine?.isRunning ?? false) { try? self.audioEngine?.start() }
      if !(self.playerNode?.isPlaying ?? false) { self.playerNode?.play() }
    }
  }

  private func scheduleOut(_ buf: AVAudioPCMBuffer) {
    guard let player = playerNode, let fmt = audioFormat else { return }
    ensureRendering()
    // Auto notch (network backends): adaptive line enhancer per channel, applied
    // on the final 48 kHz feed. Runs on audioQ (single-threaded), so the filter
    // state is safe. Local/RTL-TCP never enable it here (shim already notched).
    if notchOn, let chans = buf.floatChannelData {
      let nch = Int(buf.format.channelCount)
      while notch.count < nch { notch.append(AutoNotch()) }
      let n = Int(buf.frameLength)
      for c in 0..<nch { notch[c].process(chans[c], n) }
    }
    // Squelch closed → output silence (keeps the player fed, no underrun).
    if !squelchOpen, let chans = buf.floatChannelData {
      let nch = Int(buf.format.channelCount)
      let n = Int(buf.frameLength)
      for c in 0..<nch { memset(chans[c], 0, n * MemoryLayout<Float>.size) }
    }
    // Live-edge bound: if a delivery burst piles up more than ~0.4s of queued
    // audio, drop instead of scheduling — latency stays bounded instead of
    // accumulating forever (runs on audioQ; queuedSeconds audioQ-only).
    let dur = Double(buf.frameLength) / fmt.sampleRate
    if queuedSeconds > 0.4 { return }
    /* ★★★ A PRE-ROLL, BECAUSE THERE WAS A CEILING ON LATENCY AND NO FLOOR. Buffers were scheduled
     *     the instant they arrived, so playback sat hard against the live edge with nothing
     *     queued — and the FIRST source gap became an underrun.
     *  ★★★ AND THE SOURCE GAPS ARE REAL, EXPECTED, AND NOT OURS. An RTL gain change costs about
     *      40 ms of broken audio at the HARDWARE (measured here, and SDR++ dips on every change
     *      too), so the AGC produces one every time it moves. Stuart, with his hands off the
     *      controls entirely: "the audio is stuttering on agc action it seems. So must be the
     *      buffer." It is.
     *  ★★★ THIS IS WHY THE BROWSER DOES NOT STUTTER ON THE SAME STREAM. WebAudio buffers far more
     *      deeply and rides straight through a 40 ms hole; we had no cushion at all to ride on.
     *      The stream is identical — the difference was entirely on this side.
     *  ★★ 220 ms COSTS 220 ms OF LATENCY, DELIBERATELY. That is the trade a jitter buffer IS, and
     *     it is well under the 0.4 s ceiling above, so the live-edge bound still does its job.
     *     Once playback starts this depth persists: playback runs a fixed distance behind arrival,
     *     so the cushion is there for every later gap, not just the first.
     *  ★ Held on audioQ like everything else here, and released in arrival order. */
    if preRoll < kPreRollSeconds {
      preRoll += dur
      preRollBufs.append(buf)
      guard preRoll >= kPreRollSeconds else { return }
      let held = preRollBufs
      preRollBufs.removeAll(keepingCapacity: true)
      for b in held {
        let d = Double(b.frameLength) / fmt.sampleRate
        queuedSeconds += d
        player.scheduleBuffer(b) { [weak self] in
          guard let self else { return }
          self.audioQ.async { self.queuedSeconds = max(0, self.queuedSeconds - d) }
        }
      }
      return
    }
    queuedSeconds += dur
    player.scheduleBuffer(buf) { [weak self] in
      guard let self else { return }
      self.audioQ.async { self.queuedSeconds = max(0, self.queuedSeconds - dur) }
    }
  }

  // MARK: - Opus lifecycle

  private func initDecoder(sr: Int32, ch: Int32) {
    destroyDecoder()
    var err: Int32 = 0
    opusDecoder = opus_decoder_create(sr, ch, &err)
    NSLog("[VibePowerModule] decoder create sr=%d ch=%d err=%d", sr, ch, err)
    decoderSampleRate = sr
    decoderChannels   = ch
  }

  private func destroyDecoder() {
    if let d = opusDecoder { opus_decoder_destroy(d) }
    opusDecoder = nil; decoderSampleRate = 0; decoderChannels = 0
  }

  // MARK: - Lock screen / media controls

  private func updateNowPlaying() {
    refreshArtwork()  // keep the inset glyph/countdown in step with the state
    let mhz = String(format: "%.3f MHz", Double(currentFreq) / 1_000_000)
    // FM-DX pushes its own title from JS; never fall back to the stale UberSDR
    // freq/mode (currentFreq/currentMode belong to the last UberSDR session).
    let fallbackTitle  = fmdxAudio ? "FM-DX" : "\(mhz) \(currentMode.uppercased())"
    let fallbackArtist = instanceName.isEmpty ? currentBase : instanceName
    let title: String
    let artist: String
    if reconnectFailed {
      title  = "Failed to reconnect"
      artist = "Open VibeSDR to reconnect"
    } else if dataSaverDisconnected {
      title  = "Disconnected"
      artist = "VibeSDR — press ▶ to reconnect"
    } else {
      title  = npTitleOverride ?? fallbackTitle
      artist = npArtistOverride ?? fallbackArtist
    }
    // RTL-TCP/local "resume" pause = muted in place (engine kept alive) — that's a
    // paused state for the lock screen even though we're not disconnected.
    let paused = dataSaverDisconnected || reconnectFailed
              || (externalAudio && externalPauseMode == "resume" && isMuted)
              || (fmdxAudio && isMuted)
    var info: [String: Any] = [
      MPMediaItemPropertyTitle:             title,
      MPMediaItemPropertyArtist:            artist,
      MPMediaItemPropertyAlbumTitle:        "VibeSDR",
      MPNowPlayingInfoPropertyPlaybackRate: paused ? 0.0 : 1.0,
      MPNowPlayingInfoPropertyIsLiveStream: true,
    ]
    if let art = npArtwork { info[MPMediaItemPropertyArtwork] = art }
    let center = MPNowPlayingInfoCenter.default()
    center.nowPlayingInfo = info
    // The lock-screen / Control-Center play-pause button and route arbitration
    // (which device owns the AirPods) follow playbackState, NOT the playbackRate
    // in the info dict. Without this the button springs back to ▶ on pause and
    // iOS keeps grabbing shared AirPods from the Mac because it thinks we're
    // still playing.
    center.playbackState = paused ? .paused : .playing
  }

  /** VTS-aware now-playing strings from JS (empty string clears). */
  @objc func setNowPlaying(_ title: String, artist: String) {
    npTitleOverride  = title.isEmpty ? nil : title
    npArtistOverride = artist.isEmpty ? nil : artist
    DispatchQueue.main.async { self.updateNowPlaying() }
  }

  /** Album art: VibeSDR icon with the server-type logo inset bottom-right
   *  (multi-server prep — type picks the overlay, "ubersdr" for now). */
  @objc func setArtwork(_ serverType: String) {
    guard serverType != npArtworkType else { return }
    npArtworkType = serverType
    DispatchQueue.main.async { self.updateNowPlaying() }  // refreshes art + pushes
  }

  /// FM-DX: the resolved station logo URL (radio-browser favicon). Downloaded and
  /// inlaid on the album art. Empty clears it (→ monogram-only art).
  @objc func setStationLogo(_ url: String) {
    let u = url.trimmingCharacters(in: .whitespaces)
    if u == stationLogoUrl { return }
    stationLogoUrl = u
    if u.isEmpty {
      stationLogoImg = nil; lastArtworkKey = ""
      DispatchQueue.main.async { self.updateNowPlaying() }
      return
    }
    guard let link = URL(string: u) else { return }
    URLSession.shared.dataTask(with: link) { [weak self] data, _, _ in
      guard let self, let data, let img = UIImage(data: data), self.stationLogoUrl == u else { return }
      self.stationLogoImg = img
      self.lastArtworkKey = ""   // force a re-composite with the logo
      DispatchQueue.main.async { self.updateNowPlaying() }
    }.resume()
  }

  // Composite the album art with a state-aware bottom-right inset: the server
  // logo while playing, a muted-speaker glyph + minutes-to-disconnect while
  // muted, a disconnected glyph once the data saver has dropped the stream.
  private var lastArtworkKey = ""
  /// Draw an image centred in `rect`, PRESERVING ITS ASPECT RATIO.
  ///
  /// ★ Drawing straight into a square `rect` stretched every non-square mark. The FM-DX
  /// logo is 163x84 (~1.94:1), so its ring was squashed into an ellipse — and that same
  /// stretch was most of the blur too, since it scaled 84px up to 162px vertically. OWRX
  /// (601x512) was subtly squashed for the same reason. Fitting fixes the shape AND leaves
  /// the scale near 1:1, so it costs nothing and there is no reason to draw any other way.
  private func drawAspectFit(_ img: UIImage, in rect: CGRect) {
    guard img.size.width > 0, img.size.height > 0 else { return }
    let s = min(rect.width / img.size.width, rect.height / img.size.height)
    let w = img.size.width * s, h = img.size.height * s
    img.draw(in: CGRect(x: rect.midX - w / 2, y: rect.midY - h / 2, width: w, height: h))
  }

  private func refreshArtwork() {
    guard let base = UIImage(named: "artwork_base") else { return }
    let key = reconnectFailed ? "fail"
            : dataSaverDisconnected ? "disc"
            : "play-\(npArtworkType)-\(stationLogoImg != nil ? stationLogoUrl : "")"
    guard key != lastArtworkKey else { return }
    lastArtworkKey = key

    let size  = base.size
    let inset = size.width * 0.30
    let pad   = size.width * 0.045
    let rect  = CGRect(x: size.width - inset - pad, y: size.height - inset - pad,
                       width: inset, height: inset)
    let red    = UIColor(red: 0.92, green: 0.32, blue: 0.28, alpha: 1)
    let amber  = UIColor(red: 1.0, green: 0.74, blue: 0.20, alpha: 1)
    let img = UIGraphicsImageRenderer(size: size).image { _ in
      base.draw(in: CGRect(origin: .zero, size: size))
      if reconnectFailed {
        // disconnected glyph + a little exclamation badge bottom-right
        drawSymbol("wifi.slash", in: rect, tint: red)
        let b = CGRect(x: rect.maxX - rect.width * 0.5, y: rect.maxY - rect.height * 0.5,
                       width: rect.width * 0.5, height: rect.height * 0.5)
        drawSymbol("exclamationmark.circle.fill", in: b, tint: amber)
      } else if dataSaverDisconnected {
        drawSymbol("wifi.slash", in: rect, tint: red)
      } else if npArtworkType == "rtltcp", let icon = UIImage(named: "logo_rtltcp") {
        // RTL-TCP icon is black line art → amber-tint it (matches the Android card
        // + the RTL-TCP menu icon) so it reads on the dark album base.
        icon.withTintColor(amber, renderingMode: .alwaysOriginal).draw(in: rect)
      } else if npArtworkType == "fmdx", let icon = UIImage(named: "logo_fmdx") {
        // FM-DX brand mark (green, already coloured) centred on the album base.
        drawAspectFit(icon, in: rect.insetBy(dx: rect.width * 0.05, dy: rect.height * 0.05))
      } else if let overlay = UIImage(named: "logo_\(npArtworkType)") {
        drawAspectFit(overlay, in: rect)
      }
      // FM-DX: inlay the resolved station logo bottom-LEFT (mirrors the FM-DX
      // mark bottom-right). Aspect-fit onto a rounded dark tile so any favicon reads.
      if npArtworkType == "fmdx", let logo = stationLogoImg {
        let lrect = CGRect(x: pad, y: size.height - inset - pad, width: inset, height: inset)
        let tile = UIBezierPath(roundedRect: lrect, cornerRadius: inset * 0.14)
        UIColor(white: 1.0, alpha: 0.92).setFill(); tile.fill()
        let ip = inset * 0.12
        let box = lrect.insetBy(dx: ip, dy: ip)
        let s = min(box.width / logo.size.width, box.height / logo.size.height)
        let w = logo.size.width * s, h = logo.size.height * s
        logo.draw(in: CGRect(x: box.midX - w / 2, y: box.midY - h / 2, width: w, height: h))
      }

      // FM-DX: SHARED RECEIVER — stamped across the top of the lock-screen artwork.
      //
      // The lock screen is exactly where a listener reaches for the skip button that
      // isn't there, so it is exactly where the reason belongs. One physical tuner,
      // shared by every listener — a skip would change the station for people you cannot
      // see, so it is disabled out of courtesy rather than out of incapacity.
      //
      // A STAMP, not a paragraph: this artwork is displayed at roughly a third of the
      // size it is rendered at, so it has to survive being small. Top-centre keeps it
      // clear of the station logo (bottom-left) and the FM-DX mark (bottom-right).
      //
      // The wording is deliberately the same phrase used by the warning shown when you
      // JOIN an FM-DX server and by the About page — "one tuner, many listeners" — so the
      // three read as one policy rather than as three separate apologies.
      if npArtworkType == "fmdx" {
        let head = "SHARED RECEIVER"
        let body = "One tuner, many listeners.\nSkip is disabled out of courtesy."

        let para = NSMutableParagraphStyle()
        para.alignment = .center
        para.lineSpacing = size.height * 0.008

        let headAttrs: [NSAttributedString.Key: Any] = [
          .font: UIFont.systemFont(ofSize: size.width * 0.052, weight: .heavy),
          .foregroundColor: amber,
          .kern: size.width * 0.006,
          .paragraphStyle: para,
        ]
        let bodyAttrs: [NSAttributedString.Key: Any] = [
          .font: UIFont.systemFont(ofSize: size.width * 0.038, weight: .medium),
          .foregroundColor: UIColor(white: 1.0, alpha: 0.70),
          .paragraphStyle: para,
        ]

        let w = size.width - pad * 2
        let headH = (head as NSString).boundingRect(
          with: CGSize(width: w, height: .greatestFiniteMagnitude),
          options: .usesLineFragmentOrigin, attributes: headAttrs, context: nil).height
        let bodyH = (body as NSString).boundingRect(
          with: CGSize(width: w, height: .greatestFiniteMagnitude),
          options: .usesLineFragmentOrigin, attributes: bodyAttrs, context: nil).height

        let gap = size.height * 0.012
        var y = pad * 1.2
        (head as NSString).draw(in: CGRect(x: pad, y: y, width: w, height: headH),
                                withAttributes: headAttrs)
        y += headH + gap
        (body as NSString).draw(in: CGRect(x: pad, y: y, width: w, height: bodyH),
                                withAttributes: bodyAttrs)
      }
    }
    npArtwork = MPMediaItemArtwork(boundsSize: img.size) { _ in img }
  }

  private func drawSymbol(_ name: String, in rect: CGRect, tint: UIColor) {
    let cfg = UIImage.SymbolConfiguration(pointSize: rect.height, weight: .semibold)
    guard let sym = UIImage(systemName: name, withConfiguration: cfg)?
            .withTintColor(tint, renderingMode: .alwaysOriginal) else { return }
    // aspect-fit the symbol inside rect
    let s = min(rect.width / sym.size.width, rect.height / sym.size.height)
    let w = sym.size.width * s, h = sym.size.height * s
    sym.draw(in: CGRect(x: rect.midX - w / 2, y: rect.midY - h / 2, width: w, height: h))
  }

  private func setupRemoteCommands() {
    let cc = MPRemoteCommandCenter.shared()

    // Play = unmute
    cc.playCommand.isEnabled = true
    cc.playCommand.removeTarget(nil)
    cc.playCommand.addTarget { [weak self] _ in
      self?.setMuted(false)
      return .success
    }

    // Pause = mute
    cc.pauseCommand.isEnabled = true
    cc.pauseCommand.removeTarget(nil)
    cc.pauseCommand.addTarget { [weak self] _ in
      self?.setMuted(true)
      return .success
    }

    // Skip forward = tune up by step. DISABLED for FM-DX: it's a single shared
    // tuner, so a lock-screen / headphone skip would retune for EVERY listener.
    cc.nextTrackCommand.isEnabled = !fmdxAudio
    cc.nextTrackCommand.removeTarget(nil)
    cc.nextTrackCommand.addTarget { [weak self] _ in
      guard let self else { return .commandFailed }
      if self.fmdxAudio { return .commandFailed }
      // External (OWRX/Kiwi): tuning lives in JS — delegate the skip so we don't
      // tune the native UberSDR WS (which resurrects a UberSDR session). JS
      // handles step vs bookmark from its own media-skip setting.
      if self.externalAudio || self.skipMode == "bookmark" {
        self.sendEvent(withName: "VibeSkip", body: ["direction": "next"])
        return .success
      }
      let newFreq = self.snapStep(1)
      self.currentFreq = newFreq
      self.sendWsJson(["type": "tune", "frequency": newFreq])
      // Stale VTS strings (old station name) — fall back until JS catches up
      self.npTitleOverride = nil; self.npArtistOverride = nil
      self.updateNowPlaying()
      self.sendEvent(withName: "VibeTuned", body: ["frequency": newFreq, "mode": self.currentMode])
      return .success
    }

    // Skip back = tune down by step. DISABLED for FM-DX (shared tuner — see above).
    cc.previousTrackCommand.isEnabled = !fmdxAudio
    cc.previousTrackCommand.removeTarget(nil)
    cc.previousTrackCommand.addTarget { [weak self] _ in
      guard let self else { return .commandFailed }
      if self.fmdxAudio { return .commandFailed }
      if self.externalAudio || self.skipMode == "bookmark" {
        self.sendEvent(withName: "VibeSkip", body: ["direction": "prev"])
        return .success
      }
      let newFreq = self.snapStep(-1)
      self.currentFreq = newFreq
      self.sendWsJson(["type": "tune", "frequency": newFreq])
      self.npTitleOverride = nil; self.npArtistOverride = nil
      self.updateNowPlaying()
      self.sendEvent(withName: "VibeTuned", body: ["frequency": newFreq, "mode": self.currentMode])
      return .success
    }
  }

  /// Snap a media-control skip to the step grid, matching the VFO drum: an
  /// off-grid frequency lands on the next/previous multiple of the step; an
  /// on-grid one moves exactly one step. direction +1 = up, -1 = down.
  private func snapStep(_ direction: Int) -> Int {
    let s = currentStep
    guard s > 0 else { return max(100_000, currentFreq) }
    let snapped = direction > 0 ? (currentFreq / s + 1) * s
                                : ((currentFreq + s - 1) / s - 1) * s
    return max(100_000, snapped)
  }

  // MARK: - mDNS / Bonjour discovery of networked RTL-TCP servers
  //
  // Browses for `_rtl_tcp._tcp` services on the local network via NWBrowser (the
  // Apple-blessed, App-Store-clean path — no subnet scanning). Each discovered
  // service is resolved to host:port with a short-lived NWConnection, and the
  // friendly name is taken from the service's `name` TXT record when present.
  // Results are pushed to JS as VibeMdnsFound / VibeMdnsLost. Lives here (rather
  // than a separate module) so it compiles in the app target with no pbxproj
  // change — JS reaches it via NativeModules.VibePowerModule.
  private static let rtlTcpServiceType = "_rtl_tcp._tcp"
  // VibeServer advertises its own type so a discovered server is known to speak
  // the compressed protocol (and whether it needs a PIN) before connecting.
  private static let vibeServiceType = "_vibesdr._tcp"

  @objc(startDiscovery)
  func startDiscovery() {
    stopDiscovery()
    for type in [VibePowerModule.rtlTcpServiceType, VibePowerModule.vibeServiceType] {
      let params = NWParameters()
      params.includePeerToPeer = true
      let browser = NWBrowser(
        for: .bonjourWithTXTRecord(type: type, domain: nil), using: params)
      browser.browseResultsChangedHandler = { [weak self] _, changes in
        guard let self = self else { return }
        for change in changes {
          switch change {
          case .added(let result):
            self.resolveMdns(result)
          case .removed(let result):
            if case let .service(name, _, _, _) = result.endpoint {
              self.sendEvent(withName: "VibeMdnsLost", body: ["name": name])
            }
          default:
            break
          }
        }
      }
      browser.start(queue: mdnsQueue)
      mdnsBrowsers.append(browser)
    }
  }

  @objc(stopDiscovery)
  func stopDiscovery() {
    for b in mdnsBrowsers { b.cancel() }
    mdnsBrowsers.removeAll()
    for conn in mdnsResolvers { conn.cancel() }
    mdnsResolvers.removeAll()
  }

  private func resolveMdns(_ result: NWBrowser.Result) {
    guard case let .service(serviceName, serviceType, _, _) = result.endpoint else { return }
    let proto = serviceType.contains("vibesdr") ? "vibeserver" : "rtltcp"
    // Friendly name + pin flag from the TXT record.
    var friendly = serviceName
    var pin = false
    if case let .bonjour(txt) = result.metadata {
      if let n = txt["name"], !n.isEmpty { friendly = n }
      if txt["pin"] == "1" { pin = true }
    }
    let conn = NWConnection(to: result.endpoint, using: .tcp)
    mdnsResolvers.append(conn)
    conn.stateUpdateHandler = { [weak self, weak conn] state in
      guard let self = self, let conn = conn else { return }
      switch state {
      case .ready:
        if let path = conn.currentPath,
           case let .hostPort(host, port) = path.remoteEndpoint {
          var h = "\(host)"
          if let pct = h.firstIndex(of: "%") { h = String(h[..<pct]) }  // strip IPv6 zone id
          h = h.replacingOccurrences(of: "[", with: "").replacingOccurrences(of: "]", with: "")
          self.sendEvent(withName: "VibeMdnsFound",
                         body: ["name": friendly, "host": h, "port": Int(port.rawValue),
                                "proto": proto, "pin": pin])
        }
        self.dropResolver(conn)
      case .failed, .cancelled:
        self.dropResolver(conn)
      default:
        break
      }
    }
    conn.start(queue: mdnsQueue)
  }

  private func dropResolver(_ conn: NWConnection) {
    conn.cancel()
    mdnsResolvers.removeAll { $0 === conn }
  }
}

/// Holds the car browse payload pushed from JS (bookmarks + band plan) so a
/// future CarPlay scene (CPListTemplate) can render it. Inert today: VibeSDR
/// ships as a dev-signed sideload, and CarPlay browsing needs Apple's
/// `com.apple.developer.carplay-audio` entitlement + App Store/TestFlight
/// distribution. When that lands, add a CPTemplateApplicationSceneDelegate that
/// reads `payloadJSON` and calls `onTune` (wire it to emit "VibeCarTune" the
/// same way Android Auto's onPlayFromMediaId does). Band-aware auto mode/step
/// already works in the car today via the AVAudioSession car-audio route — it
/// does NOT depend on this.
final class VibeCarPlayData {
  static let shared = VibeCarPlayData()
  private init() {}
  var payloadJSON: String = "{}"
  /// Set by the CarPlay scene once it exists: (frequency, mode, isBand).
  var onTune: ((Double, String?, Bool) -> Void)?
}

// MARK: - Siri voice control (App Intents)

/// UserDefaults-backed glue between the Siri intents (which can run before/around
/// the React layer) and JS. All resolution (frequency parse, bookmark/band match,
/// step/mode synonyms) happens in JS (reusing searchStations); the intent only
/// passes the spoken text + a kind. Live commands post a notification that
/// VibePowerModule forwards to JS; cold-launch commands are stashed for JS to read
/// after it connects to the default instance.
enum VibeVoice {
  static let note = Notification.Name("VibeVoiceCommand")
  private static let d = UserDefaults.standard
  private static let kPending = "vibeVoicePending"     // JSON {kind, query} for cold launch
  private static let kDefault = "vibeDefaultInstance"
  private static let kConnected = "vibeVoiceConnected"
  private static let kBookmarks = "vibeVoiceBookmarks"  // JSON [{name,frequency,mode}] for Siri disambiguation

  static func setDefaultInstance(_ name: String) { d.set(name, forKey: kDefault) }
  static func setConnected(_ c: Bool) { d.set(c, forKey: kConnected) }
  /// Cache the bookmark list (from the car-browse payload) so the Tune intent can
  /// match a spoken name against it natively and, on multiple hits, drive a Siri
  /// spoken pick-list. Persisted to UserDefaults so it survives a cold launch.
  static func setBrowse(_ json: String) {
    guard let data = json.data(using: .utf8),
          let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let bms = obj["bookmarks"] as? [[String: Any]] else { return }
    if let out = try? JSONSerialization.data(withJSONObject: bms) {
      d.set(out, forKey: kBookmarks)
    }
  }
  static var hasDefault: Bool { !(d.string(forKey: kDefault) ?? "").isEmpty }
  static var isConnected: Bool { d.bool(forKey: kConnected) }

  /// Fuzzy-match a spoken station name against the cached bookmarks: every word of
  /// the query must appear in the bookmark name (case-insensitive). Frequencies
  /// ("7150") won't match a name, so they fall through to the JS resolver.
  static func allBookmarks() -> [VibeBookmarkEntity] {
    guard let data = d.data(forKey: kBookmarks),
          let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return [] }
    return arr.compactMap { b in
      guard let name = b["name"] as? String,
            let freq = (b["frequency"] as? NSNumber)?.intValue else { return nil }
      return VibeBookmarkEntity(id: "\(name)|\(freq)", name: name, freq: freq, mode: b["mode"] as? String)
    }
  }

  static func bookmark(id: String) -> VibeBookmarkEntity? { allBookmarks().first { $0.id == id } }

  /// Parse an explicit frequency hint ("11mhz", "909 khz", "at 11 mhz") → Hz. Only
  /// fires when a unit is present, so "Radio 5" is never read as a frequency.
  private static func freqHint(_ s: String) -> Int? {
    guard let re = try? NSRegularExpression(pattern: #"(\d+(?:\.\d+)?)\s*(mhz|khz|hz)\b"#),
          let m = re.firstMatch(in: s, range: NSRange(s.startIndex..., in: s)),
          let nr = Range(m.range(at: 1), in: s), let n = Double(s[nr]) else { return nil }
    let unit = Range(m.range(at: 2), in: s).map { String(s[$0]) } ?? ""
    switch unit {
    case "mhz": return Int(n * 1_000_000)
    case "khz": return Int(n * 1000)
    default:    return Int(n)
    }
  }

  /// Fuzzy-match a spoken station name against the cached bookmarks: every name
  /// word of the query must appear in the bookmark name (case-insensitive). An
  /// explicit frequency hint ("China Radio at 11MHz") narrows the matches to ±1
  /// MHz of that frequency. Returns the matches and whether a hint was present.
  /// Frequencies alone ("7150") won't match a name → fall through to JS.
  static func matchBookmarks(_ query: String) -> (matches: [VibeBookmarkEntity], hinted: Bool) {
    let lower = query.lowercased()
    let hint = freqHint(lower)
    let stop: Set<String> = ["at", "around", "near", "on", "about", "of", "the", "to", "mhz", "khz", "hz"]
    let words = lower.split(whereSeparator: { !$0.isLetter && !$0.isNumber }).map(String.init)
      .filter { w in
        if stop.contains(w) { return false }
        // When a frequency hint is present, drop bare numbers (they're the hint,
        // not part of the name); otherwise keep them ("Radio 5").
        if hint != nil && w.allSatisfy({ $0.isNumber }) { return false }
        return true
      }
    guard !words.isEmpty else { return ([], hint != nil) }
    var out = allBookmarks().filter { e in
      let ln = e.name.lowercased()
      return words.allSatisfy { ln.contains($0) }
    }
    if let h = hint { out = out.filter { abs($0.freq - h) <= 1_000_000 } }
    return (out, hint != nil)
  }

  // Candidate list persisted across the "which frequency?" re-prompt so the spoken
  // answer always has the right options to match (the intent's `target` may not
  // survive the re-run).
  private static let kCandidates = "vibeVoiceCandidates"
  static func setCandidates(_ list: [VibeBookmarkEntity]) {
    let arr = list.map { ["id": $0.id, "name": $0.name, "frequency": $0.freq, "mode": ($0.mode ?? "") as Any] }
    if let data = try? JSONSerialization.data(withJSONObject: arr) { d.set(data, forKey: kCandidates) }
  }
  static func takeCandidates() -> [VibeBookmarkEntity] {
    guard let data = d.data(forKey: kCandidates),
          let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return [] }
    return arr.compactMap { b in
      guard let id = b["id"] as? String, let name = b["name"] as? String,
            let f = (b["frequency"] as? NSNumber)?.intValue else { return nil }
      let mode = (b["mode"] as? String).flatMap { $0.isEmpty ? nil : $0 }
      return VibeBookmarkEntity(id: id, name: name, freq: f, mode: mode)
    }
  }

  /// Parse a spoken frequency answer → Hz. Mirrors the JS bare-number rule (< 30 =
  /// MHz, else kHz) so "909"→909 kHz, "11.650"→11.650 MHz, "11650"→11650 kHz.
  /// Spaces inside a number ("11 650") are collapsed first.
  static func spokenToHz(_ s: String) -> Int? {
    var lower = s.lowercased()
    // Collapse "11 650" / "11,650" thousands grouping into "11650".
    if let re = try? NSRegularExpression(pattern: #"(\d)[\s,](?=\d{3}\b)"#) {
      lower = re.stringByReplacingMatches(in: lower, range: NSRange(lower.startIndex..., in: lower), withTemplate: "$1")
    }
    guard let re = try? NSRegularExpression(pattern: #"(\d+(?:[.,]\d+)?)\s*(mhz|khz|hz)?"#),
          let m = re.firstMatch(in: lower, range: NSRange(lower.startIndex..., in: lower)),
          let nr = Range(m.range(at: 1), in: lower),
          let n = Double(lower[nr].replacingOccurrences(of: ",", with: ".")) else { return nil }
    let unit = Range(m.range(at: 2), in: lower).map { String(lower[$0]) } ?? ""
    switch unit {
    case "mhz": return Int(n * 1_000_000)
    case "khz": return Int(n * 1000)
    case "hz":  return Int(n)
    default:    return Int(n < 30 ? n * 1_000_000 : n * 1000)
    }
  }

  /// Match the user's spoken frequency to the nearest candidate in the list — we
  /// own this matching because Siri's AppEntity voice disambiguation mis-picks.
  static func pickBySpokenFreq(_ ans: String, among: [VibeBookmarkEntity]) -> VibeBookmarkEntity? {
    guard let hz = spokenToHz(ans) else { return nil }
    return among.min(by: { abs($0.freq - hz) < abs($1.freq - hz) })
  }

  /// Spoken text + kind ("tune" | "mode" | "step"). All resolution (frequency
  /// parse, bookmark/band match, mode/step synonyms) is in JS — the intent just
  /// passes the text. Live commands emit now; cold launch stashes for JS.
  static func handle(query: String, kind: String) -> String {
    let q = query.trimmingCharacters(in: .whitespaces)
    if isConnected {
      NotificationCenter.default.post(name: note, object: nil,
        userInfo: ["event": "VibeVoiceQuery", "body": ["query": q, "kind": kind]])
      switch kind {
      case "step": return "Setting step to \(q)"
      case "mode": return "Switching to \(q)"
      default:     return "Tuning to \(q)"
      }
    }
    // Not running/connected: stash so it applies when VibeSDR is next opened (we
    // no longer foreground the app, to keep tuning unlock-free while listening).
    if hasDefault {
      if let data = try? JSONSerialization.data(withJSONObject: ["kind": kind, "query": q]),
         let s = String(data: data, encoding: .utf8) { d.set(s, forKey: kPending) }
      return "Open VibeSDR to \(kind == "tune" ? "tune to \(q)" : "apply that")."
    }
    return "Open VibeSDR and connect to an instance first to use voice control."
  }

  /// Dispatch a specific picked bookmark as an explicit Hz+mode tune (reuses the
  /// JS frequency path) but speaks the friendly station name back.
  static func handleBookmark(_ b: VibeBookmarkEntity) -> String {
    // Space before "hz" is required: JS parses "\(freq) hz" but a glued "\(freq)hz"
    // fails its \bhz\b boundary and misroutes to the band search.
    let q = "\(b.freq) hz" + (b.mode.map { " \($0)" } ?? "")
    if isConnected {
      NotificationCenter.default.post(name: note, object: nil,
        userInfo: ["event": "VibeVoiceQuery", "body": ["query": q, "kind": "tune"]])
      return "Tuning to \(b.name)"
    }
    if hasDefault {
      if let data = try? JSONSerialization.data(withJSONObject: ["kind": "tune", "query": q]),
         let s = String(data: data, encoding: .utf8) { d.set(s, forKey: kPending) }
      return "Open VibeSDR to tune to \(b.name)."
    }
    return "Open VibeSDR and connect to an instance first to use voice control."
  }

  static func takePending() -> String? {
    guard let s = d.string(forKey: kPending) else { return nil }
    d.removeObject(forKey: kPending)
    return s
  }
}

/// A bookmark surfaced to Siri for native spoken disambiguation when a name hits
/// several entries (e.g. "Radio 5"). The title is frequency-forward ("909 kHz")
/// so Siri reliably matches the spoken number; the picked entity tunes exactly.
@available(iOS 16.0, *)
struct VibeBookmarkEntity: AppEntity, Identifiable {
  let id: String
  let name: String
  let freq: Int
  let mode: String?

  var freqLabel: String {
    let mhz = Double(freq) / 1_000_000.0
    return mhz >= 1 ? String(format: "%.3f MHz", mhz)
                    : String(format: "%.0f kHz", Double(freq) / 1000.0)
  }

  static var typeDisplayRepresentation: TypeDisplayRepresentation = "Frequency"
  static var defaultQuery = VibeBookmarkQuery()
  var displayRepresentation: DisplayRepresentation {
    DisplayRepresentation(title: "\(freqLabel)", subtitle: "\(name)")
  }
}

@available(iOS 16.0, *)
struct VibeBookmarkQuery: EntityQuery {
  func entities(for identifiers: [String]) async throws -> [VibeBookmarkEntity] {
    identifiers.compactMap { VibeVoice.bookmark(id: $0) }
  }
  func suggestedEntities() async throws -> [VibeBookmarkEntity] { [] }
}

@available(iOS 16.0, *)
struct TuneIntent: AppIntent {
  static var title: LocalizedStringResource = "Tune VibeSDR"
  // false = run in the background without foregrounding the app, so Siri can tune
  // from the lock screen / headphones / CarPlay WITHOUT an unlock (the app is
  // already alive playing audio and receives the command via NotificationCenter).
  static var openAppWhenRun = false
  @Parameter(title: "Station or frequency",
             requestValueDialog: "What frequency or station?") var target: String
  // Set only via native disambiguation when the spoken name matched several
  // bookmarks. Optional so Siri doesn't auto-prompt for it on every invocation.
  @Parameter(title: "Frequency") var pick: VibeBookmarkEntity?

  func perform() async throws -> some IntentResult & ProvidesDialog {
    // If the user already picked from a spoken list, tune that exact bookmark.
    if let chosen = pick {
      let full = VibeVoice.bookmark(id: chosen.id) ?? chosen
      return .result(dialog: IntentDialog(stringLiteral: VibeVoice.handleBookmark(full)))
    }
    // A spoken station name can hit multiple bookmarks (e.g. "Radio 5"). A
    // frequency hint ("China Radio at 11MHz") narrows the list to that vicinity.
    // A frequency or band (no name match) falls through to the JS resolver.
    let (matches, _) = VibeVoice.matchBookmarks(target)
    if matches.count > 1 {
      // Apple-native spoken pick-list — Siri reads the frequencies and the user
      // picks by voice; it manages the multi-turn session cleanly.
      let name = matches.first?.name ?? target
      let chosen = try await $pick.requestDisambiguation(
        among: matches,
        dialog: IntentDialog(stringLiteral: "\(name) has \(matches.count). Which frequency?"))
      let full = VibeVoice.bookmark(id: chosen.id) ?? chosen
      return .result(dialog: IntentDialog(stringLiteral: VibeVoice.handleBookmark(full)))
    }
    if matches.count == 1 {
      return .result(dialog: IntentDialog(stringLiteral: VibeVoice.handleBookmark(matches[0])))
    }
    return .result(dialog: IntentDialog(stringLiteral: VibeVoice.handle(query: target, kind: "tune")))
  }
}

@available(iOS 16.0, *)
struct StepIntent: AppIntent {
  static var title: LocalizedStringResource = "Set VibeSDR step rate"
  // false = run in the background without foregrounding the app, so Siri can tune
  // from the lock screen / headphones / CarPlay WITHOUT an unlock (the app is
  // already alive playing audio and receives the command via NotificationCenter).
  static var openAppWhenRun = false
  @Parameter(title: "Step rate", requestValueDialog: "What step rate?") var rate: String
  func perform() async throws -> some IntentResult & ProvidesDialog {
    .result(dialog: IntentDialog(stringLiteral: VibeVoice.handle(query: rate, kind: "step")))
  }
}

@available(iOS 16.0, *)
struct ModeIntent: AppIntent {
  static var title: LocalizedStringResource = "Set VibeSDR mode"
  // false = run in the background without foregrounding the app, so Siri can tune
  // from the lock screen / headphones / CarPlay WITHOUT an unlock (the app is
  // already alive playing audio and receives the command via NotificationCenter).
  static var openAppWhenRun = false
  @Parameter(title: "Mode", requestValueDialog: "Which mode?") var mode: String
  func perform() async throws -> some IntentResult & ProvidesDialog {
    .result(dialog: IntentDialog(stringLiteral: VibeVoice.handle(query: mode, kind: "mode")))
  }
}

@available(iOS 16.0, *)
struct VibeShortcuts: AppShortcutsProvider {
  static var appShortcuts: [AppShortcut] {
    // Two-step (Siri prompts for the value) — same pattern as the working Mode
    // shortcut. App name MUST be in the phrase (Apple rule).
    AppShortcut(intent: TuneIntent(), phrases: [
      "Tune \(.applicationName)",
      "Tune with \(.applicationName)",
      "\(.applicationName) tune",
    ], shortTitle: "Tune", systemImageName: "dot.radiowaves.left.and.right")
    AppShortcut(intent: ModeIntent(), phrases: [
      "Change \(.applicationName) mode",
      "Set \(.applicationName) mode",
    ], shortTitle: "Mode", systemImageName: "waveform")
    AppShortcut(intent: StepIntent(), phrases: [
      "Set \(.applicationName) step rate",
      "Change \(.applicationName) step rate",
    ], shortTitle: "Step", systemImageName: "arrow.left.and.right")
  }
}

// MARK: - Auto notch (NLMS adaptive line enhancer)
//
// Swift port of the shared C++ vibe::AutoNotch (android/.../decoders/auto_notch).
// Same spec/constants so local-shim and network notch sound identical: outputs
// the prediction error so stationary tones (carriers/heterodynes) are removed
// while broadband speech passes. Tap span (~3.5 ms) kept below the voice pitch
// period so it can't subtract voiced speech; very slow adaptation so loud voice
// neither builds a notch nor pulls the lock off the tone.
final class AutoNotch {
  private let D = 8, L = 160
  private let mu: Float = 0.003, leak: Float = 0.9999, eps: Float = 1e-6
  private var buf: [Float]
  private var w:   [Float]
  private var p = 0
  init() { buf = [Float](repeating: 0, count: 2 * (8 + 160)); w = [Float](repeating: 0, count: 160) }
  func reset() {
    for i in 0..<buf.count { buf[i] = 0 }
    for i in 0..<w.count { w[i] = 0 }
    p = 0
  }
  func process(_ x: UnsafeMutablePointer<Float>, _ count: Int) {
    let M = D + L
    buf.withUnsafeMutableBufferPointer { b in
      w.withUnsafeMutableBufferPointer { wp in
        for n in 0..<count {
          p = (p == 0) ? M - 1 : p - 1
          let inp = x[n]
          b[p] = inp; b[p + M] = inp
          let base = p + D
          var fir: Float = 0, pwr: Float = 0
          for i in 0..<L { let s = b[base + i]; fir += wp[i] * s; pwr += s * s }
          let err = inp - fir
          x[n] = err
          let g = mu * err / (eps + pwr)
          for i in 0..<L { wp[i] = leak * wp[i] + g * b[base + i] }
        }
      }
    }
  }
}

// MARK: - Native crash diagnostics
//
// ★★★ WHY THIS EXISTS. A TestFlight crash from an iPhone 11 on iOS 16.5.1 (VibeSDR
// 9.0.1 build 8, 68 minutes into a session) was an UNCAUGHT OBJ-C EXCEPTION — and the
// report contained NO `Last Exception Backtrace`, so the throw site was simply absent.
// Symbolicating it would have added nothing; the information was never recorded.
// Apple's pipeline is also opt-in, so most users' crashes never reach us at all: nine
// crashes were counted in ASC and not one produced a readable log.
//
// This records the exception ourselves, the moment it is raised, while the stack is
// still alive.
//
// ★★ LOCAL ONLY, DELIBERATELY. Nothing is transmitted: it is written to UserDefaults
// and read back by JS for the diagnostics screen, where the user can SEE it and choose
// to share it through the system share sheet. That is what keeps it out of the App
// Privacy declaration — data that never leaves the device to us is not "collected" —
// and keeps the privacy policy's "everything stays on your device" true.
//
// ★ NEVER put a PIN, an admin password or a precise location in here.
enum VibeCrashLog {
  private static let key = "vibe.lastNativeCrash"

  /// Installed once at launch. Chains to any existing handler so we do not swallow
  /// another module's reporting.
  /// ★ Held statically, NOT captured: NSSetUncaughtExceptionHandler takes a C function
  ///   pointer, and "a C function pointer cannot be formed from a closure that captures
  ///   context" — so the chained handler has to be reachable without a capture.
  private static var previousHandler: (@convention(c) (NSException) -> Void)?

  static func install() {
    previousHandler = NSGetUncaughtExceptionHandler()
    NSSetUncaughtExceptionHandler { ex in
      let frames = ex.callStackSymbols.prefix(24).joined(separator: "\n")
      let entry: [String: Any] = [
        "ts": Date().timeIntervalSince1970 * 1000,
        "name": ex.name.rawValue,
        "reason": ex.reason ?? "",
        "stack": frames,
        "os": UIDevice.current.systemVersion,
        "model": VibeCrashLog.hardwareModel(),
      ]
      // Synchronous write — the process is about to die.
      UserDefaults.standard.set(entry, forKey: "vibe.lastNativeCrash")
      UserDefaults.standard.synchronize()
      VibeCrashLog.previousHandler?(ex)
    }
  }

  /// e.g. "iPhone12,1" — the identifier crash reports use, not the marketing name.
  static func hardwareModel() -> String {
    var sysinfo = utsname(); uname(&sysinfo)
    return withUnsafePointer(to: &sysinfo.machine) {
      $0.withMemoryRebound(to: CChar.self, capacity: 1) { String(validatingUTF8: $0) ?? "?" }
    }
  }

  /// ★★ A SECOND ROUTE TO THE SAME RECORD, for when the app is not in a state to be driven.
  /// The Diagnostics screen already shows this — `getNativeCrash()` → diagnostics.ts, under
  /// "last native exception" — and that remains the route for a user. This adds the unified log,
  /// because the crash REPORT does not carry the reason (a rethrow off the throwing queue loses
  /// the Application Specific Information), and reading it from a TestFlight build we cannot
  /// attach a debugger to otherwise means talking someone through a menu on a crashing app.
  /// `log show --predicate 'process == "VibeSDR"'` gets it off the machine directly.
  /// ★ Deliberately NOT cleared here: the Diagnostics screen must still be able to show it.
  /// This only makes the record easier to reach, and adds nothing to what is stored.
  /// ★ Local only, unchanged: os_log stays on the device like the UserDefaults entry it prints.
  static func announce() {
    guard let c = take() else { return }
    NSLog("VIBE LAST NATIVE CRASH — %@: %@\nos %@ model %@\n%@",
          (c["name"]   as? String) ?? "?",
          (c["reason"] as? String) ?? "?",
          (c["os"]     as? String) ?? "?",
          (c["model"]  as? String) ?? "?",
          (c["stack"]  as? String) ?? "")
  }

  static func take() -> [String: Any]? {
    let v = UserDefaults.standard.dictionary(forKey: key)
    return v
  }

  static func clear() { UserDefaults.standard.removeObject(forKey: key) }
}
