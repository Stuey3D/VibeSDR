import SwiftUI
import WatchKit

/// Direct frequency entry — laid out like the system passcode pad.
///
/// That layout fits on one screen precisely because it is ONLY the digits, so
/// everything else has to get out of the grid's way: the readout goes above it,
/// backspace appears beside the readout only once there is something to delete,
/// and the bottom row's two spare slots become "." and TUNE.
///
/// The unit is asked AFTER you commit, not before: you dial the number you were
/// thinking of, then say what it was. ("7.155" is three different frequencies —
/// and no ham typing a CW frequency wants to spell out 7155000.)
///
/// A native TextField was tried and abandoned: watchOS gives it the ALPHANUMERIC
/// input surface, so Scribble reads "1" as "l" or "I", and the keys are too small
/// to hit. There is no public API for Apple's own numeric pad.
struct NumpadView: View {
  @EnvironmentObject var link: SpikeLink
  @Environment(\.dismiss) private var dismiss

  @State private var entry = ""
  @State private var askUnit = false

  private let cols = Array(repeating: GridItem(.flexible(), spacing: 5), count: 3)

  var body: some View {
    // The readout row lives IN THE CLOCK'S BAND (user's idea, and the thing that
    // finally made this fit). watchOS reserves that strip whether we use it or
    // not, and the clock only occupies the RIGHT of it — so leaving the left half
    // empty was donating ~40pt of a 251pt screen for nothing. Putting the
    // frequency there reclaims it, starts the digits directly under the clock, and
    // gives the X and backspace a home that costs no height at all.
    //
    // Ignoring the top safe area is therefore CORRECT here (it was wrong before,
    // when nothing was drawn up there: GeometryReader reported the full screen
    // while the content sat below the clock, so the keys were sized for space we
    // never had). Now we genuinely use the band, so the full height is genuinely
    // ours.
    // Size off the HARDWARE, not GeometryReader.
    //
    // GeometryReader kept reporting the SAFE-AREA height (215pt) while the view
    // actually drew into the full screen (251pt) — so the keys were sized for a
    // screen smaller than the one they were on, leaving ~50pt of dead space. It
    // had earlier reported the opposite and pushed the bottom row off. Rather than
    // keep guessing which number SwiftUI means, ask the device: screenBounds is
    // unambiguous.
    //
    // cornerInset: the display is a ROUNDED rectangle but every measurement we get
    // is a plain one, so anything drawn into the corners gets eaten by the curve.
    // The simulator flatters this; a photo of the real watch does not.
    Group {
      let screenH = WKInterfaceDevice.current().screenBounds.height
      let rows: CGFloat = 4
      let gaps: CGFloat = 5 * (rows - 1)
      let free = screenH - readoutH - gaps - cornerInset - bottomInset - 5
      let keyH = min(46, max(24, free / rows))

      VStack(spacing: 5) {
        readout
        LazyVGrid(columns: cols, spacing: 5) {
          ForEach(1...9, id: \.self) { d in key("\(d)", h: keyH) }
          key(".", h: keyH, round: true)   // bottom-left corner
          key("0", h: keyH)
          tuneKey(h: keyH)                 // bottom-right corner
        }
      }
      .padding(.top, cornerInset)
      .padding(.bottom, bottomInset)
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }
    .padding(.horizontal, 6)
    .ignoresSafeArea(edges: .top)
    // Hide the nav bar — a sheet's X, and then a push's back chevron, each reserved
    // ~90pt for themselves and shoved the pad off the bottom. We draw our own X in
    // the readout row instead, where it costs no height at all.
    //
    // But do NOT ignoreSafeArea(.top): watchOS keeps the clock's band reserved
    // regardless, so ignoring it made GeometryReader report the FULL screen while
    // the content was still laid out beneath the clock — sizing the keys for space
    // we were never given, which is precisely what pushed the bottom row off. Let
    // the geometry tell the truth and the keys size themselves to what's left.
    .toolbar(.hidden, for: .navigationBar)
    .confirmationDialog("Tune to", isPresented: $askUnit) {
      // The unit is the whole question, so it gets the whole screen — and each
      // option spells out what it will actually do, so a mis-tap is visible
      // BEFORE the radio jumps a thousandfold.
      ForEach(Unit.allCases, id: \.self) { u in
        if let hz = value(in: u) {
          Button(u.label(for: hz)) {
            // The unit you tuned in becomes the unit the readout speaks. Enter
            // 4582 kHz and it reads back "4582.000 kHz", not "4.582 MHz".
            link.displayUnit = u.display
            link.tune(toHz: hz)
            dismiss()
          }
        }
      }
      Button("Cancel", role: .cancel) { }
    }
  }

  private let readoutH: CGFloat = 32
  /// Width kept clear on the right for the system clock, which we cannot move.
  private let clockReserve: CGFloat = 70
  /// The display is a ROUNDED rectangle; the geometry we're handed is a plain one,
  /// so anything in a corner gets shaved by the curve.
  private let cornerInset: CGFloat = 8
  /// The bottom row sits hard against the screen's curve, and no amount of inset
  /// quite killed the last sliver of clipping without shrinking every key. So we
  /// stop fighting the curve and ECHO it: the two keys in the bottom corners
  /// ("." and TUNE) are fully rounded, which reads as intentional shaping rather
  /// than as something being cut off. The inset can then stay small and the keys
  /// keep their size.
  private let bottomInset: CGFloat = 8

