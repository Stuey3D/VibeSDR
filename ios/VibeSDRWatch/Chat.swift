import SwiftUI
import WatchKit

/// ONE saved identity, reused everywhere a chat/ident box exists — OWRX chat name, FM-DX chat name,
/// KiwiSDR `SET ident_user=`. Set once (here or in the picker), auto-fills every backend so a wrist
/// user is never "Anonymous". Shares the Kiwi ident key so the two can't drift apart.
enum ChatIdentity {
  static let key = "vibe.kiwi.ident"
  static var name: String {
    get {
      let v = (UserDefaults.standard.string(forKey: key) ?? "").trimmingCharacters(in: .whitespaces)
      return v.isEmpty ? "VibeSDR" : v
    }
    set {
      let v = newValue.trimmingCharacters(in: .whitespaces)
      UserDefaults.standard.set(v.isEmpty ? "VibeSDR" : v, forKey: key)
    }
  }
}

/// The tap-to-send canned messages. Short replies from a wrist read as gruff, so EVERY set opens with
/// the "why so terse" line — sent explicitly, never auto-prepended. Sending "switching now" does NOT
/// perform the switch: the message and the action stay two deliberate steps (the explicit-only etiquette).
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
  var tap: () -> Void

  @State private var pulse = false      // the in/out scale oscillation
  @State private var lit = false        // the orange highlight window (held for a few seconds)

  var body: some View {
    Button(action: tap) {
      HStack(spacing: 3) {
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
  @EnvironmentObject var link: SpikeLink
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
      TextField("VibeSDR", text: $callsign)
        .font(.system(size: 14)).autocorrectionDisabled().focused($editingName)
        .onChange(of: callsign) { _, v in ChatIdentity.name = v }
      Text("Used for every server's chat, so you're never Anonymous.")
        .font(.system(size: 10)).foregroundStyle(.white.opacity(0.4))
    }
  }
}
