import Foundation
import Combine
import Network
import CryptoKit

/// ★★★ ONE ANSWER TO "WHAT VERSION IS THIS?", AND IT IS THE BUNDLE'S. Jr carried THREE different
///     versions at once on the morning 1.2 was prepared: the project said 1.2, the VibeServer
///     User-Agent said 1.3, and the FM-DX one still said 1.0 — the version it shipped with in
///     July. Every one of them is sent to somebody else's receiver, where operators write filter
///     rules against the string and an operator who wants to refuse us BY NAME is entitled to a
///     name that is true.
/// ★★ Read from CFBundleShortVersionString rather than typed here, so it cannot drift from what
///    actually shipped — a hardcoded constant is exactly what drifted three ways. The phone app
///    learned this the hard way twice (see src/constants/version.ts, which carries the same
///    warning and the same scars).
/// ★ The fallback is only reachable if the Info.plist key is missing, which would be a broken
///   build; it names a version rather than "unknown" so a log line is still greppable.
enum JrVersion {
  static let short: String =
    (Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String) ?? "1.2"
}


/// A DIRECT UberSDR client, running on the WATCH. No phone anywhere in the chain.
///
/// This is the whole question JR asks: today the phone holds these sockets, does the DSP,
/// decodes the audio, and hands the watch a finished picture. Here the watch does all of
/// it. The protocol is UberSDR's own, ported from `src/services/UberSDRClient.ts` and
/// `VibePowerModule.swift` — same endpoints, same frame formats, same session uuid shared
/// between the two sockets.
///
/// Hard-coded to one server on purpose. A spike that also has to be an app is a spike that
/// never gets finished.
@MainActor

final class UberClient: ObservableObject {

  /// The UberSDR host to connect to. Selectable now (was a hardcoded `static let`) so the
  /// instance picker can point the spike at any UberSDR server the user chooses.
  var host = "stuey3d.tunnel.ubersdr.org"

  // ── A MULTI-RADIO VibeServer (V3) ────────────────────────────────────────────
  //
  // ★★★ ON SUCH A SERVER EVERY RADIO LIVES BEHIND `/r/<id>/…`, and the front door itself owns no
  //     radio: ask IT for `/ws/user-spectrum` and it answers 503, which as a WebSocket handshake
  //     arrives as a bare close with nothing in it — identical to "server down". So the prefix is
  //     not decoration, it is the difference between connecting and appearing to be offline.
  // ★★ ONE PROPERTY, INSERTED BETWEEN HOST AND PATH, because every URL this client builds hangs
  //    off the same two pieces. Prefixing at each call site would be a list to keep in step for
  //    ever, and the first one forgotten is a failure that appears only on multi-radio servers —
  //    exactly the reasoning the SERVER uses for stripping it once on arrival rather than at each
  //    of its dozens of routes.
  /// "" for an ordinary server, or "/r/<id>" once a radio has been chosen.
  var radioPath = ""

  /// The radios behind a front door, published for the UI to offer. Empty when there is no choice
  /// to make — which includes a door with exactly one radio, resolved silently below.
  @Published var radioChoices: [VibeRadio] = []
  /// The machine's name, shown above that list.
  @Published var radioChoiceName = ""

  // ── VibeServer mode ──────────────────────────────────────────────────────────
  // VibeServer is VibeSDR's OWN phone-hosted server: the shim's UberSDR-style WS protocol, so the whole
  // SPECTRUM pipeline is reused unchanged. Only three things diverge, all behind these flags (which default
  // to exact UberSDR behaviour, so a plain UberSDR client is byte-identical to before):
  //   1. `secure` — VibeServer is plain ws:// on the LAN, UberSDR is wss://.
  //   2. `authSuffix` — VibeServer PIN via HMAC (see resolveVibeAuth); "&vs_nonce=…&vs_auth=…" on the URLs.
  //   3. `localAudio` — VibeServer audio is /ws/audio with ADPCM (self-seeded, mid-side stereo), not /ws Opus.
  /// True for a VibeServer connection.
  var isVibe = false
  /// wss (UberSDR) vs ws (VibeServer LAN).
  var secure = true
  /// The PIN, if the VibeServer requires one (resolved to `authSuffix` during connect).
  var vibePin = ""
  /// ★★ SET WHEN THE SERVER WANTS A PIN AND WE HAVEN'T GOT A WORKING ONE.
  ///
  /// A missing PIN used to be indistinguishable from a bad link: the server issues
  /// a nonce regardless, so we HMAC'd it with an EMPTY key, opened the sockets with
  /// a token that could only be rejected, and then RETRIED — leaving the user on
  /// "authenticating…" then "reconnecting" with nothing ever asking for a PIN.
  /// Stuart could only get onto the Moto by turning its PIN off (2026-07-28).
  ///
  /// The prompt existed, but only on the mDNS path (`ad.pinRequired`), so a
  /// favourite or a typed IP — the only routes that work over Bluetooth — had no
  /// way in at all. This flag is what the UI watches, whichever route was used.
  @Published var needsPin = false
  private var authSuffix = ""
  /// ★ Admin OVERRIDE credentials, which must ride the CONNECT URL — the slot is
  /// claimed before any socket message could arrive, so `admin_unlock` is too late
  /// for a server that is already telling us "busy". Challenge-response, never the
  /// password: HMAC(secret, nonce), same VsAuth as the PIN, so it inherits the
  /// brute-force lockout too.
  private var adminSuffix = ""
  /// ★ One string, so the log cannot show two different names for the same app. Mirrors the value
  ///   the /connection POST already sends.
  // ★ Jr names itself SEPARATELY from the phone — it is its own app with its own version, and an
  //   operator reading a connection log wants to know which of the two arrived. Bumped with the
  //   phone release because they ship together; only the version ever moves, because operators
  //   write filter rules against this string.
  private let jrUserAgent = "VibeSDR Jr/\(JrVersion.short) (watchOS)"
  /// The server is serving somebody else. Terminal until the user retries or takes
  /// over — NOT a reconnect loop, which would just hammer a busy receiver.
  @Published var serverBusy = false
  /// …and the owner has set an admin password, so a takeover is even possible.
  @Published var takeoverPossible = false
  private var scheme: String { secure ? "wss" : "ws" }
  private var vibeAdopted = false      // adopted the server's tune on the first config yet?
  private var vibeRestored = false     // did we restore + assert a SAVED tune for this host?

  /// Per-host tune memory (VibeServer only) — so it reopens where you left it, not the 648 kHz/AM default.
  private var vibeStateKey: String { "vibe.tune.\(host)" }
  private func saveVibeState() {
    guard isVibe, frequency > 0 else { return }
    UserDefaults.standard.set(["f": frequency, "m": mode], forKey: vibeStateKey)
  }
  private func restoreVibeState() {
    guard isVibe, let s = UserDefaults.standard.dictionary(forKey: vibeStateKey),
          let f = s["f"] as? Double, f > 0 else { return }
    frequency = f
    if let m = s["m"] as? String { mode = m; if let bw = Self.modeBW[m] { bwLow = bw.low; bwHigh = bw.high } }
    vibeRestored = true
  }
  /// Per-host RTL-SDR memory (VibeServer only) — gain/bias-T/AGC/ppm/rate/de-emphasis, so the dongle comes
  /// back the way you left it instead of wanting a re-dial on every connect.
  private var vibeHwKey: String { "vibe.hw.\(host)" }
  private var vibeHwRestoring = false    // suppress save-on-set while we replay the saved values
  private func saveVibeHw() {
    guard isVibe, !vibeHwRestoring else { return }
    UserDefaults.standard.set(["auto": gainAuto, "gain": gainValue, "biasT": biasT,
                               "agc": agc, "ppm": ppm, "rate": sampleRate, "deemph": deemph,
                               "rfnotch": rspRfNotch, "dabnotch": rspDabNotch],
                              forKey: vibeHwKey)
  }
  /// Replay the saved settings. Driven by `hwinfo` rather than by connect because the gain steps and capture
  /// rates are SERVER-declared — asserting a value this dongle can't do would just be refused, and the host
  /// may have pinned the rate. Re-runs on every `hwinfo`, so a reconnect (the weak point over Bluetooth) puts
  /// the radio back rather than leaving it on the server's defaults.
  private func restoreVibeHw() {
    guard isVibe, let s = UserDefaults.standard.dictionary(forKey: vibeHwKey) else { return }
    vibeHwRestoring = true
    defer { vibeHwRestoring = false }

    if let v = s["agc"]    as? Bool, v != agc    { setAgc(v) }
    if let v = s["biasT"]  as? Bool, v != biasT  { setBiasT(v) }
    if let v = s["ppm"]    as? Int,  v != ppm    { setPpm(v) }
    if let v = s["deemph"] as? Int,  v != deemph { setDeemph(v) }
    // ★ Restore the notch PREFERENCE even on a radio that lacks them — the user may move
    //   between servers, and a setting silently dropped is a setting that never comes back.
    //   pushAllRadioSettings() gates the actual send on the advertised capability.
    if let v = s["rfnotch"]  as? Bool { rspRfNotch  = v }
    if let v = s["dabnotch"] as? Bool { rspDabNotch = v }

    // Capture rate only if the host hasn't pinned it and the server actually offers it.
    if lockedRate == 0, let r = s["rate"] as? Int, r > 0, offeredRates.contains(r), r != sampleRate {
      setCaptureRate(r)
    }
    // Gain LAST: an RTL dongle commonly resets tuner gain when the sample rate changes. Both ride the same
    // spectrum WS, so the server applies them in this order.
    if let auto = s["auto"] as? Bool, auto {
      if !gainAuto { setGainAuto(true) }
    } else if let g = s["gain"] as? Double,
              let step = offeredGains.min(by: { abs(Double($0) - g) < abs(Double($1) - g) }) {
      setGainValue(Double(step))   // snap to a step this tuner actually has
    }
  }

  private let adpcmL = ImaAdpcmDecoder(flavor: .kiwi)   // VibeServer audio: mid / left channel
  private let adpcmR = ImaAdpcmDecoder(flavor: .kiwi)   // side / right channel
  /// Server-advertised capabilities from the `hwinfo` message (offered capture rates + the owner's FFT/FPS
  /// ceiling). The adaptive-quality loop clamps to `maxFftRate`.
  @Published var offeredRates: [Int] = []
  @Published var offeredGains: [Int] = []      // tuner gain steps (tenths of dB) from hwinfo
  @Published var lockedRate = 0                // >0 = the host pinned the capture rate; hide the picker
  // ── Owner policy: session limit + admin lock ────────────────────────────────
  /// ★ The server ENFORCES both of these; the client's job is to SAY SO. A limit
  /// nobody is told about reads as a crash, and controls that are locked but drawn
  /// normally read as a broken radio ("all controls for the SDR still present").
  /// Both arrive on the config message — the shim advertises them precisely so a
  /// client need not guess.
  @Published var sessionLimitMin = 0           // owner's per-listener limit, minutes (0 = none)
  @Published var sessionSecsLeft = -1          // -1 = no limit, or we are exempt (loopback/admin)
  @Published var adminSet = false              // the owner has set an admin password
  @Published var adminOk = false               // …and we are through it
  // ── Which radio the server actually has ─────────────────────────────────────
  @Published var radioDriver = ""              // "rtl" | "sdrplay" | "airspyhf"
  @Published var radioModel = ""               // e.g. "RSPdx", "Airspy HF+ Discovery"
  @Published var radioHasBiasT = true
  // Per-radio gain capability, straight from the server's advert.
  @Published var lnaStates = 0                 // SDRplay: number of LNA states
  @Published var ifGrMin = 20                  // SDRplay: IF gain-reduction range, dB
  @Published var ifGrMax = 59
  @Published var attSteps = 0                  // Airspy HF+: attenuator positions (9 = 0..8)
  @Published var attStepDb = 6                 // …dB per step
  @Published var hasPreamp = false             // HF+ +6 dB preamp
  @Published var hasRadioAgc = false           // HF+ own AGC
  // Current per-radio gain state (what we last sent — the server does not echo these back).
  @Published var rspLna = 0
  @Published var rspIfGr = 40
  @Published var rspIfAgc = true
  /// The RSP's broadcast notches, and whether this model actually has them.
  @Published var rspRfNotch = false
  @Published var rspDabNotch = false
  @Published var radioHasRfNotch = false
  @Published var radioHasDabNotch = false
  @Published var ahfAtt = 0
  @Published var ahfPreamp = false
  @Published var ahfAgc = true
  /// ★ True once the USER has actually set one of these controls in this session.
  /// Until then we have nothing worth asserting and must leave the radio alone.
  private var radioDirty = false
  /// Live from the server's `rspstat`: total system gain and the overload flag.
  /// The owner took this receiver. Terminal — never auto-retried.
  @Published var evicted = false
  /// Our slot expired (server said so). Terminal.
  @Published var sessionEnded = false
  /// Refused because we came back inside our cooldown. Terminal.
  @Published var cooldownRefused = false
  /// Seconds before we may return — the server's number, not a guess.
  @Published var cooldownSecs = 0
  @Published var sysGainDb = 0.0
  /// ★★ The RSP's gain loop is still SETTLING — from rspstat. Until this clears, the gain
  /// figures above it are a loop in motion, not a reading. Stuart asked for the controls to be
  /// cycled behind an initialising state on connect so the AGC cannot come up stuck, which is a
  /// bug that has bitten twice; this is the visible half of that.
  @Published var radioSettling = false
  @Published var rspOverload = false
  /// Human name for the hardware sheet's header.
  var radioName: String {
    if radioModel.isEmpty { return radioDriver == "rtl" ? "RTL-SDR" : "—" }
    return radioDriver == "sdrplay" ? "SDRplay \(radioModel)" : radioModel
  }
  /// ★ A dongle's single gain slider is meaningless on an RSP, and an HF+ has NO
  /// variable gain stage. Hide, rather than offer a control whose label lies.
  var radioHasSimpleGain: Bool { radioDriver != "sdrplay" && radioDriver != "airspyhf" }
  /// PPM is an RTL trim. The HF+ calibrates in parts per BILLION and the RSP has its
  /// own scheme — two correction controls disagreeing about units is exactly the
  /// "which one is real?" confusion to avoid.
  var radioHasPpm: Bool { radioDriver != "sdrplay" && radioDriver != "airspyhf" }
  @Published var maxFftRate = 0

