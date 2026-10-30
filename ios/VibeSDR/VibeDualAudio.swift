//
//  VibeDualAudio.swift — V12 TWO SERVERS AT ONCE: the second voice, and the pan both voices are placed by.
//
//  ★★ Stuart's design (GitHub discussion #57, 2026-10-27): "server 1 moves to left ear, server 2 right ear". The
//  pairing itself — which server is where, the card, the swap — is JavaScript (src/services/dualServer.ts, pure rules in
//  src/services/dualMix.ts). This file is only the sound:
//
//   · VibeDualVoice — one server folded to mono and placed by position + level, gliding on the 400 ms raised cosine VFO
//     A/B uses (vibe_watch.h Ramp / PanMixer — the same law: L = v·√2·cos(pos·π/2), R = v·√2·sin(pos·π/2)). ONE RULE,
//     THREE READERS: dualMix.ts and VibeDualAudio.kt carry the same Ramp and the same law — change all three.
//   · `primary` is applied to the MAIN engine's buffers by VibePowerModule.deliver(), after the recorder, before the
//     speaker. Not paired = one Bool read and nothing else: the single-server path is untouched.
//   · The SECOND VOICE is its own AVAudioEngine and player, fed Opus packets from JavaScript (the socket is read there).
//     ★★★ A SECOND ENGINE, NOT A SECOND NODE ON THE FIRST — the main engine is rebuilt, paused, ducked and restarted by
//         a dozen paths in VibePowerModule (route changes, interruptions, the page duck, the self-heal), each written for
//         one player. A node of ours inside it would be torn down by all of them and owned by none. Two engines in one
//         app are mixed by the system on the one audio session the main engine already holds.
//     ★ AVAudioEngine is ONE THREAD: every engine lifecycle call here is on main; buffers are decoded and scheduled on
//       this file's own queue (scheduleBuffer is safe off main), never on the main engine's audioQ.
//
//  Mac: the iPad app on a Mac runs this same file; its volume fader reaches this voice through masterGain.
//

import Foundation
import AVFoundation

/// A raised-cosine glide (vibe_watch.h Ramp, verbatim): zero slope at both ends, so no click; re-aimed mid-move, it
/// starts from where it is.
struct DualRamp {
  var from: Float, to: Float, cur: Float
  var t = 1, dur = 1
  init(_ v: Float) { from = v; to = v; cur = v }
  mutating func set(_ v: Float) { from = v; to = v; cur = v; t = 1; dur = 1 }
  mutating func aim(_ v: Float, _ steps: Int) {
    if v == to { return }
    from = cur; to = v; t = 0; dur = max(1, steps)
  }
  var moving: Bool { t < dur }
  mutating func step() -> Float {
    if t < dur {
      t += 1
      let u = Float(t) / Float(dur)
      // ★ The last step lands EXACTLY on the target: from + (to − from)·1 can be a hair short in float (0.99999994),
      //   and "home at unity" (gain ≥ 1, width ≥ 1) is what lets the voice stand down to the plain stereo path.
      cur = t >= dur ? to : from + (to - from) * (0.5 - 0.5 * cos(Float.pi * u))
    }
    return cur
  }
}

/// One server's place in the head. Targets from any thread; process() on ONE audio thread only.
final class VibeDualVoice {
  private let lock = NSLock()
  private var wantPos: Float = 0.5, wantGain: Float = 1, wantOn = false
  /// ★ Its own stereo width, 0 = folded to mono … 1 = as sent (dualMix.ts STEREO WHEN ALONE). 0 until JS says otherwise:
  ///   an app whose JS never sends it folds every voice, as before.
  private var wantWidth: Float = 0
  private var pos = DualRamp(0.5), gain = DualRamp(1), width = DualRamp(1)
  private var on = false
  /// Smoothed level of this server's audio (0…1) — the card's MONO speaker lights with it. A Float store is atomic on
  /// every Apple CPU; a lamp can read a value a buffer old.
  private(set) var level: Float = 0
  /// ★ When this voice last had a buffer (system uptime, s; 0 = never) — dualServer.ts's "has its stream stopped?"
  ///   (a server in ADS-B / AIS / ISM / NDB sends nothing at all). A Double store, read a buffer old at worst.
  private(set) var lastAt: TimeInterval = 0

  /// ms since the last buffer, −1 = never.
  var ageMs: Double { lastAt > 0 ? (ProcessInfo.processInfo.systemUptime - lastAt) * 1000 : -1 }

  func setTarget(pos p: Float, gain g: Float, on o: Bool) {
    lock.lock(); wantPos = max(0, min(1, p)); wantGain = max(0, min(1, g)); wantOn = o; lock.unlock()
  }
  func setWidth(_ w: Float) { lock.lock(); wantWidth = max(0, min(1, w)); lock.unlock() }

