import SwiftUI

/// WristSDR — the VibeSDR JR feasibility spike.
///
/// A DIRECT UberSDR client running entirely on the watch: its own sockets, its own DSP,
/// its own Opus decode, its own audio. No phone in the chain at any point.
///
/// It exists to answer one question that no amount of reasoning can settle — the shipped
/// companion app costs ~34% of a core just to DRAW rows the phone has already computed, and
/// JR would add the network link, the spectrum DSP and the audio decode on top of that.
/// Either the watch can carry all three inside its thermal and battery budget or it cannot,
/// and the only honest way to find out is to build it and look at the number.
///
/// Deliberately a separate Xcode project with its own bundle identifier, so it cannot
/// collide with VibeSDR or its watch app on the device — you can have both installed and
/// they will not know about each other.
@main
struct WristSDRApp: App {
  var body: some Scene {
    WindowGroup {
      ContentView()
    }
  }
}
