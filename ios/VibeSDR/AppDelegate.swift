internal import Expo
import React
import ReactAppDependencyProvider
import AVFoundation
import UIKit
import UserNotifications

@main
class AppDelegate: ExpoAppDelegate {
  var window: UIWindow?

  var reactNativeDelegate: ExpoReactNativeFactoryDelegate?
  var reactNativeFactory: RCTReactNativeFactory?

  public override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
  ) -> Bool {
    /* Register as an audio app at launch so iOS tracks us for Now Playing / lock screen controls.
     * Must be done before any audio starts — omitting this was preventing media controls appearing.
     *
     * ★★★ BUT DO NOT ACTIVATE THE SESSION HERE, AND THIS IS WHY CARPLAY KEPT SWITCHING TO US.
     *     `.playback` does not mix, so `setActive(true)` INTERRUPTS whatever is playing and takes
     *     Now Playing with it. Doing that at launch would be rude in any app; in this one it is
     *     much worse, because we are launched HEADLESS every time Buddy pokes the phone — so a
     *     drive with Apple Music on became: wrist wakes the phone, VibeSDR silently seizes the
     *     audio session, and CarPlay swaps to a receiver that is not playing anything.
     *     Stuart, 2026-09-02: "the now playing keeps trying to default to VibeSDR even though I
     *     had been playing my Apple Music on the drive previously."
     * ★★ NOTHING IS LOST. setCategory alone is what declares how we behave; ACTIVATION is what
     *    claims the route, and the six paths that actually start audio each activate their own
     *    session already (VibePowerModule x5, VibeSilentAudio x2). So controls still appear the
     *    moment there is something to control — which is the only moment they should.
     * ★ The rule this belongs to: never take a shared resource before you have a use for it. A
     *   headless launch must be inaudible and invisible to the rest of the phone. */
    try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)

    /* ★★ V12 HELD SESSION — the "VibeSDR paused" note's delegate, set HERE so a tap that launches us is delivered
     *  (VibeHoldNotice in VibePowerModule.swift). Setting a delegate asks the user nothing: permission is requested only
     *  in the app, the first time a quiet mode on a holding server needs it. */
    UNUserNotificationCenter.current().delegate = VibeHoldNotice.shared

    /* ★★★ THE FIRST CRUMB, AND THE MOST IMPORTANT ONE. A watch-woken launch is HEADLESS: the
     *   state here is `B`, and every theory about the black screen turns on whether that is so.
     *   See VibeCrumbs — this goes to Documents/boot-crumbs.log, because our NSLog is not
     *   readable off Stuart's device and the unified-log archive is too short to catch a launch. */
    VibeCrumbs.session(launchOptions?.isEmpty == false ? "with options" : "plain")

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
/// ★★★ ONE SCHEME (Stuart, 2026-10-18 — src/utils/keyActions.ts, the same table the web page uses): arrows and
/// W A S D move the highlight, Enter selects, Backspace and Escape go back, < > tune, - + zoom. This window reports
/// ONLY those keys, by their DOM names, and src/utils/hardwareKeys.ts maps them — so the mapping lives in one place for
/// the app and the web page. Every older shortcut (letters that opened panels, Tab into the decoder box, Space,
/// H/K/M units, P for profiles, T/B tabs, F/D/E/S on the server list) was stripped as too complicated.
///
/// ★ Deliberately the WINDOW rather than a first-responder view, so we never fight React Native for first responder.
/// Keys we do not name (Tab, Space, every other letter, anything with Cmd/Ctrl/Alt) go straight up the responder
/// chain untouched — Full Keyboard Access, VoiceOver and Cmd-shortcuts keep them.
///
/// Down and up are separate events on purpose: tune and zoom reuse the tuner keys' tap-steps / hold-sweeps law, and a
/// sweep has to know when the key was let go. (iOS does not auto-repeat pressesBegan; PanelNav repeats movement.)
class VibeKeyWindow: UIWindow {
  /// The punctuation and letters that are ours. Shift is allowed — it is how < > + _ are typed — and every shifted
  /// form means the same as its unshifted key, so `charactersIgnoringModifiers` is all we need.
  private static let ourCharacters: Set<String> = [
    "w", "a", "s", "d", "W", "A", "S", "D",
    ",", "<", ".", ">",          // tune down / up
    "-", "_", "=", "+",          // zoom out / in (the numeric keypad's - and + arrive as these too)
  ]

