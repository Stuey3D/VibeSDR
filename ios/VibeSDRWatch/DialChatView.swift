import SwiftUI
import WatchKit

/// ★★★ THE SHARED DIAL, ON A WRIST — Buddy's copy.
///
/// ★★ THE SAME SCREEN AS Jr's, DELIBERATELY, and a separate file for the same reason the two apps
///    are separate: no shared module. What differs is only WHO acts — Jr speaks to the receiver,
///    Buddy asks the phone to.
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
  // ★ Buddy talks to the PHONE, never to the receiver — `link` is that connection, and it is
  //   always present, so unlike Jr's version there is nothing to unwrap.
  @EnvironmentObject var link: WatchLink
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

        // ── The arm switch ─────────────────────────────────────────────────────
        // ★★★ FM-DX's CONTROL, NOT A SECOND ONE. Same shape (the tune scale), same marks (green
        //   tick / red cross), same meaning — "may this watch move a dial other people are
        //   listening to". A different-looking switch for the identical question would be two
        //   lessons where one will do. Mirrors Jr; keep the two in step.
        // ★★ It stays HERE, on the chat page, deliberately: reaching for the tuner walks you past
        //   the place you would ask for it.
        Button {
          link.dialArmed.toggle()
          WKInterfaceDevice.current().play(link.dialArmed ? .start : .stop)
        } label: {
          HStack(spacing: 8) {
            TuneScaleGlyph().stroke(.white, style: StrokeStyle(lineWidth: 1.1, lineCap: .round))
              .frame(width: 18, height: 11)
              .overlay(alignment: .bottomTrailing) {
                Image(systemName: link.dialArmed ? "checkmark.circle.fill" : "xmark.circle.fill")
                  .font(.system(size: 8, weight: .bold))
                  .foregroundStyle(link.dialArmed ? .green : .red)
                  .background(Circle().fill(.black)).offset(x: 5, y: 4)
              }
              .frame(width: 36, height: 30)
            VStack(alignment: .leading, spacing: 1) {
              Text(link.dialArmed ? "TUNING ARMED" : "TUNING LOCKED")
                .font(.system(size: 11, weight: .bold))
              Text(link.dialArmed
                   ? "You can tune · disarms after \(Int(WatchLink.armMinutes)) min"
                   : "Tap to allow tuning")
                .font(.system(size: 9)).opacity(0.75).lineLimit(2)
            }
            Spacer()
          }
          .foregroundColor(link.dialArmed ? .green : .white.opacity(0.8))
          .padding(.horizontal, 8).padding(.vertical, 6)
          .frame(maxWidth: .infinity)
          .background(RoundedRectangle(cornerRadius: 8)
            .fill((link.dialArmed ? Color.green : Color.white).opacity(0.14)))
        }
        .buttonStyle(.plain)

        // ── The transcript ──────────────────────────────────────────────────────
        if link.chatLines.isEmpty {
          Text("Nobody has said anything yet.")
            .font(.system(size: 10)).foregroundColor(.white.opacity(0.45))
            .padding(.vertical, 4)
        } else {
          VStack(alignment: .leading, spacing: 4) {
            // ★ Newest last, as a conversation reads. The list is capped in the client, so this
            //   never grows without bound on a receiver that has been busy all day.
            ForEach(link.chatLines) { line in
              HStack(alignment: .top, spacing: 4) {
                Text(CannedDial.speaker(line.from, you: link.dialYou))
                  .font(.system(size: 9, weight: .bold))
                  .foregroundColor(line.from == link.dialYou ? .green : .orange)
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
            link.say(phrase.id)
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
    .onAppear { link.chatUnread = 0 }
  }

  private var roomLine: String {
    var bits = ["\(link.dialListeners) listening"]
    if link.dialMode == "spectator" {
      bits.append("the owner tunes this one")
    } else if link.dialDecoding {
      bits.append(link.dialMine ? "you are decoding" : "User \(link.dialTuner) is decoding")
    } else if link.dialTuner != 0 && link.dialMine {
      bits.append("you tuned last")
    } else if link.dialTuner != 0 {
      bits.append("User \(link.dialTuner) is tuning")
    } else {
      bits.append("nobody is tuning")
    }
    return bits.joined(separator: " · ")
  }
}
