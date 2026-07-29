# BRIEF: Watch Status Glyphs — Connection Method + Link Quality + Hint-Pill Relocation

**Branch:** `experimental` (analysis at HEAD `769e158`)
**Scope:** the standalone watch spike only — `spike/WristSDR/…`. The companion watch app is untouched.
**Files touched:** `spike/WristSDR/WristSDR/SpikeLink.swift`, `spike/WristSDR/WristSDR/ContentView.swift`, one new file `spike/WristSDR/WristSDR/StatusGlyphs.swift` (or fold into `ContentView.swift`). Reference-only: `spike/WristSDR/WristSDR/AudioSocket.swift`, `spike/WristSDR/WristSDR/ControlMenu.swift`, `src/components/SectionIcon.tsx`.

Three small, purely-additive pieces. No behaviour on the existing waterfall/audio/tuning paths changes. Implement A first (it is the data both glyphs read), then B, then C.

- **Part A — `SpikeLink.transport` + `SpikeLink.linkQuality`.** Two published values the UI can bind to. `transport` from `NWPathMonitor`; `linkQuality` derived from the signals the hint logic already uses.
- **Part B — the two glyphs.** A connection-method glyph (iPhone / WiFi / cellular / none) and a link-quality glyph (the ported three-node "instance" triangle, tinted green/yellow/red, X when down), as a vertical pill in the **bottom-right**, mirroring the battery pill.
- **Part C — relocate the hint pill.** Move the hop-diagnostic pill from the top of the screen to **directly above the band-label pill** so it stops fouling live waterfall rows and sits beside the new glyphs.

---

# PART A — `SpikeLink` transport + link-quality

## A1. `transport` (connection method)

The classification already exists in the spike — `AudioSocket.pathName(_:)` (~101–111) maps `NWPath` interface types, and its own comment records the key empirical fact: the paired-iPhone relay surfaces as `.other`. Promote that into a first-class, published value on `SpikeLink` so both the glyph and any future logic read one source of truth. (This aligns with Apple TN3135: on watchOS the companion tunnel presents as `.other`; it is a well-supported heuristic, not a documented contract — keep the mapping in this one place with a comment saying so.)

Add to `SpikeLink`:

```swift
enum Transport { case iphone, wifi, cellular, none }
@Published var transport: Transport = .none
```

Start an `NWPathMonitor` in `SpikeLink` (alongside the existing battery timer setup):

```swift
private let pathMon = NWPathMonitor()

// in start()/init:
pathMon.pathUpdateHandler = { [weak self] p in
  let t: Transport
  if p.status != .satisfied            { t = .none }
  else if p.usesInterfaceType(.wifi)     { t = .wifi }
  else if p.usesInterfaceType(.cellular) { t = .cellular }
  else if p.usesInterfaceType(.other)    { t = .iphone }   // companion relay (TN3135)
  else                                    { t = .none }
  Task { @MainActor in if self?.transport != t { self?.transport = t } }
}
pathMon.start(queue: DispatchQueue(label: "vibe.path"))
```

Cancel it in the same teardown that stops the battery timer. Note deliberately: we do **not** attempt a 4G/5G split — CoreTelephony radio-access-technology is not reliably available on watchOS, and "cellular vs not" is all the glyph needs.

## A2. `linkQuality` (server-link health)

Four discrete states, no fade. Reuse the signals the hint logic already computes so the glyph and the pill can never disagree. `rawHint` (ContentView ~909–936) already distinguishes "rows still arriving but link poor" from "rows stopped" — quality maps straight onto that.

```swift
enum LinkQuality { case good, degraded, poor, down }
```

Derivation (compute in `ContentView` beside `rawHint`, or expose from `SpikeLink`; keep it next to whatever owns `hint`):

- **`.down`** (red X): `transport == .none`, **or** the hard `stalledMessage` overlay condition is met (no server connection at all).
- **`.poor`** (red): a hint is live **and** rows have stopped (`gap > hintRowGap`) — reconnecting / stalled.
- **`.degraded`** (yellow): a hint is live **but** rows are still arriving (`gap <= hintRowGap`) — the jerky-but-working `serverHop` case `rawHint` already special-cases.
- **`.good`** (green): no hint and rows fresh.

`serverLink` today is coarse (3 while frames flow, 1 when stalled — `SpikeLink` ~56–58); it is fine as a tie-breaker but the hint/gap signals above give the cleaner four-way split. Do **not** derive quality from `link.level` — that is the tuned station's RF strength behind the readout (`ContentView.readout`, the `SignalGradient` fill), a different meter entirely. A strong station on a dying link must still show a red quality glyph.

---

# PART B — the glyphs

## B1. Method glyph (single SF Symbol)

```swift
switch link.transport {
  case .iphone:   Image(systemName: "iphone")
  case .wifi:     Image(systemName: "wifi")
  case .cellular: Image(systemName: "antenna.radiowaves.left.and.right")  // tower/broadcast; `cellularbars` is the alt
  case .none:     Image(systemName: "xmark").foregroundStyle(.red)
}
```

