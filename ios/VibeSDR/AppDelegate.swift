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

    let window = UIWindow(windowScene: windowScene)

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
