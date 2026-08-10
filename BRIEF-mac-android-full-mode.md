# BRIEF — Simple and Full mode on macOS and Android

**Status:** specified 2026-08-10 (Stuart), not started. The Mac GUI is further along than expected;
the SERVER side is the real work. See §4 — it is an architecture decision, not a port.

## The shape

Both platforms are driven by a GUI, so the mode switch is a GUI idea:

### Simple mode — **UNCHANGED, and defended**
Exactly what ships today. Nothing to change.
★★★ **It is the DEFAULT, and it stays the default.** Stuart: *"that will retain the plug radio
press start nature of VibeServer that our users love, we must preserve this at all costs."* Any
change that makes Simple mode ask a question it does not ask today is wrong, however convenient it
is for Full mode. Quickly sharing one radio on the local network is the product for most users.

### Full mode — the full VibeServer: public-facing, multiple radios, multiple users
- The **switch is the first thing at the top of the GUI**, and defaults to Simple.
- Turning it on **compacts the GUI to mirror the Linux TUI wizard**:
  1. a **list of radios with toggles** to choose how to serve them,
  2. the **admin password** (mandatory — it unlocks the browser setup page),
  3. the **optional PIN**.
- Underneath, **one button**: save, start the server, and **immediately open the setup page in the
  default browser**.
- From that point Full mode **behaves identically to Linux**.

★ The Linux TUI (`vibeserver/tui.cpp`, `runWizard`) is already exactly these three steps and no
others — everything else deliberately moved to the browser, because "asking here and editing there
is the two-editors drift". The GUI should ask the same three and no more.

## 1. What the Mac ALREADY has (better than the plan assumed)

`vibeserver/mac/VibeServerApp.swift`:
- `@AppStorage("fullMode")`, defaulting to **false** ✓
- a **Simple/Full segmented picker, first in both panes** ✓ — with a hard-won comment explaining
  why they are TWO VIEWS and not one Form full of `if`s: the single-Form attempt "rendered the
  OPPOSITE of what it said", silently. **Do not re-merge them.**
- Full pane already has the admin password, the PIN, Start, and "Open setup in your browser…" ✓

## 2. What the Mac GUI still needs
- **A radio LIST with toggles.** Today Full mode shows one "Receiver" plus a `Use` picker to choose
  *which single radio*. Full mode needs the TUI's model: every detected radio, **all ticked by
  default** (someone who plugged three in wants three receivers), space/toggle to untick.
- **One button, not two.** Today Start and "Open setup…" are separate, the latter disabled until
  running. The spec is a single save → start → open-the-browser action.
- ★ Warn on colliding RTL serials, as the TUI does — two dongles with the same serial cannot be
  told apart and settings follow the wrong radio.

## 2a. ✅ THE DIVISION OF LABOUR, confirmed 2026-08-10

Stuart: *"In full mode everything except the radio selection, password and pin is set in the
browser. Simple mode everything is done via the GUI."*

So the toggles are **serve / do not serve**, and the per-radio Mode (SingleUser vs LockedRange) is
chosen in the **browser setup page**, per radio tab, as on Linux today. Exactly three things stay
in the GUI in Full mode — the same three the TUI asks — and nothing else, because putting a
setting in two editors is the drift the TUI comment exists to warn about.

### ★★★ SWITCHING MODES MUST BE NON-DESTRUCTIVE, IN BOTH DIRECTIONS

Stuart: *"If in full mode the GUI gets hidden and any previously set settings in simple mode simply
get overridden or ignored in full mode. That way a user could switch between full/simple modes
without too much friction."*

**HIDDEN, OVERRIDDEN, IGNORED — never deleted.** This is the invariant to hold onto, and it is easy
to break by accident:

- Simple mode's settings live where they live today (`@AppStorage`/UserDefaults) and **must survive
  a trip through Full mode untouched**. Switching to Full and back must return the user to exactly
  the receiver they had, not to defaults.
- Full mode's settings live in the **config file** the browser edits. They must survive a trip
  through Simple mode untouched, for the same reason.
- ★★ So the two stores are deliberately **NOT merged and NOT synchronised**. A "helpful" copy in
  either direction is what turns a mode switch into data loss: the browser's carefully set public
  server would flatten the GUI's remembered local one, or the reverse. Two stores, one active.
- ★ The **admin password and PIN are the exception** — they are asked in BOTH modes and are the
  same credential, so they carry across rather than being kept twice.
- ★ A Simple-mode server and a Full-mode server must never run at once: switching stops one before
  starting the other. Both want the radio and the port.

## 3. Android — FULL MODE, CAPPED AT ONE RADIO  ✅ decided 2026-08-10

