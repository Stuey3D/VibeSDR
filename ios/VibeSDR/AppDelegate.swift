internal import Expo
import React
import ReactAppDependencyProvider
import AVFoundation
import UIKit

@main
class AppDelegate: ExpoAppDelegate {
  var window: UIWindow?

  var reactNativeDelegate: ExpoReactNativeFactoryDelegate?
  var reactNativeFactory: RCTReactNativeFactory?

  public override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
  ) -> Bool {
    // Register as audio app at launch so iOS tracks us for Now Playing / lock screen controls.
    // Must be done before any audio starts — omitting this was preventing media controls from appearing.
    try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
    try? AVAudioSession.sharedInstance().setActive(true)

    let delegate = ReactNativeDelegate()
    let factory = ExpoReactNativeFactory(delegate: delegate)
    delegate.dependencyProvider = RCTAppDependencyProvider()

    reactNativeDelegate = delegate
    reactNativeFactory = factory

    // NOTE: iOS 26+ requires the UIScene lifecycle. The React Native root view is
    // now created in SceneDelegate.scene(_:willConnectTo:) instead of here — the
    // window is owned by the scene. See UIApplicationSceneManifest in Info.plist.
    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  // Full process termination (the scene path covers swipe-close; this covers the rest).
  override func applicationWillTerminate(_ application: UIApplication) {
    VibeWatchModule.appWillTerminate()
    super.applicationWillTerminate(application)
  }
}

// MARK: - Scene lifecycle (iOS 26+ mandatory)

/// A window that reports HARDWARE KEY presses to JS.
///
/// ★ Deliberately the WINDOW rather than a first-responder view. Key presses travel UP
/// the responder chain, so anything that genuinely wants a key — a focused text field,
/// RN's own text input — consumes it first and never reaches here. That gives the
/// brief's "text-input focus WINS" rule for free, instead of having to suppress global
/// shortcuts whenever a field has focus. It also means we never fight React Native for
/// first responder, which is a fight nobody wins.
///
/// Down and up are separate events on purpose: the arrow keys reuse the tuner keys'
/// tap-steps / hold-sweeps semantics, and a sweep has to know when the key was let go.
class VibeKeyWindow: UIWindow {
  private static func name(for key: UIKey) -> String? {
    switch key.keyCode {
    case .keyboardLeftArrow:  return "ArrowLeft"
    case .keyboardRightArrow: return "ArrowRight"
    case .keyboardUpArrow:    return "ArrowUp"
    case .keyboardDownArrow:  return "ArrowDown"
    case .keyboardReturnOrEnter, .keypadEnter: return "Enter"
    case .keyboardEscape:     return "Escape"
    case .keyboardTab:        return "Tab"
    case .keyboardSpacebar:   return "Space"
    // Backspace steps OUT of a sub-panel (Display Settings, Bookmarks…), which
    // otherwise had no keyboard way back. Safe to claim because a focused text
    // field takes precedence — see `isTypingInTextField`.
    case .keyboardDeleteOrBackspace: return "Backspace"
    // ★ Forward delete as well as backspace. A Mac keyboard has no forward-delete key at all,
    // so anything that offers only one of the two is unusable on half the hardware — Stuart's
    // point, and the reason both are mapped rather than one being picked as canonical.
    case .keyboardDeleteForward: return "Delete"
    default:
      // Letters come through as their characters; ignore anything with modifiers so
      // system shortcuts (Cmd-Q and friends) are left entirely alone.
      guard key.modifierFlags.isEmpty else { return nil }
      let c = key.charactersIgnoringModifiers.uppercased()
      return (c.count == 1 && c >= "A" && c <= "Z") ? c : nil
    }
  }

  /// ★★ A FOCUSED TEXT FIELD OWNS THE KEYBOARD.
  ///
  /// This window claims keys before the responder chain sees them, and it used to claim
  /// them unconditionally — so while the user was typing, every letter was swallowed and
  /// never reached the field. Chat was completely dead, and Enter never arrived either, so
  /// a typed frequency could not be committed. DIGITS still worked, because `name(for:)`
  /// ignores them, which is exactly why the bug looked partial and confusing rather than
  /// total. (Stuart, 2026-07-25.)
  ///
  /// RN's TextInput is backed by a UITextField, so conformance to UITextInput is a reliable
  /// test. The walk costs a hierarchy traversal per key press, which is nothing — key
  /// presses are a human-speed event.
  private func focusedTextInput() -> UIResponder? {
    func find(_ v: UIView) -> UIResponder? {
      if v.isFirstResponder { return v }
      for s in v.subviews { if let r = find(s) { return r } }
      return nil
    }
    let r = find(self)
    return r is UITextInput ? r : nil
  }

