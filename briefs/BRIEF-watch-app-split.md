# BRIEF: Split The Watch App — VibeSDR Jr (standalone) and VibeSDR Companion (remote)

**Branch:** `experimental` @ `ef2e8b6` (20 July 07:34)
**Supersedes:** `BRIEF-watch-app-merge.md` (the merge this reverses) and an earlier `BRIEF-watch-standalone-link.md` (deleted — its diagnosis is obsolete, see §1.2).

**Severity:** Medium-High. Driven by complexity and product clarity, **not** by the outstanding connection bug, which is unresolved and tracked separately.

**Shape of the job:** restore two known-good trees from git, then port one app's UI onto the other's data source. Very little is being written from scratch.

---

## 0. GATE — before any code moves

**Build and upload VibeSDR Jr through an Xcode Cloud workflow.** Not a local archive.

Local Xcode is a **27 beta**, and App Store submission goes through **Xcode Cloud** for the shipping SDK. That matters here for two reasons:

1. It likely sidesteps a known problem. Apple Developer Forums thread 817223 reports Xcode 26.3's **Organizer** offering only Ad Hoc / Enterprise / Debugging for a **watch-only** app — no App Store Connect option — with an Apple engineer calling it likely a bug. The reporter's project shape matches Jr's exactly (`PRODUCT_TYPE = com.apple.product-type.application`, `SUPPORTED_PLATFORMS = watchos watchsimulator`, no iOS dependency). Xcode Cloud does not use Organizer, so the local-archive path being broken may simply not apply. **Do not treat a failing local archive as a blocker until Cloud has been tried.**
2. Beta-SDK builds are not accepted for App Store submission anyway, so Cloud is the only route regardless.

**What to confirm:**
- Xcode Cloud supports a **watchOS-only product** as a workflow target (verify — not assumed).
- The workflow resolves the **watchOS SDK matching the shipping Xcode**, not the 27 beta.
- The archive action produces an uploadable watch-only build.

**Already satisfied:** the spike has a shared scheme committed at `spike/WristSDR/WristSDR.xcodeproj/xcshareddata/xcschemes/WristSDR.xcscheme`. Xcode Cloud requires this; do not let `genproj.py` regenerate the project without preserving it.

> **TRAP.** `genproj.py` generates the `.xcodeproj`. Xcode Cloud builds from what is **committed**, so a generated project that is gitignored, or regenerated without the shared scheme, will fail in Cloud while building fine locally. Commit the project and the shared scheme, and make `21be91e`'s source-discovery fix part of the restore (Phase 1) rather than an afterthought.

Jr as a separate listing also means a **second Xcode Cloud workflow** and a second App Store Connect product — worth costing against compute minutes before committing.

If Cloud cannot produce an uploadable watch-only build, **stop** — Jr cannot ship standalone and the plan needs rethinking.

**Separately, in App Store Connect (commercial, not code):** confirm (a) a watch-only app can sit in a bundle with an iOS app, and (b) Complete My Bundle applies, so existing £2.99 VibeSDR owners pay only the difference. Neither could be confirmed from public documentation. Without Complete My Bundle the £3.50 bundle does nothing for existing owners.

---

## 1. Why

### 1.1 The watch app carries the entire phone connectivity surface

The spike was **13 files**. `ios/VibeSDRWatch/` at `ef2e8b6` is **40+**: `UberClient`, `KiwiClient`, `OwrxClient`, `OwrxSocket`, `FmDxClient`, `FmdxMp3Decoder`, `ImaAdpcm`, `VibeMdns`, `SDRDirectory`, `Chat`, `BandPlan`, `CpuMeter`, `TuneMemory`, plus three link layers (`WatchLink`, `PhoneClient`, `SpikeLink`). That is all of VibeSDR's backend surface plus a companion remote, in one bundle, on a wrist.

### 1.2 The connection bug is NOT the justification — cause still unknown