  // ── VibeServer hardware controls (the client drives the radio over the spectrum WS) ──
  @Published var gainAuto = true
  @Published var gainValue = 0.0               // tenths of dB
  @Published var biasT = false
  @Published var agc = false
  @Published var ppm = 0
  @Published var sampleRate = 0                // current capture rate (= spectrum span)
  @Published var deemph = 50                   // FM de-emphasis µs (50 EU / 75 Americas / 0 off)
  // ★★★ THE BROADCAST-FM TREATMENTS (VibeServer 3.1). FOUR FAULTS, FOUR SWITCHES, and they are not
  //     interchangeable: NR answers continuous NOISE, IMS a strong NEIGHBOUR, CEQ a REFLECTION, NB
  //     IMPULSES. Measured on the server they want OPPOSITE actions — narrowing the IF costs up to
  //     10 dB against noise and gains 10 dB against a close neighbour — so one combined control
  //     would be actively wrong.
  // ★★ Default TRUE, matching the server: each only acts when its own measurements say it will
  //    help, so a listener who never opens this sheet gets the better sound. And the RADIO is the
  //    authority — these are re-rendered from hwinfo, because they are sticky AND shared with
  //    every other listener on that receiver.
  @Published var fmNr  = true
  @Published var fmIms = true
  @Published var fmCeq = true
  @Published var fmNb  = true
  /// True once the server has reported any of them — an older VibeServer never does, and Jr then
  /// draws no controls rather than offering ones that cannot work.
  @Published var hasFmDsp = false
  var hasHardwareControls: Bool { isVibe }

  private func onHwInfo(_ j: [String: Any]) {
    // ★★ SESSION LIMIT AND ADMIN STATE RIDE **HWINFO**, not config. I read them off
    // `config` first and nothing ever appeared — no notice, no countdown, no lock —
    // because that message simply does not carry them (local_sdr_shim.cpp builds
    // them into `{"type":"hwinfo",…}`). A field parsed from the wrong message is
    // silent: no error, no warning, just a feature that never happens.
    if let n = (j["sessionLimitMin"] as? NSNumber)?.intValue { sessionLimitMin = n }
    if let n = (j["sessionSecsLeft"] as? NSNumber)?.intValue { sessionSecsLeft = n }
    // ★★ THE SERVER'S WORD ON ITS OWN DSP — sticky AND shared, so what this watch last asked for is
    //    irrelevant. Receiving any of them is also what REVEALS the controls: an older VibeServer,
    //    or a backend without them, never sends them and Jr draws nothing.
    if j["wsp"] != nil || j["ims"] != nil || j["ceq"] != nil || j["nb"] != nil {
      hasFmDsp = true
      // ★ Absent means an older server that HAS the treatment but does not talk about it, so the
      //   default is ON rather than OFF — the same rule the phone and the browser use.
      fmNr  = (j["wsp"] as? Bool) ?? true
      fmIms = (j["ims"] as? Bool) ?? true
      fmCeq = (j["ceq"] as? Bool) ?? true
      fmNb  = (j["nb"]  as? Bool) ?? true
    }
    if let b = j["adminSet"] as? Bool { adminSet = b }
    if let b = j["adminOk"] as? Bool { adminOk = b }
    // ★★ WHICH RADIO IS THIS? Jr drew RTL-SDR controls whatever was plugged in, so an
    // Airspy HF+ showed a gain slider it does not have (no variable gain stage at all)
    // and a PPM trim whose units are wrong for it — calibration on an HF+ is in parts
    // per BILLION. Controls that cannot work are worse than absent: the user cannot
    // tell a dead control from a dead radio. The server has always advertised this.
    // See [[one_radio_assumption_family]] — ask what it is, do not assume.
    if let r = j["radio"] as? [String: Any] {
      radioDriver = (r["driver"] as? String ?? "").lowercased()
      radioModel  = (r["model"] as? String ?? "")
      radioHasBiasT = (r["biasT"] as? Bool) ?? (radioDriver == "rtl")
      lnaStates  = (r["lnaStates"] as? NSNumber)?.intValue ?? 0
      ifGrMin    = (r["ifGrMin"] as? NSNumber)?.intValue ?? 20
      ifGrMax    = (r["ifGrMax"] as? NSNumber)?.intValue ?? 59
      attSteps   = (r["attSteps"] as? NSNumber)?.intValue ?? 0
      attStepDb  = (r["attStepDb"] as? NSNumber)?.intValue ?? 6
      hasPreamp  = (r["hfLna"] as? Bool) ?? false
      hasRadioAgc = (r["hfAgc"] as? Bool) ?? false
      // ★ The RSP's two notches. Advertised per MODEL — not every RSP has both — so ask
      //   rather than assume, the same rule as everything else in this block.
      radioHasRfNotch  = (r["rfNotch"] as? Bool) ?? false
      radioHasDabNotch = (r["dabNotch"] as? Bool) ?? false
      if rspIfGr < ifGrMin || rspIfGr > ifGrMax { rspIfGr = min(max(40, ifGrMin), ifGrMax) }
    }
    if let g = j["gains"] as? [Int] { offeredGains = g }
    if let r = j["rates"] as? [Int] { offeredRates = r }
    lockedRate = (j["lockedRate"] as? NSNumber)?.intValue ?? 0
    maxFftRate = (j["maxFftRate"] as? NSNumber)?.intValue ?? 0   // owner ceiling (needs a Moto update to appear)
    restoreVibeHw()          // the server has now declared what it offers — put the radio back
    pushAllRadioSettings()   // …including the per-radio gain path, see below
  }

  /// All hardware controls ride the SPECTRUM WS as JSON (matches UberSDRClient._sendCtl / the shim).
  /// Guarded to VibeServer — a public UberSDR server would reject them.
  func setGainAuto(_ auto: Bool) { guard isVibe else { return }; gainAuto = auto; specSock.send(json: ["type": "gain", "auto": auto]); saveVibeHw() }
  func setGainValue(_ tenthDb: Double) { guard isVibe else { return }; gainAuto = false; gainValue = tenthDb; specSock.send(json: ["type": "gain", "value": Int(tenthDb)]); saveVibeHw() }
  func setBiasT(_ on: Bool) { guard isVibe else { return }; biasT = on; specSock.send(json: ["type": "biasT", "on": on]); saveVibeHw() }
  func setRspRfNotch(_ on: Bool)  { rspRfNotch = on;  rspSend(["rfnotch": on ? 1 : 0]);  saveVibeHw() }
  func setRspDabNotch(_ on: Bool) { rspDabNotch = on; rspSend(["dabnotch": on ? 1 : 0]); saveVibeHw() }

  // ── Per-radio gain (SDRplay / Airspy HF+) ───────────────────────────────────
  // ★ The wrist gets the GAIN controls and not the calibration: ppm/ppb is a very
  // fine trim that nobody sets from a watch, and it is the one control where a
  // mis-tap is hard to notice and hard to undo. Gain is the opposite — you change
  // it because of what you are hearing right now.
  private func rspSend(_ m: [String: Any]) { guard isVibe else { return }; specSock.send(json: m.merging(["type": "rsp_control"]) { a, _ in a }) }
  private func ahfSend(_ m: [String: Any]) { guard isVibe else { return }; specSock.send(json: m.merging(["type": "ahf_control"]) { a, _ in a }) }

  /// SDRplay LNA. ★ The UI speaks GAIN (higher = more), the hardware wants a STATE
  /// (higher = less), so it is inverted on the way out — same as the web client.
  func setRspLna(_ gainIdx: Int) {
    radioDirty = true
    let g = max(0, min(max(0, lnaStates - 1), gainIdx))
    rspLna = g
    rspSend(["lna": max(0, lnaStates - 1) - g])
  }
  func setRspIfGr(_ db: Int) {
    radioDirty = true
    rspIfGr = max(ifGrMin, min(ifGrMax, db))
    if !rspIfAgc { rspSend(["ifgr": rspIfGr]) }
  }
  func setRspIfAgc(_ on: Bool) {
    radioDirty = true
    rspIfAgc = on
    rspSend(["ifagc": on ? 1 : 0])
    if !on { rspSend(["ifgr": rspIfGr]) }      // hand back the manual value it now owns
  }
  func setAhfAtt(_ step: Int) {
    radioDirty = true
    ahfAtt = max(0, min(max(0, attSteps - 1), step))
    if !ahfAgc { ahfSend(["att": ahfAtt]) }
  }
  func setAhfPreamp(_ on: Bool) {
    radioDirty = true
    ahfPreamp = on
    ahfSend(["lna": on ? 1 : 0])
  }
  /// ★ AGC LAST when turning it OFF, so the manual attenuation we hand back is not
  /// immediately overridden by the AGC that still owns the gain path.
  func setAhfAgc(_ on: Bool) {
    radioDirty = true
    ahfAgc = on
    if on { ahfSend(["agc": 1]) }
    else { ahfSend(["att": ahfAtt]); ahfSend(["agc": 0]) }
  }