  /// Cmd, Ctrl and Alt: never ours — system and app shortcuts (Cmd-Q, Cmd-W) stay theirs.
  private static func systemChord(_ key: UIKey) -> Bool {
    !key.modifierFlags.intersection([.command, .control, .alternate]).isEmpty
  }

  private static func name(for key: UIKey) -> String? {
    if systemChord(key) { return nil }
    switch key.keyCode {
    case .keyboardLeftArrow:  return "ArrowLeft"
    case .keyboardRightArrow: return "ArrowRight"
    case .keyboardUpArrow:    return "ArrowUp"
    case .keyboardDownArrow:  return "ArrowDown"
    case .keyboardReturnOrEnter, .keypadEnter: return "Enter"
    case .keyboardEscape:     return "Escape"
    case .keyboardDeleteOrBackspace: return "Backspace"
    // ★ Forward delete as well: on a PC keyboard it sits beside Backspace and gets pressed for it.
    case .keyboardDeleteForward: return "Delete"
    default:
      let raw = key.charactersIgnoringModifiers
      return ourCharacters.contains(raw) ? raw : nil
    }
  }

  /// Is this the physical ARROW key, pressed alone? The Full Keyboard Access detector (PanelNav noteKeyForFka) counts
  /// only those as "arrows reach the app" — Shift+arrow and W A S D are the workarounds FKA leaves alone, so counting
  /// them would make the detector hide its own diagnosis.
  private static func isPlain(_ key: UIKey) -> Bool {
    key.modifierFlags.intersection([.shift, .command, .control, .alternate]).isEmpty
  }

  /// ★★ A FOCUSED TEXT FIELD OWNS THE KEYBOARD. RN's TextInput is backed by a UITextField/UITextView, so conformance
  /// to UITextInput is a reliable test; the walk is a human-speed event's worth of work.
  private func focusedTextInput() -> UIResponder? { VibeKeyWindow.focusedTextInput(in: self) }

  private static func focusedTextInput(in root: UIView) -> UIResponder? {
    func find(_ v: UIView) -> UIResponder? {
      if v.isFirstResponder { return v }
      for s in v.subviews { if let r = find(s) { return r } }
      return nil
    }
    let r = find(root)
    return r is UITextInput ? r : nil
  }

  /// The text field holding the cursor in ANY of the app's windows, or nil. For the game controller (VibeGamepad),
  /// which has no window of its own to be told about presses in — the same test, so a pad and a keyboard agree on
  /// when somebody is typing.
  static func focusedTextInputInApp() -> UIResponder? {
    for scene in UIApplication.shared.connectedScenes {
      guard let ws = scene as? UIWindowScene else { continue }
      for w in ws.windows { if let r = focusedTextInput(in: w) { return r } }
    }
    return nil
  }

  private var isTypingInTextField: Bool { focusedTextInput() != nil }

  /// While typing, the app hears only these — and the field still gets them (we observe, never intercept): Enter
  /// commits a typed frequency or a search, Escape leaves the field (see pressesBegan). Everything else — letters,
  /// the arrows (caret), comma, full stop, minus, Backspace (delete) — belongs to the field alone.
  private static let typingPassthrough: Set<String> = ["Enter", "Escape"]

  private func report(_ presses: Set<UIPress>, _ event: String) -> (typing: Bool, handled: Bool) {
    let typing = isTypingInTextField
    var handled = false
    for p in presses {
      guard let k = p.key, let n = VibeKeyWindow.name(for: k) else { continue }
      if typing && !VibeKeyWindow.typingPassthrough.contains(n) { continue }
      handled = true
      VibePowerModule.emitKey(event, n, n.hasPrefix("Arrow") && VibeKeyWindow.isPlain(k), typing)
    }
    return (typing, handled)
  }

