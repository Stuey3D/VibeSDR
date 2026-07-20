import SwiftUI

@main
struct VibeSDRWatchApp: App {
  @StateObject private var link = WatchLink.shared

  var body: some Scene {
    WindowGroup {
      // NavigationStack so the numpad can be PUSHED. As a sheet it came with a
      // mandatory header (X + clock + grab handle) that stole ~100pt and cut the
      // bottom row off; that chrome cannot be removed from a sheet.
      NavigationStack {
        // Routed by BACKEND, not by a strip swap: FM-DX has no spectrum at all, so
        // it gets its own screen rather than a waterfall with the waterfall taken
        // out. The phone never tells us which screen to be — what it SENDS already
        // says: rows mean a spectrum, an FM-DX blob means a station.
        Group {
          switch link.screen {
          case .sdr:  ContentView()
          case .fmdx: FmdxView()
          case .dab:  DabView()
          case .adsb: AircraftView()
          }
        }
        .environmentObject(link)
        .navigationBarHidden(true)   // both screens are full-bleed; no bar on either
      }
      .onAppear { link.activate() }
    }
  }
}
