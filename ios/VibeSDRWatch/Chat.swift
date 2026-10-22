import SwiftUI
import WatchKit

/// ONE saved identity, reused everywhere a chat/ident box exists — OWRX chat name, FM-DX chat name,
/// KiwiSDR `SET ident_user=`. Set once (here or in the picker), auto-fills every backend so a wrist
/// user is never "Anonymous". Shares the Kiwi ident key so the two can't drift apart.
enum ChatIdentity {
  static let key = "vibe.kiwi.ident"
  /// Shown to server operators when the user has not chosen a callsign. Names the PRODUCT, so a
  /// Kiwi operator reading a user list sees a wrist client rather than an anonymous connection.
  /// A user-set name always wins — this is only the fallback.
  ///
  /// ★ This IS how we identify on a direct connection. The Kiwi/OWRX User-Agents cannot carry it:
  ///   they are deliberately browser-shaped, because Kiwi classifies anything else as `ext_api`
  ///   and time-limits or drops it.
  static let fallback = "VibeSDR Jr"
  static var name: String {
    get {
      let v = (UserDefaults.standard.string(forKey: key) ?? "").trimmingCharacters(in: .whitespaces)
      return v.isEmpty ? fallback : v
    }
    set {
      let v = newValue.trimmingCharacters(in: .whitespaces)
      UserDefaults.standard.set(v.isEmpty ? fallback : v, forKey: key)
    }
  }
}

/// The tap-to-send canned messages. Short replies from a wrist read as gruff, so EVERY set opens with
/// the "why so terse" line — sent explicitly, never auto-prepended. Sending "switching now" does NOT
/// perform the switch: the message and the action stay two deliberate steps (the explicit-only etiquette).
/// ★★★ THE SHARED-DIAL VOCABULARY — IDS, NOT TEXT.
///
/// `Canned` below is for the free-text chats (OWRX, FM-DX). A VibeServer shared dial is different
/// in kind: the vocabulary is FIXED and the wire carries an id, so there is nothing to moderate,
/// translate or inject — which is what lets a chat exist on a receiver whose owner is asleep, and
/// what lets it exist on a watch at all.
///
/// ★★ DUPLICATED FROM Jr's Chat.swift ON PURPOSE. Jr and Buddy are separate products with separate
///    targets and no shared module ([[watch_split_jr_buddy]]); the ids are the contract they share,
///    and those live in the SERVER (`chatPhrases()` in local_sdr_shim.cpp). Keep the ids identical;
///    the wording is per-client by design.
enum CannedDial {
  /* ★★★ IN THE WATCH'S OWN LANGUAGE (2026-10-22). Stuart in the UK and Kiko in Brazil on one dial: the wire carries the
   *  id, so each end draws it in its own words — Kiko's wrist says "Posso sintonizar?" and Stuart's says "Can I tune?".
   *  The words are GENERATED (ChatWords, the bottom of this file) from i18n/chat/<lang>.json, the files the phone and the
   *  browser are built from too. ★ Picked ONCE, from Locale.preferredLanguages — the person's own list, not the
   *  bundle's (this app ships one .lproj, so Bundle/Locale.current would always say English). "pt-PT" → pt-BR;
   *  anything without a table → English. */
  static let lang: String = {
    for tag in Locale.preferredLanguages {
      let primary = tag.lowercased().split(whereSeparator: { $0 == "-" || $0 == "_" }).first.map(String.init) ?? ""
      if let l = ChatWords.langOf[primary] { return l }
    }
    return "en"
  }()
  /// In the order a conversation actually runs: ask, act, answer, thank.
  static let all: [(id: String, text: String)] = ChatWords.phrases[lang] ?? ChatWords.en
  private static let words: [String: String] = ChatWords.chrome[lang] ?? ChatWords.enChrome
  /// The chat's own furniture in the watch's language ("{n}" / "{who}" filled in from `vars`).
  static func t(_ key: String, _ vars: [String: String] = [:]) -> String {
    var s = words[key] ?? ChatWords.enChrome[key] ?? key
    for (k, v) in vars { s = s.replacingOccurrences(of: "{\(k)}", with: v) }
    return s
  }
  /// ★ The answers and their band ("" = either). Everything else in `all` is always offered.
  static let answerBand: [String: String] = ChatWords.answerBand
  /// How long a question keeps its answers on offer unless somebody answers first (the phone: ANSWER_WINDOW_MS).
  static let answerWindow: TimeInterval = 5 * 60
  /// The phrases to draw: all but the answers — plus, while a question is open, the answers for the dial's band.
  static func pad(questionOpen: Bool, hz: Double) -> [(id: String, text: String)] {
    let band = hz > 0 && hz < 30_000_000 ? "hf" : "vhf"
    return all.filter { p in
      guard let b = answerBand[p.id] else { return true }
      return questionOpen && (b.isEmpty || b == band)
    }
  }
  /// True while the newest "Anyone know what this is?" came from somebody else, is under answerWindow old, and has
  /// had no answer since — the phone's and the browser's rule.
  static func questionOpen(_ lines: [(from: Int, phrase: String, at: Date)], you: Int) -> Bool {
    for l in lines.reversed() {
      if answerBand[l.phrase] != nil { return false }
      if l.phrase == "what_is_this" { return l.from != you && Date().timeIntervalSince(l.at) < answerWindow }
    }
    return false
  }
  private static let map: [String: String] = Dictionary(uniqueKeysWithValues: all.map { ($0.id, $0.text) })
  /// nil when this build cannot draw the id — the caller DROPS it rather than showing it raw.
  static func text(_ id: String) -> String? { map[id] }
  static func speaker(_ from: Int, you: Int) -> String { from == you ? t("you") : t("user", ["n": "\(from)"]) }
}

enum Canned {
  /// The one line that explains the terseness — offered as its own send button at the top of the list.
  static let jr = "Using VibeSDR Jr on an Apple Watch — replies limited 🙂"

  /// OWRX: the disruption is a PROFILE switch (retunes the shared SDR for everyone).
  static let owrx: [String] = [
    "Anyone mind if I change profile?",
    "OK to switch bands for a bit?",
    "Go ahead 👍",
    "Please wait — mid-decode",
    "No worries, I won't change it",
    "Switching profile now",
    "Done — all yours",
    "Thanks!",
    "Sorry, didn't realise!",
  ]

  /// FM-DX: the disruption is TUNING (single shared tuner). Kept here for the spike's FM-DX port (task #14).
  static let fmdx: [String] = [
    "Can I tune?",
    "Tuning now",
    "Go ahead, tune",
    "Please hold — chasing DX",
    "OK, I won't tune yet",
    "Thanks!",
    "Sorry, didn't realise!",
  ]
}

