# BRIEF: VibeServer for Linux — a .deb for Raspberry Pi OS, and a TUI

**Status:** not started, specced 2026-07-31. Next step is a Pi 500 on the network with SSH.
★ Prerequisite for `BRIEF-vibeserver-pi-iso.md` — the appliance image installs this package.

## Why now
The App Store "What's New" for 10.0.0 already claims *"run your own SDR as a server on a Mac or
**Raspberry Pi**"*. ★ **That claim is currently false** — written by Claude, spotted by Stuart
2026-07-31. Pulling a submission that has been queued since 07-30 to edit three words costs the
queue position; **building the thing makes the sentence true instead.** Also: *"we need this anyway
so I can get the Pi 500 fired up."*

---

## 1. ✅ THE PORT IS SMALL — the core was written portable
Checked 2026-07-31:
- **No `__APPLE__` anywhere in the shared C++ core.**
- **`__linux__` paths ALREADY EXIST** in `mdns_responder.cpp` and `local_sdr_shim.cpp`.
- The **only** `if(APPLE)` in `vibeserver/CMakeLists.txt` is IOKit/CoreFoundation/Security for
  libusb — already conditional.
- `vibedsp`, `net_shim`, the shim, the decoders, `ft8_lib` and the **vendored** `libairspyhf` are all
  portable C++17 built from our own tree.

### What actually needs doing
1. **Relax the static-only `find_library`.** It forces `.a` for librtlsdr/libusb/libopus because a
   notarised Mac app cannot ship a Homebrew dylib (the CMake comments explain at length). ★ **That
   reasoning does not apply on Debian** — link shared and declare `Depends:` so `apt` handles it.
2. **Skip `mac/` entirely.** The menu-bar app is a front-end; `vibeserver_core` + the CLI is the
   server.
3. **Package with CPack** (`CPACK_GENERATOR "DEB"`) straight from CMake — no debhelper needed.
   Ship: the binary, a **systemd unit**, `/etc/vibeserver/` config, and a **udev rule** so the dongle
   is usable without root.
4. **Build NATIVELY on the Pi.** Cross-compiling is where the days go; a Pi 5 compiles this in
   minutes.

**Build deps:** `build-essential cmake git librtlsdr-dev libusb-1.0-0-dev libopus-dev`
(+ `libncurses-dev` for §3). SDRplay headers optional — `sdrplay_source.cpp` `dlopen`s the library.

★★ **Raspberry Pi OS Lite (64-bit)**, not Ubuntu Server and not the desktop image:
- **64-bit gets NEON compiled in for the first time** — every benchmark figure we hold came from a
  32-bit `armv7l` PiAware box with no NEON, so those are a FLOOR ([[backend_limits_probed]] sibling
  note: `BRIEF-vibeserver-benchmark.md`).
- Pi OS is what the ISO appliance will be based on, and a `.deb` built against its library versions
  is the one users actually install.
- **Lite** proves the build does not quietly depend on something a desktop happens to have.
★ No monitor needed: **Raspberry Pi Imager** writes hostname, user, Wi-Fi and an SSH key into the
image, so it comes up on the network first boot. (Stuart has no HDMI adapter — this matters.)

---

## 2. ★★★ IT MUST WORK WITH NO COMMAND LINE AT ALL
Stuart, 2026-07-31, naming the target user as himself:
> *"To be honest I don't know command line that well, so it's more for someone like me installing it
> and using it."*

★★ **That is the acceptance test for this whole package.** The normal path must never require a flag:
- `apt install ./vibeserver_*.deb` → **service enabled, started, radio detected, already serving.**
- The install **prints the URL** to open.
- The service **always starts, detects the radios and serves them by default** (Stuart's words). Zero
  config is the default state, exactly as "plug it in and tap" is on Android.

### ★★★ TYPING `vibeserver` ON ITS OWN OPENS THE TUI — NOT A USAGE DUMP
The one thing a command-line-shy person will try is the program's name. ★ If it answers with a wall
of flags we have told them they are in the wrong place; if it opens the settings screen we have met
them where they are.
★★ **And it is coherent, not a hack:** the daemon is already running under systemd, so the binary is
not needed to serve — which leaves the bare invocation free to mean *"manage this"*.
★ Keep the flags (`--tcp`, `--device`, `--port`…) for scripts and development; they are just no
longer the front door.

---

## 3. ★★ THE TUI — "the macOS GUI, over SSH"
Stuart: *"the TUI is literally just the macOS GUI made available over an SSH session terminal window,
so a user can make the toggles etc in a GUI-like window rather than having to know command line."*
Reference point: **s-tui**.

★★ It is the third editor in the rule [[vibeserver_pi_two_products]] already sets — **"one config
schema, three editors"**: the macOS menu bar, the ISO's web page, and now a curses screen. **No new
schema, no new concepts.**

### ★★★ SAME TOGGLES, SAME ORDER, SAME WORDS AS THE MAC
If someone has watched a video of the menu-bar version or read the website, the TUI must be
recognisable as the SAME THING — not a parallel design with its own vocabulary. ★ This is the
AGENTS.md rule about copy that tells a user where a control lives, applied to a second front-end:
when a control moves on the Mac, it moves here too.

### ★★★ THE TRAP: THE TUI IS NOT THE SERVER
s-tui is a MONITOR — you close it and nothing dies. ★★ If the curses UI *were* the daemon, closing
the SSH session would kill the receiver, which is precisely wrong for a box meant to sit in a loft.
- The daemon runs under **systemd**, always, with **no TTY**.
- The TUI is a separate mode that **edits `/etc/vibeserver/…` and talks to the running service**.
- Changes apply live or restart the unit — but **the service owns the lifetime, never the terminal**.

### What it shows
- **First run:** pick the radio, set PIN and port, and **show the URL** to point a browser at.
- **Live status:** listeners, sample rate, radio, and **CPU** — `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)`
  gives this on Linux for free (already used in `tools/pi-bench/bench.cpp:46`), and a headless server
  is exactly where you want to see it.
- The CLI already exposes the whole settings surface, so there is nothing to invent:
  `--device --tcp --freq --rate --gain --mode --fft --fps --port --pin --max-bw --max-fps
  --lock-rate --no-web`.

★ `libncurses` is already on every Pi, so it costs one build dependency and nothing for the user to
install.

---

## 4. Estimate and the honest caveat
**About half a day**, most of it iterating rather than writing — the small things GCC rejects and
Clang forgives.

★★★ **It cannot be verified from the Mac.** Writing Linux code blind is guesswork; the practical
route is the one the benchmark used — Pi on the network, SSH in, build and fix directly on it.
