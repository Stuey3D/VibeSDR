import SwiftUI
import WatchKit

/// ★★★ THE SHARED DIAL, ON A WRIST.
///
/// A VibeServer running unlocked with room for several listeners hands ONE tuner to everybody and
/// enforces nothing about who turns it — Stuart, 2026-08-20: *"the dial must be like FM-DX where
/// anybody can tune it, otherwise I would need to be on the server 24/7 to allow access to it."*
/// Two things follow, and this screen is both of them:
///
///   1. **A way to ask.** Twelve buttons, no keyboard, no names. The vocabulary is fixed and the
///      wire carries IDS, so there is no moderation to do, nothing to translate and nothing to
///      inject — which is the only reason a chat can exist on a receiver whose owner is asleep,
///      and the only reason it can exist on a watch at all.
///   2. **A way NOT to.** The dial is DISARMED by default. A watch in a coat sleeve turns its own
///      crown, and the cost of that on a shared receiver is a stranger's station changing under
///      them. Arming is a deliberate act and it expires.
///
/// ★★ PEOPLE ARE ORDINALS. "User 3" needs no account, no name box and no profanity list, and the
///    room is never handed a stranger's address. The server assigns the numbers; we only draw them.
struct DialChatView: View {
  // ★ Passed in, not @EnvironmentObject: the client is `link.vibe` and is OPTIONAL — it exists only
  //   on a VibeServer connection — so the caller unwraps it and this view can assume it.
  @ObservedObject var radio: UberClient
  @Environment(\.dismiss) private var dismiss

  var body: some View {
    ScrollView {
      VStack(spacing: 8) {

        // ── Who is here, and who moved it last ──────────────────────────────────
        // ★ WHO MOVED IT LAST, not who owns it — nobody owns it. Said plainly, because a frequency
        //   that changes under you with no explanation reads as the receiver glitching, and that is
        //   the one thing a shared dial must never look like.
        Text(roomLine)
          .font(.system(size: 11)).foregroundColor(.orange.opacity(0.85))
          .multilineTextAlignment(.center).frame(maxWidth: .infinity)

        // ── The arm switch ──────────────────────────────────────────────────────
        Button { radio.dialArmed.toggle() } label: {
          HStack(spacing: 6) {
            Image(systemName: radio.dialArmed ? "lock.open.fill" : "lock.fill")
              .font(.system(size: 13))
            VStack(alignment: .leading, spacing: 1) {
              Text(radio.dialArmed ? "DIAL ARMED" : "DIAL LOCKED")
                .font(.system(size: 11, weight: .bold))
              Text(radio.dialArmed
                   ? "You can tune · unlocks off after \(Int(UberClient.armMinutes)) min"
                   : "Tap to allow tuning")
                .font(.system(size: 9)).opacity(0.75).lineLimit(2)
            }
            Spacer()
          }
          .foregroundColor(radio.dialArmed ? .green : .white.opacity(0.8))
          .padding(.horizontal, 8).padding(.vertical, 6)
          .frame(maxWidth: .infinity)
          .background(RoundedRectangle(cornerRadius: 8)
            .fill((radio.dialArmed ? Color.green : Color.white).opacity(0.12)))
        }
        .buttonStyle(.plain)

        // ── The transcript ──────────────────────────────────────────────────────
        if radio.chatLines.isEmpty {
          Text("Nobody has said anything yet.")
            .font(.system(size: 10)).foregroundColor(.white.opacity(0.45))
            .padding(.vertical, 4)
        } else {
          VStack(alignment: .leading, spacing: 4) {
            // ★ Newest last, as a conversation reads. The list is capped in the client, so this
            //   never grows without bound on a receiver that has been busy all day.
            ForEach(radio.chatLines) { line in
              HStack(alignment: .top, spacing: 4) {
                Text(CannedDial.speaker(line.from, you: radio.dialYou))
                  .font(.system(size: 9, weight: .bold))
                  .foregroundColor(line.from == radio.dialYou ? .green : .orange)
                Text(CannedDial.text(line.phrase) ?? "")
                  .font(.system(size: 10)).foregroundColor(.white.opacity(0.9))
                Spacer(minLength: 0)
              }
            }
          }
          .frame(maxWidth: .infinity, alignment: .leading)
        }

        Divider().background(Color.white.opacity(0.15))

        // ── The phrases ─────────────────────────────────────────────────────────
        ForEach(CannedDial.all, id: \.id) { phrase in
          Button {
            radio.say(phrase.id)
            WKInterfaceDevice.current().play(.click)
            // ★ NO LOCAL ECHO. What everybody else sees is what the SERVER accepted, so waiting for
            //   it to come back is the only way this transcript matches theirs — and if flood
            //   control drops the phrase, nothing is shown that nobody received.
          } label: {
            Text(phrase.text)
              .font(.system(size: 11)).foregroundColor(.white)
              .frame(maxWidth: .infinity, alignment: .leading)
              .padding(.horizontal, 8).padding(.vertical, 6)
              .background(RoundedRectangle(cornerRadius: 8).fill(Color.white.opacity(0.10)))
          }
          .buttonStyle(.plain)
        }
      }
      .padding(.horizontal, 4)
    }
    .navigationTitle("CHAT")
    // ★ Opening the room IS reading it. The badge exists to say "something happened while you were
    //   looking at the waterfall", and it has done its job the moment you are here.
    .onAppear { radio.chatUnread = 0 }
  }

  private var roomLine: String {
    var bits = ["\(radio.dialListeners) listening"]
    if radio.dialMode == "spectator" {
      bits.append("the owner tunes this one")
    } else if radio.dialDecoding {
      bits.append(radio.dialMine ? "you are decoding" : "User \(radio.dialTuner) is decoding")
    } else if radio.dialTuner != 0 && radio.dialMine {
      bits.append("you tuned last")
    } else if radio.dialTuner != 0 {
      bits.append("User \(radio.dialTuner) is tuning")
    } else {
      bits.append("nobody is tuning")
    }
    return bits.joined(separator: " · ")
  }
}