  /// Sits in the clock's band: X, the frequency in a box, backspace — then the
  /// clock. The box matters: it says "this is the field you are editing", which a
  /// bare number floating above a keypad does not.
  private var readout: some View {
    HStack(spacing: 2) {
      Button { dismiss() } label: {
        Image(systemName: "xmark")
          .font(.system(size: 14, weight: .semibold))
          // Tap target is the FRAME, not the glyph — a bare icon is only as
          // tappable as it is big, which is why these were a struggle to hit.
          .frame(width: 30, height: readoutH)
          .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .foregroundStyle(.secondary)

      Text(entry.isEmpty ? "—" : entry)
        .font(.system(size: 19, weight: .semibold, design: .rounded))
        .monospacedDigit()
        .lineLimit(1)
        .minimumScaleFactor(0.5)
        .frame(maxWidth: .infinity, minHeight: readoutH - 8)
        .padding(.horizontal, 5)
        .background(
          RoundedRectangle(cornerRadius: 7)
            .fill(.white.opacity(0.10))
            .overlay(RoundedRectangle(cornerRadius: 7)
              .stroke(.orange.opacity(0.55), lineWidth: 1))
        )

      Button { entry.removeLast() } label: {
        Image(systemName: "delete.left")
          .font(.system(size: 14))
          .frame(width: 30, height: readoutH)
          .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .foregroundStyle(.orange)
      // Kept in the layout when empty, so the field doesn't jump sideways the
      // moment you type the first digit.
      .opacity(entry.isEmpty ? 0 : 1)
      .disabled(entry.isEmpty)

      // The clock's territory. We can't move it, so we simply don't go there.
      Color.clear.frame(width: clockReserve, height: 1)
    }
    .frame(height: readoutH)
    // Clear of the top-left curve, which was swallowing the X.
    .padding(.leading, 10)
  }

  /// PLAIN buttons with our own background, NOT .bordered.
  ///
  /// `.bordered` adds its own padding around the label, so a key asked for at 42pt
  /// rendered at ~48 — and four rows of that drift is ~30pt, which is precisely
  /// what kept pushing the bottom row off the screen no matter how the maths was
  /// tuned. The arithmetic was right about the LABEL; the button was quietly
  /// bigger than the label. Owning the background means a 42pt key is 42pt.
  /// `round` = the key sits in a bottom CORNER, so it takes the screen's own
  /// radius. Echoing the curve makes it look shaped on purpose; fighting it just
  /// looked like something had been cut off.
  private func key(_ k: String, h: CGFloat, round: Bool = false) -> some View {
    Button { tap(k) } label: {
      Text(k)
        // The glyph scales with the key, so a smaller watch gets a smaller digit
        // rather than a cramped one.
        .font(.system(size: h * 0.48, weight: .medium, design: .rounded))
        .foregroundStyle(.white)
        .frame(maxWidth: .infinity)
        .frame(height: h)
        .background(RoundedRectangle(cornerRadius: round ? h * 0.5 : h * 0.30)
          .fill(.white.opacity(0.16)))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }

  private func tuneKey(h: CGFloat) -> some View {
    Button { if !entry.isEmpty { askUnit = true } } label: {
      Text("TUNE")
        .font(.system(size: max(10, h * 0.30), weight: .bold, design: .rounded))
        .minimumScaleFactor(0.7)
        .foregroundStyle(.black)
        .frame(maxWidth: .infinity)
        .frame(height: h)
        // Fully rounded: it's the bottom-right corner key.
        .background(RoundedRectangle(cornerRadius: h * 0.5)
          .fill(entry.isEmpty ? Color.green.opacity(0.3) : Color.green))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .disabled(entry.isEmpty)
  }

  private func tap(_ k: String) {
    if k == "." {
      guard !entry.contains(".") else { return }
      entry += entry.isEmpty ? "0." : "."
    } else if entry.count < 10 {
      entry += k
    }
  }

  private func value(in u: Unit) -> Double? {
    guard let v = Double(entry), v > 0 else { return nil }
    let hz = v * u.multiplier
    return hz.isFinite ? hz : nil
  }

  enum Unit: CaseIterable {
    case mhz, khz, hz
    var multiplier: Double {
      switch self {
      case .hz:  return 1
      case .khz: return 1_000
      case .mhz: return 1_000_000
      }
    }
    /// What the main readout should speak from now on.
    var display: SpikeLink.DisplayUnit {
      switch self {
      case .hz:  return .hz
      case .khz: return .khz
      case .mhz: return .mhz
      }
    }
    /// Spell out the destination, not just the unit — "7.155 MHz" tells you far
    /// more than "MHz" does.
    func label(for hz: Double) -> String {
      switch self {
      case .mhz: return String(format: "%.4f MHz", hz / 1_000_000)
      case .khz: return String(format: "%.2f kHz", hz / 1_000)
      case .hz:  return String(format: "%.0f Hz", hz)
      }
    }
  }
}
