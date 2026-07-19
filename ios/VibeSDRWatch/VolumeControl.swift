import SwiftUI
import WatchKit

/// The NATIVE watchOS volume control — `WKInterfaceVolumeControl` hosted in SwiftUI via
/// `WKInterfaceObjectRepresentable`. This is the real system-integrated volume the earlier
/// spike work wrongly recorded as "doesn't exist on watchOS": it was never tried through this
/// bridge. When focused it owns the Digital Crown and drives actual output volume, with Apple's
/// own volume UI/haptics.
///
/// `origin: .local` drives THIS app's audio-session volume — what the listener hears from our
/// stream — rather than `.device` (the whole system). That's the right scope for a radio: the
/// crown sets how loud the radio is, exactly like the Now Playing app.
struct VolumeControl: WKInterfaceObjectRepresentable {

  // Declared explicitly: without it Swift's associatedtype inference for this protocol
  // cascades into a bogus "does not conform / add stubs for WKInterfaceObjectType, Coordinator".
  typealias WKInterfaceObjectType = WKInterfaceVolumeControl

  /// When true, take the crown. The parent releases its own SwiftUI crown focus while volume
  /// mode is active, so this is the sole consumer; false lets tuning have it back.
  var focused: Bool

  func makeWKInterfaceObject(context: Context) -> WKInterfaceVolumeControl {
    let c = WKInterfaceVolumeControl(origin: .local)
    // Grab the crown AFTER the object is in the hierarchy and SwiftUI has released its own
    // crown focus (crownFocused -> false in ContentView). Calling focus() synchronously here
    // races that release, and the crown lands in limbo — neither volume nor tune ("gets
    // stuck"). A short delay lets the release land first, then we assert.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { c.focus() }
    return c
  }

  func updateWKInterfaceObject(_ obj: WKInterfaceVolumeControl, context: Context) {
    if focused {
      DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { obj.focus() }
    } else {
      obj.resignFocus()
    }
  }
}
