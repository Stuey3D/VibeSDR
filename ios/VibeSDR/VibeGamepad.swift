import Foundation
import GameController
import UIKit

/// ★★★ GAME CONTROLLERS IN THE APP (V12) — iPhone, iPad, and the Mac (the iPad app, "Designed for iPad").
///
/// This file only REPORTS: the whole pad, as a 'VibePad' event, whenever something on it changes. Buttons go by
/// POSITION (south / east / west / north — an Xbox pad's A and a PlayStation pad's ✕ are both "south"), sticks as
/// −1…1 with y DOWN (the web Gamepad API's convention, so the shared jog wheel needs no flip). src/utils/padActions.ts
/// turns that into the keyboard's own keys by Stuart's scheme and src/utils/hardwareKeys.ts feeds them into the same
/// bus as the hardware keyboard — so a pad drives the app exactly like the keyboard, and the mapping lives in ONE place
/// for the app and the web page. The bit numbers below are PAD_BIT there; keep the two in step (VibeGamepad.kt too).
///
/// ★ Zero cost without a pad: two notification observers and nothing else — no polling, no display link. With one
///   connected, GameController calls valueChangedHandler on the main queue, and we send only real changes (a stick is
///   quantised and dead-zoned, so a resting stick's jitter sends nothing).
/// ★ GCSupportsControllerUserInteraction is NOT set, so UIKit does not ALSO move its own focus with the pad: there is
///   one highlight, ours.
/// ★ `typing`: while a text field has the cursor, the JS side lets only enter and back through (the keyboard's rule)
///   and back lets go of the cursor — VibePowerModule.leaveTextField.
final class VibeGamepad: NSObject {
  static let shared = VibeGamepad()

  // PAD_BIT (src/utils/padActions.ts)
  private static let UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8
  private static let SOUTH = 16, EAST = 32, WEST = 64, NORTH = 128
  private static let L1 = 256, R1 = 512, START = 1024

  private struct Snap: Equatable { var b = 0; var lx: Float = 0; var ly: Float = 0; var rx: Float = 0; var ry: Float = 0 }

  private var started = false
  private var ids: [ObjectIdentifier: String] = [:]
  private var last: [ObjectIdentifier: Snap] = [:]
  private var serial = 0
  private var typing = false
  private var typingAt: CFTimeInterval = 0

  /// Called once, on the main queue, when the bridge creates VibePowerModule.
  func start() {
    dispatchPrecondition(condition: .onQueue(.main))
    guard !started else { return }
    started = true
    let nc = NotificationCenter.default
    nc.addObserver(self, selector: #selector(didConnect(_:)), name: .GCControllerDidConnect, object: nil)
    nc.addObserver(self, selector: #selector(didDisconnect(_:)), name: .GCControllerDidDisconnect, object: nil)
    // A pad already paired and on before the app started is in the list now and never announces itself.
    for c in GCController.controllers() { attach(c) }
  }

  @objc private func didConnect(_ n: Notification) {
    guard let c = n.object as? GCController else { return }
    DispatchQueue.main.async { self.attach(c) }
  }

  @objc private func didDisconnect(_ n: Notification) {
    guard let c = n.object as? GCController else { return }
    DispatchQueue.main.async {
      let k = ObjectIdentifier(c)
      // ★ "gone" makes the JS side send UP for anything still held — a pad switched off mid-press must not leave a
      //   tune sweep running or the highlight walking on its own.
      if let id = self.ids[k] { VibePowerModule.emitPad(["id": id, "gone": true]) }
      self.ids[k] = nil
      self.last[k] = nil
    }
  }

  private func attach(_ c: GCController) {
    let k = ObjectIdentifier(c)
    if ids[k] != nil { return }
    serial += 1
    ids[k] = "gc\(serial)"
    c.handlerQueue = .main
    if let g = c.extendedGamepad {
      g.valueChangedHandler = { [weak self, weak c] _, _ in
        guard let self = self, let c = c else { return }
        self.report(c)
      }
    } else if let m = c.microGamepad {
      m.reportsAbsoluteDpadValues = true
      m.valueChangedHandler = { [weak self, weak c] _, _ in
        guard let self = self, let c = c else { return }
        self.report(c)
      }
    } else {
      ids[k] = nil          // a motion-only or keyboard-like "controller": nothing of ours on it
      return
    }
    report(c)
  }

  /// Dead zone and quantise: a resting stick's jitter sends nothing; a turning one sends ~every 1/32 of travel.
  private static func axis(_ v: Float) -> Float {
    if !v.isFinite || abs(v) < 0.12 { return 0 }
    return (max(-1, min(1, v)) * 32).rounded() / 32
  }

  private func report(_ c: GCController) {
    let k = ObjectIdentifier(c)
    guard let id = ids[k] else { return }
    var s = Snap()
    var sticks = false
    func on(_ p: Bool, _ bit: Int) { if p { s.b |= bit } }
    if let g = c.extendedGamepad {
      sticks = true
      on(g.dpad.up.isPressed, Self.UP); on(g.dpad.down.isPressed, Self.DOWN)
      on(g.dpad.left.isPressed, Self.LEFT); on(g.dpad.right.isPressed, Self.RIGHT)
      // GameController names the face buttons by POSITION on every pad it knows: A bottom, B right, X left, Y top.
      on(g.buttonA.isPressed, Self.SOUTH); on(g.buttonB.isPressed, Self.EAST)
      on(g.buttonX.isPressed, Self.WEST); on(g.buttonY.isPressed, Self.NORTH)
      on(g.leftShoulder.isPressed, Self.L1); on(g.rightShoulder.isPressed, Self.R1)
      on(g.buttonMenu.isPressed, Self.START)
      s.lx = Self.axis(g.leftThumbstick.xAxis.value)
      s.ly = Self.axis(-g.leftThumbstick.yAxis.value)     // GameController's y is UP; ours is DOWN
      s.rx = Self.axis(g.rightThumbstick.xAxis.value)
      s.ry = Self.axis(-g.rightThumbstick.yAxis.value)
    } else if let m = c.microGamepad {
      on(m.dpad.up.isPressed, Self.UP); on(m.dpad.down.isPressed, Self.DOWN)
      on(m.dpad.left.isPressed, Self.LEFT); on(m.dpad.right.isPressed, Self.RIGHT)
      on(m.buttonA.isPressed, Self.SOUTH); on(m.buttonX.isPressed, Self.WEST)
      on(m.buttonMenu.isPressed, Self.START)
    } else {
      return
    }
    if last[k] == s { return }
    let buttonsChanged = last[k]?.b != s.b
    last[k] = s
    // ★ Is a text field holding the cursor? A walk of the view tree — fine for a button, too much to repeat sixty
    //   times a second while a stick turns, so a stick-only change reuses an answer up to a quarter of a second old.
    let now = CACurrentMediaTime()
    if buttonsChanged || now - typingAt > 0.25 {
      typing = VibeKeyWindow.focusedTextInputInApp() != nil
      typingAt = now
    }
    VibePowerModule.emitPad([
      "id": id, "sticks": sticks, "b": s.b,
      "lx": Double(s.lx), "ly": Double(s.ly), "rx": Double(s.rx), "ry": Double(s.ry),
      "typing": typing,
    ])
  }
}
