import SwiftUI
import WatchKit

/// Passband (bandwidth) editor, opened from the ControlMenu. Two edge boxes — LSB (lower) and
/// USB (upper) — with a SYNC toggle between them. TAP a box to select it (it GLOWS), then the
/// crown adjusts that edge; a crown glyph on the selected box says how. SYNC (default ON for
/// symmetric modes, OFF for SSB) mirrors both edges around the carrier. Step is per-demod
/// (0.1 kHz voice, 5 kHz for wide FM). Pushes edges to the server via SpikeLink.setBandwidth,
/// which also lights the VFO's dashed sideband lines.
struct BandwidthView: View {
  @EnvironmentObject var link: SpikeLink
  @Environment(\.dismiss) private var dismiss

  enum Edge { case lower, upper }
  @State private var editing: Edge = .upper
  @State private var sync = false
  @State private var lo = 0.0        // Hz offset, negative
  @State private var hi = 0.0        // Hz offset, positive
  @State private var crown = 0.0
  @State private var lastDetent = 0
  @FocusState private var focused: Bool

  private let minEdge = 50.0
  private let maxEdge = 120_000.0    // covers WFM

  var body: some View {
    VStack(spacing: 12) {
      Text("BANDWIDTH")
        .font(.system(size: 11, weight: .semibold)).tracking(2)
        .foregroundStyle(.secondary)

      HStack(spacing: 7) {
        edgeBox("LSB", lo, .lower)
        syncButton
        edgeBox("USB", hi, .upper)
      }

      Text("Turn the crown to adjust")
        .font(.system(size: 10)).foregroundStyle(.secondary)
    }
    .padding(.horizontal, 6)
    .focusable(true)
    .focused($focused)
    .digitalCrownRotation($crown, from: 0, through: 10_000, by: 1,
                          sensitivity: .low, isContinuous: true)
    .onChange(of: crown) { _, new in
      let d = Int(new.rounded())
      var delta = d - lastDetent
      if delta >  5_000 { delta -= 10_000 }
      if delta < -5_000 { delta += 10_000 }
      lastDetent = d
      guard delta != 0 else { return }
      apply(delta)
    }
    .onAppear {
      lo = link.filtLo != 0 ? link.filtLo : -2_700
      hi = link.filtHi != 0 ? link.filtHi :  2_700
      sync = link.symmetricMode
      focused = true
    }
  }

  private func apply(_ delta: Int) {
    let step = link.bwStep()
    switch editing {
    case .upper:
      hi = min(maxEdge, max(minEdge, hi + Double(delta) * step))   // clockwise = wider
      if sync { lo = -hi }
    case .lower:
      lo = max(-maxEdge, min(-minEdge, lo - Double(delta) * step)) // more negative = wider
      if sync { hi = -lo }
    }
    link.setBandwidth(lo, hi)
    WKInterfaceDevice.current().play(.click)
  }

  private func edgeBox(_ title: String, _ value: Double, _ edge: Edge) -> some View {
    let selected = editing == edge
    return Button { editing = edge } label: {
      VStack(spacing: 2) {
        Text(title).font(.system(size: 10, weight: .semibold)).foregroundStyle(.secondary)
        Text(kHz(value)).font(.system(size: 17, weight: .bold, design: .rounded))
          .minimumScaleFactor(0.6).lineLimit(1)
        Image(systemName: "digitalcrown.arrow.clockwise")
          .font(.system(size: 9, weight: .semibold))
          .foregroundStyle(selected ? .orange : .clear)
      }
      .foregroundStyle(.white)
      .frame(width: 64, height: 66)
      .background(RoundedRectangle(cornerRadius: 12).fill(.white.opacity(0.14)))
      .overlay(RoundedRectangle(cornerRadius: 12)
        .stroke(selected ? Color.orange : .clear, lineWidth: 2))
      .shadow(color: selected ? .orange.opacity(0.55) : .clear, radius: 7)   // the "glow"
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }

  private var syncButton: some View {
    Button {
      sync.toggle()
      if sync { let m = max(abs(lo), abs(hi)); lo = -m; hi = m; link.setBandwidth(lo, hi) }
    } label: {
      VStack(spacing: 3) {
        Image(systemName: sync ? "arrow.left.and.right" : "arrow.left.and.right.circle")
          .font(.system(size: 13, weight: .semibold))
        Text("SYNC").font(.system(size: 8, weight: .bold))
      }
      .foregroundStyle(sync ? .orange : .white.opacity(0.55))
      .frame(width: 40, height: 66)
      .background(RoundedRectangle(cornerRadius: 10)
        .fill(sync ? Color.orange.opacity(0.16) : .white.opacity(0.08)))
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }

  private func kHz(_ hz: Double) -> String {
    let k = abs(hz) / 1000
    return k >= 10 ? String(format: "%.0fk", k) : String(format: "%.1fk", k)
  }
}
