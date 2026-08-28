import SwiftUI

/// ★★★ TYPE THE PIN ON THE WRIST, because the alternative is taking the phone out.
///
///  A PIN-protected VibeServer stops a watch-driven connect dead: the phone puts up an
///  Alert.prompt, and a prompt on a device in somebody's pocket is not a prompt, it is a stall.
///  Stuart, 2026-08-28: "we need to be able to enter the pin without having to pull the iPhone
///  out ... Buddy needs to operate almost identically to Jr except using the iPhone to handle the
///  actual connection and audio." Jr has had a PIN sheet since it could connect at all.
///
/// ★★ THE PHONE KEEPS THE PIN, NOT US. It owns the saved-PIN store, the auth handshake and the
///    connection; the wrist supplies four digits and forgets them. A second copy on the watch
///    would be a second thing to keep in step and a second place to leak from.
/// ★ A KEYPAD, NOT A TEXTFIELD. Jr can afford a text field because it is typed once on a screen
///   you are already looking at; on Buddy this arrives mid-connect and has to be answerable in a
///   couple of seconds with one thumb. Big targets, no keyboard, no scrolling.
struct PinSheet: View {
    let serverName: String
    let onSubmit: (String) -> Void
    let onCancel: () -> Void

    @State private var entry = ""

    private let keys = ["1","2","3","4","5","6","7","8","9","⌫","0","✓"]

    var body: some View {
        VStack(spacing: 4) {
            Text(serverName)
                .font(.system(size: 12)).foregroundStyle(.secondary)
                .lineLimit(1).truncationMode(.middle)
            // ★ Dots, not digits: a PIN typed on a wrist is read over shoulders on trains. The
            //   count is what the typist needs — that the last press registered.
            Text(entry.isEmpty ? "enter PIN" : String(repeating: "•", count: entry.count))
                .font(.system(size: 20, weight: .semibold, design: .rounded))
                .foregroundStyle(entry.isEmpty ? .secondary : .primary)
                .frame(height: 26)

            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 4), count: 3), spacing: 4) {
                ForEach(keys, id: \.self) { k in
                    Button {
                        switch k {
                        case "⌫": if !entry.isEmpty { entry.removeLast() }
                        case "✓": onSubmit(entry)
                        // ★ Bounded: a PIN is short, and an unbounded field on a 41mm screen grows
                        //   into the keypad it is being typed on.
                        default: if entry.count < 12 { entry += k }
                        }
                    } label: {
                        Text(k)
                            .font(.system(size: 16, weight: .semibold, design: .rounded))
                            .frame(maxWidth: .infinity, minHeight: 30)
                            .background(RoundedRectangle(cornerRadius: 6)
                                .fill(k == "✓" ? Color.green.opacity(0.30)
                                     : k == "⌫" ? Color.orange.opacity(0.22)
                                                : Color.white.opacity(0.12)))
                    }
                    .buttonStyle(.plain)
                    // ★ ✓ is dead until there is something to send — a connect with an empty PIN
                    //   just fails a second later on the phone, where nobody is looking.
                    .disabled(k == "✓" && entry.isEmpty)
                    .opacity(k == "✓" && entry.isEmpty ? 0.4 : 1)
                }
            }
            Button("Cancel", role: .cancel) { onCancel() }
                .font(.system(size: 12)).tint(.orange)
        }
        .padding(.horizontal, 6)
    }
}