**Do not implement this split as a fix for the standalone reconnecting.** The earlier diagnosis (an ungated WCSession heartbeat starving the watch's own link) is obsolete: `ef2e8b6` already confines pinging to `beginDriving()`, `activate()` starts no heartbeat, and `endCompanion()` sends `cmd:stop` and invalidates it. The symptom persists on builds containing that fix, across a reboot of both devices.

Residual Standalone traffic is only `WCSession.activate()` (listening, sends nothing) and a local 1s row-rate timer. **The cause is unidentified.** Next step is device logs from a reconnecting session — which socket drops (audio, spectrum, or both), whether `LinkManager` ladders down before each drop, whether any WCSession activity appears — not further code reading.

The split may incidentally help. It must not be sold as the fix, and shipping it must not close the investigation.

### 1.3 Product

"Which server am I on, and over what link?" has no answer on a 40mm screen today. Two icons answer it before the app opens.

---

## 2. Target state

| | **VibeSDR Jr** | **VibeSDR Companion** |
|---|---|---|
| What | Watch-only app, own listing, 99p | Watch target inside the VibeSDR iOS app |
| Base | `spike/` @ `1c3839e` | `ios/VibeSDRWatch/` @ `0721b20` |
| Connects to | Servers directly, own clients | The iPhone, via WCSession |
| Screens | As-is (already optimised) | **Jr's screens, ported** |
| Needs iPhone | No | Yes |
| WCSession | **None. Framework not linked.** | Yes |
| iCloud | Reads phone-written prefs | **None** — gets everything over WCSession |

> **TRAP.** Jr must contain **zero** `WatchConnectivity` — not disabled, not gated, absent. If the framework is linked and a session can be activated, the bug can return through some future path. The absence is the fix.

---

## 3. Work

### Phase 1 — Restore Jr

The merge began at `1cf6296` (19 July 22:16). Everything before is pre-merge.

```
git checkout -b jr-restore 1c3839e -- spike/
```

`1c3839e` (19 July 19:27) is the last independent spike commit — the state Stuart describes as feature-complete and fully tested on-wrist.

Then:
1. **Cherry-pick `21be91e`** — `genproj.py` discovers sources instead of overwriting the project with half of them. Tooling fix, landed after `1c3839e`, wanted.
2. Promote `spike/WristSDR/` to a first-class project (`watch/VibeSDRJr/` or similar). Bundle ID, product name, watchOS SDK matching the Xcode Cloud toolchain (not the local 27 beta). Keep the shared scheme committed — Xcode Cloud needs it.
3. **Full app icon.** Currently a placeholder `AppIcon.png`. Needs the complete appiconset.
4. **Own `APPSTORE-EXCEPTION.md` §7 grant.** Jr is a separately distributed GPL-3.0 binary; it does not inherit VibeSDR's implicitly. Source must be available for it specifically.
5. Copy `WaterfallBuffer.swift` in.

> **TRAP.** **Do not build a shared Swift package.** `WaterfallBuffer.swift` was the *only* byte-identical file between the two trees; everything else had already diverged. With one file in common a package costs more than it saves and re-couples the apps. Copy it and let them diverge. Revisit only if a third real consumer appears.

**Deferred, Stuart to spec as he goes:** additional waterfall/spectrum settings in Jr. Not in scope here.

### Phase 2 — Restore Companion and port Jr's screens

```
git checkout -b companion-restore 0721b20 -- ios/VibeSDRWatch/
```

`0721b20` (19 July 22:04) is Companion immediately before the mode chooser landed.

Then port Jr's screens onto it. This is the bulk of the job and it is **replacement, not reconciliation**:

- Jr's views read `@Published` properties — `frequency`, `span`, `snr`, `meter`, `level`, `mode`, `filtLo`, `filtHi` — that `WatchLink` already publishes under the same names. The view layer does not care whether those arrived over a WebSocket or over WCSession.
- **Take Jr's view files wholesale, swap the data source to `WatchLink`, delete Companion's old view files.**

> **TRAP.** Do not merge two layouts. Replace one. Reconciling them is exactly how the single bloated app came about.

**What does not port**, because it only exists in Jr by virtue of owning the connection: antenna / cEQ / iMS server settings, chat, and anything writing directly to a backend. In Companion these are either absent or a remote-control request to the phone.

**The one deliberate change to Companion:** replace the favourites list with the **full server picker**, controlling the *phone's* connection rather than the watch's.

`41a035a` already did this work — cherry-pick it rather than rebuilding. It touches `InstancePickerView.swift` (+33/−16) and `SDRDirectory.swift` (+13), and is filtered by which servers the phone can actually receive.

### Phase 3 — Teardown contract (already implemented — retain)

`cmd:stop` exists (`dd5fb8a`, `038dde8`): Companion tells the phone to stop sending the waterfall; `endCompanion()` invalidates the heartbeat.

Carry it across unchanged, but **fire it on `.inactive`/`.background` as well as on explicit exit** — once Companion is a separate screen within the phone app rather than a mode, leaving it IS ending the session. Confirm the phone zeroes `lastWatchMsgAt` on receipt.

> **TRAP.** Do not shorten the phone's 10s `linkAlive` window as an alternative. It is recency-based specifically so a transport blip cannot strand the downlink (see `sawWatch()`). Shortening it reintroduces the stale-flag bug that design killed.

### Phase 4 — iCloud preference sync (phone ↔ Jr only; do last, shippable without)

`NSUbiquitousKeyValueStore`. **Companion is not involved** — it lives inside the iPhone app and reads the phone's own state over WCSession.

Scope is deliberately tiny: **saved servers and display preferences. Nothing else.**

**Carries:**
- **Manually saved servers.** The one that earns its keep — typing a URL on a watch keyboard is miserable. Save a manual server on the iPhone, it is on Jr. Phone → watch is the direction that must be reliable; sync both ways, but that is the use case.
- **Spectrum/waterfall appearance:** palette LUT choice, smoothing, sharpness, needle colour + intensity, peak hold, display unit.

**Does NOT carry:** rows, frequency, mode, meter, tuning state, bookmarks, EiBi schedule, **and not the last-used server** — that is session state, and syncing it would have Jr open on whatever the phone was last tuned to, precisely the surprise the split exists to remove. Each app remembers its own. Bookmarks and EiBi stay local; Jr fetches its own.

**Cadence: once or twice per session. Pull on launch/foreground, push on background or explicit save. No live observer** — do not register for `NSUbiquitousKeyValueStoreDidChangeExternallyNotification`, do not merge mid-session. No conflict handling, because there is no concurrent editing to conflict over.

> **TRAP.** KVS is eventually-consistent (seconds to minutes) and needs an iCloud account and network. It is a **preference** channel, not a state channel. Jr must work fully with iCloud unavailable — treat every synced value as an optional with a local default.

Accepted consequence, documented rather than fixed: a server saved on the phone while Jr is already open will not appear until Jr next launches. That is correct under this model. If reported as a bug, the answer is "by design" — adding live observation would reintroduce exactly the continuous background chatter the split removes.

Quota is roughly 1 MB / ~1024 keys (verify against current docs). A handful of URLs and a 1 KB LUT is nowhere near it; a bookmarks database would blow it.

---

## 4. Verification

**Jr:** sustained session with the paired phone in range and VibeSDR foregrounded on the SDR screen — *this is the regression test*. Also: phone force-quit; phone out of range on cellular; extended session with battery draw measured against the spike's baseline (~34% of a core). Confirm no `WatchConnectivity` symbols in the binary.

**Companion:** full regression — rows, crown tuning, volume, FM-DX, DAB, ADS-B, one-way-link recovery. Plus the new server picker: changing server from the wrist moves the *phone's* connection, and the phone's own UI follows.

**Both installed:** switched between repeatedly in both directions. Jr's session must survive Companion being foregrounded and backgrounded.

**iCloud:** server saved on phone appears in Jr **after Jr is next launched** (not while open — see Phase 4); Jr works fully with iCloud signed out; neither app blocks on sync at launch; no background sync traffic during a sustained Jr session.

## 5. Out of scope

- The standalone reconnecting bug (§1.2) — separate investigation, needs device logs.
- Additional Jr waterfall/spectrum settings — Stuart to spec.
- Any change to batching, backpressure or in-flight-row limits in `VibeWatchModule`.
- Any change to `linkAlive` recency semantics beyond the existing `cmd:stop`.
- A shared Swift package (see Phase 1 TRAP).
