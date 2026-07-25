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
    default:
      // Letters come through as their characters; ignore anything with modifiers so
      // system shortcuts (Cmd-Q and friends) are left entirely alone.
      guard key.modifierFlags.isEmpty else { return nil }
      let c = key.charactersIgnoringModifiers.uppercased()
      return (c.count == 1 && c >= "A" && c <= "Z") ? c : nil
    }
  }

  override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
    var handled = false
    for p in presses {
      guard let k = p.key, let n = VibeKeyWindow.name(for: k) else { continue }
      VibePowerModule.emitKey("VibeKeyDown", n)
      handled = true
    }
    if !handled { super.pressesBegan(presses, with: event) }
  }

  override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
    var handled = false
    for p in presses {
      guard let k = p.key, let n = VibeKeyWindow.name(for: k) else { continue }
      VibePowerModule.emitKey("VibeKeyUp", n)
      handled = true
    }
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
        VibePowerModule.emitScroll(Double(dx), Double(dy))
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
