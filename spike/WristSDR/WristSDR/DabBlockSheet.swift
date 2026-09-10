import SwiftUI
import WatchKit

/// The block picker — the whole of Band III as a scrolling list, with what the server has HEARD
/// on each block beside the number, and a live narration of the one you are tuning.
///
/// ★★★ WHY A LIST AND NOT A CROWN NUDGE. Stepping blind was the fault Stuart hit: "the block moving
///  felt really sluggish with no visual feedback and made me almost about to come here and say it
///  wasnt working until it jumped from 5a - 10a". Band III is forty-one blocks and on a typical
///  aerial maybe six of them carry anything, so nudging one at a time is a hunt through mostly
///  silence with no way to know which of them are worth stopping on. The server already knows —
///  it has decoded those ensembles before — so it says so, and the hunt becomes a choice:
///
///      7C                        ← nothing ever heard here
///      7D   NNDAB                ← heard before, not tuned now
///      9A   Rugby+Daventry
///     10A   Searching…           ← tuning right now, nothing yet
///     10B   BBC National · 11 stations
///
/// ★★ AND IT NARRATES WHILE IT TUNES, which is the actual answer to "no visual feedback" (Stuart,
///  2026-09-10): pick a block and the row goes "Searching…", then the ensemble name the moment the
///  FIC gives one up, then the service count when the list lands — or "No services" if the dwell
///  expires with nothing. You are never looking at a screen that has stopped saying anything.
/// ★ Tap to pick; the crown scrolls the list, as it does in every other watchOS list. No mode to
///   learn and no second meaning for the crown.
struct DabBlockSheet: View {
  @EnvironmentObject var link: SpikeLink
  /// Called with the Band III index to tune. The sheet STAYS OPEN — you are picking, and picking
  /// often means trying two or three before one is worth listening to.
  var onPick: (Int) -> Void
  var onClose: () -> Void

  /// When the current selection was made, so "Searching…" can become "No services" rather than
  /// spinning for ever. ★ Wall clock is wrong here (it moves when the user does); uptime is not.
  @State private var pickedAt: TimeInterval = 0
  @State private var picked: Int? = nil
  @State private var now: TimeInterval = ProcessInfo.processInfo.systemUptime

  /// How long a block gets to prove itself before the row says there is nothing on it.
  /// ★ 8 s, not 3: a weak multiplex on a cold gain loop genuinely takes that long — the server's
  ///   own acquisition dwells 0.4 s a step and 10D wants ~36 dB of climb. Calling it dead early is
  ///   how a listener skips past a block that would have come good.
  private let dwell: TimeInterval = 8

  var body: some View {
    List {
      ForEach(Array(UberClient.dabBlocks.enumerated()), id: \.offset) { i, blk in
        Button { pick(i) } label: { row(index: i, name: blk.name) }
          .buttonStyle(.plain)
          .listRowBackground(
            (blk.name == link.dabBlockName ? Color.cyan.opacity(0.22) : Color.clear)
              .clipShape(RoundedRectangle(cornerRadius: 8))
          )
      }
    }
    .navigationTitle("Block")
    .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Done") { onClose() } } }
    // ★ 2 Hz is plenty to animate "Searching…" into a station count, and a watch list redrawing
    //   faster than that costs battery for nothing.
    .onReceive(Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()) { _ in
      now = ProcessInfo.processInfo.systemUptime
    }
    .onAppear {
      // ★ Start on the block being decoded, not at 5A — the list is 41 rows and the one you care
      //   about is almost always the one you are on.
      if let i = UberClient.dabBlocks.firstIndex(where: { $0.name == link.dabBlockName }) { picked = i }
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
    let isTuned = (name == link.dabBlockName)
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
  ///  the live one is the truth — an ensemble can be re-planned, and a stale name presented as
  ///  current is the "client must not decide what only the server knows" trap.
  private func caption(index i: Int, name: String, isTuned: Bool) -> String {
    let remembered = link.dabBlockNames[name] ?? ""
    guard isTuned else { return remembered }

    let ens = link.dabEnsembleName.isEmpty ? remembered : link.dabEnsembleName
    let n = link.dabProgrammes.count
    if n > 0 {
      let stations = n == 1 ? "1 station" : "\(n) stations"
      return ens.isEmpty ? stations : ens + " · " + stations
    }
    // Nothing yet. Say WHAT we are waiting for, and give up honestly when the dwell is spent.
    let waited = now - pickedAt
    if pickedAt > 0 && waited > dwell { return ens.isEmpty ? "No services" : ens + " · no services" }
    return ens.isEmpty ? "Searching…" : ens + " · searching…"
  }
}