  private var isTypingInTextField: Bool { focusedTextInput() != nil }

  /// ★★ Is the focused field a NUMBER PAD? If so, a letter can never be valid input there,
  /// so it is ours to use as a shortcut rather than the field's to swallow.
  ///
  /// This is what makes [H]z / [K]Hz / [M]Hz and [T]une / [B]ookmarks work while the
  /// frequency box has focus. Without it those keys vanished into a field that could not
  /// accept them anyway — the letters did nothing at all, which is exactly what Stuart saw.
  /// A normal keyboard (chat, server names, the bookmark box) is untouched: there a letter
  /// is real input and must stay the field's.
  private var focusedFieldIsNumeric: Bool {
    guard let traits = focusedTextInput() as? UITextInputTraits else { return false }
    switch traits.keyboardType {
    case .some(.numberPad), .some(.decimalPad), .some(.numbersAndPunctuation), .some(.phonePad):
      return true
    default:
      return false
    }
  }

  /// Keys the APP still needs to hear while the user is typing. Enter commits a typed
  /// frequency or sends a chat line; Escape backs out. Everything else — letters, arrows
  /// (which move the caret), Backspace (which deletes) — belongs to the field alone.
  ///
  /// ★ UP and DOWN are mirrored too, and LEFT/RIGHT deliberately are not. In a search box
  /// there is only one line, so up/down have no caret meaning and are exactly how you would
  /// expect to step from the query into the results beneath it — whereas left/right move the
  /// caret and must stay the field's. `super` is still called either way, so a multiline box
  /// keeps its line-to-line caret movement regardless.
  private static let typingPassthrough: Set<String> = ["Enter", "Escape", "ArrowUp", "ArrowDown"]

  override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
    let typing = isTypingInTextField
    // A NUMBER PAD cannot use letters, so they stay ours even while it has focus.
    let numeric = typing && focusedFieldIsNumeric
    for p in presses {
      guard let k = p.key, let n = VibeKeyWindow.name(for: k) else { continue }
      let mine = VibeKeyWindow.typingPassthrough.contains(n)
              || (numeric && n.count == 1 && n >= "A" && n <= "Z")
      if typing && !mine { continue }
      VibePowerModule.emitKey("VibeKeyDown", n)
    }
    // ★ ALWAYS call super while typing, even for the keys we mirrored: the field must
    // still receive them. We are observing here, not intercepting.
    if typing { super.pressesBegan(presses, with: event); return }
    let handled = presses.contains { $0.key.flatMap(VibeKeyWindow.name(for:)) != nil }
    if !handled { super.pressesBegan(presses, with: event) }
  }

  override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
    let typing = isTypingInTextField
    // A NUMBER PAD cannot use letters, so they stay ours even while it has focus.
    let numeric = typing && focusedFieldIsNumeric
    for p in presses {
      guard let k = p.key, let n = VibeKeyWindow.name(for: k) else { continue }
      let mine = VibeKeyWindow.typingPassthrough.contains(n)
              || (numeric && n.count == 1 && n >= "A" && n <= "Z")
      if typing && !mine { continue }
      VibePowerModule.emitKey("VibeKeyUp", n)
    }
    if typing { super.pressesEnded(presses, with: event); return }
    let handled = presses.contains { $0.key.flatMap(VibeKeyWindow.name(for:)) != nil }
    if !handled { super.pressesEnded(presses, with: event) }
  }

  // ── Pointer scroll (mouse wheel / trackpad) ────────────────────────────────
  // ★ There is no scroll event in React Native. The way to receive an indirect
  // scroll on iPadOS/Mac is a UIPanGestureRecognizer with allowedScrollTypesMask
  // set — it then reports wheel and two-finger trackpad scrolling as a pan.
  //
  // ★ allowedTouchTypes is emptied so it can NEVER recognise a real finger: a
  // recogniser on the window that swallowed direct touches would break every
  // gesture in the app, the drums included. cancelsTouchesInView is off for the
  // same reason — this observes, it does not intercept.
  private var lastScroll: CGPoint = .zero

  func installScrollRecognizer() {
    let pan = UIPanGestureRecognizer(target: self, action: #selector(onScroll(_:)))
    pan.allowedScrollTypesMask = .all
    pan.allowedTouchTypes = []            // indirect input only — never a finger
    pan.cancelsTouchesInView = false
    addGestureRecognizer(pan)
  }

  @objc private func onScroll(_ g: UIPanGestureRecognizer) {
    switch g.state {
    case .began:
      lastScroll = .zero
    case .changed:
      let t = g.translation(in: self)
      // Deltas, not absolutes: JS accumulates its own, exactly as the drums do.
      let dx = t.x - lastScroll.x
      let dy = t.y - lastScroll.y
      lastScroll = t
      if dx != 0 || dy != 0 {
        // ★ Location included so JS can HOVER-SCOPE: a scroll over the VFO drum
        // drives that drum, over the zoom drum drives that one. Without the
        // pointer position the mapping can only be global.
        let p = g.location(in: self)
        VibePowerModule.emitScroll(Double(dx), Double(dy), Double(p.x), Double(p.y))
      }
    default:
      lastScroll = .zero
    }
  }

  // A cancelled press must release a sweep too, or a held arrow could stick.
  override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
    for p in presses {
      if let k = p.key, let n = VibeKeyWindow.name(for: k) {
        VibePowerModule.emitKey("VibeKeyUp", n)
      }
    }
    super.pressesCancelled(presses, with: event)
  }
}

