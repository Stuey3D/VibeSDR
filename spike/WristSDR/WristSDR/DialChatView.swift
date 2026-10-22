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

        // ── The arm switch ─────────────────────────────────────────────────────
        // ★★★ FM-DX's CONTROL, NOT A SECOND ONE. Same shape (the tune scale), same marks (green
        //   tick / red cross), same meaning — "may this watch move a dial other people are
        //   listening to". FM-DX got here first and taught it; a different-looking switch for the
        //   identical question would be two lessons where one will do.
        // ★★ It stays HERE, on the chat page, deliberately. Stuart: "keep it as it is now with the
        //   chat being the place to unlock the tuner as that prompts a user to ask." Reaching for
        //   the tuner walks you past the place you would ask for it.
        // ★★★ ASK BEFORE TUNING (docs/TUNING-ETIQUETTE.md): the SERVER has not let this listener in yet, so arming is
        //   not offered — the row says why and what to do. "Can I tune?" is the first key below it.
        if radio.dialTuneLocked {
          HStack(spacing: 8) {
            Image(systemName: "bubble.left.and.exclamationmark.bubble.right")
              .font(.system(size: 14)).frame(width: 36, height: 30)
            VStack(alignment: .leading, spacing: 1) {
              // ★ In the watch's language, like the phrases (CannedDial.t — i18n/chat/*.json).
              Text(radio.dialAskState == "open" ? CannedDial.t("bannerAwaiting")
                   : radio.dialAskState == "no" ? CannedDial.t("watchAskedToWait") : CannedDial.t("bannerAskBefore"))
                .font(.system(size: 11, weight: .bold))
              Text(radio.dialAskState == "open" ? CannedDial.t("watchAskOpenSub")
                   : radio.dialAskState == "no" ? CannedDial.t("watchAskNoSub")
                   : CannedDial.t("watchAskSub"))
                .font(.system(size: 9)).opacity(0.75).lineLimit(2)
            }
            Spacer()
          }
          .foregroundColor(.orange)
          .padding(.horizontal, 8).padding(.vertical, 6)
          .frame(maxWidth: .infinity)
          .background(RoundedRectangle(cornerRadius: 8).fill(Color.orange.opacity(0.14)))
        } else {
        Button {
          radio.dialArmed.toggle()
          WKInterfaceDevice.current().play(radio.dialArmed ? .start : .stop)
        } label: {
          HStack(spacing: 8) {
            TuneScaleGlyph().stroke(.white, style: StrokeStyle(lineWidth: 1.1, lineCap: .round))
              .frame(width: 18, height: 11)
              .overlay(alignment: .bottomTrailing) {
                Image(systemName: radio.dialArmed ? "checkmark.circle.fill" : "xmark.circle.fill")
                  .font(.system(size: 8, weight: .bold))
                  .foregroundStyle(radio.dialArmed ? .green : .red)
                  .background(Circle().fill(.black)).offset(x: 5, y: 4)
              }
              .frame(width: 36, height: 30)
            VStack(alignment: .leading, spacing: 1) {
              Text(radio.dialArmed ? "TUNING ARMED" : "TUNING LOCKED")
                .font(.system(size: 11, weight: .bold))
              Text(radio.dialArmed
                   ? "You can tune · disarms after \(Int(UberClient.armMinutes)) min"
                   : "Tap to allow tuning")
                .font(.system(size: 9)).opacity(0.75).lineLimit(2)
            }
            Spacer()
          }
          .foregroundColor(radio.dialArmed ? .green : .white.opacity(0.8))
          .padding(.horizontal, 8).padding(.vertical, 6)
          .frame(maxWidth: .infinity)
          .background(RoundedRectangle(cornerRadius: 8)
            .fill((radio.dialArmed ? Color.green : Color.white).opacity(0.14)))
        }
        .buttonStyle(.plain)
        }

        // ── The transcript ──────────────────────────────────────────────────────
        if radio.chatLines.isEmpty {
          Text(CannedDial.t("watchEmpty"))
            .font(.system(size: 10)).foregroundColor(.white.opacity(0.45))
            .padding(.vertical, 4)
        } else {
          VStack(alignment: .leading, spacing: 4) {
            // ★ Newest last, as a conversation reads. The list is capped in the client, so this
            //   never grows without bound on a receiver that has been busy all day.
            ForEach(radio.chatLines) { line in
              HStack(alignment: .top, spacing: 4) {
                Text(CannedDial.speaker(line.from, you: radio.dialYou, admin: line.admin))
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
        // ★★ The answers to "Anyone know what this is?" appear only while somebody else's question is open, and only
        //    those for the dial's band — on a watch above all, the list must not grow by twenty-seven (2026-10-09).
        ForEach(CannedDial.pad(questionOpen: CannedDial.questionOpen(radio.chatLines.map { ($0.from, $0.phrase, $0.at) },
                                                                    you: radio.dialYou),
                               hz: radio.frequency), id: \.id) { phrase in
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
    .navigationTitle(CannedDial.t("chatTitle"))
    // ★ Opening the room IS reading it. The badge exists to say "something happened while you were
    //   looking at the waterfall", and it has done its job the moment you are here.
    .onAppear { radio.chatUnread = 0 }
  }

  private var roomLine: String {
    let user = CannedDial.t("user", ["n": "\(radio.dialTuner)"])
    var bits = [CannedDial.t("listening", ["n": "\(radio.dialListeners)"])]
    if radio.dialMode == "spectator" {
      bits.append(CannedDial.t("watchOwnerTunes"))
    } else if radio.dialDecoding {
      bits.append(radio.dialMine ? CannedDial.t("youDecoding") : CannedDial.t("userDecoding", ["who": user]))
    } else if radio.dialTuner != 0 && radio.dialMine {
      bits.append(CannedDial.t("youTunedLast"))
    } else if radio.dialTuner != 0 {
      bits.append(CannedDial.t("userTuning", ["who": user]))
    } else {
      bits.append(CannedDial.t("watchNobody"))
    }
    return bits.joined(separator: " · ")
  }
}
