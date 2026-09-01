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
  private static func name(for key: UIKey, typing: Bool = false) -> String? {
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
      let raw = key.charactersIgnoringModifiers
      // ★★ THE ARROW ALIASES. `,` `.` `-` `=` ARE the arrow keys — see the note above the
      // arrowAlias table. Suppressed while typing, because every one of them is a character
      // somebody needs in a text box.
      if !typing, let a = VibeKeyWindow.arrowAlias[raw] { return a }
      let c = raw.uppercased()
      return (c.count == 1 && c >= "A" && c <= "Z") ? c : nil
    }
  }

  /// ★★ FOUR PUNCTUATION KEYS THAT ARE LITERALLY THE ARROW KEYS.
  ///
  /// Stuart's find, and the good part is the framing: `<` and `>` are the LEFT-RIGHT axis,
  /// `-` and `+` are the UP-DOWN axis. Not a tuning shortcut — an alias for the arrows
  /// themselves, resolved here at the very bottom so that NOTHING downstream knows the
  /// difference. Every menu, list, dropdown, decoder box and dial gets them for nothing, and
  /// there is no second code path to keep in step with the first. On the waterfall they read
  /// as tune and zoom; in a menu the same keys move the highlight and drag the sliders.
  ///
  /// ★ They are unconditional rather than a Full Keyboard Access fallback. FKA is what sent us
  /// looking, but a key that only exists in a mode nobody can see is a key nobody finds — and
  /// `<` `>` for tuning is what every radio ever built is marked with anyway.
  ///
  /// ★ Chosen because they are NOT letters, so they cannot collide with any shortcut on any
  /// surface, and because iOS leaves them alone: FKA claims the actual navigation keys, which
  /// is precisely why these get through when the arrows themselves do not.
  ///
  /// ★★ SUPPRESSED WHILE TYPING, and this is the sharp edge. `.` is a decimal point, `-` and
  /// `.` both live in URLs and IP addresses. Worse, they alias to names in `typingPassthrough`,
  /// so without the guard a `-` typed into a server address would BOTH type itself and scroll
  /// the list underneath. Hence `typing` is threaded down here rather than checked by the
  /// caller alone.
  ///
  /// ★ The consequence is a gap we cannot design away: inside a focused text box there is no
  /// single-key substitute available AT ALL, because every key is a character somebody needs.
  /// The bookmarks search — where the arrows walk the results while you are still typing — is
  /// the one place that still wants Shift with an arrow under FKA. One documented island is a
  /// better trade than blocking the whole thing on the only case that has no solution.
  /// Did this name come from the physical key it claims to be? False for the punctuation
  /// aliases and for anything reached with a modifier held — see emitKey's note.
  private static func isPlain(_ key: UIKey) -> Bool {
    key.modifierFlags.isEmpty && arrowAlias[key.charactersIgnoringModifiers] == nil
  }

  private static let arrowAlias: [String: String] = [
    ",": "ArrowLeft", "<": "ArrowLeft",
    ".": "ArrowRight", ">": "ArrowRight",
    "=": "ArrowUp", "+": "ArrowUp",       // zoom IN / move up
    "-": "ArrowDown", "_": "ArrowDown",   // zoom OUT / move down
  ]

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
      guard let k = p.key, let n = VibeKeyWindow.name(for: k, typing: typing) else { continue }
      let mine = VibeKeyWindow.typingPassthrough.contains(n)
              || (numeric && n.count == 1 && n >= "A" && n <= "Z")
      if typing && !mine { continue }
      VibePowerModule.emitKey("VibeKeyDown", n, VibeKeyWindow.isPlain(k))
    }
    // ★ ALWAYS call super while typing, even for the keys we mirrored: the field must
    // still receive them. We are observing here, not intercepting.
    if typing { super.pressesBegan(presses, with: event); return }
    let handled = presses.contains { $0.key.flatMap { VibeKeyWindow.name(for: $0, typing: typing) } != nil }
    if !handled { super.pressesBegan(presses, with: event) }
  }

  override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
    let typing = isTypingInTextField
    // A NUMBER PAD cannot use letters, so they stay ours even while it has focus.
    let numeric = typing && focusedFieldIsNumeric
    for p in presses {
      guard let k = p.key, let n = VibeKeyWindow.name(for: k, typing: typing) else { continue }
      let mine = VibeKeyWindow.typingPassthrough.contains(n)
              || (numeric && n.count == 1 && n >= "A" && n <= "Z")
      if typing && !mine { continue }
      VibePowerModule.emitKey("VibeKeyUp", n, VibeKeyWindow.isPlain(k))
    }
    if typing { super.pressesEnded(presses, with: event); return }
    let handled = presses.contains { $0.key.flatMap { VibeKeyWindow.name(for: $0, typing: typing) } != nil }
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

    /* ★★★ NEVER START REACT NATIVE TWICE. scene(willConnectTo:) runs on every scene connection,
     *   and a scene can connect a SECOND time in one process: the system releases the scene from
     *   an app that is still running in the background (ours is, whenever Buddy is driving it and
     *   audio is holding the process up), and reconnects it when the user opens the app again.
     *   This method then built a second window and called startReactNative over a JavaScript
     *   runtime that was already mounted — a second surface for the same "main" module on top of
     *   a live one. A prime suspect for the PURE BLACK SCREEN on opening the app after Buddy has
     *   been using it, which is exactly and only when this can happen.
     * ★★★ AND IT IS NOT THE CRASH BOUNDARY, which is what makes this the lead worth taking: since
     *   build 222 that boundary renders the error and the component stack, and Stuart reports the
     *   screen is still PURE black with no text on 224. Something is drawing over everything, or
     *   nothing is being drawn at all — not a React error being caught and reported.
     * ★ Logged either way, so the next test says which of those it is instead of us guessing
     *   again. If this line appears twice in one launch, that is the bug. */
    /* ★★★ AND A REUSED WINDOW MUST BE RE-PARENTED TO THE SCENE THAT IS SHOWING IT. A UIWindow
     *   belongs to a windowScene; showing one that still points at a different (or dead) scene
     *   puts it nowhere, no frame is ever drawn, and iOS goes on displaying the LAUNCH SCREEN —
     *   which is systemBackgroundColor, i.e. PURE BLACK in dark mode, with nothing on it.
     *   That is the black screen's shape exactly: no text, no crash-boundary message, nothing to
     *   read, because what is on screen is not our UI failing to render — it is the launch image
     *   that was never replaced.
     * ★★★ WHICH MEANS MY OWN GUARD FROM BUILD 225 COULD HAVE BEEN CAUSING IT. Stuart saw a black
     *   screen before that, so it is not the origin — but a reuse branch that hands a stale window
     *   to a fresh scene would make it deterministic rather than occasional, and I added it while
     *   trying to fix this. Re-parenting first is the correction. */
    if let existing = self.window {
      NSLog("[VibeSDR] scene reconnect — re-parenting the existing RN window to the new scene")
      existing.windowScene = windowScene
      existing.isHidden = false
      existing.makeKeyAndVisible()
      appDelegate.window = existing
      return
    }
    NSLog("[VibeSDR] scene connect — starting React Native")

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

  /// The scene went away — EITHER the user swiped the app out of the switcher OR the system
  /// reclaimed the UI from an app that is still running. Those need opposite responses, and
  /// sceneDisconnected() tells them apart by checking whether we are still alive afterwards.
  /// See the note on VibeWatchModule.sceneDisconnected.
  func sceneDidDisconnect(_ scene: UIScene) {
    NSLog("[VibeSDR] scene disconnected (state=%ld)", UIApplication.shared.applicationState.rawValue)
    window = nil
    VibeWatchModule.sceneDisconnected()
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