The phone glyph means "the watch is reaching the internet via the paired iPhone" — how the iPhone itself is connected is irrelevant and unknowable to the watch, which is exactly the Control-Centre semantic.

## B2. Quality glyph — port the phone's "instance" triangle 1:1

The instances menu uses `SectionIcon name="instance"` — three connected dots in a triangle (`SectionIcon.tsx:58–59`). Reproduce it exactly as a SwiftUI `Shape` so phone and watch are pixel-identical; no SF Symbol matches it. Source geometry (viewBox `0 0 24 24`, stroke 1.7, round caps, no fill):

- nodes (circles, r = 2): **(6, 7)**, **(18, 7)**, **(12, 18)**
- wires: top edge `(8,7)→(16,7)`; left diagonal `(7.2, 8.7)→(10.8, 16.3)`; right diagonal `(16.8, 8.7)→(13.2, 16.3)`

```swift
struct InstanceNodes: Shape {
  func path(in r: CGRect) -> Path {
    let s = min(r.width, r.height) / 24            // scale from the 24×24 source
    func P(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x*s, y: y*s) }
    var p = Path()
    // wires
    p.move(to: P(8,7));    p.addLine(to: P(16,7))
    p.move(to: P(7.2,8.7)); p.addLine(to: P(10.8,16.3))
    p.move(to: P(16.8,8.7)); p.addLine(to: P(13.2,16.3))
    // nodes
    for c in [P(6,7), P(18,7), P(12,18)] {
      p.addEllipse(in: CGRect(x: c.x - 2*s, y: c.y - 2*s, width: 4*s, height: 4*s))
    }
    return p
  }
}
```

Render tinted, `xmark` swapped in when down:

```swift
Group {
  if quality == .down {
    Image(systemName: "xmark").foregroundStyle(.red)
  } else {
    InstanceNodes()
      .stroke(qualityTint, style: StrokeStyle(lineWidth: 1.7 * scale, lineCap: .round, lineJoin: .round))
  }
}
.opacity(link.hint != nil ? pulse : 1)   // breathe only while a hint pill is up

var qualityTint: Color {
  switch quality { case .good: .green; case .degraded: .yellow; case .poor: .red; case .down: .red }
}
```

**Breathing:** reuse the existing `pulse` state (ContentView line 87) — it is already driven `1 ↔ 0.5` while the hint pill is on screen and reset to `1` on disappear (hintPill `.onAppear`/`.onDisappear`, ~979–980). Binding the glyph's opacity to `pulse` gated on `hint != nil` keeps it in lockstep with the pill: static when the link is healthy, gently breathing whenever a warning is showing. Opacity only — nothing per-frame or re-laid-out.

## B3. Placement — mirror the battery

The battery lives bottom-left as a slim **vertical** pill on a `Color.black.opacity(0.55)` capsule scrim (`BatteryPillV`, `ControlMenu.swift` ~168; placed in `ContentView` ~226–234 with `.padding(.leading, 8).padding(.bottom, 6)`, hit-testing off). Make the status cluster its mirror twin:

```swift
VStack {
  Spacer()
  HStack {
    Spacer()
    VStack(spacing: 5) {          // vertical column hugs the corner curve like the battery
      methodGlyph                 // top: what we're connected THROUGH
      qualityGlyph                // below: how WELL
    }
    .font(.system(size: 13, weight: .semibold))
    .foregroundStyle(.white)      // method glyph white; quality glyph carries its own tint
    .padding(.horizontal, 4).padding(.vertical, 5)
    .background(Color.black.opacity(0.55), in: Capsule())   // solid scrim, never blur
    .padding(.trailing, 8).padding(.bottom, 6)
  }
}
.ignoresSafeArea()
.allowsHitTesting(false)          // must never eat a waterfall tap — same as the battery
```

Rationale, so it isn't "fixed" later:
- **Vertical, not horizontal.** The readout was pulled to centre because the watch's rounded corners clip figures shoved into the bottom corners (see the comment atop `ContentView.readout`). The battery dodges that by being a slim vertical pill; a horizontal pair would poke back into the corner arc. A vertical column stays clear the same way.
- **One shared scrim.** A single capsule holding both glyphs reads as the battery's symmetric counterpart, not two floating dots. Darkening, never frosting — the house rule for everything over the waterfall.
- **`.allowsHitTesting(false)`** — status chrome, like the battery; it must not steal taps meant for the waterfall/numpad.

## B4. One collision to eyeball

When `crownMode != .tune`, `crownOverlay` draws a meter hard against the **right edge**, vertically centred (~765–822). The status column is at the very bottom-right, so they occupy different bands and should clear — but check on-device. If it's tight on the 40mm, fade the status column out while that crown meter is up.

---

# PART C — relocate the hint pill

Today the pill renders in a top-anchored VStack with `.padding(.top, 46)` to clear the clock (ContentView ~263–276). Move it into the bottom stack, immediately above `bandLabel`.