  /// In place. Bypassed — one lock and a Bool — unless the pairing has it on or it is still gliding back to the middle.
  func process(_ buf: AVAudioPCMBuffer, extraGain: Float = 1) {
    lock.lock(); let p = wantPos, g = wantGain, o = wantOn, w = wantWidth; lock.unlock()
    if !o && !on { return }
    lastAt = ProcessInfo.processInfo.systemUptime
    guard let ch = buf.floatChannelData, buf.format.channelCount == 2 else { return }
    // ★ Coming in from the plain path = the centre at unity with its own stereo, which is exactly what the plain path
    //   sounds like, so the first glide starts with no step.
    if !on { on = true; pos.set(0.5); gain.set(1); width.set(1) }
    let n = Int(buf.frameLength)
    let steps = Int((0.4 * buf.format.sampleRate).rounded())   // dualMix.ts DUAL_RAMP_SEC
    pos.aim(p, steps); gain.aim(g, steps); width.aim(w, steps)
    let L = ch[0], R = ch[1]
    let k = Float(2).squareRoot() * extraGain, hp = Float.pi / 2
    var sq: Float = 0
    // ★ Mid (m) is placed by the pan law; side (s, the stereo difference) is added back at `width` — at the centre at
    //   unity with width 1 that is L and R exactly as sent; width 0 is the mono fold.
    if !pos.moving && !gain.moving && !width.moving {
      // ★ At rest (nearly always): the ear gains once per buffer, not per sample.
      let gl = gain.cur * k * cos(pos.cur * hp), gr = gain.cur * k * sin(pos.cur * hp), gs = gain.cur * width.cur * extraGain
      for i in 0..<n {
        let m = 0.5 * (L[i] + R[i]), d = 0.5 * (L[i] - R[i]); sq += m * m
        L[i] = m * gl + d * gs; R[i] = m * gr - d * gs
      }
    } else {
      for i in 0..<n {
        let m = 0.5 * (L[i] + R[i]), d = 0.5 * (L[i] - R[i]); sq += m * m
        let th = pos.step() * hp, gn = gain.step(), v = m * gn * k, ds = d * gn * width.step() * extraGain
        L[i] = v * cos(th) + ds; R[i] = v * sin(th) - ds
      }
    }
    let rms = n > 0 ? (sq / Float(n)).squareRoot() : 0
    level = max(rms, level * 0.85)
    // ★ Home and resting with the pairing over: stand down; the next buffer takes the plain path.
    if !o && !pos.moving && !gain.moving && !width.moving && abs(pos.cur - 0.5) < 1e-6 && gain.cur >= 1 && width.cur >= 1 {
      on = false
    }
  }
}

@objc(VibeDualAudio)
final class VibeDualAudio: NSObject {
  /// The main engine's voice — VibePowerModule.deliver() calls primary.process on its audioQ.
  static let primary = VibeDualVoice()
  /// The second voice's placement.
  static let auxVoice = VibeDualVoice()
  /// ⏸ on the main engine (VibePowerModule.setMuted) pauses this voice too — one app, one pause.
  static var primaryMuted = false
  /// The Mac's volume fader (VibePowerModule.setMacOutputGain); 1 elsewhere.
  static var masterGain: Float = 1

  @objc static func requiresMainQueueSetup() -> Bool { false }

  private let fmt = AVAudioFormat(standardFormatWithSampleRate: 48_000, channels: 2)!
  private let q = DispatchQueue(label: "com.vibesdr.dualaux", qos: .userInteractive)

  // main only
  private var engine: AVAudioEngine?
  private var wanted = false
  private var observers: [NSObjectProtocol] = []
  private var lastRebuildAt: TimeInterval = 0

  // the player, read on q, replaced on main
  private let pLock = NSLock()
  private var playerRef: AVAudioPlayerNode?
  private var gen = 0

  // q only
  private var dec: OpaquePointer?
  private var decCh: Int32 = 0
  private var decFails = 0
  private var conv: AVAudioConverter?
  private var convIn: AVAudioFormat?
  private var queued: Double = 0
  private var held: [AVAudioPCMBuffer] = []
  private var heldSecs: Double = 0
  private var pushes = 0
  private static let preRollSecs = 0.22   // the main engine's cushion (kPreRollSeconds)
  private static let liveEdgeCap = 0.5

  // MARK: - JS

  @objc func setNativeMix(_ pos: NSNumber, gain: NSNumber, active: Bool) {
    VibeDualAudio.primary.setTarget(pos: pos.floatValue, gain: gain.floatValue, on: active)
  }