Stuart: *"Android gets a single radio only full mode which then gets all the IP and reverse proxy
stuff designed for a server opened to the public, and the radio still gets the option to be single
user per radio or locked profile shared users. Basically all of full mode but for 1 radio only."*

★★★ **This needs nothing invented — the model is already per-radio.** `RadioConfig` carries
`Mode::SingleUser` vs `Mode::LockedRange` (exactly "single user per radio or locked profile shared
users"), and `Sharing::Local` vs `Public`, `trustedProxies`, `allowRanges`/`blockRanges`, session
limits and the ban list are all already there. **Android Full mode is the existing model with a
`radios[]` of length one.** Same shape as Linux, so no second implementation and no divergence —
which is the whole reason to prefer it over "Android is Simple-only".

Why the cap is the right line rather than a compromise:
- **CPU was never the binding constraint** — a phone beats a Pi 500 easily. **Power is:** a phone
  cannot feed three SDRs over OTG, so multi-radio on a phone needs a powered hub regardless.
- **Lifecycle.** Android kills the app process routinely — `VibeServerRestore.kt` exists purely
  because "a zombie is worse than a clean stop" after a low-memory kill. Front door + ONE radio is
  two processes; the general case is N+1, none of which Android tracks as app processes. Capping
  at one keeps the hardest existing problem from being multiplied.
- Thermal: sustained multi-radio DSP on a phone throttles in a way the Pi does not.

★★ **The USB fd is the one genuinely Android-shaped problem, and the plumbing exists.** The radio
is opened by KOTLIN (`UsbManager.openDevice()` → `conn.fileDescriptor` → native); a child process
cannot call UsbManager itself. So the app opens the device and passes the fd down — and
`fd_passing.cpp` (SCM_RIGHTS) is already in `android/app/src/main/cpp/`, the same mechanism the
front door uses for connection hand-off.

★ **W^X:** since API 29 an app may not exec from its data dir. The binary must ship in
`nativeLibraryDir`, i.e. packaged as `libvibeserver.so` in jniLibs. Today Android's CMake builds
only `vibelocalsdr` as a SHARED LIBRARY and **no executable at all** — that is new build work.

▶ Do macOS first regardless: it proves the spawn-and-supervise path against the real front door
with none of the sandbox questions, leaving Android's remaining unknowns narrow (fd hand-off and
foreground-service lifecycle) rather than the whole model.

## 4. ★★★ THE REAL WORK IS NOT THE GUI — IT IS THAT `fullMode` REACHES NOTHING

`fullMode` is currently used in **exactly one place**: choosing which SwiftUI Form to draw. It
never reaches the server. `Server.start()` builds one `VsConfig` with a single `cfg.deviceIndex`
and starts the core **in-process** via the `vs_*` C API.

So today the Mac's "Full mode" is a different pane over the same single-radio V2 server. There is
no front door, no `radios[]`, no per-radio process, no `/r/<id>/` routing, no admin page.

**Linux Full mode is multi-PROCESS**: a front door that owns no radio and holds the public port,
one process per radio, SCM_RIGHTS hand-off, a per-radio flock so a second copy declines instead of
fighting for the device.

Two ways to close that, and this is the decision to make first:

- **(a) Spawn the real binary.** The GUI becomes a launcher/supervisor: it writes the config and
  starts the same `vibeserver` front door, which supervises its own radio processes. ★★ **The
  no-systemd work already exists and was written for exactly this** — the front door supervises
  its own radios when `/run/systemd/system` is absent, re-execs itself on a settings save, and
  holds the per-radio lock. macOS and Android have no systemd either. This is the path that makes
  "behaves identically to Linux" true by construction rather than by re-implementation.
  ▶ **Android risk to settle early:** spawning and supervising child processes from an app
  sandbox is far more constrained than on macOS. Establish whether this is viable on Android
  BEFORE committing both platforms to it — if it is not, the two platforms diverge here, which is
  precisely what the "servers first" ordering exists to prevent.
- **(b) Re-implement multi-radio in-process**, threads instead of processes. Avoids the sandbox
  question and keeps the current embedding, but it is a second implementation of the model — and a
  second implementation is what "clients written against a server that behaves differently on
  three platforms grow three code paths" is warning about, one layer down.

## 5. What travels for free
- `vibe_bands.h` (frequency allow/block + the ITU-region band plan) is shared C++.
- The **setup page is a CLIENT of the config API**, so it should port cheaply — it is already just
  a browser page talking to endpoints.
- The admin page, geoip/ASN, country flags and the connection log are all server-side and come
  with the front door under (a).

Related: [[next_bring_mac_android_and_apps_to_v3]], `BRIEF-apps-multiradio-v10-1.md`,
`BRIEF-multi-radio.md`
