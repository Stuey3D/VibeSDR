import SwiftUI
import WatchKit

/// The block picker — the whole of Band III as a scrolling list, with what the server has HEARD on
/// each block beside the number, and a live narration of the one you are tuning.
///
/// ★★★ Buddy's copy of Jr's sheet, deliberately the same screen. The two watch apps must not
///  disagree about a gesture, and this one is the only way to change multiplex on either now.
///  It differs from Jr's in exactly one respect and it is not cosmetic: Buddy owns no socket, so
///  everything here is relayed by the phone — the block list, the remembered names and the pick.
///
///      7C                        ← nothing ever heard here
///      7D   NNDAB                ← heard before, not tuned now
///      9A   Rugby+Daventry
///     10A   Searching…           ← tuning right now, nothing yet
///     10B   BBC National · 11 stations
///
/// ★★ IT NARRATES WHILE IT TUNES, which is the answer to "no visual feedback" (Stuart,
///  2026-09-10). Pick a block and the row says "Searching…", then the ensemble name as soon as the
///  FIC gives one up, then the service count when the list lands — or "No services" once the dwell
///  is spent. The screen never goes quiet on you.
/// ★ Tap to pick; the crown scrolls, as in every other watchOS list. No mode to learn.
struct DabBlockSheet: View {
  @EnvironmentObject var link: WatchLink
  /// Called with the Band III index to tune. The sheet STAYS OPEN — picking often means trying two
  /// or three before one is worth listening to.
  var onPick: (Int) -> Void
  var onClose: () -> Void

  @State private var pickedAt: TimeInterval = 0
  @State private var picked: Int? = nil
  @State private var now: TimeInterval = ProcessInfo.processInfo.systemUptime

  /// How long a block gets before the row admits there is nothing on it.
  /// ★★ 10 s here against Jr's 8: every message on this screen has crossed the watch link and the
  ///  phone's own decoder before it arrives, and calling a block dead while its ensemble is still
  ///  in flight would be a lie the listener acts on by skipping a block that was coming good.
  private let dwell: TimeInterval = 10

  var body: some View {
    List {
      ForEach(Array(WatchLink.dabBlocks.enumerated()), id: \.offset) { i, name in
        Button { pick(i) } label: { row(index: i, name: name) }
          .buttonStyle(.plain)
          .listRowBackground(
            (name == link.dabBlock ? Color.cyan.opacity(0.22) : Color.clear)
              .clipShape(RoundedRectangle(cornerRadius: 8))
          )
      }
    }
    .navigationTitle("Block")
    .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Done") { onClose() } } }
    .onReceive(Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()) { _ in
      now = ProcessInfo.processInfo.systemUptime
    }
    .onAppear {
      // ★ Start on the block being decoded, not at 5A — 41 rows, and the one you care about is
      //   almost always the one you are on.
      if let i = WatchLink.dabBlocks.firstIndex(of: link.dabBlock) { picked = i }
    }
  }

  private func pick(_ i: Int) {
    guard i != picked || link.dabProgrammes.isEmpty else { return }
    picked = i
    pickedAt = ProcessInfo.processInfo.systemUptime
    onPick(i)
    WKInterfaceDevice.current().play(.click)
  }

  @ViewBuilder
  private func row(index i: Int, name: String) -> some View {
    let isTuned = (name == link.dabBlock)
    HStack(alignment: .firstTextBaseline, spacing: 6) {
      Text(name)
        .font(.system(size: 14, weight: .bold, design: .rounded))
        .foregroundStyle(isTuned ? .cyan : .white)
        .frame(width: 34, alignment: .leading)
      Text(caption(index: i, name: name, isTuned: isTuned))
        .font(.system(size: 12, weight: isTuned ? .semibold : .regular, design: .rounded))
        .foregroundStyle(isTuned ? .white : .white.opacity(0.55))
        .lineLimit(1).minimumScaleFactor(0.7)
      Spacer(minLength: 0)
    }
    .padding(.vertical, 1)
  }

  /// What the row says to the right of the block number.
  ///
  /// ★★★ THE TUNED ROW OUTRANKS THE REMEMBERED NAME, and the two are different facts: the memory is
  ///  what this server heard here once, the live line is what it is hearing NOW. When they disagree
  ///  the live one wins — an ensemble can be re-planned, and a stale name presented as current is
  ///  the "client must not decide what only the server knows" trap.
  private func caption(index i: Int, name: String, isTuned: Bool) -> String {
    let remembered = link.dabBlockNames[name] ?? ""
    guard isTuned else { return remembered }

    let ens = link.dabEnsembleName.isEmpty ? remembered : link.dabEnsembleName
    let n = link.dabProgrammes.count
    if n > 0 {
      let stations = n == 1 ? "1 station" : "\(n) stations"
      return ens.isEmpty ? stations : ens + " · " + stations
    }
    let waited = now - pickedAt
    if pickedAt > 0 && waited > dwell { return ens.isEmpty ? "No services" : ens + " · no services" }
    return ens.isEmpty ? "Searching…" : ens + " · searching…"
  }
}