  @objc func setAuxMix(_ pos: NSNumber, gain: NSNumber) {
    VibeDualAudio.auxVoice.setTarget(pos: pos.floatValue, gain: gain.floatValue, on: true)
  }

  @objc func setNativeWidth(_ w: NSNumber) { VibeDualAudio.primary.setWidth(w.floatValue) }
  @objc func setAuxWidth(_ w: NSNumber) { VibeDualAudio.auxVoice.setWidth(w.floatValue) }

  @objc func startAux() {
    DispatchQueue.main.async {
      self.wanted = true
      if self.engine == nil { self.build() }
    }
  }

  @objc func stopAux() {
    DispatchQueue.main.async {
      self.wanted = false
      self.teardown()
    }
    q.async {
      if let d = self.dec { opus_decoder_destroy(d) }
      self.dec = nil; self.decCh = 0
      self.conv = nil; self.convIn = nil
      self.held.removeAll(); self.heldSecs = 0; self.queued = 0
    }
  }

  @objc func pushAuxOpus(_ b64: String, rate: NSNumber, channels: NSNumber) {
    guard let pkt = Data(base64Encoded: b64), pkt.count >= 3 else { return }
    let ch: Int32 = channels.intValue == 2 ? 2 : 1
    q.async {
      // ★ Decoded at 48 kHz whatever the stream's rate: an Opus decoder may run at any of its rates, so the voice
      //   needs no resampler and never rebuilds on a mode change that flips the server's audio rate.
      if self.dec == nil || self.decCh != ch {
        if let d = self.dec { opus_decoder_destroy(d) }
        var err: Int32 = 0
        self.dec = opus_decoder_create(48_000, ch, &err)
        self.decCh = ch
      }
      guard let d = self.dec else { return }
      var pcm = [Int16](repeating: 0, count: 5760 * Int(ch))
      let n = pkt.withUnsafeBytes { raw -> Int32 in
        opus_decode(d, raw.baseAddress!.assumingMemoryBound(to: UInt8.self), Int32(pkt.count), &pcm, 5760, 0)
      }
      // ★ A failed decode REBUILDS the decoder (the main path's lesson: a sad decoder stays sad).
      if n <= 0 {
        self.decFails += 1
        if self.decFails == 1 || self.decFails % 50 == 0 { NSLog("[VibeDualAudio] opus decode failed (%d) x%d", n, self.decFails) }
        opus_decoder_destroy(d); self.dec = nil; self.decCh = 0
        return
      }
      self.decFails = 0
      guard let b = AVAudioPCMBuffer(pcmFormat: self.fmt, frameCapacity: AVAudioFrameCount(n)),
            let out = b.floatChannelData else { return }
      b.frameLength = AVAudioFrameCount(n)
      let L = out[0], R = out[1]
      for i in 0..<Int(n) {
        if ch == 2 { L[i] = Float(pcm[2 * i]) / 32768; R[i] = Float(pcm[2 * i + 1]) / 32768 }
        else { let v = Float(pcm[i]) / 32768; L[i] = v; R[i] = v }
      }
      self.play(b)
    }
  }

