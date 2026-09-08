import SwiftUI
import WatchKit
import Combine

/// DAB: a LIST, not a band — ported from the shipping companion's DabView, wired to SpikeLink.
///
/// A DAB multiplex is one wide block carrying a dozen-odd services. There's nothing to hunt for inside
/// it and nothing to tune — the ensemble hands you an id→name map and you switch service with
/// `selectDabService`, which re-sends the demod without moving frequency. So: NO waterfall (a DAB block's
/// spectrum is a featureless slab), and the CROWN SELECTS a service (moves a cursor) rather than tuning.
/// Turning moves the cursor; tapping commits (switching on every detent would tear the audio down as you
/// spun past). The speed-fix lives in the header (the dablin chipmunk workaround), as Stuart asked.
struct DabView: View {
  @EnvironmentObject var link: WatchLink

  // ★★★ THE TICK IS A STORED PROPERTY, NOT BUILT IN body(). Timer.publish() creates a NEW
  // publisher every time body is evaluated, and .onReceive resubscribes when the publisher
  // changes — so an inline one is torn down and restarted on every render, and a view that
  // re-renders faster than the interval NEVER REACHES ITS DEADLINE. `link` is an
  // @EnvironmentObject whose level/snr/span are republished on every waterfall row (10 Hz), so
  // every view observing it re-renders at that rate — faster than both ticks that were inline.
  // ★ The correct form was already here: WaterfallCanvas in ContentView.swift holds
  //   `private let driver = Timer.publish(...)` and its comment explains the isolation. Three of
  //   the five Timer.publish sites were stored properties; these two were not.
  private let tickClock = Timer.publish(every: 0.25, on: .main, in: .common).autoconnect()

  @State private var cursor = 0
  @State private var crown = 0.0
  @State private var lastDetent = 0
  @State private var showProfiles = false
  @State private var showMenu = false
  @State private var showChat = false
  @State private var locked = false
  @State private var volumeMode = false        // crown drives volume (native HUD) instead of the list
  /* ★★★ THE THIRD THING THE CROWN CAN BE, on a VibeServer: the MULTIPLEX. A watch has one
   *  continuous input and DAB has two lists — the services in this ensemble, and the forty
   *  multiplexes of Band III. Tapping the block hands the crown to the blocks and lights the
   *  capsule; tapping again gives it back to the services. Same shape as the volume button beside
   *  it, so there is one idea to learn rather than two. (Jr does exactly this — the two watch apps
   *  must not disagree about a gesture.) */
  @State private var blockMode = false
  @State private var volTimeout: DispatchWorkItem?
  @AppStorage("seenDabTutorial") private var seenDabTut = false
  @State private var showDabTut = false
  @FocusState private var crownFocused: Bool

  /// Auto-exit volume mode after a few idle seconds (reset on each volume change). Matches the native
  /// volume HUD, which fades out on its own.
  private func armVolTimeout() {
    volTimeout?.cancel()
    let work = DispatchWorkItem { volumeMode = false }
    volTimeout = work
    DispatchQueue.main.asyncAfter(deadline: .now() + 15, execute: work)   // generous — native crown isn't visible to reset on
  }


  private static let detents = 1000.0