/// The person-glyph + client-count badge, shared with the companion's FM-DX screen so the wrist speaks
/// one visual language. Passive by default; when a chat message lands it BREATHES (a gentle pulse), and a
/// tap opens the chat sheet. Sits where the dev CPU badge used to — top-left, clear of the clock/battery.
struct ChatGlyph: View {
  let clients: Int
  let activity: Int          // bumps on each inbound message — drives the breathe
  /* ★★★ THE DIAL'S ARM STATE RIDES ON THE SAME BUTTON — tapping opens the chat AND the arm switch.
   *   On a shared dial the polite move is to ASK before taking the tuner, so the control that lets
   *   you take it sits behind the screen where you would ask.
   * ★★ Marked with FM-DX's tick/cross, not a padlock of its own: that screen already taught this
   *   watch's users one symbol for "may I tune on a shared receiver".
   * ★ nil = no shared dial, nothing to arm, no mark drawn. Mirrors Jr — keep the two in step.
   * ★★ DECLARED AFTER `activity`, and that is not cosmetic: a struct's memberwise initialiser
   *   takes its arguments in DECLARATION order, so putting this first made every existing
   *   `ChatGlyph(clients:activity:)` call site a compile error. It cost a Cloud build. */
  var dialArmed: Bool? = nil
  var tap: () -> Void

  @State private var pulse = false      // the in/out scale oscillation
  @State private var lit = false        // the orange highlight window (held for a few seconds)

  var body: some View {
    Button(action: tap) {
      HStack(spacing: 3) {
        // ★★ Order is state · person · count: the state leads because it is what you are deciding
        //    about, and a count reaching two digits would otherwise shove it along the row.
        if let armed = dialArmed {
          Image(systemName: armed ? "checkmark.circle.fill" : "xmark.circle.fill")
            .font(.system(size: 10, weight: .bold))
            .foregroundStyle(armed ? .green : .red)
        }
        Image(systemName: "person.2.fill").font(.system(size: 11, weight: .semibold))
        if clients > 0 {
          Text("\(clients)").font(.system(size: 11, weight: .semibold, design: .rounded)).monospacedDigit()
        }
      }
      .foregroundStyle(lit ? .orange : .white.opacity(0.75))
      .padding(.horizontal, 6).padding(.vertical, 3)
      .background(Capsule().fill(lit ? .orange.opacity(0.18) : .black.opacity(0.35)))
      .scaleEffect(pulse ? 1.22 : 1.0)
      .contentShape(Capsule())
    }
    .buttonStyle(.plain)
    // BREATHE FOR A FEW SECONDS on each new inbound message — a single pulse was too easy to miss.
    // Orange (the app accent) reads far more than the old cyan; red would say "error". onChange fires
    // per message, so a burst keeps re-arming the window.
    .onChange(of: activity) { _, _ in
      WKInterfaceDevice.current().play(.notification)
      lit = true
      // ~4.5s of gentle in/out (8 half-cycles ≈ 0.55s each). Odd/even count doesn't matter — we hard-reset below.
      withAnimation(.easeInOut(duration: 0.55).repeatCount(8, autoreverses: true)) { pulse = true }
      DispatchQueue.main.asyncAfter(deadline: .now() + 4.6) {
        pulse = false
        withAnimation(.easeInOut(duration: 0.6)) { lit = false }
      }
    }
  }
}

/// The chat sheet: the recent conversation up top, then tap-to-send canned replies, the Jr "why terse"
/// line, and the callsign/chat-name editor. No wrist keyboard — everything is a tap.
struct ChatSheet: View {
  @EnvironmentObject var link: WatchLink
  @Environment(\.dismiss) private var dismiss
  @State private var callsign = ChatIdentity.name
  @FocusState private var editingName: Bool

  private var messages: [ChatLine] { link.chatLog }

  var body: some View {
    ScrollViewReader { proxy in
      ScrollView {
        VStack(alignment: .leading, spacing: 8) {
          header

          // Conversation
          if messages.isEmpty {
            Text("No messages yet. Say hello 👋")
              .font(.system(size: 12)).foregroundStyle(.white.opacity(0.4))
              .frame(maxWidth: .infinity, alignment: .center).padding(.vertical, 6)
          } else {
            ForEach(messages) { m in bubble(m) }
          }
          Color.clear.frame(height: 1).id("BOTTOM")

          Divider().background(.white.opacity(0.15))

          // Canned replies — tap to send
          Text("TAP TO SEND").font(.system(size: 9, weight: .bold)).foregroundStyle(.white.opacity(0.4))
          sendChip(Canned.jr, tint: .orange)
          ForEach(cannedForBackend, id: \.self) { c in sendChip(c, tint: .cyan) }

          Divider().background(.white.opacity(0.15)).padding(.top, 2)
          nameEditor
        }
        .padding(.horizontal, 6).padding(.bottom, 10)
      }
      .onChange(of: messages.count) { _, _ in withAnimation { proxy.scrollTo("BOTTOM", anchor: .bottom) } }
      .onAppear { proxy.scrollTo("BOTTOM", anchor: .bottom) }
    }
    .navigationTitle("Chat")
  }

  // OWRX = profile-switch etiquette; FM-DX = tuning etiquette (one shared tuner).
  private var cannedForBackend: [String] { link.isFmDx ? Canned.fmdx : Canned.owrx }

  private var header: some View {
    HStack(spacing: 6) {
      Image(systemName: "bubble.left.and.bubble.right.fill").foregroundStyle(.cyan)
      Text("Server chat").font(.headline)
      Spacer()
      if link.clients > 0 {
        HStack(spacing: 3) {
          Image(systemName: "person.2.fill").font(.system(size: 11))
          Text("\(link.clients)").font(.system(size: 12, weight: .semibold)).monospacedDigit()
        }.foregroundStyle(.white.opacity(0.6))
      }
    }.padding(.top, 2)
  }