1. **Delete** the top-anchored `if let h = hint { VStack { hintPill(h).padding(.top,46); Spacer() } }` block.
2. **Insert** the pill into the existing bottom VStack (~288–309), above `bandLabel`:

```swift
VStack(spacing: 2) {
  Spacer()
  if let h = hint { hintPill(h).padding(.bottom, 2) }   // now above the band pill
  bandLabel
  Button { if !locked { showNumpad = true } } label: { readout }
    .buttonStyle(.plain)
}
.padding(.horizontal, 6)
.padding(.bottom, 4)
```

3. Drop the `.padding(.top, 46)` — it was only there to dodge the clock, which is no longer relevant at the bottom.
4. Leave `hintPill`'s internals, `rawHint`/`syncHint` debounce, and the `pulse` animation exactly as they are. The pill keeps its own scrim, so stacking it tight above the band label (which also has its own scrim) reads fine.

Why: the newest waterfall rows are at the **top** — the line you tune by. When the link is rough, data is often still flowing there, just raggedly, so a top pill covers the very thing the user is watching. Moving it down puts it over seconds-old waterfall nobody is reading, and lands it right next to the new glyphs so the pill + method + quality read as one "state of the link" cluster.

---

# Constraints

- **Spike-only.** Nothing outside `spike/WristSDR/` changes. The companion app's own status chrome is out of scope.
- **No per-frame React/Skia-equivalent churn.** `transport` and `linkQuality` update on path changes and the existing state cadence, not per waterfall frame. The glyphs are plain SwiftUI; the only animation is the shared `pulse` opacity, which already exists.
- **Solid scrims, never blur** — every new background is `Color.black.opacity(0.55)`, matching the battery/lock/band chrome. Blur smears the waterfall underneath (the codebase rejects it repeatedly).
- **Hit-testing off** on the status cluster — status chrome must not intercept taps.
- **Do not wire quality to `link.level`.** Link health and station RF strength are different meters (see A2).
- **`.other` = iPhone is a heuristic.** Confine the mapping to the one `pathUpdateHandler`, commented, referencing TN3135. No 4G/5G split.
- GPL-3.0 + `APPSTORE-EXCEPTION.md`: all new code original, Stuart Carr copyright. No third-party additions.

---

# Acceptance criteria / test matrix

Primary device: iPhone 17 Pro Max + watch; reference server: the UberSDR instance. The Raunds-style test (phone powered off, watch cellular) is the headline case for the method glyph.

1. **Method — iPhone relay:** phone on and nearby, watch streaming. Method glyph shows `iphone`.
2. **Method — watch WiFi:** phone off / out of Bluetooth range, watch on known WiFi. Glyph shows `wifi` within a second or two of the path settling.
3. **Method — watch cellular:** phone fully powered off, watch on cellular only (the Raunds run). Glyph shows the cellular/tower symbol — **not** a phone, **not** an X.
4. **Method — none:** airplane mode on the watch. Glyph shows red `xmark`.
5. **Quality — good:** healthy stream, no hint pill. Triangle is green and **static** (no breathing).
6. **Quality — degraded:** induce a rough-but-flowing link (weak cellular; rows still arriving). Triangle goes yellow **and** breathes; the hint pill is visible **above the band label**, not at the top.
7. **Quality — poor:** rows stop under a live reconnect. Triangle goes red and breathes; pill reads the reconnecting/hop state.
8. **Quality — down:** kill the server socket / no path. Triangle becomes a red X; breathing stops.
9. **Pill relocation:** in every degraded/poor case the pill sits above the band pill and the **top** waterfall rows are never covered. Confirm the pill + glyph cluster read as one group bottom-right.
10. **Corner-clip check (40mm / SE):** the vertical status pill is fully visible, not clipped by the rounded corner, and does not overlap the centred readout even with a long CW frequency string (which scales down).
11. **Crown-overlay coexistence:** enter Volume/Zoom (crown meter on the right edge) while a hint is showing — status column and crown meter do not visually collide (or the column fades per B4).
12. **Tap-through:** tapping through the status pill region still hits the waterfall/opens the numpad as before.

Add `dbg()` on each `transport` change and each `linkQuality` transition (with the reason: which of A2's branches fired) — routes through the existing debug sink, free in release.

---

# Notes / non-goals

- **No 4G/5G indicator** — deliberately. The cellular glyph alone carries "on the watch's own cellular," which is all the spec asks and all watchOS reliably exposes.
- **No tap behaviour** on the new glyphs — they are passive status, like the battery. If a future version wants "tap the quality glyph to see detail," that's a separate change and would need the cluster to opt back into hit-testing.
- **`linkQuality` intentionally piggybacks on the hint machinery** rather than inventing a parallel health model — one set of thresholds, one debounce, pill and glyph guaranteed consistent.
- The initial `NWPathMonitor` callback always fires once with the current path on start; that's fine here (we want the current state immediately), unlike the recovery brief where the first report had to be ignored as a non-change.
