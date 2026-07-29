import SwiftUI

/// FM-DX SERVER SETTINGS — the things an FM-DX Webserver actually lets a listener change.
///
/// This replaced a bare "back to servers" button. FM-DX has no demod, step or bandwidth to offer
/// (the server does all demod), but it does expose a receiver's **antenna switch** and the **cEQ /
/// iMS** filters, and those are worth having on the wrist — antenna especially, since swapping to a
/// beam pointed the right way is the single biggest thing you can do for a marginal DX signal.
///
/// Wire format is taken from the phone's `FmdxAdapter` so the two clients cannot disagree:
///   `G<eq><ims>`  — BOTH filter bits, always sent together
///   `Z<n>`        — antenna select, 0-based
struct FmdxSettingsSheet: View {
  @EnvironmentObject var link: SpikeLink
  @Environment(\.dismiss) private var dismiss
  @State private var showAntennas = false

  private var info: FmdxInfo { link.fmdx ?? FmdxInfo() }

  var body: some View {
    ScrollView {
      VStack(spacing: 8) {

        // ANTENNA — a pushed list, not inline buttons: a server can advertise several with real
        // names ("Wideband vertical", "5-el beam NE") which will not fit as a row on a wrist.
        // Hidden entirely when the server advertises none or one — never offer a control whose
        // every use is a no-op (the same rule as OWRX's lockedRate).
        if info.antennas.count > 1 {
          Button { showAntennas = true } label: {
            HStack(spacing: 6) {
              Image(systemName: "antenna.radiowaves.left.and.right")
                .font(.system(size: 13, weight: .semibold)).foregroundStyle(.cyan)
              VStack(alignment: .leading, spacing: 1) {
                Text("ANTENNA").font(.system(size: 9, weight: .bold)).foregroundColor(.white.opacity(0.5))
                Text(currentAntennaName)
                  .font(.system(size: 13, weight: .semibold)).foregroundStyle(.white)
                  .lineLimit(1).minimumScaleFactor(0.7)
              }
              Spacer(minLength: 0)
              Image(systemName: "chevron.right").font(.system(size: 10, weight: .semibold))
                .foregroundStyle(.white.opacity(0.4))
            }
            .padding(.horizontal, 10).padding(.vertical, 8)
            .background(RoundedRectangle(cornerRadius: 10).fill(.white.opacity(0.12)))
          }.buttonStyle(.plain)
        }

        // cEQ and iMS. Two independent toggles, but ONE wire command carries both — the client
        // sends the other's current value alongside, or the unmentioned one gets cleared.
        toggleRow(title: "cEQ", on: info.eq,
                  blurb: "Corrects the audio response") { link.fmdxClient?.setEq(!info.eq) }
        toggleRow(title: "iMS", on: info.ims,
                  blurb: "Suppresses multipath distortion") { link.fmdxClient?.setIms(!info.ims) }

        // SERVERS — at the bottom, as the way OUT of this receiver rather than a setting of it.
        Button { dismiss(); link.backToPicker() } label: {
          HStack(spacing: 6) {
            Image(systemName: "rectangle.stack").font(.system(size: 13, weight: .semibold))
            Text("Servers").font(.system(size: 13, weight: .semibold))
            Spacer(minLength: 0)
          }
          .foregroundStyle(.white)
          .padding(.horizontal, 10).padding(.vertical, 8)
          .background(RoundedRectangle(cornerRadius: 10).fill(.white.opacity(0.12)))
        }.buttonStyle(.plain).padding(.top, 4)
      }
      .padding(.horizontal, 6).padding(.bottom, 10)
    }
    .navigationTitle("Settings")
    .navigationDestination(isPresented: $showAntennas) {
      AntennaPicker(antennas: info.antennas, current: info.antenna) { id in
        link.fmdxClient?.setAntenna(id)
        showAntennas = false
      }
    }
  }

  private var currentAntennaName: String {
    info.antennas.first { $0.id == info.antenna }?.name ?? "Antenna \(info.antenna + 1)"
  }

  /// A full-width toggle with a one-line explanation — cEQ and iMS are not self-explanatory, and a
  /// bare acronym on a wrist is a control nobody dares press.
  private func toggleRow(title: String, on: Bool, blurb: String, tap: @escaping () -> Void) -> some View {
    Button(action: tap) {
      HStack {
        VStack(alignment: .leading, spacing: 1) {
          Text(title).font(.system(size: 13, weight: .semibold))
          Text(blurb).font(.system(size: 9))
            .foregroundColor(on ? .black.opacity(0.6) : .white.opacity(0.5))
            .lineLimit(2).multilineTextAlignment(.leading)
        }
        Spacer(minLength: 4)
        Text(on ? "ON" : "OFF").font(.system(size: 12, weight: .bold))
      }
      .padding(.horizontal, 10).padding(.vertical, 8)
      .foregroundColor(on ? .black : .white)
      .background(RoundedRectangle(cornerRadius: 10)
        .fill(on ? AnyShapeStyle(Color.green) : AnyShapeStyle(.white.opacity(0.12))))
    }.buttonStyle(.plain)
  }
}

/// The antenna list — same shape as the OWRX profile picker: a pushed list with big targets and a
/// checkmark on the current one, rather than a row of tiny inline buttons.
struct AntennaPicker: View {
  let antennas: [FmdxAntenna]
  let current: Int
  let pick: (Int) -> Void

  var body: some View {
    List {
      ForEach(antennas) { a in
        Button { pick(a.id) } label: {
          HStack {
            Text(a.name).font(.system(size: 14)).lineLimit(2).minimumScaleFactor(0.7)
            Spacer(minLength: 4)
            if a.id == current {
              Image(systemName: "checkmark").foregroundStyle(.green)
            }
          }
        }.buttonStyle(.plain)
      }
    }
    .navigationTitle("Antenna")
  }
}