  private func bubble(_ m: ChatLine) -> some View {
    VStack(alignment: m.mine ? .trailing : .leading, spacing: 1) {
      Text(m.name).font(.system(size: 9, weight: .semibold))
        .foregroundStyle(m.mine ? .orange.opacity(0.9) : .cyan.opacity(0.85))
      Text(m.text).font(.system(size: 13)).foregroundStyle(.white)
        .fixedSize(horizontal: false, vertical: true)
        .padding(.horizontal, 8).padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 9).fill(m.mine ? .orange.opacity(0.18) : .white.opacity(0.1)))
    }
    .frame(maxWidth: .infinity, alignment: m.mine ? .trailing : .leading)
  }

  private func sendChip(_ text: String, tint: Color) -> some View {
    Button {
      link.sendChat(text)
      WKInterfaceDevice.current().play(.click)
    } label: {
      Text(text).font(.system(size: 12)).foregroundStyle(.white)
        .frame(maxWidth: .infinity, alignment: .leading)
        .fixedSize(horizontal: false, vertical: true)
        .padding(.horizontal, 9).padding(.vertical, 7)
        .background(RoundedRectangle(cornerRadius: 9).fill(tint.opacity(0.16)))
        .overlay(RoundedRectangle(cornerRadius: 9).stroke(tint.opacity(0.35), lineWidth: 1))
    }.buttonStyle(.plain)
  }

  private var nameEditor: some View {
    VStack(alignment: .leading, spacing: 3) {
      Text("CALLSIGN / CHAT NAME").font(.system(size: 9, weight: .bold)).foregroundStyle(.white.opacity(0.4))
      TextField(ChatIdentity.fallback, text: $callsign)
        .font(.system(size: 14)).autocorrectionDisabled().focused($editingName)
        .onChange(of: callsign) { _, v in ChatIdentity.name = v }
      Text("Used for every server's chat, so you're never Anonymous.")
        .font(.system(size: 10)).foregroundStyle(.white.opacity(0.4))
    }
  }
}