  /// Uncompressed int16 (an old server without Opus, or ADPCM decoded in JS) at its own rate → 48 kHz stereo.
  @objc func pushAuxPcm(_ b64: String, rate: NSNumber, channels: NSNumber) {
    guard let data = Data(base64Encoded: b64), data.count >= 2 else { return }
    let sr = rate.doubleValue
    guard sr >= 8000, sr <= 384_000 else { return }
    let stereo = channels.intValue == 2
    q.async {
      let total = data.count / 2
      let n = stereo ? total / 2 : total
      guard n > 0,
            let inFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: sr, channels: stereo ? 2 : 1,
                                      interleaved: false),
            let inBuf = AVAudioPCMBuffer(pcmFormat: inFmt, frameCapacity: AVAudioFrameCount(n)),
            let ic = inBuf.floatChannelData else { return }
      inBuf.frameLength = AVAudioFrameCount(n)
      data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
        let s = raw.bindMemory(to: Int16.self)
        for i in 0..<n {
          if stereo { ic[0][i] = Float(Int16(littleEndian: s[2 * i])) / 32768; ic[1][i] = Float(Int16(littleEndian: s[2 * i + 1])) / 32768 }
          else { ic[0][i] = Float(Int16(littleEndian: s[i])) / 32768 }
        }
      }
      if self.conv == nil || self.convIn != inFmt {
        self.conv = AVAudioConverter(from: inFmt, to: self.fmt); self.convIn = inFmt
      }
      guard let cv = self.conv,
            let outBuf = AVAudioPCMBuffer(pcmFormat: self.fmt,
                                          frameCapacity: AVAudioFrameCount(Double(n) * 48_000 / sr) + 64) else { return }
      var fed = false
      var err: NSError?
      let st = cv.convert(to: outBuf, error: &err) { _, o in
        if fed { o.pointee = .noDataNow; return nil }
        fed = true; o.pointee = .haveData; return inBuf
      }
      guard st != .error, outBuf.frameLength > 0 else { return }
      // ★ A mono source through the converter lands in the LEFT channel only — copy it across before the fold, or the
      //   fold would halve it.
      if !stereo, let oc = outBuf.floatChannelData {
        for i in 0..<Int(outBuf.frameLength) { oc[1][i] = oc[0][i] }
      }
      self.play(outBuf)
    }
  }

  @objc func getLevels(_ resolve: @escaping RCTPromiseResolveBlock, rejecter reject: @escaping RCTPromiseRejectBlock) {
    resolve(["native": VibeDualAudio.primary.level, "aux": VibeDualAudio.auxVoice.level,
             "nativeAge": VibeDualAudio.primary.ageMs, "auxAge": VibeDualAudio.auxVoice.ageMs])
  }

  // MARK: - Playing (q)

  private func play(_ b: AVAudioPCMBuffer) {
    VibeDualAudio.auxVoice.process(b, extraGain: VibeDualAudio.masterGain)
    pushes += 1
    if pushes % 50 == 0 { DispatchQueue.main.async { self.ensureRunning() } }   // ~once a second
    if VibeDualAudio.primaryMuted { held.removeAll(); heldSecs = 0; return }
    pLock.lock(); let p = playerRef; let g = gen; pLock.unlock()
    guard let player = p else { return }
    let d = Double(b.frameLength) / 48_000
    if queued > VibeDualAudio.liveEdgeCap { return }   // a burst never builds latency
    // ★ A cushion before the first buffer plays, as the main engine has (VibePowerModule scheduleOut's pre-roll):
    //   without it the first network gap is an underrun.
    if queued <= 0 && heldSecs < VibeDualAudio.preRollSecs {
      held.append(b); heldSecs += d
      if heldSecs < VibeDualAudio.preRollSecs { return }
      let all = held; held.removeAll(); heldSecs = 0
      for x in all { sched(player, x, gen: g) }
      return
    }
    sched(player, b, gen: g)
  }

  private func sched(_ player: AVAudioPlayerNode, _ b: AVAudioPCMBuffer, gen g: Int) {
    let d = Double(b.frameLength) / 48_000
    queued += d
    player.scheduleBuffer(b) { [weak self] in
      guard let self else { return }
      self.q.async {
        self.pLock.lock(); let same = self.gen == g; self.pLock.unlock()
        if same { self.queued = max(0, self.queued - d) }
      }
    }
  }

  // MARK: - The engine (main)

  private func build() {
    guard wanted, engine == nil else { return }
    let e = AVAudioEngine()
    let p = AVAudioPlayerNode()
    e.attach(p)
    e.connect(p, to: e.mainMixerNode, format: fmt)
    e.prepare()
    do { try e.start() } catch {
      NSLog("[VibeDualAudio] engine start failed: %@", "\(error)")
      return
    }
    p.play()
    engine = e
    pLock.lock(); playerRef = p; gen &+= 1; pLock.unlock()
    q.async { self.queued = 0; self.held.removeAll(); self.heldSecs = 0 }
    let nc = NotificationCenter.default
    observers.append(nc.addObserver(forName: .AVAudioEngineConfigurationChange, object: e, queue: .main) { [weak self] _ in
      self?.rebuild("configuration change")
    })
    observers.append(nc.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] n in
      let raw = n.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
      if raw == AVAudioSession.InterruptionType.ended.rawValue { self?.rebuild("interruption ended") }
    })
  }

  private func teardown() {
    observers.forEach { NotificationCenter.default.removeObserver($0) }
    observers.removeAll()
    pLock.lock(); let p = playerRef; playerRef = nil; gen &+= 1; pLock.unlock()
    p?.stop()
    engine?.stop()
    engine = nil
  }

  private func rebuild(_ why: String) {
    guard wanted else { return }
    NSLog("[VibeDualAudio] rebuilding the second voice: %@", why)
    lastRebuildAt = ProcessInfo.processInfo.systemUptime
    teardown()
    build()
  }

  /// A stopped engine (the system stopped it, or a route change we missed) is rebuilt — at most once every 2 s.
  private func ensureRunning() {
    guard wanted else { return }
    if let e = engine, e.isRunning { return }
    if ProcessInfo.processInfo.systemUptime - lastRebuildAt < 2 { return }
    rebuild("engine not running")
  }
}