  /// Is the server already serving somebody? Also records whether an admin password
  /// exists, so the UI only offers a takeover where one could work.
  private func vibeIsBusy() async -> Bool {
    let httpScheme = secure ? "https" : "http"
    guard let url = URL(string: "\(httpScheme)://\(host)\(radioPath)/vibeserver.json"),
          let (data, _) = try? await httpSession.data(from: url),
          let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
      return false      // ★ Can't tell ⇒ carry on. A failed probe must not block a connect.
    }
    takeoverPossible = (j["admin"] as? Bool) ?? false
    return (j["busy"] as? Bool) ?? false
  }

  /// Whether the owner's password has been proved for the NEXT connection.
  @Published var adminProved = false
  @Published var adminProveFailed = false

  /**
   * Prove the owner's password BEFORE connecting, so the credential rides the connect URL.
   *
   * ★★★ THIS CANNOT BE DONE AFTER CONNECTING. A full server and one holding us on a cooldown both
   *     refuse during the HANDSHAKE — the slot is claimed before any socket message could arrive —
   *     so `admin_unlock`, which is a message, is far too late. That is why the owner of a busy
   *     receiver could only ever get in by being told IN USE first and answering the prompt: the
   *     way in existed, but only as a reaction to being turned away (Stuart, 2026-08-13, wanting
   *     it on the picker so an owner can simply go in).
   * ★★ Proved at the DOOR — `radioPath` is still empty here — because the door owns the
   *    machine-wide password and every radio behind it honours what the door issued. Proving at a
   *    radio would bind the credential to that one process.
   * ★ Sets no `serverBusy`/reconnect of its own: this only ARMS the credential. The connection is
   *   made by picking a radio, exactly as it was before.
   */
  func proveAdminForConnect(_ password: String) {
    guard isVibe, !password.isEmpty else { return }
    adminProveFailed = false
    Task { [weak self] in
      guard let self else { return }
      let httpScheme = secure ? "https" : "http"
      guard let url = URL(string: "\(httpScheme)://\(host)\(radioPath)/vibeserver/auth"),
            let (data, _) = try? await httpSession.data(from: url),
            let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let nonce = j["nonce"] as? String, !nonce.isEmpty else {
        await MainActor.run { self.adminProveFailed = true }
        return
      }
      let mac = HMAC<SHA256>.authenticationCode(for: Data(nonce.utf8),
                                                using: SymmetricKey(data: Data(password.utf8)))
      let token = mac.map { String(format: "%02x", $0) }.joined()
      await MainActor.run {
        // ★★ A WRONG PASSWORD CANNOT BE DETECTED HERE. The nonce is handed out to anyone; only the
        //    server can judge the HMAC, and it does that at the handshake. So this reports
        //    "armed", not "correct" — and a wrong one simply connects as an ordinary listener,
        //    which is the same outcome as not trying. Saying "unlocked" here would be a lie we
        //    could not stand behind.
        self.adminSuffix = "&vs_admin_nonce=\(nonce)&vs_admin_auth=\(token)"
        self.adminProved = true
      }
    }
  }

  /// Take the receiver with the owner's admin password. The displaced listener is
  /// told why (`evicted`), which is what makes this safe where plain takeover was not.
  func takeOverWithAdmin(_ password: String) {
    guard isVibe, !password.isEmpty else { return }
    Task { [weak self] in
      guard let self else { return }
      let httpScheme = secure ? "https" : "http"
      guard let url = URL(string: "\(httpScheme)://\(host)\(radioPath)/vibeserver/auth"),
            let (data, _) = try? await httpSession.data(from: url),
            let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let nonce = j["nonce"] as? String, !nonce.isEmpty else {
        await MainActor.run { self.status = "admin: can't reach server" }
        return
      }
      let mac = HMAC<SHA256>.authenticationCode(for: Data(nonce.utf8),
                                                using: SymmetricKey(data: Data(password.utf8)))
      let token = mac.map { String(format: "%02x", $0) }.joined()
      await MainActor.run {
        self.adminSuffix = "&vs_admin_nonce=\(nonce)&vs_admin_auth=\(token)"
        self.serverBusy = false
        self.status = "taking over"
        self.openVibeSockets()
      }
    }
  }

  /// Re-assert every per-radio setting the moment the radio announces itself.
  ///
  /// ★★ THIS IS WHAT STOPS THE RSP's AGC SITTING INERT. The server-side cause was
  /// fixed in 05f330f (gRdB must be SEEDED before the AGC is handed the register —
  /// writing it afterwards is refused, which is why "disable AGC, wiggle the IF
  /// slider, re-enable" used to be the workaround). The client half is this: send
  /// the settings again on every hwinfo, which covers a server restart, a reconnect
  /// and a fresh launch alike. Without it the radio comes back on defaults while the
  /// sheet still shows the positions you chose — controls and hardware quietly
  /// disagreeing, which is worse than losing them, because nothing looks wrong.
  ///
  /// ★ ORDER MATTERS. AGC LAST, and a manual gain-reduction only while AGC is OFF —
  /// otherwise the server correctly refuses it and the value vanishes in silence.
  func pushAllRadioSettings() {
    guard isVibe else { return }
    // ★★★ NEVER PUSH DEFAULTS WE INVENTED. This re-assert exists so a server restart
    // does not lose the settings the USER chose — but until they have chosen any, our
    // fields hold constructor defaults, and pushing those SETS THE RADIO to them.
    //
    // It did real damage: rspLna starts at 0 and the hardware wants a STATE where
    // higher = less gain, so the push sent lnaStates-1 — MINIMUM GAIN — and on an HF+
    // it sent lna:0, turning the PREAMP OFF. Re-asserted on every hwinfo, so every
    // reconnect quietly wrecked the owner's front end. RDS is the first casualty when
    // sensitivity drops, which is how it surfaced: "struggling on stations that
    // worked before" (Stuart, 2026-07-28).
    //
    // ★ The web client's contract is the same shape but has the precondition Jr
    // lacked: it pushes values the user picked, held in its own prefs. No choice, no
    // push. We only speak when we have something of the user's to say.
    guard radioDirty else { return }
    switch radioDriver {
    case "sdrplay":
      guard lnaStates > 0 else { return }
      rspSend(["lna": max(0, lnaStates - 1) - rspLna])
      if !rspIfAgc { rspSend(["ifgr": rspIfGr]) }
      rspSend(["ifagc": rspIfAgc ? 1 : 0])
      if radioHasRfNotch  { rspSend(["rfnotch": rspRfNotch ? 1 : 0]) }
      if radioHasDabNotch { rspSend(["dabnotch": rspDabNotch ? 1 : 0]) }
    case "airspyhf":
      if hasPreamp { ahfSend(["lna": ahfPreamp ? 1 : 0]) }
      if !ahfAgc, attSteps > 0 { ahfSend(["att": ahfAtt]) }
      if hasRadioAgc { ahfSend(["agc": ahfAgc ? 1 : 0]) }
    default: break
    }
  }

  /// Unlock the owner-protected controls with the admin password.
  ///
  /// ★ SAME HANDSHAKE AS THE PIN — GET /vibeserver/auth for a nonce, HMAC-SHA256 it
  /// with the password, send `admin_unlock`. The server answers with an `admin`
  /// message and re-advertises `adminOk` on the next config, so we do not guess at
  /// success: `adminOk` is whatever the SERVER says it is.
  func adminUnlock(_ password: String) {
    guard isVibe, !password.isEmpty else { return }
    Task { [weak self] in
      guard let self else { return }
      let httpScheme = secure ? "https" : "http"
      guard let url = URL(string: "\(httpScheme)://\(host)\(radioPath)/vibeserver/auth"),
            let (data, _) = try? await httpSession.data(from: url),
            let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let nonce = j["nonce"] as? String, !nonce.isEmpty else {
        await MainActor.run { self.status = "admin: can't reach server" }
        return
      }
      let mac = HMAC<SHA256>.authenticationCode(for: Data(nonce.utf8),
                                                using: SymmetricKey(data: Data(password.utf8)))
      let token = mac.map { String(format: "%02x", $0) }.joined()
      await MainActor.run {
        self.specSock.send(json: ["type": "admin_unlock", "nonce": nonce, "token": token])
      }
    }
  }
  func setAgc(_ on: Bool) { guard isVibe else { return }; agc = on; specSock.send(json: ["type": "agc", "on": on]); saveVibeHw() }
  func setPpm(_ v: Int) { guard isVibe else { return }; ppm = v; specSock.send(json: ["type": "ppm", "value": v]); saveVibeHw() }
  func setCaptureRate(_ hz: Int) { guard isVibe else { return }; sampleRate = hz; specSock.send(json: ["type": "sampleRate", "value": hz]); saveVibeHw() }
  /// ★★★ THE WIRE WANTS SECONDS, THE UI TALKS MICROSECONDS. `deemph` is 0/50/75 µs because that is
  ///   how the world labels de-emphasis and how the menu shows it — but the shim's handler is
  ///   explicit: "tau in SECONDS (0 = off, 50e-6 or 75e-6)". We were sending the bare 50, i.e.
  ///   asking for a FIFTY SECOND time constant: a one-pole lowpass a few millihertz up, which
  ///   strips everything audible and leaves DC.
  ///
  ///   ★★ It presented as "de-emphasis kills the audio", and it survived a restart because the
  ///   setting is persisted and re-sent on connect. OFF worked only because tau=0 bypasses the
  ///   filter entirely (pipeline.cpp: `useDeemph_ = (tau > 0.0)`), which made "off" look like the
  ///   fix rather than the diagnosis. ★ Nothing detected it: the server keeps sending audio
  ///   packets at the normal rate, so the queue never runs dry, the silence keeper stays quiet and
  ///   Now Playing keeps saying it is playing. Near-silence and silence are not the same thing to
  ///   any of our liveness checks.
  ///
  ///   The web client had it right all along (`f.setDeemph(d * 1e-6)`) — this is Jr's bug alone.
  func setDeemph(_ tau: Int) {
    guard isVibe else { return }
    deemph = tau
    specSock.send(json: ["type": "deemph", "tau": Double(tau) * 1e-6])
    saveVibeHw()
  }
  /// FFT frame rate — the primary adaptive-quality lever (the shim's `fftRate`).
  func setFftRate(_ fps: Int) { guard isVibe else { return }; specSock.send(json: ["type": "fftRate", "value": fps]) }
  /// Force mono — the ABR last resort (only meaningful on WFM).
  func setStereo(_ on: Bool) { guard isVibe else { return }; specSock.send(json: ["type": "stereo", "on": on]) }

  // ── The broadcast-FM treatments ───────────────────────────────────────────
  // ★ Optimistic locally so the button responds on the wrist immediately, then corrected by the
  //   next hwinfo — which is authoritative, because another listener may have changed it.
  func setFmNr(_ on: Bool)  { guard isVibe else { return }; fmNr = on;  specSock.send(json: ["type": "wsp", "on": on]) }
  func setFmIms(_ on: Bool) { guard isVibe else { return }; fmIms = on; specSock.send(json: ["type": "ims", "on": on]) }
  func setFmCeq(_ on: Bool) { guard isVibe else { return }; fmCeq = on; specSock.send(json: ["type": "ceq", "on": on]) }
  func setFmNb(_ on: Bool)  { guard isVibe else { return }; fmNb = on;  specSock.send(json: ["type": "nb", "on": on]) }

  // ── Published state (the UI mirrors this and nothing else) ────────────────
  @Published var status = "starting"
  /// ★★★ STICKY AUTH DIAGNOSTIC. `status` is owned by the reconnect ladder, which stamps
  /// "reconnecting" over it before every retry — and `SpikeLink.driverTick` only SAMPLES the
  /// status at 20fps, so a reason written and overwritten between two ticks is never even
  /// mirrored, let alone read. That is why the Series 6 showed no error code: the code was
  /// being set correctly and destroyed milliseconds later, every time. This field is written
  /// ONLY by resolveVibeAuth and cleared ONLY when the server actually answers, so it
  /// survives the churn and stays on screen where it can be read off the wrist.
  @Published var vibeDiag = ""
  @Published var frequency: Double = 648_000        // Radio Caroline
  @Published var mode = "am"

  /// RDS from VibeServer (WFM only), pushed on the spectrum socket as `{"type":"rds",...}` ~1/sec.
  /// `rdsPs` is the 8-char station name — surfaced via `stationName` into the band strip in place of
  /// the band label (exactly like Kiwi/FM-DX). ★ FLUSHED the instant the station is lost (server
  /// sends ps="") or we retune / change mode / switch server, so it can NEVER persist onto a
  /// different frequency or backend — the FM-DX stale-RDS bug Stuart called out.
  @Published private(set) var rdsPs = ""
  @Published private(set) var rdsText = ""
  var stationName: String { rdsPs }
  private func clearRds() { if !rdsPs.isEmpty || !rdsText.isEmpty { rdsPs = ""; rdsText = "" } }
  /// Passband edges as Hz offsets from the carrier (low negative = below). Mirrors the phone's
  /// MODE_BANDWIDTHS server defaults; the UI edits them and setBandwidth pushes them.
  @Published var bwLow: Double  = -5_000            // am default
  @Published var bwHigh: Double =  5_000

  /// Server-side per-mode defaults (websocket.go, verbatim — matches UberSDRClient.ts).
  static let modeBW: [String: (low: Double, high: Double)] = [
    "usb": (50, 2_700),      "lsb": (-2_700, -50),
    "am":  (-5_000, 5_000),  "sam": (-5_000, 5_000),
    "cwu": (-200, 200),      "cwl": (-200, 200),
    "fm":  (-6_000, 6_000),  "nfm": (-5_000, 5_000),
    "wfm": (-100_000, 100_000),
  ]
  @Published var binCount = 0
  @Published var binBandwidth: Double = 0
  @Published var centerHz: Double = 0
  @Published var audioRoute = "—"
  @Published var audioLive = false
  /// Signal readout from the spectrum DSP (near-free — see SignalProcessor). signalDb = SNR in
  /// dB (for the meter text), signalLevel = 0…1 fill for the bar behind the frequency pill.
  @Published var signalLevel: Double = 0
  @Published var signalDb: Double = 0
  /// radiod's per-packet CHANNEL SNR (dBFS, −30 corrected), the zoom-independent meter number. NaN
  /// until the first audio packet lands (then it takes over from the spectrum SNR). Written off-main
  /// in the audio handler, read on the spectrum tick — a benign Double race.
  private var chanSnr: Double = .nan
  /// radiod's raw baseband power in dBFS — the absolute level, used by the dBFS and S-unit meter
  /// units. NaN until the first audio packet. Same benign off-main write as chanSnr.
  var chanDbfs: Double = .nan
  /// Absolute signal level in dBFS for the meter text. NaN while we only have spectrum SNR (which
  /// is a RATIO, not a level — there is no honest dBFS to show then, so the readout falls back).
  @Published var signalDbfs: Double = .nan
  @Published var framesPerSec: Double = 0
  @Published var audioPerSec: Double = 0
  /// DEBUG: total incoming KB/s (spectrum + audio), for the on-wrist counter.
  @Published var kbps: Double = 0
  /// SDRClient — feeds the relay-load glyph. ★ The protocol DEFAULT is 0, which
  /// reads as "comfortable" rather than "unknown", so every client that does not
  /// override this is silently invisible to the bandwidth warning. OWRX was the
  /// only one that did. Covers UberSDR and VibeServer, both of which run through
  /// this client.
  var inboundKbPerSec: Int { Int(kbps.rounded()) }

  /// NONISOLATED, deliberately.
  ///
  /// This class is `@MainActor`, but SwiftUI's `Canvas` draw closure does NOT necessarily
  /// run on the main actor — so reading `client.waterfall` from inside it is an isolation
  /// violation, and it traps the instant frames start arriving. "It crashes when it tries
  /// to render" was exactly that, and it was in the code all along.
  ///
  /// The shipped watch app never hits it because `WatchLink` is a plain ObservableObject
  /// with no actor isolation. The buffer itself is built for this: rows go in from the data
  /// path, pixels come out on the render clock, and it has been doing that on the wrist for
  /// weeks.
  nonisolated(unsafe) let waterfall: WaterfallBuffer

  /// The waterfall buffer is INJECTED by `SpikeLink` so the adapter and the client share one
  /// instance — the processed rows the client pushes land in the exact buffer the UI draws.
  init(waterfall: WaterfallBuffer = WaterfallBuffer()) {
    self.waterfall = waterfall
  }

  /// A STABLE session id, persisted across launches.
  ///
  /// It was a fresh UUID every launch, and that is poison on a watch: watchOS suspends the
  /// app WITHOUT WARNING, so the sockets never close cleanly and the server keeps the
  /// session open. A new uuid next launch means a new session — and the old one is still
  /// sitting there. Six test runs later the server rejects you for being connected six
  /// times, and it is quite right to.
  ///
  /// Reusing one id means a relaunch RE-ATTACHES to the session it left behind instead of
  /// stacking another on top of it. JR needs this for exactly the same reason: a watch app
  /// does not get to run its teardown, so it must not depend on having done so.
  private var uuid: String = {
    let key = "wristsdr.session.uuid"
    if let s = UserDefaults.standard.string(forKey: key), !s.isEmpty { return s }
    let s = UUID().uuidString.lowercased()
    UserDefaults.standard.set(s, forKey: key)
    return s
  }()

  /// Once per launch, and only after a rejection.
  private var rotated = false
  private func rotateSession() -> Bool {
    guard !rotated else { return false }
    rotated = true
    uuid = UUID().uuidString.lowercased()
    UserDefaults.standard.set(uuid, forKey: "wristsdr.session.uuid")
    return true
  }
  // ★★ decodeQueue ONLY (see the spectrum path) — never main, so no lock is needed.
  nonisolated(unsafe) private let proc = SignalProcessor()
  nonisolated(unsafe) private let specDecodeQueue = DispatchQueue(label: "uber.spec")
  private let opus = OpusDecoder()
  private let audio = WatchAudio()

  /// BOTH sockets are Network framework. NEITHER is URLSession.
  ///
  /// Moving only the audio across was not enough: audio then ran perfectly at 50 packets/sec
  /// and the SPECTRUM — the one still on `URLSessionWebSocketTask` — failed with "Socket is
  /// not connected". The loser is not "the second one", it is "the URLSession one". Whatever
  /// watchOS does to a WebSocket task once another stream is live, it does it reliably, and
  /// no arrangement of separate URLSessions avoids it. NWConnection is simply the API that
  /// works here — which is, in the end, the same conclusion the PHONE reached in v5.1.2 when
  /// URLSessionWebSocketTask stalled its audio and we replaced it with NWConnection.
  private let specSock  = AudioSocket(name: "spec")
  private let audioSock = AudioSocket(name: "audio")

  /// ONE URLSession PER SOCKET.
  ///
  /// They shared a session, and the SECOND WebSocket to come up simply never connected —
  /// "Socket is not connected", with the first one running perfectly beside it. Audio at 26
  /// packets/sec and a spectrum socket that had never opened at all.
  ///
  /// The phone never hits this because it runs its audio through native NWConnection and
  /// only the spectrum through a WebSocket: one socket per stack, by accident. Two
  /// concurrent WebSocket tasks on one URLSession is the case nobody tests, and on watchOS
  /// it does not work. Worth knowing for JR, which needs exactly two.
  private lazy var audioSession = Self.makeSession()
  private lazy var httpSession  = Self.makeSession()

  private static func makeSession() -> URLSession {
    let c = URLSessionConfiguration.default
    c.waitsForConnectivity = true
    // ★ BOUND THE WAIT. waitsForConnectivity means a request with no link WAITS — and the default
    // resource timeout is SEVEN DAYS, so on resume (Wi-Fi still reassociating after bedtime/suspend)
    // the /vibeserver/auth GET could hang effectively forever and connect() sat on "authenticating"
    // with no way back. Cap it so a stalled auth FAILS in ~12s and the reconnect path can retry.
    c.timeoutIntervalForRequest = 8
    c.timeoutIntervalForResource = 12
    // The watch's own Wi-Fi/cellular — nothing is proxied through the phone.
    return URLSession(configuration: c)
  }

  /// The predicted view, exactly as the phone does it: gestures must move the picture NOW,
  /// not one round-trip later.
  private var viewCenterHz: Double = 0

  /// SPECTRUM ALIGNMENT ACROSS A RECONNECT.
  ///
  /// Every binary frame carries its own centre `freq`, so the centre is always right. The
  /// SPAN it is drawn at is `binBandwidth`, which arrives SEPARATELY in a `config` JSON
  /// message. On a fresh socket the two are briefly out of step: binary rows can land before
  /// the new config does, so they get painted at the PREVIOUS session's scale — signals show
  /// up in the wrong place until config catches up ("wrong, then snaps right").
  ///
  /// So gate the paint: bump `specSubscribeSeq` every time we (re)subscribe, stamp
  /// `specConfigSeq` when a config arrives, and don't paint a row until the two match — the
  /// scale we would draw with is the scale the server just confirmed. Frames are still
  /// COUNTED (the watchdog needs that); we simply hold the picture until it is trustworthy.
  private var specSubscribeSeq = 0
  private var specConfigSeq = -1
  /// Fail-open: if the server never volunteers a config, don't blank forever. After this many
  /// gated frames (~2s at 10fps) accept the scale we have and paint.
  private var gatedFrames = 0
  private var viewBinBw: Double = 0

  private var frameCount = 0
  private var audioCount = 0
  private var specBytes  = 0        // DEBUG byte tallies for kbps
  private var audioBytes = 0
  private var rateTimer: Timer?

  // ── Lifecycle ─────────────────────────────────────────────────────────────

  func start() {
    // The LUT (Sonar Green) is set by SpikeLink on the shared buffer before start().
    waterfall.contrast = 0          // the DSP does the contrast; the buffer just paints
    waterfall.brightness = 0
    proc.autoContrast = 5           // 10 (UberSDR's own) crushes the noise floor to black

    // .common mode. `Timer.scheduledTimer` installs in DEFAULT mode, and while you are
    // turning the crown the run loop is in TRACKING mode — where default-mode timers do not
    // fire at all. The counters would freeze exactly when you were interacting, which is
    // exactly when you want to read them.
    let t = Timer(timeInterval: 1, repeats: true) { [weak self] _ in
      Task { @MainActor in
        guard let self else { return }
        self.framesPerSec = Double(self.frameCount)
        self.audioPerSec  = Double(self.audioCount)
        self.kbps = Double(self.specBytes + self.audioBytes) / 1024.0
        self.specBytes = 0; self.audioBytes = 0
        let hadAudioThisSec = self.audioCount > 0
        let hadSpectrumThisSec = self.frameCount > 0
        self.frameCount = 0
        self.audioCount = 0

        // AUDIO LIVENESS WATCHDOG (Stuart, 2026-07-21). The spectrum self-heals via
        // reconnectIfNeeded/retrySpectrum, but the audio socket only recovered on an EXPLICIT
        // failed/recv: state. A SILENT audio death — bytes just stop while the socket still looks
        // open — left the waterfall running and the audio gone for good ("faded out, server rough,
        // never came back"). The health signal was spectrum frames, so a live waterfall masked a dead
        // audio path entirely. Fix: if audio was ever flowing and the link is otherwise ALIVE (spectrum
        // frames still arriving, proving server + connection are fine), yet no audio packet has landed
        // for a few seconds, reopen it — re-arming every 5s until it comes home.
        if hadAudioThisSec {
          self.lastAudioAt = Date()
          self.everHadAudio = true
          self.audioRetries = 0          // healthy again — reset the backoff
        }
        if self.everHadAudio, self.status == "live", hadSpectrumThisSec,
           Date().timeIntervalSince(self.lastAudioAt) > 5 {
          self.audioWsState = "audio silent — recovering"
          self.lastAudioAt = Date()      // re-arm: try again in another 5s if still silent
          self.retryAudio()
        }

        self.stepLinkManagement()
      }
    }
    RunLoop.main.add(t, forMode: .common)
    rateTimer = t

    startPathMonitor()
    Task { await connect() }
  }

  // ── Network path → transport glyph ONLY ──────────────────────────────────────
  //
  // We watch the path solely to tell the UI what the watch is connected THROUGH (wifi /
  // cellular / iPhone relay) for the status glyph. It deliberately drives NO recovery.
  //
  // An earlier version rebuilt the spectrum socket on interface changes to fix the rare "walk
  // out of the house on wifi" stuck case — but that risked disturbing a connection that was
  // otherwise coping, and the field test WITHOUT it was already near-perfect. So recovery is
  // left entirely to the socket's own failed→retry, the ready-but-silent watchdog and
  // resumeSpectrum (which worked great), and the wifi→cellular launch edge case is handled by a
  // simple close-and-reopen. (Stuart's call, 2026-07-17: revert to what worked.)
  private let pathMonitor = NWPathMonitor()
  private let pathQueue = DispatchQueue(label: "wristsdr.path")

  /// How the watch is reaching the server — for the connection-method glyph (SpikeLink mirrors
  /// this). Updated on every path callback, including the first, so the glyph is right from the start.
  @Published var transport: Transport = .none

  private func startPathMonitor() {
    pathMonitor.pathUpdateHandler = { [weak self] path in
      let tr = UberClient.transportFor(path)
      Task { @MainActor in
        guard let self else { return }
        if self.transport != tr { self.transport = tr }
      }
    }
    pathMonitor.start(queue: pathQueue)
  }

  /// `.other` = the paired-iPhone Bluetooth relay on watchOS (Apple TN3135). Heuristic, kept in
  /// this ONE place. No 4G/5G split — CoreTelephony RAT isn't reliable on watchOS and the glyph
  /// only needs "on the watch's own cellular".
  nonisolated private static func transportFor(_ p: NWPath) -> Transport {
    guard p.status == .satisfied else { return .none }
    // ★★ `.other` FIRST — CORRECTED 2026-07-26 against direct evidence, and this
    // file previously claimed the opposite order was field-verified.
    //
    // Proof: with Jr streaming from a VibeServer on the Mac, the SERVER's own
    // peer address was the iPhone's (192.168.86.245 = stuey3ds-iphone.lan), not
    // the watch's — so every byte was relayed, while this function was returning
    // .wifi. The companion bridge advertises BOTH .other AND .wifi, so a
    // .wifi-first test reports WiFi whenever the phone is merely NEARBY.
    //
    // SpikeLink.startPathMonitor() has always had it this way round, so the two
    // monitors in this app disagreed and the glyph (SpikeLink) was right while
    // this one was wrong. ★ When two code paths answer the same question
    // differently, the tie-breaker is an observation from OUTSIDE both of them —
    // here, who the server says is talking to it.
    if p.usesInterfaceType(.other)    { return .iphone }
    if p.usesInterfaceType(.wifi)     { return .wifi }
    if p.usesInterfaceType(.cellular) { return .cellular }
    return .none
  }

  /// The listener has picked a radio behind the front door — take it and connect.
  ///
  /// ★ Clearing the list is what dismisses the chooser, so it happens FIRST: leaving it up while
  ///   the sockets open would let a second tap choose a different radio mid-connect.
  func chooseRadio(_ r: VibeRadio) {
    radioPath = "/r/\(r.id)"
    radioChoices = []
    Task { await connect() }
  }

  private func connect() async {
    guard !goingIdle else { return }   // discarded mid-connect (server switch) — don't open anything

    // ★★★ WHICH RADIO — BEFORE ANY SOCKET IS OPENED. Resolving after the fact would mean every
    //     connection to a multi-radio server began with a failed handshake against the door, and
    //     the reconnect logic would then race the resolution. It is done HERE, in the one place
    //     the connection is actually made, rather than in the picker: Jr reaches a server from
    //     several directions (a favourite, the last server on launch, the phone handing one over)
    //     and a check placed on one of those paths silently misses the rest.
    // ★ Only when we have not already chosen. A reconnect keeps the radio it was using — being
    //   thrown back to a chooser because the link blipped would be its own bug.
    if isVibe, radioPath.isEmpty, !goingIdle {
        if let door = await FrontDoor.probe(host: host, tls: secure) {
            if door.radios.count == 1 {
                // ★★ A LIST OF ONE IS NOT A CHOICE. A single-radio V3 still needs the prefix, so
                //    it is resolved silently and the watch shows what it always did.
                radioPath = "/r/\(door.radios[0].id)"
            } else {
                await MainActor.run {
                    radioChoiceName = door.name
                    radioChoices = door.radios
                    status = "choose a radio"
                }
                return          // ContentView offers the list; picking one calls chooseRadio()
            }
        }
    }

    // ★★★ A NEW STREAM GETS A CLEAN PROCESSOR. Without this the waterfall's derived range
    // outlived the connection that produced it, so a run of corrupt frames on a marginal link
    // left a black waterfall that RECONNECTING COULD NOT FIX — only force-quitting the app.
    // ★ Stuart saw it on VibeServer and reports the same on UberSDR, which fits: both paths
    //   share this one processor instance.
    specDecodeQueue.async { self.proc.reset() }

    // VibeServer: resolve the PIN handshake up front (GET /vibeserver/auth → nonce → HMAC), then open the
    // sockets with the auth suffix. No UberSDR /connection preflight — the shim answers that unconditionally
    // and doesn't need it. The nonce is a reusable 1-hour session credential shared by both WS.
    if isVibe {
      restoreVibeState()          // reopen where we left it (per host); else adopt the server's tune on config
      status = "authenticating"
      // ★ RESUME-SAFE. On a reconnect after suspend the first auth can fail transiently — the link is
      // still coming back, DNS/route not settled. A single return left "reconnecting/authenticating"
      // stuck on screen with no recovery until a force-quit. Retry a couple of times with a short
      // backoff before giving up; the bounded session timeout (makeSession) keeps each try honest.
      var ok = await resolveVibeAuth()
      var tries = 0
      // ★ needsPin is NOT a transient failure — retrying cannot fix a PIN we do not
      // have, and each attempt walks the server closer to locking us out. Stop and
      // let the UI ask.
      while !ok, !needsPin, tries < 2, !goingIdle {
        tries += 1
        try? await Task.sleep(nanoseconds: 1_500_000_000)
        guard !goingIdle else { return }
        status = "reconnecting"
        ok = await resolveVibeAuth()
      }
      if !ok { return }           // status carries the reason (PIN wrong / locked / offline)
      // ★ ASK WHETHER THE SEAT IS FREE before taking it. A busy server refuses the
      // socket, and a client that just retries hammers a receiver somebody else is
      // using — while showing its own user nothing but "reconnecting". /vibeserver.json
      // says both whether it is busy AND whether a takeover is even possible.
      if adminSuffix.isEmpty, await vibeIsBusy() {
        serverBusy = true
        status = "in use"
        return
      }
      openVibeSockets()
      return
    }

    status = "registering"
    if !(await postConnection()) {
      // DO NOT overwrite the reason. postConnection() already put the SERVER'S OWN words in
      // `status`, and clobbering them with "REJECTED by server" threw away the only piece of
      // evidence there was.
      //
      // One retry, on a FRESH session id. The stable id is right — it stops a suspended app
      // stacking a new session on top of the one it abandoned — but it has a failure mode:
      // if the server is still holding that id open, we are locked out of our own session
      // forever, and no relaunch can fix it. Rotating once turns a permanent lockout into a
      // single bad connect.
      let old = status
      guard rotateSession() else { return }
      status = "retrying (fresh session) · was: \(old)"
      if !(await postConnection()) { return }
    }

    // AUDIO FIRST, THEN SPECTRUM WHEN THE AUDIO SOCKET SAYS IT IS UP.
    //
    // This used to be two guessed sleeps — 1.5s for the session to register, 3s for the rate
    // limiter — and guessed sleeps are why the connection was a lottery. Both numbers were
    // invented, neither was observable, and when either was wrong the socket died.
    //
    // The socket already knows. It reports `.ready` the moment the handshake completes, so
    // the spectrum socket now waits for THAT, not for a clock. The remaining 1s is the one
    // delay that is real and documented: UberSDR rate-limits new WebSockets to 2/sec per IP.
    //
    // Order matters and is not arbitrary: the audio socket is the DURABLE one — it survives
    // lock and background, and it is the only one the user can hear die. The spectrum socket
    // is disposable by design; we already drop and reopen it on every pause/resume. So audio
    // gets the clean slot, and the spectrum goes second because it is the one we know how to
    // lose cheaply.
    // RESET THE GATE. `specOpened` is a one-shot guard, and connect() runs more than once —
    // the retry path calls it, the wrist-up reconnect calls it. Leaving it latched from a
    // previous attempt meant the callback returned early and openSpectrum() was NEVER CALLED
    // AT ALL on any connect after the first. Not "failed": never attempted. That is what an
    // empty `S: —` on screen was telling us, and it is the whole reason the waterfall was
    // missing while the audio was fine.
    specOpened = false

    audioSock.onReady = { [weak self] in
      Task { @MainActor in
        guard let self, !self.specOpened else { return }
        self.specOpened = true
        try? await Task.sleep(nanoseconds: 1_000_000_000)   // the server's 2/sec limit
        self.openSpectrum()
      }
    }
    openAudio()

    // AND NEVER LET THE SPECTRUM BE HOSTAGE TO THE AUDIO. Chaining the waterfall off the
    // audio socket's handshake is right when the audio comes up — but if it never does, the
    // waterfall must not be punished for it. Open it anyway.
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: 8_000_000_000)
      guard !self.specOpened else { return }
      self.specOpened = true
      self.specWsState = "spec: audio never readied — opening anyway"
      self.openSpectrum()
    }

    audio.start { [weak self] ok, info in
      Task { @MainActor in
        self?.audioLive = ok
        self?.audioRoute = ok ? info : "FAILED: \(info)"
      }
    }
    status = "live"
  }

  private var specOpened = false

  private func postConnection() async -> Bool {
    // ★★★ HONOUR THE SCHEME. This was hardcoded `https://` while every other call site in
    //   this file does `secure ? "https" : "http"` — so a LOCAL UberSDR served over plain
    //   HTTP got a TLS handshake against a non-TLS port and could never connect. Public
    //   UberSDR instances are all https, which is why it was invisible for so long.
    //   ★ Found 2026-07-29 by a test of Stuart's: on a Series 6, OWRX connected by LAN
    //     address and UberSDR did not — which isolated it to this client rather than to
    //     the network, the watch or watchOS.
    let httpScheme = secure ? "https" : "http"
    guard let cu = URL(string: "\(httpScheme)://\(host)\(radioPath)/connection") else {
      status = "bad server URL"; return false
    }
    var req = URLRequest(url: cu)
    req.httpMethod = "POST"
    req.setValue("application/json", forHTTPHeaderField: "Content-Type")
    // The same headers the phone sends. It is not obvious that the server cares — but a
    // rejection with no reason and an unfamiliar User-Agent is not the moment to be
    // different for the sake of it.
    // ★ UberSDR is the one backend where we announce the PRODUCT rather than spoofing a
    //   browser, so this string is what an operator actually sees in their user list. The spike
    //   is the product now, and it is called VibeSDR Jr.
    req.setValue(jrUserAgent, forHTTPHeaderField: "User-Agent")
    req.setValue("VibeSDR", forHTTPHeaderField: "X-Requested-With")
    req.httpBody = try? JSONSerialization.data(withJSONObject: ["user_session_id": uuid])
    do {
      let (data, resp) = try await httpSession.data(for: req)
      guard let http = resp as? HTTPURLResponse else {
        status = "no HTTP response"
        return false
      }
      let obj = try? JSONSerialization.jsonObject(with: data)
      let j = obj as? [String: Any]
      let allowed = (j?["allowed"] as? Bool) ?? false
      if !allowed {
        // THE SERVER SAID WHY. Throwing that away and printing "REJECTED" was the single
        // most useless thing this app could have done — the answer was in the response
        // body the whole time.
        let reason = (j?["reason"] as? String)
          ?? String(data: data.prefix(80), encoding: .utf8)
          ?? "no reason given"
        status = "HTTP \(http.statusCode): \(reason)"
      }
      return allowed
    } catch {
      status = "connection failed: \(error.localizedDescription)"
      return false
    }
  }

  // ── Spectrum ──────────────────────────────────────────────────────────────

  /// Bumped on every (re)open so a stale ready-but-silent watchdog from a superseded socket
  /// bails instead of tearing down the fresh one.
  private var specOpenSeq = 0

  private func openSpectrum() {
    guard !goingIdle else { return }   // never reopen a torn-down client (server switch)
    specOpenSeq &+= 1
    let seq = specOpenSeq
    // FFT/BIN lever (VibeServer only): ask for exactly our waterfall width instead of the server's
    // full 4096. The server keeps its full sample rate and peak-holds its fine FFT down to this many
    // bins as it crops for zoom — so zoomed out is coarse (UberSDR-style), zoomed in sharpens, and
    // the wire cost is a flat ~128 bins/frame at every zoom. Cuts each SPEC frame ~32x. UberSDR
    // ignores the param (it sends its own count, which we downsample as before).
    let binsParam = isVibe ? "&bins=\(WaterfallBuffer.width)" : ""
    // ★★ NAMED IN THE QUERY TOO. The header is set as well, but a platform may own User-Agent on a
    //    WebSocket upgrade — and when it does, the owner's connection log shows "—" for us. The
    //    server prefers a real header and falls back to this.
    let url = URL(string: "\(scheme)://\(host)\(radioPath)/ws/user-spectrum?user_session_id=\(uuid)&mode=binary8\(binsParam)\(authSuffix)\(adminSuffix)&client=\(jrUserAgent.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? "VibeSDR-Jr")")!

    specSock.onData = { [weak self] d in
      Task { @MainActor in self?.onSpectrumBinary(d) }
    }
    specSock.onText = { [weak self] t in
      Task { @MainActor in self?.onSpectrumJSON(Data(t.utf8)) }
    }
    specSock.onState = { [weak self] st in
      Task { @MainActor in
        guard let self else { return }
        self.specWsState = st
        if st.contains("failed") || st.contains("recv:") { self.retrySpectrum() }
      }
    }
    // SUBSCRIBE ONLY ONCE THE SOCKET IS ACTUALLY UP.
    //
    // This used to fire the instant `open()` was called — before the handshake had finished.
    // `NWConnection.send()` on a connection that is not yet `.ready` does not reliably queue;
    // the subscribe simply evaporated. The socket then opened beautifully, the server never
    // learned which band we wanted, and it sent nothing. Forever.
    //
    // And because the socket had not FAILED, the retry never fired — a silent socket looked
    // exactly like a healthy one. That is the same shape as the frozen-waterfall bug v9 just
    // fixed on the phone, and it is worth saying plainly: an open socket is not a working
    // socket, and only FRAMES prove it.
    specSock.onReady = { [weak self] in
      Task { @MainActor in
        guard let self, self.specOpenSeq == seq else { return }
        self.sendView(self.frequency, self.viewBinBw > 0 ? self.viewBinBw : 100)
        self.armSpectrumWatchdog()
      }
    }
    // ★★ SAY WHO WE ARE. The server logs the User-Agent of the WS upgrade, and without one Jr's
    //    sessions appeared in the owner's connection log as "—" — indistinguishable from a bot or
    //    a bare script, next to browsers that name themselves in full (Stuart, 2026-08-13). An
    //    owner deciding whether to BLOCK an address is exactly who needs to know it is our app.
    //    ★ The /connection POST has always sent it; the SOCKET, which is what the log records,
    //      never did.
    specSock.open(url: url, headers: [("User-Agent", jrUserAgent)])
  }

  /// FRAMES OR IT DIDN'T HAPPEN. Ready and silent is a real state, and it needs its own way
  /// out — first re-ask for the band, then tear the socket down and start again.
  private func armSpectrumWatchdog() {
    let n = frameCount
    let seq = specOpenSeq
    // "Initialised" = BOTH the binary frames flowing AND a config JSON confirmed for THIS
    // subscription. They arrive as separate messages: if the config is lost but the binary
    // frames aren't, the paint gate fails open and draws with a zero/stale span — a waterfall
    // that never scales, while frameCount happily ticks. Checking frames alone let that slide
    // through forever (audio fine, waterfall dead-on-arrival). Require the config too.
    func healthy() -> Bool { frameCount != n && specConfigSeq == specSubscribeSeq }
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: 5_000_000_000)
      guard !self.goingIdle, self.specOpenSeq == seq else { return }   // torn down / newer socket supersedes
      guard !healthy() else { return }                       // frames AND config — all well
      self.specWsState = "spec ready but not INITIALISED · re-subscribing"
      self.sendView(self.frequency, self.viewBinBw > 0 ? self.viewBinBw : 100)

      try? await Task.sleep(nanoseconds: 5_000_000_000)
      guard !self.goingIdle, self.specOpenSeq == seq else { return }
      guard !healthy() else { return }
      self.specWsState = "spec not INITIALISED · reopening"
      self.specSock.cancel()
      self.framesPerSec = 0    // we just cancelled — let retrySpectrum's `framesPerSec == 0` guard pass
                               // even though binary frames WERE flowing (only the config was missing)
      self.retrySpectrum()
    }
  }

  /// What the sockets are actually doing. On screen, because a spike that cannot tell you
  /// why it is empty is not a spike, it is a mystery.
  @Published var wsDiag = ""
  @Published var specWsState = ""

  /// WRIST DOWN KILLS THE SOCKETS, and nothing was watching.
  ///
  /// watchOS suspends the app when the screen sleeps; the WebSockets die with it, and on
  /// wake there is nothing to bring them back — so the waterfall stopped and stayed
  /// stopped. This is the SAME class of bug V9 just fixed on the phone (a spectrum socket
  /// with no watchdog), and finding it here within minutes says something: it is not an
  /// UberSDR quirk, it is what happens to any long-lived socket on a device that sleeps.
  ///
  /// JR would need the same starvation watchdog the phone now has. Noting it as a real
  /// cost, not papering over it — the spike just needs to survive long enough to measure.
  /// Going away — say so, rather than being killed and leaving the server holding a socket
  /// it thinks is alive. Best effort: watchOS may suspend us before this ever runs, which
  /// is precisely why the session uuid above is stable and not fresh each launch.
  func suspend() {
    // DROP THE SPECTRUM. KEEP THE AUDIO. This is the whole point of background audio, and we
    // were defeating it ourselves: suspend() used to cancel BOTH sockets, so the moment the
    // wrist dropped the app shut the audio off — and then we went looking for the watchOS
    // setting that would keep it alive. No entitlement can save audio the app itself kills.
    //
    // The asymmetry is the same one the phone already lives by: nobody is looking at the
    // waterfall with the screen off, so the spectrum socket is pure cost and goes. The audio
    // is the reason the app is still running at all, so it stays — that is what
    // WKBackgroundModes=[audio] and .longFormAudio are FOR.
    specSock.cancel()
    specOpened = false
    // Drop the delayed rows too — on resume we start fresh, and a second of pre-pause
    // spectrum flashing up before the live feed catches on would be worse than the brief
    // "syncing" refill.
    specQueue.removeAll()
    spectrumSyncing = true
    status = "background · audio only"
  }

  /// The app is really going away (not just wrist-down) — let the server go too, rather than
  /// leaving it holding a socket it believes is alive.
  func teardown() {
    pathMonitor.cancel()
    specSock.cancel()
    audioSock.cancel()
    audio.stop()
  }

  /// Go quiet without dying — the user backed out to the instance picker. Drop both sockets and
  /// the audio, but leave the once-only timers/path monitor alone so a later reconnect is cheap.
  func goIdle() {
    // TEAR DOWN FOR GOOD — this client is being discarded (server switch / back to picker). Without the
    // goingIdle latch the retry Tasks (which pass their `framesPerSec == 0` guard precisely BECAUSE we
    // zero it here) reopen the sockets, so the old client keeps reconnecting and pegs the CPU while the
    // NEXT server starts on top of it (the "2nd server 93% hang"). Latch it and kill the once-only timers.
    goingIdle = true
    rateTimer?.invalidate(); rateTimer = nil
    pathMonitor.cancel()
    specSock.cancel()
    audioSock.cancel()
    audio.stop()
    specOpened = false
    framesPerSec = 0
    clearRds()            // never carry a station name across to the next server
    status = "idle"
  }
  private var goingIdle = false

  /// Point at a DIFFERENT server and connect fresh (from the picker). `start()` did the one-time
  /// setup; this reuses it. Safe because connect() is built to run repeatedly (retry/wrist-up).
  func reconnect(host newHost: String) {
    host = newHost
    specSock.cancel()
    audioSock.cancel()
    audio.stop()
    specOpened = false
    clearRds()               // different server — start with no station name
    _ = rotateSession()      // fresh session id for the new server
    Task { await connect() }
  }

  /// Wrist back up: the audio never stopped, so only the waterfall needs bringing home.
  /// Foreground recovery — ONE entry for every "came back to the app" case, curing the slow
  /// spectrum restore. Resets the retry backoff (so we never inherit the 2→10 s max built up during
  /// an erratic session), then brings back ONLY what actually died: nothing if frames are still
  /// flowing (a quick glance), the spectrum socket ALONE if audio is warm (the common case — fast,
  /// one RTT, the pixels stay on screen), or a full reconnect if the whole pipe went. Replaces the
  /// old resumeSpectrum-vs-reconnectIfNeeded branch, which after a LONGER background left the status
  /// in limbo — neither path fired promptly and recovery fell to the slow accumulated retry backoff
  /// (the "eternity", 2026-07-23).
  func wake() {
    guard !goingIdle, everHadFrames else { return }   // never got going — the initial connect owns it
    specRetries = 0                                     // ★ fresh, immediate backoff — not the 10 s max
    if framesPerSec > 0 { return }                      // spectrum still flowing — the glance survived
    if audioPerSec == 0 {
      // Whole pipe died (a long suspend killed audio too) — full reconnect. Fast: the session uuid
      // is stable, so the server re-attaches us rather than cold-starting.
      status = "reconnecting"
      specSock.cancel(); audioSock.cancel()
      Task { await connect() }
    } else {
      // Audio is warm, only the spectrum socket died → reopen JUST the spectrum, NOW.
      if status.hasPrefix("background") { status = "live" }
      // ★ The reopened spectrum socket dumps a burst that briefly starves audio over the weak watch
      // link → a stutter (VibeServer only — UberSDR's audio is an independent server channel). Give
      // audio a slightly bigger cushion to ride it out. One-shot: the next tune flushes it, so tuning
      // stays snappy. 0.6s vs the usual 0.35s — deliberately modest to keep listening latency low.
      if isVibe { audio.primeCushion(0.6) }
      specOpened = true
      specSock.cancel()
      openSpectrum()
    }
  }

  func resumeSpectrum() {
    guard status.hasPrefix("background") else { return }
    status = "live"
    specRetries = 0
    specOpened = true
    openSpectrum()
  }

  func reconnectIfNeeded() {
    // ONLY IF IT WAS EVER WORKING, and only if it has been dead for a while.
    //
    // Without those two guards this kills the connection it exists to protect: scenePhase
    // fires `.active` the moment the view appears, when fps is legitimately 0 because
    // nothing has arrived YET — and it cancelled the sockets it had just opened. A
    // watchdog that cannot tell "not started" from "died" is worse than no watchdog.
    guard everHadFrames, status == "live" else { return }
    guard Date().timeIntervalSince(lastFrameAt) > 3 else { return }
    status = "reconnecting"
    specSock.cancel()
    audioSock.cancel()
    Task { await connect() }
  }
  private var everHadFrames = false
  private var lastFrameAt = Date.distantPast
  // Audio-liveness watchdog state (see the rateTimer). lastAudioAt tracks the last second in which
  // an audio packet arrived; everHadAudio gates the watchdog so it never fires before audio has begun.
  private var everHadAudio = false
  private var lastAudioAt = Date.distantPast

  /// UberSDR sends its JSON config as a GZIPPED BINARY frame, not a text frame — the magic
  /// bytes are the only way to tell it from a spectrum frame. (The web client sniffs for
  /// exactly this before reaching for DecompressionStream.)
  private func onSpectrumBinary(_ d: Data) {
    if d.count >= 2, d[0] == 0x1f, d[1] == 0x8b {
      if let un = Gzip.inflate(d) { onSpectrumJSON(un) }
      return
    }
    guard d.count >= 22 else { return }

    let magic: UInt32 = d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt32.self) }
    guard magic == 0x4345_5053 else { return }   // "SPEC" little-endian

    // COUNT IT HERE. It used to be counted after the `flags` switch — which `return`s early
    // on frame types we don't decode — so a working feed could report 0 fps and the whole
    // measurement was of nothing at all. Count the frame when the frame ARRIVES; whether we
    // like its format is a separate question.
    frameCount += 1
    specBytes += d.count
    everHadFrames = true
    lastFrameAt = Date()

    let flags = d[5]
    let lo: UInt32 = d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 14, as: UInt32.self) }
    let hi: UInt32 = d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 18, as: UInt32.self) }
    let freq = Double(lo) + Double(hi) * 4_294_967_296

    let body = d.subdata(in: 22..<d.count)

    // binary8: uint8 = clamp(dBFS,-256,0)+256  →  dBFS = uint8 - 256
    switch flags {
    case 0x03:                                     // full uint8
      if bins.count != body.count { bins = [Float](repeating: -120, count: body.count) }
      body.withUnsafeBytes { raw in
        let p = raw.bindMemory(to: UInt8.self)
        for i in 0..<body.count { bins[i] = Float(Int(p[i]) - 256) }
      }
    case 0x04:                                     // delta uint8
      guard body.count >= 2 else { return }
      let changes: UInt16 = body.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt16.self) }
      var off = 2
      for _ in 0..<Int(changes) {
        guard off + 3 <= body.count else { break }
        let idx: UInt16 = body.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: off, as: UInt16.self) }
        let val = body[body.startIndex + off + 2]
        off += 3
        if Int(idx) < bins.count { bins[Int(idx)] = Float(Int(val) - 256) }
      }
    case 0x01:                                     // full float32
      let n = body.count / 4
      if bins.count != n { bins = [Float](repeating: -120, count: n) }
      body.withUnsafeBytes { raw in
        for i in 0..<n { bins[i] = raw.loadUnaligned(fromByteOffset: i * 4, as: Float32.self) }
      }
    case 0x02:                                     // delta float32
      guard body.count >= 2 else { return }
      let changes: UInt16 = body.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt16.self) }
      var off = 2
      for _ in 0..<Int(changes) {
        guard off + 6 <= body.count else { break }
        let idx: UInt16 = body.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: off, as: UInt16.self) }
        let v: Float32 = body.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: off + 2, as: Float32.self) }
        off += 6
        if Int(idx) < bins.count { bins[Int(idx)] = v }
      }
    default:
      // Say so, loudly. An unhandled frame type used to be a silent `return` — the frame
      // counter ticked up and the waterfall stayed black, which looks like a render bug and
      // is a protocol bug.
      unknownFlags = flags
      return
    }

    centerHz = freq

    // GATE ON A FRESH CONFIG. Until the server has confirmed the scale for THIS subscription,
    // any row we draw would use a span left over from the last one — the reconnect-misalignment
    // bug. Hold the paint (frames are already counted above, so the watchdog still sees life).
    // Fail open after ~2s so a server that never re-sends config can't blank us forever.
    if specConfigSeq != specSubscribeSeq {
      gatedFrames += 1
      if gatedFrames < 20 { return }
      specConfigSeq = specSubscribeSeq          // give up waiting; draw with what we have
    }
    gatedFrames = 0

    // ── THE COST JR PAYS. Unwrap, then the full DSP, then the paint. Every frame.
    let n = bins.count
    guard n > 1 else { return }
    if unwrapped.count != n { unwrapped = [Float](repeating: 0, count: n) }
    let half = n / 2
    // radiod sends [DC→+Nyquist, −Nyquist→DC]; the display wants [negative, positive].
    // Without this every signal is drawn half a span from where it actually is.
    for i in 0..<half { unwrapped[i] = bins[half + i] }
    for i in 0..<half { unwrapped[half + i] = bins[i] }

    // ★★★ THE DSP RUNS OFF MAIN. It used to run here, on the main actor, every frame — the
    // histogram, the 5-tap smooth, the normalise/clip/stretch and the decimate. On a watch that
    // competes with UI rendering and keeps a core awake that could otherwise idle, so it costs
    // responsiveness AND battery (Stuart, 2026-07-28: "we need to optimise for CPU and Power
    // consumption"). Main now only hands over a buffer and takes back a finished row.
    // ★ `proc`, `out256` and the working copy live on specDecodeQueue and are touched nowhere else.
    let work = unwrapped
    let centre = freq, span = binBandwidth * Double(n), packetSnr = chanSnr, packetDbfs = chanDbfs
    specDecodeQueue.async { [weak self] in
      guard let self else { return }
      let row = self.proc.process(work, centerHz: centre, bwHz: span)
      let dec = self.decimate(row, to: WaterfallBuffer.width)
      let lvl = self.proc.level, snr = self.proc.snrDb
      Task { @MainActor in
        self.signalLevel = lvl      // SNR bar — computed for free inside process()
        // Meter SNR from radiod's per-packet CHANNEL SNR (zoom-independent, the true number, same
        // as the phone) when we have it; fall back to the spectrum SNR only until the first audio
        // packet lands.
        self.signalDb   = packetSnr.isNaN ? snr : packetSnr
        self.signalDbfs = packetDbfs
        // WaterfallBuffer DROPS rows that aren't exactly its width, silently. A blank waterfall
        // with a healthy frame count is exactly what that looks like.
        // DELAY THE ROW instead of drawing it now. The audio runs a ~1s cushion for stability,
        // so a live spectrum sits ~1s AHEAD of the sound — you see a signal, then hear it a beat
        // later, which with a trace on screen is glaring. Hold each row the same ~1s and the two
        // line up. Drained on the main actor from the render tick (see drainSpectrum).
        if dec.count == WaterfallBuffer.width {
          self.rowsPushed += 1
          self.specQueue.append((ProcessInfo.processInfo.systemUptime, dec))
        }
      }
    }
  }

  // ── Spectrum delay (audio-sync) ─────────────────────────────────────────────
  private var specQueue: [(t: Double, row: [UInt8])] = []
  private var lastSpecPush: Double = 0
  /// Match the audio cushion (WatchAudio.targetQueued), less the spectrum's own inherent
  /// latency (~0.20s), so signal and sound arrive together. COUPLED to targetQueued: when the
  /// cushion moves, this must move with it or the waterfall and audio de-sync.
  /// Dialled in by ear against the buzzer (UVB-76). The audio cushion breathes a little with
  /// network jitter, so no fixed delay is always perfect — this minimises the average error.
  /// LOWERED 0.35 -> 0.15 (2026-07-17) tracking targetQueued 0.55 -> 0.35 (same 0.20 gap) —
  /// see the WatchAudio.targetQueued note for why the cushion shrank.
  private let spectrumDelay: Double = 0.15
  /// True while the delay buffer is refilling after a resume — nothing old enough to draw
  /// yet, so the UI says "syncing" rather than showing a frozen picture.
  @Published var spectrumSyncing = false

  /// Push every row that has now waited out the delay. MUST be called on the main actor
  /// (the WaterfallBuffer is drawn from a non-isolated Canvas closure — pushing from there
  /// would trap). ContentView calls this from its 20fps driver tick.
  func drainSpectrum(now: Double) {
    while let first = specQueue.first, now - first.t >= spectrumDelay {
      waterfall.push(row: first.row)
      specQueue.removeFirst()
      lastSpecPush = now
    }
    // Refilling after a pause: rows are arriving but none has aged in yet, and we haven't
    // drawn for a moment. Say so.
    spectrumSyncing = !specQueue.isEmpty && (now - lastSpecPush) > 0.3
  }

  /// UberSDR sends 1024 bins; the watch draws 256. So three quarters of every frame is
  /// received, DSP'd, and then thrown away — which is the concrete argument for making
  /// `binCount` requestable in the VibeServer protocol rather than inherited.
  ///
  /// PEAK, not mean. Averaging four bins together buries a narrow carrier in the noise
  /// beside it — the signal you are hunting is exactly the one a mean would erase.
  nonisolated private func decimate(_ row: [UInt8], to width: Int) -> [UInt8] {
    let n = row.count
    if n == width { return row }
    guard n > 0 else { return [] }
    if out.count != width { out = [UInt8](repeating: 0, count: width) }
    let ratio = Double(n) / Double(width)
    for i in 0..<width {
      let lo = Int(Double(i) * ratio)
      let hi = min(n, max(lo + 1, Int(Double(i + 1) * ratio)))
      var m: UInt8 = 0
      for k in lo..<hi where row[k] > m { m = row[k] }
      out[i] = m
    }
    return out
  }

  private var bins: [Float] = []
  private var unwrapped: [Float] = []
  nonisolated(unsafe) private var out: [UInt8] = []   // specDecodeQueue only, with proc
  /// Non-zero = the server is sending a frame format we do not decode, and the waterfall is
  /// black for a reason that has nothing to do with the waterfall.
  @Published var unknownFlags: UInt8 = 0
  /// Rows actually handed to the renderer. `fps` counts frames RECEIVED — if these two
  /// disagree, the data is arriving and being thrown away, which is a completely different
  /// bug from the data not arriving.
  @Published var rowsPushed = 0

  private func onSpectrumJSON(_ d: Data) {
    guard let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { return }
    let type = j["type"] as? String
    if type == "hwinfo" { onHwInfo(j); return }         // VibeServer: offered gains/rates + owner ceiling
    if type == "rds" {
      // Station naming from WFM RDS. An empty ps IS the "station lost" signal (the server change-detects
      // and sends it once), so assigning it straight through clears the band strip — no stale name.
      rdsPs   = (j["ps"] as? String ?? "").trimmingCharacters(in: .whitespaces)
      rdsText = (j["radiotext"] as? String ?? "").trimmingCharacters(in: .whitespaces)
      return
    }
    // ★ Session warnings arrive as their own message (T-120 / T-30). Treat them as a
    // clock correction, not as the only source of truth: driving the countdown purely
    // off these showed a 30-minute listener NOTHING for 28 minutes, which reads as the
    // limit not having taken. `sessionSecsLeft` on config is the deadline; this is the
    // nudge.
    // The server's verdict on an unlock attempt. `refused` means the password was
    // wrong (or we are locked out); either way adminOk is the server's word.
    // ★ THE AGC MADE VISIBLE. sysGain moves as the loop works, which is the only way
    // to tell a working AGC from a stuck one without disabling it and wiggling a
    // slider — the very workaround that diagnosed the original bug.
    // ★★ EVICTED — the owner took the receiver with an admin override. TERMINAL: the
    // shim's comment is explicit that the displaced client must not retry, because
    // every client auto-reconnecting is what made plain takeover unworkable (two
    // listeners displacing each other forever). Jr ignored this message entirely, so
    // the watch just saw the socket close and started reconnecting — the listener was
    // thrown off with no idea why (Stuart, on the wrist, 2026-07-28).
    // ★★ THE SERVER TURNING US AWAY IS TERMINAL — never retried. Each of these is
    // a deliberate refusal, and the default reconnect would hammer a receiver that
    // is busy telling us to go away, while showing our own user nothing but
    // "reconnecting". The web client has said this properly for a while; Jr and the
    // phone handled NEITHER message, so a listener whose time ran out simply
    // dropped and started reconnecting (2026-07-28).
    if type == "session_expired" {
      sessionEnded = true
      cooldownSecs = (j["cooldown"] as? NSNumber)?.intValue ?? 0
      // ★ goIdle(), not just the latch: the latch stops REOPENING, but the audio
      // socket already open keeps playing. The web client had exactly this —
      // audio came back under a "TIME UP" screen once the cooldown lapsed. A
      // terminal refusal must reach EVERY socket, not only the one that heard it.
      goIdle()
      status = "session ended"
      return
    }
    if type == "cooldown" {
      cooldownRefused = true
      cooldownSecs = (j["secs"] as? NSNumber)?.intValue ?? 0
      // ★ goIdle(), not just the latch: the latch stops REOPENING, but the audio
      // socket already open keeps playing. The web client had exactly this —
      // audio came back under a "TIME UP" screen once the cooldown lapsed. A
      // terminal refusal must reach EVERY socket, not only the one that heard it.
      goIdle()
      status = "cooldown"
      return
    }
    if type == "evicted" {
      evicted = true
      // ★ goIdle(), not just the latch: the latch stops REOPENING, but the audio
      // socket already open keeps playing. The web client had exactly this —
      // audio came back under a "TIME UP" screen once the cooldown lapsed. A
      // terminal refusal must reach EVERY socket, not only the one that heard it.
      goIdle()
      status = "taken over by the owner"
      return
    }
    if type == "rspstat" {
      sysGainDb = (j["sysGain"] as? NSNumber)?.doubleValue ?? sysGainDb
      rspLna    = max(0, lnaStates - 1) - ((j["lna"] as? NSNumber)?.intValue ?? 0)
      rspIfGr   = (j["ifgr"] as? NSNumber)?.intValue ?? rspIfGr
      rspOverload = ((j["overload"] as? NSNumber)?.intValue ?? 0) != 0
      radioSettling = ((j["settling"] as? NSNumber)?.intValue ?? 0) != 0
      return
    }
    if type == "admin" {
      adminOk = (j["ok"] as? Bool) ?? false
      if (j["refused"] as? Bool) == true { status = "admin password refused" }
      return
    }
    if type == "session_warning" {
      if let left = (j["secsLeft"] as? NSNumber)?.intValue { sessionSecsLeft = left }
      return
    }
    guard type == "config" else { return }
    if let bc = j["binCount"] as? Int { binCount = bc }
    if let bb = j["binBandwidth"] as? Double { binBandwidth = bb }
    if let cf = j["centerFreq"] as? Double { centerHz = cf }
    // The server has confirmed the scale for the current subscription — rows may paint now.
    specConfigSeq = specSubscribeSeq

    // ADOPT THE VIBESERVER'S CURRENT TUNE. Unlike real UberSDR (where the client drives the radio),
    // VibeServer holds its OWN state — it's already tuned (e.g. 96.6 FM, set on the phone) and streaming
    // that audio. So on the FIRST config we must MATCH the server, not force our 648 kHz/AM default onto
    // it (which left the spectrum viewing off-band as a bouncing line while the audio played 96.6 FM).
    // If we have a saved tune for this host we've already restored + asserted it in openVibeSockets, so
    // skip. (Mode isn't in the shim config — it's restored from per-host memory when we have it.)
    if isVibe, !vibeAdopted {
      vibeAdopted = true
      if !vibeRestored, centerHz > 0 {
        frequency = centerHz
        viewCenterHz = centerHz
        // The shim doesn't send the current MODE, so infer it: a centre in the FM broadcast band is
        // almost certainly WFM (the common VibeServer case), so the UI matches the audio without the
        // user having to pick FM. Other bands keep the default until they choose.
        if centerHz >= 87_000_000, centerHz <= 108_500_000 {
          mode = "wfm"
          if let d = Self.modeBW["wfm"] { bwLow = d.low; bwHigh = d.high }
        }
      }
    }

    // VFO-LOCKED CENTRE. drawVFO puts the needle at the display centre, so the view must stay
    // centred on the tuned frequency. A config whose centre has drifted from it — the wide
    // default a FRESH session starts at (spectrum shows mid-band with the needle stuck centre
    // until a zoom nudges it, the "starts centre not at 648 kHz" bug), or a reset — must be
    // forced back to the VFO. The bin-width tolerance absorbs the server snapping the centre
    // to its bin grid, so a legitimately-acked centre doesn't ping-pong.
    if abs(centerHz - frequency) > max(binBandwidth, 1) {
      sendView(frequency, viewBinBw > 0 ? viewBinBw : binBandwidth)
      return
    }

    // PRESERVE THE ZOOM ACROSS A RECONNECT. A fresh/reconnected server session starts at
    // FULL SPAN, so a config that arrives at a different scale than the zoom we want — with
    // no request of ours just sent — is a session reset, NOT a real change. Re-assert our
    // view instead of adopting the wide default (the "waterfall snaps zoomed-out after a
    // blip" bug — present on the UberSDR site too). Ported from the phone client's onConfig
    // unsolicited-change handling (UberSDRClient.ts).
    //
    // The recent-send guard is essential: the server snaps binBandwidth to a ladder, so the
    // config that ACKs our OWN zoom can differ slightly from what we asked. Adopt that (it's
    // the truth); only re-assert when the mismatch is NOT the echo of a request we just made.
    let recentlyRequested = ProcessInfo.processInfo.systemUptime - lastViewSentAt < 1.5
    if viewBinBw == 0, binBandwidth > 0 {
      viewBinBw = binBandwidth          // first config of the session — adopt
      viewCenterHz = centerHz
    } else if recentlyRequested {
      viewBinBw = binBandwidth          // ack of our own zoom (ladder-snapped) — adopt
      viewCenterHz = centerHz
    } else if viewBinBw > 0, abs(binBandwidth - viewBinBw) > viewBinBw * 1e-3 {
      // Unsolicited reset to full span after a blip — force our zoom back and hold the paint
      // (sendView bumps the subscribe seq, so the gate won't draw the wide frame).
      sendView(viewCenterHz > 0 ? viewCenterHz : frequency, viewBinBw)
      return
    }

    // RE-ASSERT THE RATE. A binBandwidth change means the session may have MIGRATED between
    // the shared default channel and a private one — and `set_rate` works only on a private
    // session (the shared SSRC is hardcoded to every 2nd tick, and ignores us). So a zoom
    // can silently take away the rate we asked for, or hand us one we didn't. The phone
    // client re-sends on exactly this signal for exactly this reason.
    if binBandwidth != lastRateBinBw {
      lastRateBinBw = binBandwidth
      // Open at whatever rung Link Management starts on (middle, not full) — see LinkManager.init.
      if rateDivisor != linkMgr.rung { rateDivisor = linkMgr.rung }   // didSet sends set_rate
      else if rateDivisor > 1 { sendRate() }
    }
  }
  private var lastRateBinBw: Double = 0

  /// When we last asked the server for a view. A config arriving within a short window of
  /// this is the ACK of our own request (possibly ladder-snapped) and must be adopted;
  /// a config arriving OUTSIDE it at the wrong scale is an unsolicited reset to re-assert
  /// against. See onSpectrumJSON.
  private var lastViewSentAt: Double = 0

  // ── Coalesced view sends (rapid gestures) ─────────────────────────────────────
  private var pendingView: (freq: Double, binBw: Double)?
  private var viewFlushWork: DispatchWorkItem?

  /// For RAPID gestures (crown zoom, drum tune). Update the local view IMMEDIATELY — so the
  /// zoom factor compounds detent-to-detent and the needle math stays right — but DEBOUNCE the
  /// network send + the paint gate. A fast spin fired one sendView per detent, each
  /// re-subscribing and re-arming the paint gate, which froze the waterfall mid-spin then
  /// snapped it ("gets stuck, then jumps"). This collapses the flurry into one clean
  /// re-subscribe once the gesture settles.
  private func sendViewCoalesced(_ freq: Double, _ binBw: Double) {
    viewCenterHz = freq
    viewBinBw = binBw
    pendingView = (freq, binBw)
    viewFlushWork?.cancel()
    let work = DispatchWorkItem { [weak self] in
      guard let self, let p = self.pendingView else { return }
      self.pendingView = nil
      self.sendView(p.freq, p.binBw)
    }
    viewFlushWork = work
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.12, execute: work)
  }

  private func sendView(_ freq: Double, _ binBw: Double) {
    viewCenterHz = freq
    viewBinBw = binBw
    lastViewSentAt = ProcessInfo.processInfo.systemUptime
    // New subscription state — hold the paint until the server confirms the scale (see the
    // gate in onSpectrumBinary). Reset the fail-open counter for this fresh wait.
    specSubscribeSeq &+= 1
    gatedFrames = 0
    // ★ The scale is about to change under us — drop the derived range with it rather than
    //   average the new span against the old one's history.
    specDecodeQueue.async { self.proc.reset() }
    let msg: [String: Any] = ["type": "zoom",
                              "frequency": Int(freq.rounded()),
                              "binBandwidth": binBw]
    specSock.send(json: msg)
  }

  // ── Audio ─────────────────────────────────────────────────────────────────

  private func openAudio() {
    guard !goingIdle else { return }   // never reopen a torn-down client (server switch)

    let url: URL?
    if isVibe {
      // VibeServer: /ws/audio. Ask for Opus — Jr decodes it (OpusDecoder), and it is ~4x lighter
      // than PCM, which is what makes VibeServer usable over Bluetooth. An OLD (pre-Opus) VibeServer
      // simply ignores the codec param and sends ADPCM, which decodeVibeAudio still handles.
      // ★ user_session_id MUST match the spectrum socket's. VibeServer's single-occupant guard keys
      // on it: our spectrum socket carries `uuid`, so the audio socket MUST carry the SAME uuid or
      // the server sees two different clients and REFUSES the second as "busy" — which killed the
      // spectrum entirely (audio claimed the slot anonymously, spectrum was turned away). 2026-07-23.
      var extra = authSuffix.isEmpty ? "" : "&" + authSuffix.dropFirst()
      if !adminSuffix.isEmpty { extra += adminSuffix }
      // ★ On a MONO output route (the watch speaker) ask for a fullband mono stream — the server folds
      // stereo before encoding so all 64k lands in one channel. On AirPods (stereo) omit it and keep
      // the stereo image. Re-requested on a route change via audio.onRouteChange (openVibeSockets).
      let chParam = audio.outputIsMono ? "&channels=1" : ""
      url = URL(string: "\(scheme)://\(host)\(radioPath)/ws/audio?user_session_id=\(uuid)&codec=opus\(chParam)\(extra)")
      audioSock.onData = { [weak self] d in
        guard let self else { return }
        self.decodeVibeAudio(d)
        let n = d.count
        Task { @MainActor in self.audioCount += 1; self.audioBytes += n }
      }
    } else {
      // UberSDR: `/ws`, tune rides the query string. Taken verbatim from VibePowerModule.audioWsURL.
      // ★★ HONOUR THE SCHEME. Hardcoded "wss://" — the THIRD place in this client that
      //    assumed a public UberSDR: a LOCAL instance on plain ws got a TLS handshake
      //    against a non-TLS port, so it connected partway and then had no audio socket.
      //    Symptom on a Series 6, 2026-07-29: "local UberSDR partially connected, no
      //    spectrum". (The other two were postConnection's https and SpikeLink never
      //    deriving `secure` at all.)
      url = URL(string:
        "\(scheme)://\(host)\(radioPath)/ws?user_session_id=\(uuid)" +
        "&frequency=\(Int(frequency))&mode=\(mode)&format=opus&version=2")
      audioSock.onData = { [weak self] d in
        guard let self else { return }
        // radiod carries the CHANNEL SNR (basebandPower − noiseDensity, both dBFS) in the 21-byte
        // packet header — zoom-INDEPENDENT, the true meter number (same source the phone uses). Feed
        // signalDb from this. −30 corrects radiod's known audio-stream floor offset (its SNR over-reads).
        if d.count > 21 {
          let bb = Float(bitPattern: d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 13, as: UInt32.self) })
          let nd = Float(bitPattern: d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 17, as: UInt32.self) })
          if bb > -900, nd > -900 { self.chanSnr = Double(bb - nd) - 30 }
          // Baseband power on its own is the dBFS reading — what the "dBFS" and "S-unit" meter
          // units want. Keep it raw; the −30 above is an SNR correction and must NOT apply here.
          if bb > -900 { self.chanDbfs = Double(bb) }
        }
        // Decode + play OFF the main actor: ~50 packets/sec, and it must never fight the
        // waterfall for the main thread.
        if let out = self.opus.decode(d) {
          self.audio.play(pcm: out.pcm, rate: out.rate, channels: out.channels)
          let n = d.count
          Task { @MainActor in self.audioCount += 1; self.audioBytes += n }
        }
      }
    }
    guard let url else { return }
    audioSock.onState = { [weak self] s in
      Task { @MainActor in
        guard let self else { return }
        self.audioWsState = s
        // A rate-limited socket is not a broken socket — it is an EARLY socket. Back off and
        // come back, rather than sitting there dead until the app is relaunched.
        if s.contains("failed") || s.contains("recv:") { self.retryAudio() }
      }
    }
    audioSock.open(url: url, headers: [("User-Agent", jrUserAgent)])
  }

  // ── VibeServer: auth + sockets + ADPCM ───────────────────────────────────────

  /// GET /vibeserver/auth → nonce → HMAC-SHA256(pin, nonce) → `authSuffix`. Reusable 1-hour credential.
  /// Returns false (with `status` carrying the human reason) on offline / wrong-PIN / locked-out.
  private func resolveVibeAuth() async -> Bool {
    // ★ AN EMPTY HOST IS NOT A NETWORK PROBLEM, so it must not be reported as one. "http:///path"
    //   passes `URL(string:)` happily, and the resulting timeout blamed the server, the Wi-Fi and
    //   the watch for a fault entirely inside the app. Say what is actually wrong.
    guard !host.isEmpty else {
      Vitals.crumb("VIBE auth ABORT empty host")
      status = "bad server URL"
      vibeDiag = "no host in this favourite"
      return false
    }
    let httpScheme = secure ? "https" : "http"
    guard let url = URL(string: "\(httpScheme)://\(host)\(radioPath)/vibeserver/auth") else { status = "bad server URL"; return false }
    // ★ WHAT DID WE ACTUALLY ASK FOR, AND WHAT CAME BACK. A Series 6 on watchOS 26 reaches
    //   every internet backend but never reaches a LAN VibeServer — and the server sees ZERO
    //   packets, on Wi-Fi and on the phone relay, same subnet, with the Mac able to ping the
    //   watch. Server, network, subnet, mDNS, transport and Local Network permission are all
    //   eliminated from outside, so the remaining candidates are invisible without this: the
    //   URL not being what we think (a .local name cannot resolve on the watch), or URLSession
    //   declining to send. One attempt with these crumbs settles it.
    Vitals.crumb("VIBE auth GET \(url.absoluteString)")
    do {
      let (data, _) = try await httpSession.data(from: url)
      Vitals.crumb("VIBE auth OK \(data.count)B")
      vibeDiag = ""          // we reached it — drop any stale reason
      guard let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
        status = "not a VibeServer?"; return false
      }
      if !((j["required"] as? Bool) ?? false) { authSuffix = ""; return true }   // no PIN
      guard let nonce = j["nonce"] as? String, !nonce.isEmpty else {
        let locked = (j["lockedFor"] as? NSNumber)?.intValue ?? 0
        // ★ Do NOT re-prompt while locked out: more attempts extend the lockout,
        // so asking for a PIN we cannot try yet actively harms the user.
        if locked > 0 {
          status = "locked \(locked)s — too many PIN tries"
        } else {
          needsPin = true
          status = "PIN needed"
        }
        return false
      }
      // ★ No PIN and one is required ⇒ ASK, do not sign with an empty key. Signing
      // anyway produces a token the server must refuse, which the retry loop then
      // reads as a flaky connection and hides behind "reconnecting".
      if vibePin.isEmpty {
        needsPin = true
        status = "PIN required"
        return false
      }
      // HMAC key = the PIN bytes, message = the nonce's ASCII-hex STRING (not decoded), lowercase hex out.
      let mac = HMAC<SHA256>.authenticationCode(for: Data(nonce.utf8), using: SymmetricKey(data: Data(vibePin.utf8)))
      let token = mac.map { String(format: "%02x", $0) }.joined()
      authSuffix = "&vs_nonce=\(nonce)&vs_auth=\(token)"
      needsPin = false
      return true
    } catch {
      // ★ The REASON, not just the verdict. "can't reach server" is what the user sees;
      //   the crumb is what tells us whether it was a timeout, a refusal, a DNS failure or
      //   no route at all — four different bugs that look identical on the wrist.
      let ns = error as NSError
      Vitals.crumb("VIBE auth FAIL \(ns.domain) \(ns.code): \(ns.localizedDescription)")
      // ★ SHOW THE CODE ON THE WRIST. The crumb lands in jr-vitals.log, which we have no way
      //   to get off a watch Xcode cannot see — so the number goes in the status line, where
      //   it can simply be read. It is the whole diagnosis in one integer:
      //     -1003 cannot find host      → the URL is a name that will not resolve
      //     -1004 could not connect     → refused, or no route to that host
      //     -1001 timed out             → reached the network, got no answer
      //     -1009 offline               → no usable interface at all
      //   Four different bugs that are indistinguishable behind "can't reach server".
      // ★ RELEASE: the raw URLSession code is a DIAGNOSTIC and does not belong in front of a
      //   user — "can't reach server (-1004)" reads as an internal error escaping. The code is
      //   still recorded in the crumb log above, where it is just as useful to us and invisible
      //   to them. Put it back in the status line only for an internal TestFlight build.
      status = "can't reach server"
      // ★ vibeDiag stays for the reasons a PERSON can act on ("no host in this favourite"); a
      //   bare errno and a hostname is not one of them.
      vibeDiag = ""
      return false
    }
  }

  /// LAN, single-user, no 2/sec rate limit — open both sockets straight away (no UberSDR audio-ready dance).
  private func openVibeSockets() {
    specOpened = true          // suppress the UberSDR "audio never readied" fallback path
    // Re-request the matching channel count when the output route flips (AirPods in/out). Only the
    // audio socket carries the channels param, so reopen JUST that — spectrum is untouched. Rides the
    // same-session takeover on the server, so the reclaim is instant. isVibe only.
    audio.onRouteChange = { [weak self] _ in
      guard let self, self.isVibe, !self.goingIdle else { return }
      self.audioSock.cancel()
      self.openAudio()
    }
    openAudio()
    openSpectrum()
    audio.start { [weak self] ok, info in
      Task { @MainActor in self?.audioLive = ok; self?.audioRoute = ok ? info : "FAILED: \(info)" }
    }
    // If we restored a saved tune, ASSERT it so the server matches our remembered freq+mode. (Without a
    // saved tune we adopt the server's current state on the first config instead — see onSpectrumJSON.)
    if vibeRestored {
      Task { @MainActor in
        try? await Task.sleep(nanoseconds: 500_000_000)   // let the audio socket come up first
        guard !self.goingIdle else { return }
        self.sendTune()
      }
    }
    status = "live"
  }

  /// Decode one /ws/audio binary frame: [0]=ch [1]=format(0 raw/1 ADPCM mono/2 ADPCM mid-side)
  /// [2..5]=rate LE. Raw → int16 from offset 6. ADPCM → [6..7]=count/ch, one self-seeded block per channel
  /// from offset 8. Format is read PER FRAME (stereo silently drops to mono when the pilot is unlocked).
  private func decodeVibeAudio(_ d: Data) {
    guard d.count >= 6 else { return }
    let format = d[1]
    let rate = Int(d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 2, as: UInt32.self) })

    if format == 3 {                                   // Opus (VibeServer compressed audio)
      let ch = max(1, Int(d[0]))
      let packet = d.subdata(in: 6..<d.count)
      if let pcm = opus.decodeRaw(packet, rate: Int32(rate), ch: Int32(ch)) {
        audio.play(pcm: pcm, rate: Int32(rate), channels: Int32(ch))
      }
      return
    }
    if format == 0 {
      let ch = max(1, Int(d[0]))
      let pcm = d.subdata(in: 6..<d.count).withUnsafeBytes { Array($0.bindMemory(to: Int16.self)) }
      audio.play(pcm: pcm, rate: Int32(rate), channels: Int32(ch))
      return
    }

    guard d.count >= 8 else { return }
    let count = Int(d.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 6, as: UInt16.self) })
    guard count > 0 else { return }
    let blockBytes = 4 + (count + 1) / 2                    // [pred i16][index u8][pad] + ceil(count/2) nibbles

    if format == 1 {
      guard d.count >= 8 + blockBytes else { return }
      let mono = decodeAdpcmBlock(d.subdata(in: 8..<(8 + blockBytes)), count: count, dec: adpcmL)
      audio.play(pcm: mono, rate: Int32(rate), channels: 1)
    } else if format == 2 {
      guard d.count >= 8 + 2 * blockBytes else { return }
      let mid  = decodeAdpcmBlock(d.subdata(in: 8..<(8 + blockBytes)), count: count, dec: adpcmL)
      let side = decodeAdpcmBlock(d.subdata(in: (8 + blockBytes)..<(8 + 2 * blockBytes)), count: count, dec: adpcmR)
      let n = min(mid.count, side.count)
      var inter = [Int16](repeating: 0, count: n * 2)
      for i in 0..<n {                                       // L = M+S, R = M-S
        inter[i * 2]     = Int16(clamping: Int(mid[i]) + Int(side[i]))
        inter[i * 2 + 1] = Int16(clamping: Int(mid[i]) - Int(side[i]))
      }
      audio.play(pcm: inter, rate: Int32(rate), channels: 2)
    }
  }

  /// One self-seeded IMA-ADPCM block → PCM. Seed: [pred i16 LE][index u8][pad]; nibbles low-then-high.
  private func decodeAdpcmBlock(_ block: Data, count: Int, dec: ImaAdpcmDecoder) -> [Int16] {
    guard block.count >= 4 else { return [] }
    let pred = Int(block.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: Int16.self) })
    dec.setState(index: Int(block[2]), predictor: pred)
    var out = [Int16](); out.reserveCapacity(count)
    var off = 4
    var i = 0
    while i < count, off < block.count {
      let byte = Int(block[off]); off += 1
      out.append(Int16(clamping: dec.decodeNibble(byte & 0x0f)))          // even sample = low nibble
      if i + 1 < count { out.append(Int16(clamping: dec.decodeNibble((byte >> 4) & 0x0f))) }  // odd = high
      i += 2
    }
    return out
  }

  /// What the audio socket is doing, in its own words.
  @Published var audioWsState = ""

  private var specRetries = 0
  private func retrySpectrum() {
    // NEVER STOP TRYING. It gave up after four attempts, which on a wrist — where the app is
    // suspended, the radio sleeps and the path changes under you — means "dead until you
    // relaunch". A receiver that quits on you is not a receiver.
    specRetries += 1
    let wait = UInt64(min(specRetries, 5)) * 2_000_000_000   // 2s → 10s, then hold
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: wait)
      guard !self.goingIdle, self.framesPerSec == 0 else { return }   // torn down, or recovered on its own
      self.specWsState = "spec retry \(self.specRetries)…"
      self.openSpectrum()
    }
  }

  private var audioRetries = 0
  private func retryAudio() {
    audioRetries += 1
    let wait = UInt64(min(audioRetries, 5)) * 2_000_000_000
    Task { @MainActor in
      try? await Task.sleep(nanoseconds: wait)
      guard !self.goingIdle, self.audioPerSec == 0 else { return }   // torn down, or recovered on its own
      self.audioWsState = "audio retry \(self.audioRetries)…"
      self.openAudio()
    }
  }

  private func sendTune() {
    // ★ Int64: on arm64_32 `Int` is 32-bit and traps past 2.147e9, which a tune in Hz
    //   reaches on an SDRplay RSP (2 GHz) — uncomfortably close, and a trap is a crash.
    let msg: [String: Any] = ["type": "tune", "frequency": Int64(frequency), "mode": mode]
    audioSock.send(json: msg)
    // NOTE: we deliberately do NOT flush the audio/spectrum on tune. The buffer draining at the
    // old frequency is the "swishing through the stations" sweep as you cross signals — Stuart
    // likes it, and it keeps audio+waterfall in sync. The residual tune lag is the server
    // round-trip + this cushion, and the cushion is wanted, so we leave it. (flush() exists on
    // WatchAudio if we ever want an instant-jump mode.)
  }

  // ── Crown-tune DEBOUNCE (100ms) — MATCH THE MAIN APP / COMPANION ──────────────
  //
  // The main app debounces tuning to 100ms (UberSDR's supported rate; m9psy/MadPsy's
  // advice) — it feels "heavier" but tunes rock-solid. The spike sent a tune per detent,
  // which felt slicker but meant the two modes felt DIFFERENT. Stuart's call: keep the
  // heavier feel for reliability AND make direct (spike) and remote (companion) feel the
  // SAME. So the spike throttles the SERVER send to the latest frequency ≤1/100ms too —
  // `frequency` still updates instantly (the readout/needle stay snappy), only the network
  // tune is rate-limited, trailing-edge so the final freq always lands.
  private var lastTuneSentAt: Double = 0
  private var tuneFlushWork: DispatchWorkItem?
  private func sendTuneThrottled() {
    let now = ProcessInfo.processInfo.systemUptime
    let wait = lastTuneSentAt + 0.1 - now
    if wait <= 0 {
      lastTuneSentAt = now
      tuneFlushWork?.cancel(); tuneFlushWork = nil
      sendTune()
    } else if tuneFlushWork == nil {
      let work = DispatchWorkItem { [weak self] in
        guard let self else { return }
        self.tuneFlushWork = nil
        self.lastTuneSentAt = ProcessInfo.processInfo.systemUptime
        self.sendTune()   // sends self.frequency — the latest, already updated by tune()
      }
      tuneFlushWork = work
      DispatchQueue.main.asyncAfter(deadline: .now() + wait, execute: work)
    }
  }

  // ── Controls ──────────────────────────────────────────────────────────────

  /// Tuning range. UberSDR is HF-only (≤30 MHz); a VibeServer serves an RTL-SDR dongle that reaches VHF/UHF
  /// (up to ~1.8 GHz), so the 30 MHz clamp would have locked the whole VHF/UHF band away.
  private var freqMin: Double { 10_000 }
  private var freqMax: Double { isVibe ? 1_800_000_000 : 30_000_000 }
  var tuneMinHz: Double { freqMin }
  var tuneMaxHz: Double { freqMax }
  func setAutoContrast(_ v: Double) { proc.autoContrast = v }
  func setManualRange(_ on: Bool, floor: Double, ceil: Double) {
    proc.manualRange = on; proc.manualFloorDb = floor; proc.manualCeilDb = ceil
  }
  /// SNR audio gate → radiod, same wire as the phone: +30 corrects radiod's audio-stream floor offset.
  func setSquelch(_ minSnr: Double) {
    audioSock.send(json: ["type": "set_audio_gate", "min_snr": minSnr <= -999 ? -999 : Int((minSnr + 30).rounded())])
  }

  /// Crown tuning. The audio socket carries the tune; the spectrum view follows it.
  func tune(delta: Int, step: Double) {
    guard delta != 0 else { return }
    let base = delta > 0 ? (frequency / step).rounded(.down) : (frequency / step).rounded(.up)
    let f = max(freqMin, min(freqMax, (base + Double(delta)) * step))
    guard f != frequency else { return }
    frequency = f
    clearRds()            // moved off the old station — don't show its name on the new frequency
    saveVibeState()
    sendTuneThrottled()   // 100ms debounce — match the companion/main app (see sendTuneThrottled)
    sendViewCoalesced(f, viewBinBw > 0 ? viewBinBw : binBandwidth)
  }

  /// App playback gain (0…1), forwarded to the audio engine's master mixer — the only volume a
  /// watch app can actually set (no system-volume API on watchOS).
  func setVolume(_ v: Double) { audio.setVolume(Float(v)) }

  /// Set the passband edges (Hz offsets from the carrier) and push them to the server. Sent as
  /// a `tune` message carrying bandwidthLow/High on the AUDIO socket — the exact message the
  /// phone's native path emits (VibePowerModule.swift `sendWsJson(["type":"tune", ...])`).
  func setBandwidth(_ low: Double, _ high: Double) {
    bwLow = low
    bwHigh = high
    audioSock.send(json: ["type": "tune",
                          "bandwidthLow":  Int(low.rounded()),
                          "bandwidthHigh": Int(high.rounded())])
  }

  func setMode(_ m: String) {
    guard m != mode else { return }
    mode = m
    clearRds()            // RDS is WFM-only; leaving/among modes must not leave a station name up
    // A mode change resets the passband to that mode's server default (the fresh socket below
    // applies the same default server-side; we just mirror it for the VFO lines / UI).
    if let d = Self.modeBW[m] { bwLow = d.low; bwHigh = d.high }
    saveVibeState()
    if isVibe {
      // VibeServer: /ws/audio carries NO query, so the mode rides a tune JSON — reopening the socket
      // (the UberSDR trick) wouldn't change the server's demod. The shim rebuilds its ADPCM stream itself.
      sendTune()
      return
    }
    // REOPEN THE AUDIO SOCKET — do NOT just send a `tune`. The server builds its Opus
    // encoder ONCE, when the socket opens, at the sample rate that suits the mode (SSB is
    // narrower than AM/FM). A mid-session tune changes the DEMOD but not the encoder, so the
    // stream keeps carrying the OLD rate in its header while the audio behind it is the new
    // rate: the decoder plays it back too fast — chipmunks. A fresh socket = a fresh encoder
    // at the right rate, and the OpusDecoder re-inits itself from the new header. (The audio
    // URL bakes mode+frequency into the query string, so no separate tune message is needed.)
    audioSock.cancel()
    openAudio()
  }

  /// Absolute tune, for the numpad — jump straight from 648 kHz AM to the 40m band
  /// without spinning the crown across 6 MHz.
  func tuneTo(_ hz: Double) {
    let f = max(freqMin, min(freqMax, hz))
    guard f != frequency else { return }
    frequency = f
    clearRds()            // jumped bands — drop the old station's name immediately
    saveVibeState()
    sendTune()
    sendViewCoalesced(f, viewBinBw > 0 ? viewBinBw : binBandwidth)
  }

  /// FRAME RATE, in our back pocket.
  ///
  /// UberSDR polls radiod every 100ms — 10 Hz — and `set_rate` divides that server-side, so
  /// halving the frame rate costs us NOTHING to try: no re-render, no re-decode, no rebuild.
  /// The server simply sends half as many frames, and every per-frame cost we are here to
  /// measure (the receive, the unwrap, the DSP, the decimate, the paint) halves with it.
  ///
  /// If 10fps proves too expensive on the wrist, 5fps is the answer — and the waterfall
  /// already interpolates to a 20fps render clock, so the SCROLL stays smooth either way.
  /// What you lose is time resolution, not fluidity. Being able to A/B it on the wrist is
  /// the whole reason it is a toggle and not a constant.
  @Published var rateDivisor = 1 {
    didSet { sendRate() }
  }

  // ── LINK MANAGEMENT (UberSDR only) ───────────────────────────────────────────
  //
  // Ask the server for FEWER waterfall frames when the link can't carry them, and step back up
  // when it recovers. Measured against a real server: divisor 1/2/3 = 10/5/3.3 fps and
  // 12.4/6.1/4.2 KB/s — the byte rate scales linearly, so a rung is a true ~⅓ cut each time.
  //
  // Why this is safe to do behind the user's back: the waterfall already interpolates onto a
  // 20fps render clock, so a lower frame rate costs TIME RESOLUTION, not scroll smoothness. A
  // stuttering link, by contrast, is visible and ugly. Trading the former for the latter is the
  // whole point — but it IS a trade, hence the toggle.
  //
  // ASYMMETRIC ON PURPOSE. Degrade after 3s of starvation, recover only after 20s of health. A
  // wrong step down costs a little time resolution nobody sees; a wrong step up costs a visible
  // stutter. Ladder stops at 3: below ~3fps the interpolator is inventing most of what you see.
  /// Read live from defaults each tick rather than held as state, so the toggle can live anywhere in
  /// the menu tree (as `@AppStorage("vibeAutoLink")`) without plumbing UberClient into the root menu.
  /// Adaptive waterfall rate. UberSDR ladder: divisor 1/2/3 = 10/5/3.3 fps (measured).
  /// Low Data may pin rung 2 (5 fps) but never rung 3 — 3.3 fps is jerky and reserved for a
  /// genuinely poor link. VibeServer has richer levers and is driven separately.
  lazy var linkMgr = LinkManager(ladder: [10, 5, 10.0 / 3.0], lowDataRung: 2) { [weak self] rung, fps in
    self?.rateDivisor = rung                     // didSet sends set_rate
    self?.waterfall.setExpectedRowRate(fps)      // don't make the interpolator rediscover it
  }
  /// How far the controller has had to back off (1 = not throttled). Mirrors to SpikeLink for
  /// the link glyph — see LinkManager.adaptiveRung for why this is not `rateDivisor`.
  /// VibeServer isn't rung-throttled (tick() is skipped for it), so linkMgr.adaptiveRung would sit
  /// at its init value (2 = throttled amber) forever. Its lower fps is a DELIBERATE bins/fftRate
  /// choice, not a symptom — the glyph must read healthy (1), never a throttled/red indicator.
  var adaptiveRung: Int { isVibe ? 1 : linkMgr.adaptiveRung }
  /// ★ VibeServer NEVER "settles": stepLinkManagement() guards on `!isVibe`, so linkMgr.tick() is
  /// never called for it — and `settling` inits TRUE in adaptive/AUTO mode, so it would otherwise
  /// stay true FOREVER (VibeServer showed a permanent "Initialising" pill + breathing/colour-cycling
  /// link icon, including on every wrist-up). VibeServer sets bins/fftRate directly at both ends, so
  /// there is no rate to "work out" — the honest answer is not-settling. UberSDR still settles (and
  /// the 12s deadline in LinkManager.tick clears a stuck hold-band case there).
  var linkSettling: Bool { isVibe ? false : linkMgr.settling }

  private func stepLinkManagement() {
    // UberSDR only: VibeServer has fftRate/bins and we own both ends there.
    guard !isVibe, !goingIdle else { return }
    linkMgr.tick(fps: framesPerSec,
                 live: status == "live" && everHadFrames,
                 // A zoom/tune re-subscribes and legitimately pauses frames.
                 settled: ProcessInfo.processInfo.systemUptime - lastViewSentAt > 3)
  }

  private func sendRate() {
    let msg: [String: Any] = ["type": "set_rate", "divisor": max(1, min(8, rateDivisor))]
    specSock.send(json: msg)
  }

  /// Crown zoom. The REAL server zoom — finer bins, not a magnified crop. That is the only
  /// thing that beats the bin-resolution ceiling, and it is why the watch feels sharp.
  func zoom(delta: Int) {
    guard delta != 0, viewBinBw > 0 else { return }
    // Half an octave per detent. The server zooms in DISCRETE octaves, so a small per-detent
    // creep (was /6 = 6 clicks) produced no visible change until the server's nearest level
    // finally flipped — the "ring fills as you spin, then pops back" stickiness. Half-octave
    // steps cross a server level every ~1–2 clicks, snappy without being a full-octave lurch.
    let factor = pow(2.0, -Double(delta) / 2.0)
    let n = Double(max(binCount, 256))
    let bb = max(6_000 / n, min(viewBinBw * factor, 30_000_000 / n))
    // Anchor the zoom on the TUNED frequency, not viewCenterHz. viewCenterHz gets
    // overwritten by the server's reported frame centre (see line ~578), which can
    // drift a touch from where we're actually tuned — so zooming off viewCenterHz
    // pulled the spectrum to a stale centre and it only snapped back on the next
    // tune nudge (which resets viewCenterHz to the truth). The VFO is always the
    // source of truth here, exactly as the phone anchors zoom on the tuned freq.
    sendViewCoalesced(frequency, bb)
  }

  var spanHz: Double { binBandwidth * Double(max(binCount, 1)) }

  /// The span the ticker should draw at — the width actually ON SCREEN, which is the DESIRED
  /// view (`viewBinBw`), not the server's raw `binBandwidth`. They agree in steady state, but
  /// across a reconnect the server sends a wide-default config first: `binBandwidth` jumps to
  /// full span for a moment before our re-asserted zoom lands, so `spanHz` would snap wide and
  /// then narrow. The waterfall doesn't snap — its rows are held until the scale is trustworthy
  /// — and `viewBinBw` holds the user's zoom throughout, so the ticker built on it stays put
  /// too. (Zoom stays instant: `viewBinBw` is the PREDICTED width, updated the moment you zoom.)
  var displaySpanHz: Double { (viewBinBw > 0 ? viewBinBw : binBandwidth) * Double(max(binCount, 1)) }
}