// ── BEGIN GENERATED by scripts/gen-chat-phrases.mjs from i18n/chat/*.json — DO NOT EDIT: edit the JSON and run it ──
/// ★★★ THE CANNED CHAT'S WORDS, ONE TABLE PER LANGUAGE — the same files the phone and the browser are built from.
/// The ids never change with the language; only the words do. `CannedDial` picks the table once, from the watch's
/// own language list.
enum ChatWords {
  /// ★ The answers and their band ("" = either). Everything else is always offered.
  static let answerBand: [String: String] = [
    "not_sure": "", "is_wefax": "hf", "is_rtty": "hf", "is_navtex": "hf", "is_sstv": "hf", "is_ft8": "hf", "is_cw": "hf", "is_ssb": "hf", "is_drm": "hf", "is_stanag": "hf", "is_ale": "hf", "is_hfdl": "hf", "is_codar": "hf", "is_oth": "hf", "is_time": "hf", "is_dmr": "vhf", "is_dstar": "vhf", "is_p25": "vhf", "is_nxdn": "vhf", "is_dpmr": "vhf", "is_pocsag": "vhf", "is_aprs": "vhf", "is_adsb": "vhf", "is_acars": "vhf", "is_ais": "vhf", "is_apt": "vhf", "is_fm_bc": "vhf"
  ]
  /// A language subtag ("pt", "de") → its table. Anything not here is English.
  static let langOf: [String: String] = ["en": "en", "de": "de", "es": "es", "fr": "fr", "it": "it", "nl": "nl", "pl": "pl", "pt": "pt-BR"]
  /// A listener's country (the server's "cc") → its language, where it has only one main language.
  static let countryLang: [String: String] = ["DE": "de", "AT": "de", "LI": "de", "ES": "es", "MX": "es", "AR": "es", "CO": "es", "CL": "es", "PE": "es", "VE": "es", "UY": "es", "EC": "es", "BO": "es", "PY": "es", "CR": "es", "PA": "es", "DO": "es", "GT": "es", "HN": "es", "SV": "es", "NI": "es", "CU": "es", "PR": "es", "FR": "fr", "LU": "fr", "MC": "fr", "IT": "it", "SM": "it", "VA": "it", "NL": "nl", "SR": "nl", "PL": "pl", "BR": "pt-BR", "PT": "pt-BR", "AO": "pt-BR", "MZ": "pt-BR"]
  // ── English ──
  static let en: [(id: String, text: String)] = [
    ("ask_tune", "Can I tune?"),
    ("anyone_using", "Anyone using this?"),
    ("tuning_now", "Tuning now"),
    ("go_ahead", "Go ahead, tune"),
    ("please_hold", "Please hold — chasing DX"),
    ("yes_go_ahead", "Yes, on it — go ahead"),
    ("yes_hold", "Yes, on it — please hold"),
    ("listening_few_min", "Listening — a few more minutes?"),
    ("mid_decode", "Mid-decode — please wait"),
    ("decoding_10min", "Decoding — about 10 min"),
    ("decode_done", "Decode done — all yours"),
    ("wont_tune", "OK, I won't tune yet"),
    ("all_yours", "Done — all yours"),
    ("thanks", "Thanks!"),
    ("sorry", "Sorry, didn't realise!"),
    ("tune_back", "Tuning back — I was listening to that"),
    ("hello", "Hello everyone!"),
    ("just_scanning", "Just scanning"),
    ("what_is_this", "Anyone know what this is?"),
    ("sounds_awesome", "This sounds awesome!"),
    ("sounds_great", "This sounds great"),
    ("sounds_interesting", "This sounds interesting"),
    ("sounds_weird", "This sounds weird"),
    ("sounds_distorted", "This sounds distorted"),
    ("sounds_bad", "This sounds bad"),
    ("nice_catch", "Nice catch!"),
    ("not_my_music", "Not my kind of music"),
    ("good_conditions", "Great conditions today"),
    ("poor_conditions", "Poor conditions today"),
    ("off_73", "Off now — 73!"),
    ("not_sure", "Not sure, sorry"),
    ("is_wefax", "It's WEFAX (weather fax)"),
    ("is_rtty", "It's RTTY"),
    ("is_navtex", "It's NAVTEX"),
    ("is_sstv", "It's SSTV (slow-scan TV)"),
    ("is_ft8", "It's FT8"),
    ("is_cw", "It's Morse (CW)"),
    ("is_ssb", "It's SSB voice"),
    ("is_drm", "It's DRM (digital radio)"),
    ("is_stanag", "It's STANAG (military data)"),
    ("is_ale", "It's ALE"),
    ("is_hfdl", "It's HFDL (aircraft data)"),
    ("is_codar", "It's CODAR (ocean radar)"),
    ("is_oth", "It's over-the-horizon radar"),
    ("is_time", "It's a time signal"),
    ("is_dmr", "It's DMR"),
    ("is_dstar", "It's D-STAR"),
    ("is_p25", "It's P25"),
    ("is_nxdn", "It's NXDN"),
    ("is_dpmr", "It's dPMR"),
    ("is_pocsag", "It's POCSAG (pager)"),
    ("is_aprs", "It's APRS"),
    ("is_adsb", "It's ADS-B (aircraft)"),
    ("is_acars", "It's ACARS (aircraft data)"),
    ("is_ais", "It's AIS (ships)"),
    ("is_apt", "It's a weather satellite (APT)"),
    ("is_fm_bc", "It's an FM broadcast station"),
  ]
  static let enChrome: [String: String] = [
    "chatTitle": "CHAT",
    "you": "You",
    "user": "User {n}",
    "admin": "{who} (admin)",
    "listening": "{n} listening",
    "youDecoding": "you are decoding",
    "userDecoding": "{who} is decoding",
    "youTunedLast": "you tuned last",
    "userTuning": "{who} is tuning",
    "watchOwnerTunes": "the owner tunes this one",
    "watchNobody": "nobody is tuning",
    "bannerAwaiting": "AWAITING A REPLY",
    "watchAskedToWait": "ASKED TO WAIT",
    "bannerAskBefore": "ASK BEFORE TUNING",
    "watchAskSub": "Ask in chat — others are listening",
    "watchAskOpenSub": "No answer in 15 s and it's yours",
    "watchAskNoSub": "Yours in 5 min at most",
    "watchEmpty": "Nobody has said anything yet.",
  ]
  // ── Deutsch ──
  static let de: [(id: String, text: String)] = [
    ("ask_tune", "Darf ich die Frequenz wechseln?"),
    ("anyone_using", "Nutzt den gerade jemand?"),
    ("tuning_now", "Ich wechsle jetzt"),
    ("go_ahead", "Nur zu, wechsle ruhig"),
    ("please_hold", "Bitte warten — ich jage DX"),
    ("yes_go_ahead", "Ja, höre zu — nur zu"),
    ("yes_hold", "Ja, ich höre zu — bitte noch warten"),
    ("listening_few_min", "Höre zu — noch ein paar Minuten?"),
    ("mid_decode", "Ich dekodiere gerade — kannst du warten?"),
    ("decoding_10min", "Dekodiere — etwa 10 Minuten"),
    ("decode_done", "Dekodierung fertig — gehört dir"),
    ("wont_tune", "OK, ich wechsle noch nicht"),
    ("all_yours", "Fertig — gehört dir"),
    ("thanks", "Danke!"),
    ("sorry", "Sorry, hab's nicht gemerkt!"),
    ("tune_back", "Ich stelle zurück — das habe ich gehört"),
    ("hello", "Hallo zusammen!"),
    ("just_scanning", "Schaue nur, was so los ist"),
    ("what_is_this", "Weiß jemand, was das ist?"),
    ("sounds_awesome", "Klingt klasse!"),
    ("sounds_great", "Klingt super"),
    ("sounds_interesting", "Klingt interessant"),
    ("sounds_weird", "Klingt komisch"),
    ("sounds_distorted", "Klingt verzerrt"),
    ("sounds_bad", "Klingt schlecht"),
    ("nice_catch", "Schöner Fang!"),
    ("not_my_music", "Die Musik ist nicht so mein Fall"),
    ("good_conditions", "Die Condx sind heute super"),
    ("poor_conditions", "Die Condx sind heute mies"),
    ("off_73", "Bin dann mal weg — 73!"),
    ("not_sure", "Keine Ahnung, sorry"),
    ("is_wefax", "Das ist WEFAX (Wetterfax)"),
    ("is_rtty", "Das ist RTTY"),
    ("is_navtex", "Das ist NAVTEX"),
    ("is_sstv", "Das ist SSTV (Slow-Scan-TV)"),
    ("is_ft8", "Das ist FT8"),
    ("is_cw", "Das ist Morse (CW)"),
    ("is_ssb", "Das ist SSB-Sprechfunk"),
    ("is_drm", "Das ist DRM (Digitalradio)"),
    ("is_stanag", "Das ist STANAG (Militärdaten)"),
    ("is_ale", "Das ist ALE"),
    ("is_hfdl", "Das ist HFDL (Flugzeugdaten)"),
    ("is_codar", "Das ist CODAR (Meeresradar)"),
    ("is_oth", "Das ist ein Überhorizontradar"),
    ("is_time", "Das ist ein Zeitsignal"),
    ("is_dmr", "Das ist DMR"),
    ("is_dstar", "Das ist D-STAR"),
    ("is_p25", "Das ist P25"),
    ("is_nxdn", "Das ist NXDN"),
    ("is_dpmr", "Das ist dPMR"),
    ("is_pocsag", "Das ist POCSAG (Pager)"),
    ("is_aprs", "Das ist APRS"),
    ("is_adsb", "Das ist ADS-B (Flugzeuge)"),
    ("is_acars", "Das ist ACARS (Flugzeugdaten)"),
    ("is_ais", "Das ist AIS (Schiffe)"),
    ("is_apt", "Das ist ein Wettersatellit (APT)"),
    ("is_fm_bc", "Das ist ein FM-Radiosender"),
  ]
  static let deChrome: [String: String] = [
    "chatTitle": "CHAT",
    "you": "Du",
    "user": "Nutzer {n}",
    "admin": "{who} (Admin)",
    "listening": "Zuhörer: {n}",
    "youDecoding": "du dekodierst",
    "userDecoding": "{who} dekodiert",
    "youTunedLast": "du hast zuletzt abgestimmt",
    "userTuning": "{who} stimmt ab",
    "watchOwnerTunes": "der Besitzer stimmt ab",
    "watchNobody": "niemand stimmt ab",
    "bannerAwaiting": "WARTE AUF ANTWORT",
    "watchAskedToWait": "BITTE WARTEN",
    "bannerAskBefore": "VOR DEM ABSTIMMEN FRAGEN",
    "watchAskSub": "Im Chat fragen — andere hören zu",
    "watchAskOpenSub": "Keine Antwort in 15 s, dann deiner",
    "watchAskNoSub": "Spätestens in 5 Min. deiner",
    "watchEmpty": "Noch hat niemand etwas gesagt.",
  ]
  // ── Español ──
  static let es: [(id: String, text: String)] = [
    ("ask_tune", "¿Puedo sintonizar?"),
    ("anyone_using", "¿Alguien lo está usando?"),
    ("tuning_now", "Sintonizando ahora"),
    ("go_ahead", "Adelante, sintoniza"),
    ("please_hold", "Espera, por favor — busco DX"),
    ("yes_go_ahead", "Sí, escuchando — adelante"),
    ("yes_hold", "Sí, escuchando — espera"),
    ("listening_few_min", "Escuchando — ¿unos minutos más?"),
    ("mid_decode", "Decodificando — espera"),
    ("decoding_10min", "Decodificando — unos 10 minutos"),
    ("decode_done", "Terminado — todo tuyo"),
    ("wont_tune", "OK, todavía no sintonizo"),
    ("all_yours", "Listo — todo tuyo"),
    ("thanks", "¡Gracias!"),
    ("sorry", "¡Perdón, no me di cuenta!"),
    ("tune_back", "Vuelvo — estaba escuchando eso"),
    ("hello", "¡Hola a todos!"),
    ("just_scanning", "Solo explorando"),
    ("what_is_this", "¿Alguien sabe qué es esto?"),
    ("sounds_awesome", "¡Suena increíble!"),
    ("sounds_great", "Suena genial"),
    ("sounds_interesting", "Suena interesante"),
    ("sounds_weird", "Suena raro"),
    ("sounds_distorted", "Suena distorsionado"),
    ("sounds_bad", "Suena mal"),
    ("nice_catch", "¡Buena escucha!"),
    ("not_my_music", "Esta música no es lo mío"),
    ("good_conditions", "Muy buena propagación hoy"),
    ("poor_conditions", "Propagación fatal hoy"),
    ("off_73", "Me voy — ¡73!"),
    ("not_sure", "No estoy seguro, perdón"),
    ("is_wefax", "Es WEFAX (fax meteorológico)"),
    ("is_rtty", "Es RTTY"),
    ("is_navtex", "Es NAVTEX"),
    ("is_sstv", "Es SSTV (TV de barrido lento)"),
    ("is_ft8", "Es FT8"),
    ("is_cw", "Es Morse (CW)"),
    ("is_ssb", "Es voz en SSB"),
    ("is_drm", "Es DRM (radio digital)"),
    ("is_stanag", "Es STANAG (datos militares)"),
    ("is_ale", "Es ALE"),
    ("is_hfdl", "Es HFDL (datos de aviones)"),
    ("is_codar", "Es CODAR (radar oceánico)"),
    ("is_oth", "Es un radar transhorizonte"),
    ("is_time", "Es una señal horaria"),
    ("is_dmr", "Es DMR"),
    ("is_dstar", "Es D-STAR"),
    ("is_p25", "Es P25"),
    ("is_nxdn", "Es NXDN"),
    ("is_dpmr", "Es dPMR"),
    ("is_pocsag", "Es POCSAG (buscapersonas)"),
    ("is_aprs", "Es APRS"),
    ("is_adsb", "Es ADS-B (aviones)"),
    ("is_acars", "Es ACARS (datos de aviones)"),
    ("is_ais", "Es AIS (barcos)"),
    ("is_apt", "Es un satélite meteorológico (APT)"),
    ("is_fm_bc", "Es una emisora de FM"),
  ]
  static let esChrome: [String: String] = [
    "chatTitle": "CHAT",
    "you": "Tú",
    "user": "Usuario {n}",
    "admin": "{who} (admin)",
    "listening": "{n} escuchando",
    "youDecoding": "estás decodificando",
    "userDecoding": "{who} está decodificando",
    "youTunedLast": "sintonizaste tú el último",
    "userTuning": "{who} está sintonizando",
    "watchOwnerTunes": "lo sintoniza el dueño",
    "watchNobody": "nadie sintoniza",
    "bannerAwaiting": "ESPERANDO RESPUESTA",
    "watchAskedToWait": "TE PIDIERON ESPERAR",
    "bannerAskBefore": "PREGUNTA ANTES",
    "watchAskSub": "Pregunta en el chat — hay oyentes",
    "watchAskOpenSub": "Sin respuesta en 15 s, es tuyo",
    "watchAskNoSub": "Tuyo en 5 min como mucho",
    "watchEmpty": "Nadie ha dicho nada todavía.",
  ]
  // ── Français ──
  static let fr: [(id: String, text: String)] = [
    ("ask_tune", "Je peux syntoniser ?"),
    ("anyone_using", "Quelqu'un l'utilise ?"),
    ("tuning_now", "Je change de fréquence"),
    ("go_ahead", "Vas-y, syntonise"),
    ("please_hold", "Attends, je chasse le DX"),
    ("yes_go_ahead", "Oui, j'écoute — vas-y"),
    ("yes_hold", "Oui, j'écoute — attends un peu"),
    ("listening_few_min", "J'écoute — encore quelques min ?"),
    ("mid_decode", "Je décode — attends un peu"),
    ("decoding_10min", "Décodage — env. 10 min"),
    ("decode_done", "Décodage terminé — à toi"),
    ("wont_tune", "OK, je ne change pas encore"),
    ("all_yours", "Terminé — à toi"),
    ("thanks", "Merci !"),
    ("sorry", "Désolé, je n'avais pas vu !"),
    ("tune_back", "Je reviens dessus — j'écoutais ça"),
    ("hello", "Salut tout le monde !"),
    ("just_scanning", "Je balaye la bande"),
    ("what_is_this", "Quelqu'un sait ce que c'est ?"),
    ("sounds_awesome", "Le son est génial !"),
    ("sounds_great", "Le son est super"),
    ("sounds_interesting", "Le son est intéressant"),
    ("sounds_weird", "Le son est bizarre"),
    ("sounds_distorted", "Le son est distordu"),
    ("sounds_bad", "Le son est mauvais"),
    ("nice_catch", "Joli DX !"),
    ("not_my_music", "Pas trop mon style de musique"),
    ("good_conditions", "Super propagation aujourd'hui"),
    ("poor_conditions", "Mauvaise propagation aujourd'hui"),
    ("off_73", "Je file — 73 !"),
    ("not_sure", "Je ne sais pas, désolé"),
    ("is_wefax", "C'est du WEFAX (fax météo)"),
    ("is_rtty", "C'est du RTTY"),
    ("is_navtex", "C'est du NAVTEX"),
    ("is_sstv", "C'est de la SSTV (TV à balayage lent)"),
    ("is_ft8", "C'est du FT8"),
    ("is_cw", "C'est du Morse (CW)"),
    ("is_ssb", "C'est de la phonie SSB"),
    ("is_drm", "C'est du DRM (radio numérique)"),
    ("is_stanag", "C'est du STANAG (données militaires)"),
    ("is_ale", "C'est de l'ALE"),
    ("is_hfdl", "C'est du HFDL (données avion)"),
    ("is_codar", "C'est un CODAR (radar océanique)"),
    ("is_oth", "C'est un radar transhorizon"),
    ("is_time", "C'est un signal horaire"),
    ("is_dmr", "C'est du DMR"),
    ("is_dstar", "C'est du D-STAR"),
    ("is_p25", "C'est du P25"),
    ("is_nxdn", "C'est du NXDN"),
    ("is_dpmr", "C'est du dPMR"),
    ("is_pocsag", "C'est du POCSAG (bipeur)"),
    ("is_aprs", "C'est de l'APRS"),
    ("is_adsb", "C'est de l'ADS-B (avions)"),
    ("is_acars", "C'est de l'ACARS (données avion)"),
    ("is_ais", "C'est de l'AIS (navires)"),
    ("is_apt", "C'est un satellite météo (APT)"),
    ("is_fm_bc", "C'est une station de radio FM"),
  ]
  static let frChrome: [String: String] = [
    "chatTitle": "CHAT",
    "you": "Toi",
    "user": "Utilisateur {n}",
    "admin": "{who} (admin)",
    "listening": "{n} à l'écoute",
    "youDecoding": "tu décodes",
    "userDecoding": "{who} décode",
    "youTunedLast": "tu as syntonisé en dernier",
    "userTuning": "{who} syntonise",
    "watchOwnerTunes": "le propriétaire règle",
    "watchNobody": "personne ne syntonise",
    "bannerAwaiting": "EN ATTENTE DE RÉPONSE",
    "watchAskedToWait": "ATTENTE DEMANDÉE",
    "bannerAskBefore": "DEMANDE AVANT",
    "watchAskSub": "Demande dans le chat — d'autres écoutent",
    "watchAskOpenSub": "Sans réponse en 15 s, c'est à toi",
    "watchAskNoSub": "À toi dans 5 min au plus",
    "watchEmpty": "Personne n'a encore rien dit.",
  ]
  // ── Italiano ──
  static let it: [(id: String, text: String)] = [
    ("ask_tune", "Posso sintonizzare?"),
    ("anyone_using", "Qualcuno lo sta usando?"),
    ("tuning_now", "Sto sintonizzando"),
    ("go_ahead", "Vai pure, sintonizza"),
    ("please_hold", "Aspetta un attimo — sto cercando DX"),
    ("yes_go_ahead", "Sì, ascolto — vai pure"),
    ("yes_hold", "Sì, ascolto — aspetta"),
    ("listening_few_min", "Ascolto — ancora qualche minuto?"),
    ("mid_decode", "Decoder in funzione — puoi aspettare?"),
    ("decoding_10min", "Decodifica — circa 10 min"),
    ("decode_done", "Decodifica finita — è tutto tuo"),
    ("wont_tune", "OK, non sintonizzo ancora"),
    ("all_yours", "Fatto — è tutto tuo"),
    ("thanks", "Grazie!"),
    ("sorry", "Scusa, non me n'ero accorto!"),
    ("tune_back", "Torno — stavo ascoltando quella"),
    ("hello", "Ciao a tutti!"),
    ("just_scanning", "Sto solo girando"),
    ("what_is_this", "Qualcuno sa cos'è questo?"),
    ("sounds_awesome", "Si sente alla grande!"),
    ("sounds_great", "Si sente benissimo"),
    ("sounds_interesting", "Interessante questo segnale"),
    ("sounds_weird", "Si sente strano"),
    ("sounds_distorted", "Si sente distorto"),
    ("sounds_bad", "Si sente male"),
    ("nice_catch", "Bel colpo!"),
    ("not_my_music", "Non è proprio il mio genere"),
    ("good_conditions", "Oggi la propagazione è ottima"),
    ("poor_conditions", "Oggi la propagazione è scarsa"),
    ("off_73", "Chiudo — 73!"),
    ("not_sure", "Non lo so, scusa"),
    ("is_wefax", "È WEFAX (fax meteo)"),
    ("is_rtty", "È RTTY"),
    ("is_navtex", "È NAVTEX"),
    ("is_sstv", "È SSTV (TV a scansione lenta)"),
    ("is_ft8", "È FT8"),
    ("is_cw", "È Morse (CW)"),
    ("is_ssb", "È fonia in SSB"),
    ("is_drm", "È DRM (radio digitale)"),
    ("is_stanag", "È STANAG (dati militari)"),
    ("is_ale", "È ALE"),
    ("is_hfdl", "È HFDL (dati aerei)"),
    ("is_codar", "È CODAR (radar oceanico)"),
    ("is_oth", "È un radar oltre l'orizzonte"),
    ("is_time", "È un segnale orario"),
    ("is_dmr", "È DMR"),
    ("is_dstar", "È D-STAR"),
    ("is_p25", "È P25"),
    ("is_nxdn", "È NXDN"),
    ("is_dpmr", "È dPMR"),
    ("is_pocsag", "È POCSAG (cercapersone)"),
    ("is_aprs", "È APRS"),
    ("is_adsb", "È ADS-B (aerei)"),
    ("is_acars", "È ACARS (dati aerei)"),
    ("is_ais", "È AIS (navi)"),
    ("is_apt", "È un satellite meteo (APT)"),
    ("is_fm_bc", "È una stazione radio FM"),
  ]
  static let itChrome: [String: String] = [
    "chatTitle": "CHAT",
    "you": "Tu",
    "user": "Utente {n}",
    "admin": "{who} (admin)",
    "listening": "{n} in ascolto",
    "youDecoding": "stai decodificando",
    "userDecoding": "{who} sta decodificando",
    "youTunedLast": "hai sintonizzato tu per ultimo",
    "userTuning": "{who} sta sintonizzando",
    "watchOwnerTunes": "lo sintonizza il proprietario",
    "watchNobody": "nessuno sintonizza",
    "bannerAwaiting": "IN ATTESA DI RISPOSTA",
    "watchAskedToWait": "DEVI ASPETTARE",
    "bannerAskBefore": "CHIEDI PRIMA",
    "watchAskSub": "Chiedi in chat — altri ascoltano",
    "watchAskOpenSub": "Nessuna risposta in 15 s ed è tuo",
    "watchAskNoSub": "Tuo entro 5 min al massimo",
    "watchEmpty": "Nessuno ha ancora detto niente.",
  ]
  // ── Nederlands ──
  static let nl: [(id: String, text: String)] = [
    ("ask_tune", "Mag ik afstemmen?"),
    ("anyone_using", "Gebruikt iemand deze?"),
    ("tuning_now", "Ik stem nu af"),
    ("go_ahead", "Ga je gang, stem af"),
    ("please_hold", "Even wachten — ik zit op DX te jagen"),
    ("yes_go_ahead", "Ja, luister — ga je gang"),
    ("yes_hold", "Ja, ik luister — even wachten graag"),
    ("listening_few_min", "Luister — nog een paar minuten?"),
    ("mid_decode", "Decoder draait — kun je even wachten?"),
    ("decoding_10min", "Decoderen — zo'n 10 min"),
    ("decode_done", "Klaar — helemaal van jou"),
    ("wont_tune", "Oké, ik stem nog niet af"),
    ("all_yours", "Klaar — helemaal van jou"),
    ("thanks", "Bedankt!"),
    ("sorry", "Sorry, had het niet door!"),
    ("tune_back", "Ik stem terug — ik luisterde daarnaar"),
    ("hello", "Hallo allemaal!"),
    ("just_scanning", "Even rondkijken"),
    ("what_is_this", "Weet iemand wat dit is?"),
    ("sounds_awesome", "Klinkt geweldig!"),
    ("sounds_great", "Klinkt goed"),
    ("sounds_interesting", "Klinkt interessant"),
    ("sounds_weird", "Klinkt raar"),
    ("sounds_distorted", "Klinkt vervormd"),
    ("sounds_bad", "Klinkt slecht"),
    ("nice_catch", "Mooie vangst!"),
    ("not_my_music", "Niet echt mijn ding"),
    ("good_conditions", "De condities zijn top vandaag"),
    ("poor_conditions", "Slechte condities vandaag"),
    ("off_73", "Ik ben weg — 73!"),
    ("not_sure", "Geen idee, sorry"),
    ("is_wefax", "Het is WEFAX (weerfax)"),
    ("is_rtty", "Het is RTTY"),
    ("is_navtex", "Het is NAVTEX"),
    ("is_sstv", "Het is SSTV (slow-scan-tv)"),
    ("is_ft8", "Het is FT8"),
    ("is_cw", "Het is morse (CW)"),
    ("is_ssb", "Het is SSB-spraak"),
    ("is_drm", "Het is DRM (digitale radio)"),
    ("is_stanag", "Het is STANAG (militaire data)"),
    ("is_ale", "Het is ALE"),
    ("is_hfdl", "Het is HFDL (vliegtuigdata)"),
    ("is_codar", "Het is CODAR (oceaanradar)"),
    ("is_oth", "Het is over-de-horizonradar"),
    ("is_time", "Het is een tijdsignaal"),
    ("is_dmr", "Het is DMR"),
    ("is_dstar", "Het is D-STAR"),
    ("is_p25", "Het is P25"),
    ("is_nxdn", "Het is NXDN"),
    ("is_dpmr", "Het is dPMR"),
    ("is_pocsag", "Het is POCSAG (pager)"),
    ("is_aprs", "Het is APRS"),
    ("is_adsb", "Het is ADS-B (vliegtuigen)"),
    ("is_acars", "Het is ACARS (vliegtuigdata)"),
    ("is_ais", "Het is AIS (schepen)"),
    ("is_apt", "Het is een weersatelliet (APT)"),
    ("is_fm_bc", "Het is een FM-zender"),
  ]
  static let nlChrome: [String: String] = [
    "chatTitle": "CHAT",
    "you": "Jij",
    "user": "Gebruiker {n}",
    "admin": "{who} (admin)",
    "listening": "luisteraars: {n}",
    "youDecoding": "jij decodeert",
    "userDecoding": "{who} decodeert",
    "youTunedLast": "jij stemde als laatste af",
    "userTuning": "{who} stemt af",
    "watchOwnerTunes": "de eigenaar stemt af",
    "watchNobody": "niemand stemt af",
    "bannerAwaiting": "WACHT OP ANTWOORD",
    "watchAskedToWait": "WACHTEN GEVRAAGD",
    "bannerAskBefore": "VRAAG VOOR JE AFSTEMT",
    "watchAskSub": "Vraag het in de chat — anderen luisteren",
    "watchAskOpenSub": "Geen antwoord in 15 s en hij is van jou",
    "watchAskNoSub": "Uiterlijk over 5 min van jou",
    "watchEmpty": "Nog niemand heeft iets gezegd.",
  ]
  // ── Polski ──
  static let pl: [(id: String, text: String)] = [
    ("ask_tune", "Mogę przestroić?"),
    ("anyone_using", "Ktoś tego słucha?"),
    ("tuning_now", "Przestrajam"),
    ("go_ahead", "Śmiało, przestrajaj"),
    ("please_hold", "Chwila — poluję na DX"),
    ("yes_go_ahead", "Tak, słucham — śmiało"),
    ("yes_hold", "Tak, słucham — poczekaj chwilę"),
    ("listening_few_min", "Słucham — jeszcze kilka minut?"),
    ("mid_decode", "Mam włączony dekoder — możesz poczekać?"),
    ("decoding_10min", "Dekoduję — jakieś 10 minut"),
    ("decode_done", "Dekodowanie skończone — cały twój"),
    ("wont_tune", "OK, jeszcze nie przestrajam"),
    ("all_yours", "Gotowe — cały twój"),
    ("thanks", "Dzięki!"),
    ("sorry", "Sorry, umknęło mi!"),
    ("tune_back", "Wracam — tamtego właśnie słucham"),
    ("hello", "Cześć wszystkim!"),
    ("just_scanning", "Tylko skanuję, co słychać"),
    ("what_is_this", "Ktoś wie, co to jest?"),
    ("sounds_awesome", "Brzmi super!"),
    ("sounds_great", "Brzmi świetnie"),
    ("sounds_interesting", "Brzmi ciekawie"),
    ("sounds_weird", "Brzmi dziwnie"),
    ("sounds_distorted", "Dźwięk jest zniekształcony"),
    ("sounds_bad", "Brzmi źle"),
    ("nice_catch", "Świetny odbiór!"),
    ("not_my_music", "To nie moja muzyka"),
    ("good_conditions", "Propagacja dziś super"),
    ("poor_conditions", "Propagacja dziś słaba"),
    ("off_73", "Spadam — 73!"),
    ("not_sure", "Nie wiem, sorry"),
    ("is_wefax", "To WEFAX (faks pogodowy)"),
    ("is_rtty", "To RTTY"),
    ("is_navtex", "To NAVTEX"),
    ("is_sstv", "To SSTV (telewizja wolnoskanowa)"),
    ("is_ft8", "To FT8"),
    ("is_cw", "To Morse (CW)"),
    ("is_ssb", "To fonia SSB"),
    ("is_drm", "To DRM (radio cyfrowe)"),
    ("is_stanag", "To STANAG (dane wojskowe)"),
    ("is_ale", "To ALE"),
    ("is_hfdl", "To HFDL (dane lotnicze)"),
    ("is_codar", "To CODAR (radar oceaniczny)"),
    ("is_oth", "To radar pozahoryzontalny"),
    ("is_time", "To sygnał czasu"),
    ("is_dmr", "To DMR"),
    ("is_dstar", "To D-STAR"),
    ("is_p25", "To P25"),
    ("is_nxdn", "To NXDN"),
    ("is_dpmr", "To dPMR"),
    ("is_pocsag", "To POCSAG (pager)"),
    ("is_aprs", "To APRS"),
    ("is_adsb", "To ADS-B (samoloty)"),
    ("is_acars", "To ACARS (dane lotnicze)"),
    ("is_ais", "To AIS (statki)"),
    ("is_apt", "To satelita pogodowy (APT)"),
    ("is_fm_bc", "To stacja radiowa FM"),
  ]
  static let plChrome: [String: String] = [
    "chatTitle": "CZAT",
    "you": "Ty",
    "user": "Użytkownik {n}",
    "admin": "{who} (admin)",
    "listening": "słuchający: {n}",
    "youDecoding": "dekodujesz",
    "userDecoding": "{who} dekoduje",
    "youTunedLast": "ostatnie strojenie: ty",
    "userTuning": "{who} stroi",
    "watchOwnerTunes": "stroi właściciel",
    "watchNobody": "nikt nie stroi",
    "bannerAwaiting": "CZEKAM NA ODPOWIEDŹ",
    "watchAskedToWait": "PROŚBA O CZEKANIE",
    "bannerAskBefore": "ZAPYTAJ PRZED STROJENIEM",
    "watchAskSub": "Zapytaj na czacie — inni słuchają",
    "watchAskOpenSub": "Brak odpowiedzi w 15 s — twój",
    "watchAskNoSub": "Twój najpóźniej za 5 min",
    "watchEmpty": "Nikt jeszcze nic nie napisał.",
  ]
  // ── Português (Brasil) ──
  static let pt_BR: [(id: String, text: String)] = [
    ("ask_tune", "Posso sintonizar?"),
    ("anyone_using", "Alguém está usando?"),
    ("tuning_now", "Sintonizando agora"),
    ("go_ahead", "Pode sintonizar"),
    ("please_hold", "Espera aí — tô caçando DX"),
    ("yes_go_ahead", "Sim, ouvindo — pode ir"),
    ("yes_hold", "Sim, ouvindo — espera aí"),
    ("listening_few_min", "Ouvindo — mais uns minutinhos?"),
    ("mid_decode", "Decodificando — espera aí"),
    ("decoding_10min", "Decodificando — uns 10 minutos"),
    ("decode_done", "Terminei — é todo seu"),
    ("wont_tune", "Beleza, não vou sintonizar ainda"),
    ("all_yours", "Pronto — é todo seu"),
    ("thanks", "Valeu!"),
    ("sorry", "Foi mal, não percebi!"),
    ("tune_back", "Voltando — eu tava ouvindo aquilo"),
    ("hello", "Olá, pessoal!"),
    ("just_scanning", "Só dando uma olhada"),
    ("what_is_this", "Alguém sabe o que é isso?"),
    ("sounds_awesome", "O som tá incrível!"),
    ("sounds_great", "O som tá ótimo"),
    ("sounds_interesting", "O som tá interessante"),
    ("sounds_weird", "O som tá estranho"),
    ("sounds_distorted", "O som tá distorcido"),
    ("sounds_bad", "O som tá ruim"),
    ("nice_catch", "Bela escuta!"),
    ("not_my_music", "Essa música não é muito a minha praia"),
    ("good_conditions", "Propagação ótima hoje"),
    ("poor_conditions", "Propagação fraca hoje"),
    ("off_73", "Tô saindo — 73!"),
    ("not_sure", "Não sei, desculpa"),
    ("is_wefax", "É WEFAX (fax meteorológico)"),
    ("is_rtty", "É RTTY"),
    ("is_navtex", "É NAVTEX"),
    ("is_sstv", "É SSTV (TV de varredura lenta)"),
    ("is_ft8", "É FT8"),
    ("is_cw", "É Morse (CW)"),
    ("is_ssb", "É voz em SSB"),
    ("is_drm", "É DRM (rádio digital)"),
    ("is_stanag", "É STANAG (dados militares)"),
    ("is_ale", "É ALE"),
    ("is_hfdl", "É HFDL (dados de aeronaves)"),
    ("is_codar", "É CODAR (radar oceânico)"),
    ("is_oth", "É radar além do horizonte"),
    ("is_time", "É um sinal horário"),
    ("is_dmr", "É DMR"),
    ("is_dstar", "É D-STAR"),
    ("is_p25", "É P25"),
    ("is_nxdn", "É NXDN"),
    ("is_dpmr", "É dPMR"),
    ("is_pocsag", "É POCSAG (pager)"),
    ("is_aprs", "É APRS"),
    ("is_adsb", "É ADS-B (aeronaves)"),
    ("is_acars", "É ACARS (dados de aeronaves)"),
    ("is_ais", "É AIS (navios)"),
    ("is_apt", "É um satélite meteorológico (APT)"),
    ("is_fm_bc", "É uma rádio FM"),
  ]
  static let pt_BRChrome: [String: String] = [
    "chatTitle": "CHAT",
    "you": "Você",
    "user": "Usuário {n}",
    "admin": "{who} (admin)",
    "listening": "{n} ouvindo",
    "youDecoding": "você está decodificando",
    "userDecoding": "{who} está decodificando",
    "youTunedLast": "você sintonizou por último",
    "userTuning": "{who} está sintonizando",
    "watchOwnerTunes": "quem sintoniza é o dono",
    "watchNobody": "ninguém sintonizando",
    "bannerAwaiting": "AGUARDANDO RESPOSTA",
    "watchAskedToWait": "PEDIRAM PARA ESPERAR",
    "bannerAskBefore": "PERGUNTE ANTES",
    "watchAskSub": "Peça no chat — tem gente ouvindo",
    "watchAskOpenSub": "Sem resposta em 15 s, é seu",
    "watchAskNoSub": "Seu em no máximo 5 min",
    "watchEmpty": "Ninguém disse nada ainda.",
  ]
  static let phrases: [String: [(id: String, text: String)]] = ["en": en, "de": de, "es": es, "fr": fr, "it": it, "nl": nl, "pl": pl, "pt-BR": pt_BR]
  static let chrome: [String: [String: String]] = ["en": enChrome, "de": deChrome, "es": esChrome, "fr": frChrome, "it": itChrome, "nl": nlChrome, "pl": plChrome, "pt-BR": pt_BRChrome]
}
// ── END GENERATED ──
