import SwiftUI
import WatchKit

/// DAB: a LIST, not a band.
///
/// A DAB multiplex is one wide block carrying a dozen-odd services. There is nothing
/// to hunt for inside it and nothing to tune — the ensemble hands you an id→name map
/// and you switch service with `setAudioServiceId()`, which re-sends the demod without
/// moving the frequency at all. So:
///
/// - **No waterfall.** The spectrum of a DAB block is a featureless slab. The SERVICES
///   are the content, exactly as the station is the content on FM-DX.
/// - **The crown SELECTS, it does not tune.** This inverts the rule the rest of the app
///   lives by. Everywhere else the crown is a continuous control and the readout must
///   chase it (hence prediction, settle windows, all of that). Here it steps through a
///   finite list: there is nothing to overshoot and nothing to predict.
/// - **The phone already refuses to tune in DAB** (SDRScreen guards the drum, the
///   waterfall tap, direct tune AND the watch crown): a nudge knocks you off the
///   ensemble block, which kills the decode and is a nuisance to re-find. Giving the
///   crown a job it can actually do is better than leaving it inert.
///
/// Turning moves a CURSOR; tapping switches. Switching on every detent would tear the
/// audio stream down and rebuild it once per service as you spun past — so you browse
/// the mux without interrupting what you're hearing, and commit deliberately.
struct DabView: View {
  @EnvironmentObject var link: WatchLink

  @State private var showFavs = false
  @State private var cursor = 0
  @State private var crown = 0.0
  @State private var lastDetent = 0
  @FocusState private var crownFocused: Bool

  private static let detents = 1000.0

  private var dab: WatchLink.DabState { link.dab ?? .init() }

  var body: some View {
    VStack(spacing: 0) {
      header
      list
    }
    .background(Color.black.ignoresSafeArea())
    .focusable(true)
    .focused($crownFocused)
    .digitalCrownRotation(
      $crown,
      from: 0, through: Self.detents, by: 1,
      sensitivity: .low,                 // one service per click, not a blur of them
      isContinuous: true,
      isHapticFeedbackEnabled: true
    )
    .onChange(of: crown) { _, new in
      let detent = Int(new.rounded())
      guard detent != lastDetent else { return }
      var delta = detent - lastDetent
      let range = Int(Self.detents)
      if delta >  range / 2 { delta -= range }
      if delta < -range / 2 { delta += range }
      lastDetent = detent

      let n = dab.list.count
      guard n > 0 else { return }
      // CLAMP, don't wrap. A list has ends; spinning off the bottom and reappearing
      // at the top is disorienting when you can see the whole list at once.
      cursor = min(n - 1, max(0, cursor + delta))
    }
    .sheet(isPresented: $showFavs) {
      FavouritesList(favs: link.favourites) { url in
        link.selectInstance(url)
        showFavs = false
      }
    }
    .onAppear {
      crownFocused = true
      // Start on what's PLAYING, not at the top — you almost always want the thing
      // next to what you're already listening to.
      if let i = dab.list.firstIndex(where: { $0.id == dab.active }) { cursor = i }
      link.ping()
    }
    .onChange(of: dab.active) { _, id in
      if let i = dab.list.firstIndex(where: { $0.id == id }) { cursor = i }
    }
  }

  // MARK: - Header (the clock's band is free height)

  private var header: some View {
    VStack(spacing: 1) {
      HStack(spacing: 5) {
        Image(systemName: "square.stack.3d.up.fill")
          .font(.system(size: 10, weight: .semibold))
          .foregroundStyle(.cyan)
        Text(dab.ensemble.isEmpty ? "DAB" : dab.ensemble)
          .font(.system(size: 12, weight: .semibold, design: .rounded))
          .foregroundStyle(.white)
          .lineLimit(1)
          .minimumScaleFactor(0.7)
        // A way out: DAB has no menu (it's a list, not a control surface), so without
        // this the wrist was stranded on whatever server the phone happened to be on.
        ServersButton(show: $showFavs)
        Spacer(minLength: 0)
        Color.clear.frame(width: 58, height: 1)   // the clock's territory
      }

      // The phone's meter, mirrored — the one number that still means something on a
      // screen with nothing to tune.
      HStack(spacing: 5) {
        Text(link.meter.isEmpty ? "—" : link.meter)
          .font(.system(size: 10, weight: .semibold, design: .rounded))
          .monospacedDigit()
          .foregroundStyle(.white.opacity(0.75))
        signalBar
      }
    }
    .padding(.horizontal, 12)
    .padding(.top, 10)
    .padding(.bottom, 5)
  }

  private var signalBar: some View {
    GeometryReader { geo in
      ZStack(alignment: .leading) {
        Capsule().fill(.white.opacity(0.18))
        Capsule()
          .fill(LinearGradient(colors: [.red, .yellow, .green],
                               startPoint: .leading, endPoint: .trailing))
          .frame(width: max(2, geo.size.width * min(1, max(0, link.level))))
      }
    }
    .frame(height: 3)
  }

  // MARK: - The services

  private var list: some View {
    ScrollViewReader { proxy in
      ScrollView {
        LazyVStack(spacing: 4) {
          ForEach(Array(dab.list.enumerated()), id: \.element.id) { i, svc in
            row(svc, focused: i == cursor)
              .id(svc.id)
              .onTapGesture {
                cursor = i
                link.selectDab(svc.id)
                WKInterfaceDevice.current().play(.click)
              }
          }
        }
        .padding(.horizontal, 8)
        .padding(.bottom, 8)
      }
      // Keep the cursor on screen as the crown moves it — the crown is scrolling a
      // SELECTION, so the list has to follow it rather than the other way round.
      .onChange(of: cursor) { _, i in
        guard dab.list.indices.contains(i) else { return }
        withAnimation(.easeOut(duration: 0.15)) {
          proxy.scrollTo(dab.list[i].id, anchor: .center)
        }
      }
    }
  }

  private func row(_ svc: WatchLink.DabService, focused: Bool) -> some View {
    let playing = svc.id == dab.active
    return HStack(spacing: 6) {
      // PLAYING and SELECTED are different things, and both need to be legible: the
      // cursor is where the crown is, the speaker is what you can hear.
      Image(systemName: playing ? "speaker.wave.2.fill" : "circle")
        .font(.system(size: playing ? 11 : 7, weight: .semibold))
        .foregroundStyle(playing ? .green : .white.opacity(0.3))
        .frame(width: 14)

      Text(svc.name)
        .font(.system(size: 14, weight: playing ? .bold : .semibold, design: .rounded))
        .foregroundStyle(playing ? .white : .white.opacity(0.85))
        .lineLimit(1)
        .minimumScaleFactor(0.7)

      Spacer(minLength: 0)
    }
    .padding(.horizontal, 8)
    .padding(.vertical, 7)
    .background(
      RoundedRectangle(cornerRadius: 8)
        .fill(focused ? .cyan.opacity(0.22) : .white.opacity(0.08))
    )
    .overlay(
      RoundedRectangle(cornerRadius: 8)
        .stroke(focused ? .cyan : .clear, lineWidth: 1.2)
    )
  }
}