  override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
    let r = report(presses, "VibeKeyDown")
    // ★ ALWAYS call super while typing, even for the keys we mirrored: the field must still receive them.
    if r.typing || !r.handled { super.pressesBegan(presses, with: event) }
    // ★★ ESCAPE LEAVES THE FIELD — one layer, like every other back. The cursor goes, and the arrows / W A S D then move
    //    the highlight round the panel the field sits in (the frequency card's units and tabs, the bookmark results); a
    //    second Escape or Backspace closes the panel. The web page does the same.
    if r.typing, presses.contains(where: { $0.key?.keyCode == .keyboardEscape }) {
      DispatchQueue.main.async { [weak self] in _ = self?.focusedTextInput()?.resignFirstResponder() }
    }
  }

  override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
    let r = report(presses, "VibeKeyUp")
    if r.typing || !r.handled { super.pressesEnded(presses, with: event) }
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

  // A cancelled press must release a sweep too, or a held tune/zoom key could stick.
  override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
    for p in presses {
      if let k = p.key, let n = VibeKeyWindow.name(for: k) {
        VibePowerModule.emitKey("VibeKeyUp", n, false, false)
      }
    }
    super.pressesCancelled(presses, with: event)
  }
}

private extension Optional where Wrapped == String {
  var isNilOrEmpty: Bool { self?.isEmpty ?? true }
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
      VibeCrumbs.log("scene willConnect — REUSE, re-parenting existing RN window")
      NSLog("[VibeSDR] scene reconnect — re-parenting the existing RN window to the new scene")
      existing.windowScene = windowScene
      existing.isHidden = false
      existing.makeKeyAndVisible()
      appDelegate.window = existing
      return
    }
    /* ★★ `activationState` rather than UIApplication.applicationState: a scene knows its OWN
     *  state, and on a headless launch the two can disagree. If this says background, the root
     *  view is being built where nothing draws — which is the theory on trial. */
    VibeCrumbs.log("scene willConnect — FRESH, sceneState=\(windowScene.activationState.rawValue)")
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
    VibeCrumbs.log("startReactNative returned — \(describe(window))")

    // Universal links (https) still arrive as user activities.
    for activity in connectionOptions.userActivities {
      _ = RCTLinkingManager.application(UIApplication.shared, continue: activity, restorationHandler: { _ in })
    }
  }

  /* ★★★ A ROOT VIEW BUILT IN A BACKGROUND SCENE DOES NOT START DRAWING BY ITSELF.
   *
   *  Measured from the device log, 2026-09-01, on a Buddy-driven cold boot: the app is launched
   *  HEADLESS, scene(willConnectTo:) runs THEN — while we are in the background — and
   *  startReactNative builds the window and root view there. When the user later opens the app,
   *  UIKit only re-lays-out the scene it already has: there is no second willConnectTo, no
   *  "starting React Native" line, and — the point — no first frame. iOS therefore goes on showing
   *  its own launch image, which is systemBackgroundColor: PURE BLACK in dark mode, with nothing
   *  on it and a perfectly healthy app behind it. That is the black screen, and it is why there was
   *  never any text: nothing was failing to render, nothing was rendering at all.
   *  ★★ WHICH IS WHY IT ONLY EVER HAPPENED AFTER BUDDY. A normal launch creates the scene in the
   *     foreground and draws immediately; only a watch-woken one creates it in the background.
   *  ★ So nudge it when we come forward: make the window key and visible again and force a layout
   *    pass. Cheap, idempotent, and a no-op on a normal launch where the frame already exists. */
  func sceneWillEnterForeground(_ scene: UIScene) {
    guard let w = window else { return }
    VibeCrumbs.log("scene willEnterForeground — before nudge: \(describe(w))")
    NSLog("[VibeSDR] scene entering foreground — nudging the root view to draw")
    w.isHidden = false
    w.makeKeyAndVisible()
    w.rootViewController?.view.setNeedsLayout()
    w.rootViewController?.view.layoutIfNeeded()
    w.setNeedsLayout()
    VibeCrumbs.log("scene willEnterForeground — after nudge:  \(describe(w))")
  }

  /* ★★★ THE CRUMB THAT ANSWERS THE QUESTION. `didBecomeActive` is the moment the user is LOOKING
   *   at the screen, so what this records is literally what they are seeing. If the black screen is
   *   "the root view never drew", this line says so out loud: a zero frame, a missing
   *   rootViewController, or a root view with NO SUBVIEWS while the app is plainly alive and
   *   feeding the watch. If instead everything here reads healthy, the launch-image theory in
   *   commit fb3deac2 is dead and the fault is above us in React.
   * ★ Deferred one runloop turn as well as sampled immediately: a frame that arrives late is a
   *   different fault from one that never arrives, and only two readings can tell them apart. */
  func sceneDidBecomeActive(_ scene: UIScene) {
    guard let w = window else { VibeCrumbs.log("scene didBecomeActive — NO WINDOW"); return }
    VibeCrumbs.log("scene didBecomeActive — \(describe(w))")
    VibeCrumbs.log("tree: \(hierarchy(w))")
    VibeCrumbs.log("vcs: \(vcTree(w))")
    kickScreenStack(w)
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [weak self] in
      guard let self, let w = self.window else { return }
      VibeCrumbs.log("scene +600ms — \(self.describe(w))")
    }
  }

  /* ★★★ RE-RUN react-native-screens' DEFERRED PUSH, WHICH IS THE BLACK SCREEN.
   *
   *  Measured, build 237: on a watch-driven VibeServer connect React does everything correctly —
   *  "SDRScreen mounted", "nav: [InstancePicker > SDR] index=1" — and the native tree shows
   *  RNSScreenStackView > UILayoutContainerView > UINavigationTransitionView >
   *  UIViewControllerWrapperView with NOTHING INSIDE IT. 12 views where a healthy screen is 441+.
   *  The JS stack is right and the UIKit stack is empty.
   *
   *  RNSScreenStack.mm:599 `updateContainer` returns early when the view has no window, and defers
   *  again while a transitionCoordinator is running; RNSScreenStack.mm:298
   *  `maybeAddToParentAndUpdateContainer` is its ONLY retry, and it is driven by `didMoveToWindow`.
   *  A push that happens while we are HEADLESS — which is every Buddy-driven connect — is deferred,
   *  and because the window never changes afterwards, didMoveToWindow never fires again and the
   *  push is simply never applied. The session runs (audio and spectrum are native, which is why
   *  the wrist works perfectly) and the phone shows an empty navigation controller.
   *
   * ★★ SO CALL THAT HOOK OURSELVES, once, when we come forward. didMoveToWindow is public API and
   *    the path behind it is idempotent: updateContainer's first act is to compare the controller
   *    list and return if it already matches, so on a healthy launch this costs one comparison.
   * ★★★ AND IT DOES NOT REMOUNT REACT. That is the whole reason to do it here rather than
   *     re-dispatching navigation state from JS: resetRoot with the same state would tear down and
   *     rebuild SDRScreen, killing the live session and the audio with it — fixing the picture by
   *     dropping the listener. This touches only UIKit containment.
   *  ★ This is what commit fb3deac2 was reaching for and could not find. Its conclusion — "the root
   *    view is built in a background scene and never draws" — was wrong about the WINDOW (scene,
   *    key window and rootVC are healthy in every capture) and right about the shape one layer
   *    down, at the screen stack. */
  /* ★★★ THE VIEW CONTROLLERS, WHICH IS WHERE THIS ACTUALLY LIVES.
   *
   *  The kick above found the screen stack and changed nothing (viewsTotal 12 before, 12 after,
   *  still 12 at +600 ms — a real failure, not a sampling artefact). And the telling detail is
   *  that the stack is empty from the FIRST foreground: the PICKER never rendered natively either,
   *  so this was never about the SDR push. On a headless launch no screen mounts at all.
   *  ★★ Views cannot answer that. UIViewControllerWrapperView being empty means no child view
   *     controller's view is loaded — and whether that is "no child VC exists" (React/RN Screens
   *     never mounted one) or "the VC exists and UIKit has not loaded its view" (a background app
   *     is not asked to) are opposite bugs with one appearance. The VC tree separates them in one
   *     line, and I have now guessed at this node twice.
   *  ★ isViewLoaded is the whole point and must NOT be read as `vc.view != nil` — touching `.view`
   *    LOADS it, so the naive check destroys the very thing it is measuring. */
  private func vcTree(_ w: UIWindow) -> String {
    var out: [String] = []
    func walk(_ vc: UIViewController, _ d: Int) {
      if out.count >= 20 { return }
      out.append("\(String(repeating: ".", count: d))\(type(of: vc))"
                 + "|loaded=\(vc.isViewLoaded)"
                 + "|children=\(vc.children.count)"
                 + (vc.parent == nil ? "|NOPARENT" : ""))
      for c in vc.children { walk(c, d + 1) }
      if let nav = vc as? UINavigationController {
        out.append("\(String(repeating: ".", count: d)):navStack=\(nav.viewControllers.count)")
      }
    }
    if let r = w.rootViewController { walk(r, 0) }
    return out.joined(separator: " ")
  }

  private func kickScreenStack(_ w: UIWindow) {
    guard let root = w.rootViewController?.view else { return }
    var kicked = 0
    func walk(_ v: UIView) {
      if String(describing: type(of: v)) == "RNSScreenStackView" {
        v.didMoveToWindow()
        kicked += 1
      }
      for s in v.subviews { walk(s) }
    }
    walk(root)
    guard kicked > 0 else { VibeCrumbs.log("kick: no RNSScreenStackView found"); return }
    // ★ Say whether it WORKED, with the same number the fault was diagnosed by. A fix that cannot
    //   be seen in the next capture is a fix nobody can confirm.
    let after = { () -> Int in
      var n = 0
      func c(_ v: UIView) { n += 1; for s in v.subviews { c(s) } }
      c(root); return n
    }()
    VibeCrumbs.log("kick: nudged \(kicked) screen stack(s), viewsTotal now \(after)")

    /* ★★ AND IF THE CONTROLLERS ARE THERE BUT UNLOADED, LOAD THEM. UIKit does not load a view
     *  controller's view while the app is in the background — it has no reason to — and the push
     *  that happened headlessly may therefore have left a perfectly good VC with no view. This is a
     *  no-op when there is no navigation controller or its view is already loaded, which is exactly
     *  the case the vcs: line above will have recorded, so the measurement is not confounded by the
     *  attempt. Re-measured on the next runloop, because loading a view is not synchronous with
     *  the subtree appearing. */
    var navs: [UINavigationController] = []
    func findNav(_ vc: UIViewController) {
      if let n = vc as? UINavigationController { navs.append(n) }
      for c in vc.children { findNav(c) }
    }
    if let r = w.rootViewController { findNav(r) }
    for n in navs {
      n.loadViewIfNeeded()
      n.topViewController?.loadViewIfNeeded()
      n.view.setNeedsLayout(); n.view.layoutIfNeeded()
    }
    if !navs.isEmpty {
      DispatchQueue.main.async { [weak self] in
        guard let self, let w2 = self.window else { return }
        VibeCrumbs.log("kick: after loadViewIfNeeded on \(navs.count) nav(s) — \(self.describe(w2))")
      }
    }
  }

  /// One line describing whether this window can possibly be showing anything: is it in a scene,
  /// is it key, is it hidden, what size is it, and does the React root view have any content yet.
  private func describe(_ w: UIWindow) -> String {
    let rv = w.rootViewController?.view
    let kids = rv?.subviews.count ?? -1
    let deepest = rv.map { v -> Int in
      var n = 0
      func walk(_ v: UIView) { n += 1; for s in v.subviews { walk(s) } }
      walk(v); return n
    } ?? -1
    return "scene=\(w.windowScene == nil ? "NIL" : "set") key=\(w.isKeyWindow) hidden=\(w.isHidden) "
         + "alpha=\(w.alpha) size=\(Int(w.bounds.width))x\(Int(w.bounds.height)) "
         + "rootVC=\(w.rootViewController == nil ? "NIL" : "yes") rootSubviews=\(kids) viewsTotal=\(deepest)"
  }

  /* ★★★ THIRTEEN VIEWS. WHICH THIRTEEN?
   *
   *  Measured on 2026-09-02: a healthy foreground reports viewsTotal=565, and a black screen after
   *  a VibeServer connect reports 13 — with the session ALIVE and feeding the watch, because the
   *  audio and spectrum pumps are native and do not care what JS is doing. So the tree is real but
   *  nearly empty, and the count alone cannot say whether that is the CrashBoundary's error view,
   *  App's full-screen splash overlay (#0A0A12, zIndex 9999 — near-black and would look exactly
   *  like this), a picker still in its loading state, or a root view with nothing in it at all.
   *  Those are four different bugs with one appearance, and guessing between them is what this
   *  whole exercise exists to stop.
   * ★★ So name the classes, with frame, hidden and alpha. An RN view tree carries its component
   *    identity in the class name, so this distinguishes them in one line — and alpha/hidden catch
   *    the case where the real tree IS mounted but something opaque sits on top of it.
   * ★ Depth-capped and count-capped: this is a diagnostic on a hot path, not a dump. */
  /* ★★ THE CAPS WERE TOO TIGHT AND THE DUMP LIED. At depth 4 sat RNSScreenStackView — exactly the
   *  node whose CHILDREN are the question — so the walk stopped there and the output read as "the
   *  navigator is rendering no screens at all". It was rendering some; I simply refused to look.
   *  (`viewsTotal` in describe() has no cap and was right all along: 12 views against a healthy
   *  441-565, so ~6 of them are below this line and unaccounted for.)
   * ★ Deep enough to reach a screen's actual content, and still bounded — a diagnostic, not a dump. */
  private func hierarchy(_ w: UIWindow, limit: Int = 60) -> String {
    var out: [String] = []
    func walk(_ v: UIView, _ depth: Int) {
      if out.count >= limit || depth > 12 { return }
      let f = v.frame
      /* ★ Any TEXT the view carries, because that is what names the screen. A picker mid-load, a
       *  crash message and an empty SDR screen are three different bugs with the same shape, and
       *  one label tells them apart instantly. */
      var label = ""
      if let l = v as? UILabel, let t = l.text, !t.isEmpty { label = "|\"\(t.prefix(24))\"" }
      else if !v.accessibilityIdentifier.isNilOrEmpty { label = "|#\(v.accessibilityIdentifier!)" }
      else if let a = v.accessibilityLabel, !a.isEmpty { label = "|@\(a.prefix(24))" }
      out.append("\(String(repeating: ".", count: depth))\(type(of: v))"
                 + "[\(Int(f.width))x\(Int(f.height))]"
                 + (v.isHidden ? "|HID" : "")
                 + (v.alpha < 0.99 ? "|a\(String(format: "%.2f", v.alpha))" : "")
                 + (v.backgroundColor == nil ? "" : "|bg")
                 + label)
      for s in v.subviews { walk(s, depth + 1) }
    }
    if let rv = w.rootViewController?.view { walk(rv, 0) }
    return out.joined(separator: " ")
  }

  /// The scene went away — EITHER the user swiped the app out of the switcher OR the system
  /// reclaimed the UI from an app that is still running. Those need opposite responses, and
  /// sceneDisconnected() tells them apart by checking whether we are still alive afterwards.
  /// See the note on VibeWatchModule.sceneDisconnected.
  func sceneDidDisconnect(_ scene: UIScene) {
    VibeCrumbs.log("scene didDisconnect")
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