  var body: some View {
    VStack(spacing: 0) {
      header
      list
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    .background(Color.black.ignoresSafeArea())
    .ignoresSafeArea(edges: .top)   // reclaim the tall reserved top strip so content sits under the status band
    // WATER LOCK: the crown still scrolls the cursor and the pinch gesture still fires (both bypass the
    // locked touchSCREEN), so DAB is fully usable wet — crown selects, pinch commits. `.primaryAction` is
    // the Water-Lock-capable hand-gesture hook (same one the main screen uses for crown-mode cycling).
    .overlay(alignment: .bottom) {
      Button(action: commitCursor) { Color.clear.frame(width: 1, height: 1) }
        .buttonStyle(.plain)
        .handGestureShortcut(.primaryAction)
    }
    /* ★★★ THE CROWN STAYS OURS — the same leftover FM-DX had, found by looking for it. This
     *   released the crown "so the native control owns it", and Buddy HAS no native control: the
     *   overlay below is an EmptyView and says so. In volume mode the crown went to nothing and DAB
     *   volume could not be changed from the wrist at all.
     * ★★ On Buddy the volume IS the phone's system volume, relayed as cmd:vol — there is nothing
     *   local to hand the crown to, which is precisely why keeping it is the mechanism. (Jr keeps
     *   the old rule: it has a real WKInterfaceVolumeControl, because Jr plays the audio itself.) */
    .focusable()
    .focused($crownFocused)
    .digitalCrownRotation($crown, from: 0, through: Self.detents, by: 1,
                          sensitivity: .low, isContinuous: true, isHapticFeedbackEnabled: true)
    .overlay(alignment: .trailing) {
      // ★ No native VolumeControl in Buddy: WKInterfaceVolumeControl drives the WATCH's own

      //   output, and Buddy's crown drives the PHONE's system volume via cmd:vol. Keeping the crown

      //   ours is what makes the phone's real level readable and adjustable from the wrist.

      EmptyView()
    }
    .onChange(of: volumeMode) { _, v in
      // ★ Focus is kept in BOTH directions now — the crown is ours either way, it just means volume
      //   instead of the list cursor. Dropping it is what left volume mode dead.
      if v { armVolTimeout(); DispatchQueue.main.async { crownFocused = true } }
      else { volTimeout?.cancel(); DispatchQueue.main.async { crownFocused = true } }
    }
    .onChange(of: crown) { _, new in
      let detent = Int(new.rounded())
      guard detent != lastDetent else { return }
      var delta = detent - lastDetent
      let range = Int(Self.detents)
      if delta >  range / 2 { delta -= range }
      if delta < -range / 2 { delta += range }
      lastDetent = detent
      // ★★ VOLUME MODE MEANS THE CROWN IS THE VOLUME. Without this the delta fell through to the
      //    list cursor — or, with focus released, was never delivered at all.
      if volumeMode { link.volume(delta: delta); return }
      /* ★★ ONE DETENT, ONE BLOCK — never accumulated into a sweep. Every step is a retune and a
       *  re-acquire on the server, a second or two of silence each, so a flick across twenty
       *  blocks would be twenty of them. The crown's own detents are the rate limit. */
      if blockMode { link.stepDabBlock(delta > 0 ? 1 : -1); return }
      let n = link.dabProgrammes.count
      guard n > 0 else { return }
      cursor = min(n - 1, max(0, cursor - delta))   // clamp, don't wrap — a list has ends (crown up = up)
    }
    .sheet(isPresented: $showProfiles) {
      ProfileSheet { id in link.selectProfile(id); showProfiles = false }
        .environmentObject(link)
    }
    .navigationDestination(isPresented: $showMenu) {
      ControlMenu { _ in }.environmentObject(link)
    }
    .sheet(isPresented: $showChat) { NavigationStack { ChatSheet().environmentObject(link) } }
    // Passive status icons in the clock's band, top-left (clock keeps the right corner).
    .overlay(alignment: .topLeading) { chrome }
    // Drive the client→UI mirror here — driverTick lives on ContentView (not rendered on this screen), so
    // without this the service list never grows and the "playing" icon never moves. 4 Hz suits a list.
    .onReceive(tickClock) { _ in
      link.driverTick(now: ProcessInfo.processInfo.systemUptime)
    }
    .onAppear {
      crownFocused = true
      if let i = link.dabProgrammes.firstIndex(where: { $0.id == link.dabActiveId }) { cursor = i }
      if !seenDabTut { DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { showDabTut = true } }
    }
    .sheet(isPresented: $showDabTut) {
      TutorialSheet(title: "DAB radio", tips: dabTutorialTips()) { seenDabTut = true; showDabTut = false }
    }
    .onChange(of: link.dabActiveId) { _, id in
      if let i = link.dabProgrammes.firstIndex(where: { $0.id == id }) { cursor = i }
    }
  }

  // PASSIVE status icons only — safe up in the clock's band (which doesn't take touches). The lock/menu
  // BUTTONS live in the header (tappable area).
  private var chrome: some View {
    // ★ Left alone deliberately: this row sits at the top-LEADING edge, so it never met the
    //   corner-arc clip that FM-DX's trailing capsule did. Nothing to fix here.
    HStack(spacing: 6) {
      BatteryPill(level: link.battery)
      ConnGlyph(transport: link.transport).font(.system(size: 11))
      QualityGlyph(link: link)
    }
    .padding(.leading, 28).padding(.top, 3)
    .ignoresSafeArea(edges: .top)
  }

  /// Commit the service under the cursor — from a tap OR the pinch gesture (the latter works in Water
  /// Lock, where taps don't). No-op if the cursor's service is already playing.
  private func commitCursor() {
    guard !locked else { return }
    guard link.dabProgrammes.indices.contains(cursor) else { return }
    let id = link.dabProgrammes[cursor].id
    guard id != link.dabActiveId else { return }
    link.selectDabService(id)
    WKInterfaceDevice.current().play(.click)
  }

  // MARK: - Header (ensemble + speed fix + a way out)

  private var header: some View {
    VStack(alignment: .leading, spacing: 4) {
      // Ensemble label — non-interactive, rides high just under the status band.
      HStack(spacing: 5) {
        Image(systemName: "square.stack.3d.up.fill")
          .font(.system(size: 10, weight: .semibold)).foregroundStyle(.cyan)
        /* ★ Drawn only where there IS a block to choose. On OWRX the multiplex is the owner's
         *  profile and there is nothing here to pick, so a dead capsule would be exactly the
         *  "control that works in one scenario only" AGENTS.md says to remove. */
        if !link.dabBlock.isEmpty {
          Button {
            guard !locked else { return }
            blockMode.toggle()
            if blockMode { volumeMode = false }
            WKInterfaceDevice.current().play(.click)
          } label: {
            Text(link.dabBlock)
              .font(.system(size: 12, weight: .bold, design: .rounded))
              .foregroundColor(blockMode ? .black : .cyan)
              .padding(.horizontal, 7).padding(.vertical, 2)
              .background(blockMode ? Color.cyan : Color.cyan.opacity(0.18), in: Capsule())
          }.buttonStyle(.plain).disabled(locked)
        }
        Text(link.dabEnsembleName.isEmpty ? (link.dabOn ? "searching…" : "DAB") : link.dabEnsembleName)
          .font(.system(size: 12, weight: .semibold, design: .rounded))
          .foregroundStyle(.white).lineLimit(1).minimumScaleFactor(0.7)
        Spacer(minLength: 0)
      }
      // Buttons row — Lock · Volume · Menu · Chat (TAPPABLE area).
      // SPACERS, not a fixed gap. The old `spacing: 24` fitted THREE buttons on a 49mm; adding a
      // fourth needs ~200pt and a 41mm has ~162, so the row overflowed and dragged the whole screen
      // off its left edge (lock button gone entirely). Even distribution adapts to any width.
      HStack(spacing: 0) {
        LockButton(locked: $locked, size: 18)
        Spacer(minLength: 2)
        // VOLUME: flips the crown to volume (native HUD) and back; auto-times out.
        Button { if !locked { volumeMode.toggle(); if volumeMode { blockMode = false }; WKInterfaceDevice.current().play(.click) } } label: {
          Image(systemName: volumeMode ? "speaker.wave.2.fill" : "speaker.wave.2")
            .font(.system(size: 18, weight: .semibold))
            .foregroundStyle(locked ? .white.opacity(0.3) : (volumeMode ? .orange : .white))
            .padding(4).contentShape(Rectangle())
        }.buttonStyle(.plain).disabled(locked)
        Spacer(minLength: 2)
        Button { if !locked { showMenu = true } } label: {
          Image(systemName: "line.3.horizontal").font(.system(size: 18, weight: .semibold))
            .foregroundStyle(locked ? .white.opacity(0.3) : .white)
            .padding(4).contentShape(Rectangle())
        }.buttonStyle(.plain).disabled(locked)
        // Chat was missing from DAB and ADS-B entirely — same OWRX server, same room of listeners,
        // no way to talk to them from these screens. The glyph carries the listener COUNT too.
        if link.supportsChat {
          Spacer(minLength: 2)
          ChatGlyph(clients: link.clients, activity: link.chatActivity) {
            if !locked { showChat = true }
          }
        }
        Spacer(minLength: 0)
      }
      /* ★★★ NO SPEED FIX HERE. Buddy's went through the phone, which never implemented it — the
       *  sheet drew, the tap did nothing (setDabScale was a no-op). A control that does nothing
       *  must not be there (AGENTS.md). Jr keeps its own, which works, for OWRX. */
      if link.dabNoDecoder {
        Text("No sound: this server has no DAB+ decoder")
          .font(.system(size: 10, weight: .semibold)).foregroundStyle(.orange)
          .lineLimit(2).minimumScaleFactor(0.8)
      }
      if !link.dabDlsText.isEmpty {
        Text(link.dabDlsText)
          .font(.system(size: 10, weight: .medium)).foregroundStyle(.white.opacity(0.65))
          .lineLimit(1).truncationMode(.tail)
      }
      /* ★★★ THE WAY OUT. On OWRX you leave DAB by choosing another profile and this is not drawn;
       *  on a VibeServer DAB is a mode nothing else can end, so without it the wrist is stuck on
       *  this screen until the phone is picked up. */
      if link.dabOn && !link.dabBlock.isEmpty {
        Button {
          guard !locked else { return }
          blockMode = false
          link.setDabMode(false)
          WKInterfaceDevice.current().play(.click)
        } label: {
          HStack(spacing: 4) {
            Image(systemName: "arrow.uturn.left").font(.system(size: 10, weight: .semibold))
            Text("Exit DAB").font(.system(size: 11, weight: .semibold))
          }
          .foregroundColor(.white)
          .padding(.horizontal, 10).padding(.vertical, 4)
          .background(Color.white.opacity(0.12), in: Capsule())
          .fixedSize()
        }.buttonStyle(.plain).disabled(locked)
      }
    }
    .padding(.horizontal, 10)
    .padding(.top, 40)   // ensemble label sits just under the status band (top ignored)
    .padding(.bottom, 4)
  }

  // MARK: - The services

  private var list: some View {
    ScrollViewReader { proxy in
      ScrollView {
        LazyVStack(spacing: 4) {
          ForEach(Array(link.dabProgrammes.enumerated()), id: \.element.id) { i, svc in
            row(svc, focused: i == cursor)
              .id(svc.id)
              .onTapGesture {
                guard !locked else { return }
                cursor = i
                link.selectDabService(svc.id)
                WKInterfaceDevice.current().play(.click)
              }
          }
        }
        .padding(.horizontal, 8).padding(.bottom, 8)
      }
      .onChange(of: cursor) { _, i in
        guard link.dabProgrammes.indices.contains(i) else { return }
        withAnimation(.easeOut(duration: 0.15)) { proxy.scrollTo(link.dabProgrammes[i].id, anchor: .center) }
      }
    }
  }

  private func row(_ svc: DabProgramme, focused: Bool) -> some View {
    let playing = svc.id == link.dabActiveId
    return HStack(spacing: 6) {
      // PLAYING and SELECTED are different: the cursor is where the crown is, the speaker is audible.
      Image(systemName: playing ? "speaker.wave.2.fill" : "circle")
        .font(.system(size: playing ? 11 : 7, weight: .semibold))
        .foregroundStyle(playing ? .green : .white.opacity(0.3)).frame(width: 14)
      /* ★ The station's picture, when the server has one. A fixed box whether or not it does, so
       *  the names do not shuffle sideways as logos land — the phone panel's rule. */
      if let u = svc.logo {
        AsyncImage(url: u) { img in img.resizable().scaledToFit() } placeholder: { Color.clear }
          .frame(width: 20, height: 20).clipShape(RoundedRectangle(cornerRadius: 3))
      }
      VStack(alignment: .leading, spacing: 1) {
        Text(svc.name)
          .font(.system(size: 14, weight: playing ? .bold : .semibold, design: .rounded))
          .foregroundStyle(playing ? .white : .white.opacity(0.85)).lineLimit(1).minimumScaleFactor(0.7)
        /* ★ Every station's live text, as the phone and the browser show it. Truncated, not
         *  scrolled: a moving label per row is a battery cost for a glance. */
        if !svc.dls.isEmpty {
          Text(svc.dls)
            .font(.system(size: 9, weight: .medium)).foregroundStyle(.white.opacity(0.55))
            .lineLimit(1).truncationMode(.tail)
        }
      }
      Spacer(minLength: 0)
    }
    .padding(.horizontal, 8).padding(.vertical, 7)
    .background(RoundedRectangle(cornerRadius: 8).fill(focused ? .cyan.opacity(0.22) : .white.opacity(0.08)))
    .overlay(RoundedRectangle(cornerRadius: 8).stroke(focused ? .cyan : .clear, lineWidth: 1.2))
  }
}