class SceneDelegate: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?

  func scene(
    _ scene: UIScene,
    willConnectTo session: UISceneSession,
    options connectionOptions: UIScene.ConnectionOptions
  ) {
    guard let windowScene = scene as? UIWindowScene else { return }
    guard let appDelegate = UIApplication.shared.delegate as? AppDelegate,
          let factory = appDelegate.reactNativeFactory else { return }

    let window = VibeKeyWindow(windowScene: windowScene)

    // Cold-start deep link (vibesdr://). Under the scene lifecycle the launch URL
    // arrives HERE, in connectionOptions — never in didFinishLaunchingWithOptions.
    //
    // It must be handed to RN as a launch option: Linking.getInitialURL() resolves
    // from launchOptions[.url], so passing nil makes it return null on EVERY cold
    // start. Posting RCTOpenURLNotification instead does not work either — that
    // fires while RN is still starting, before JS mounts its 'url' listener, so
    // the link is silently dropped and the app opens the default instance.
    var launchOptions: [AnyHashable: Any] = [:]
    if let url = connectionOptions.urlContexts.first?.url {
      launchOptions[UIApplication.LaunchOptionsKey.url] = url
    }

    // Reuses RN's high-level start path, but hosts the root view in the scene's
    // window (sets rootViewController + makeKeyAndVisible internally).
    factory.startReactNative(withModuleName: "main", in: window, launchOptions: launchOptions)
    window.installScrollRecognizer()
    self.window = window
    appDelegate.window = window

    // Universal links (https) still arrive as user activities.
    for activity in connectionOptions.userActivities {
      _ = RCTLinkingManager.application(UIApplication.shared, continue: activity, restorationHandler: { _ in })
    }
  }

  // The user swiped the app out of the switcher (or the system is releasing the scene).
  // Fire the goodbye so the watch shows "Phone app closed" and STOPS pinging us — otherwise
  // its heartbeat relaunches us headless and SDR audio pours out the speaker mid-call.
  func sceneDidDisconnect(_ scene: UIScene) {
    VibeWatchModule.appWillTerminate()
  }

  // Warm deep link (app already running).
  func scene(_ scene: UIScene, openURLContexts URLContexts: Set<UIOpenURLContext>) {
    guard let url = URLContexts.first?.url else { return }
    RCTLinkingManager.application(UIApplication.shared, open: url, options: [:])
  }

  // Warm universal link.
  func scene(_ scene: UIScene, continue userActivity: NSUserActivity) {
    _ = RCTLinkingManager.application(UIApplication.shared, continue: userActivity, restorationHandler: { _ in })
  }
}

class ReactNativeDelegate: ExpoReactNativeFactoryDelegate {
  // Extension point for config-plugins

  override func sourceURL(for bridge: RCTBridge) -> URL? {
    // needed to return the correct URL for expo-dev-client.
    bridge.bundleURL ?? bundleURL()
  }

  override func bundleURL() -> URL? {
#if DEBUG
    return RCTBundleURLProvider.sharedSettings().jsBundleURL(forBundleRoot: ".expo/.virtual-metro-entry")
#else
    return Bundle.main.url(forResource: "main", withExtension: "jsbundle")
#endif
  }
}
