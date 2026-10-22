# OpenRSP in every VibeServer — bundle plan

2026-10-21. Stuart: the OpenRSP source is out, so bundle OpenRSP in every VibeServer build and an SDRplay
RSP works with no separate driver install. This document holds the research, the decisions, what is built on
branch `worktree-agent-a92dfb043140e163e`, what is still to do on each platform, and the device tests.

Status at a glance:

| Build | Status |
|---|---|
| Linux .deb amd64 / arm64 / armhf | **Built.** vendor/openrsp is built and packaged. A full arm64 package was built and its gates passed in a local bookworm container. armhf and amd64 OpenRSP were cross-compiled and checked. Not yet run with a receiver. |
| Linux .deb i386 | **Blocked upstream.** OpenRSP 3.15.2-20 does not build for i386 Linux (see §6). RSPs stay compiled out, as before. |
| Linux ARMv6 binary (in the armhf package) | **Excluded.** It never had SDRplay; postinst removes the ARMv7 RSP service on a CPU without NEON. |
| Mac VibeServer.app | **Built (§5.2), branch `v12-rsp-platforms`.** Library, service, firmware and licence bundled; the app supervises the service when no SDRplay API is installed. Bundle gate passed. Not yet run with a receiver. |
| Android app server + VibeServer Lite | **Built (§5.3), branch `v12-rsp-platforms`.** libsdrplay_api.so (embedded service) for arm64-v8a + armeabi-v7a in the app, armeabi-v7a in Lite, dlopen'd on API ≥ 26 only; firmware as checked assets; USB attach/re-attach/detach in VibeRsp.kt. Debug APKs built and inspected. Not yet run with a receiver. |
| iOS / iPadOS / App Store builds | **Out** (decided 10-19; licence reason in §2). |

## 1. What OpenRSP is (from openrsp.dev and the source)

- An independent open-source re-implementation of the whole SDRplay RSP driver stack: the API library
  (`libsdrplay_api.so.3`, ABI-identical to SDRplay API 3.15 with 101 exports), the device service
  (`sdrplay_apiService`), the USB bridge firmware (8051, written from scratch), and its own API headers. It is
  not affiliated with SDRplay Ltd. Its packages contain no SDRplay code, binaries or firmware. Its stated legal
  basis is UK CDPA s50BA (reverse engineering for interoperability).
- **The pin.** Release `v3.15.2-openrsp.25` = OpenRSP **3.15.2-20**, built 2026-10-21. The source tarball is
  `https://openrsp.dev/downloads/openrsp-3.15.2-20-src.tar.gz`, SHA-256
  `288828d04beb4ecb0812ee8a3635b31b539c9b1b4dbb4344e2f127fd6ec20057`. This matches the site's SHA256SUMS and was
  verified on download. **No public git repository is linked from the site**: the release tarball is the
  source of record. The tarball carries no `tests/` directory, no `tools/fw8051/fwcheck.py` and no prebuilt
  `firmware/own/`. Our build does not need any of them.
- **The build.** CMake ≥ 3.16 and C++17. It needs libusb-1.0 on every OS, libudev on Linux (upstream), sdcc 4.2
  for the firmware, and pthread/rt. Android builds its own libusb 1.0.27 from a pinned download. Optional AVX2 /
  AVX-512 / NEON kernels are chosen at run time, so the armhf baseline is VFP with NEON detected at run time.
- **Models.** RSP1B (the reference unit), RSP1A and RSPduo are tested on real units. RSP1, RSP2, RSPdx and
  RSPdx-R2 are experimental (tested on an emulator only). The Lenovo's RSP1A is in the tested set.
- **Platforms (upstream).** Linux x86-64 / arm64 / armhf, Android 8+ (arm64, armv7, x86-64), macOS (universal,
  "working"; its installer is "soon"), Windows x64/x86, and iPadOS (DriverKit).
- **Deliberate differences from SDRplay's stack.** It never opens a network socket (SDRplay's service reports
  serials over HTTP). Invalid call sequences return an error instead of crashing. A stopped service makes clients
  report "device removed" instead of spinning at 100 % CPU. The service stops promptly instead of hanging 90 s.
  The AGC no longer locks up after a rate change. The ppm tuning is fixed at synthesiser step edges. On Android,
  bulk transfers are used everywhere.
- **The firmware is not stored on the receiver.** The bridge boots its ROM loader on every power-up, and the
  service uploads OpenRSP's image each time. Unplugging, or going back to SDRplay's driver, restores the
  original behaviour.

## 2. Licence verdict

**Bundling is permitted, on the GPL's ordinary terms. No permission is needed for the Linux .deb, the notarised
Mac app or the GitHub APKs.**

- OpenRSP is **GPL-3.0-or-later** (the SPDX header in every file; README: "version 3 or later"), © 2026 the
  OpenRSP authors. VibeSDR is GPLv3 (LICENSE). The licences are the same, so linking, dlopen and shipping the
  service and firmware together raise no compatibility question. Even if a court treated VibeServer and OpenRSP
  as one work, that work would be GPLv3.
- **Our obligations when we convey the binaries (§4–§6):**
  1. Ship the licence. Done: `/usr/share/doc/vibeserver/COPYING.openrsp`.
  2. Provide the Corresponding Source. We do this through §6(d), "equivalent access from a designated place":
     `vendor/openrsp` in the public VibeSDR repository at the release tag. Our build rules
     (`vibeserver/CMakeLists.txt`, `linux/openrsp-glibc-floor.cmake`, the Dockerfiles) are public in the same
     place, and they are part of "the scripts used to control compilation". `README.openrsp` in the package
     says all this. ★ **Every release that ships OpenRSP must be tagged and pushed**, so that the designated
     place exists for as long as the binaries are offered. That is already our practice. A release built from
     unpushed commits would breach §6.
  3. Mark our modifications (§5a). Done: three opt-in build switches, each marked "VibeSDR change
     (2026-10-21)" in its file and listed in `vendor/openrsp/VIBESDR-CHANGES`. The unmodified tarball was
     committed first (e9ce7e26), so the diff is exact.
- **The firmware** is OpenRSP's own GPL source (`firmware/src`), and we build it ourselves with sdcc. It is not
  a vendor blob, so there is nothing extra to redistribute or license. Bookworm's sdcc 4.2.0 reproduces OpenRSP's
  released images **byte for byte** (checked against `openrsp_3.15.2-20_arm64.deb`), so the images we ship are
  provably built from the source we publish.
- ★★ **The app stores are the one catch.** `APPSTORE-EXCEPTION.md` is Stuart's own §7 additional permission
  for the Apple App Store, TestFlight and **Google Play**. It covers only code Stuart holds the copyright to.
  OpenRSP's authors have granted no such permission. Therefore:
  - The iOS and Mac App Store builds must never contain OpenRSP. That is already the plan: no iOS.
  - **The Google Play AAB of the Android app: DECIDED, it may carry OpenRSP** (Stuart, 2026-10-21). Play is
    treated as compatible with GPLv3, and the Play app already ships librtlsdr (GPL-2.0-or-later, not Stuart's)
    on exactly those terms. M9PSY has consented to the integration (Discord, 2026-10-21). Only the Apple
    stores need a §7 exception we cannot give for someone else's code, so the line stays at Apple.
- libusb (LGPL-2.1+) is already linked statically on Linux and the Mac, so nothing changes there.
- **Trade marks.** OpenRSP uses SDRplay's file names only where compatibility forces it. We go further and use
  none of their paths: everything lives under `/usr/lib/vibeserver/openrsp`, and the unit is called
  `vibeserver-rsp`. "SDRplay RSP" appears only to say which receiver this is.

### Quiet, as Stuart asked (2026-10-21)
**Do not advertise "driver-free SDRplay" anywhere.** No website copy, no store copy, no tour, no feature-list
line. We do not want to upset SDRplay. What goes in:
- **Credits** (done on this branch). OpenRSP has its OWN plain entry in `AboutOverlay.tsx` `CREDITS` ("OpenRSP
  (GPL-3.0-or-later)") — NOT under M9PSY's name: he asked (2026-10-21) not to be credited for it, "just call it
  OpenRSP". Never attach a person's name to OpenRSP in user-facing copy. The shipped-code table in README
  gains an OpenRSP row, and its SDRplay API row now says "never bundled".
- **One plain line in the release notes** (AboutOverlay, the next version's entry, which does not exist yet; the
  version number was not to be changed here). Suggested wording:
  *"VibeServer for Linux now includes its own driver for SDRplay RSP receivers, so nothing else needs
  installing."*
- At release, **correct the instructions that tell owners to install SDRplay's API**. This is a correction, not
  an announcement. These are left alone on this branch because they describe the package that is live today:
  `scripts/apt-index.html` ("If you have an SDRplay RSP"), `website/join.html:146`,
  `vibeserver/linux/INSTALL.md` (around lines 203 and 246), and `scripts/install.sh:557-566`. Change the last one
  to "RTL-SDR, Airspy, HackRF and SDRplay RSP need nothing extra", and on i386 say plainly that RSPs are not
  supported there. Also `vibeserver/mac/RELEASE-alpha.md` once the Mac carries it.

## 3. Service or in-process?

**On Linux: a service — ours, under our own name. On Android: in-process (upstream's embedded mode).**

- The library is only ever a client. It talks to a service over shared memory (`/dev/shm/Glbl\sdrSrv…`) with
  named semaphores and mutexes. On Android, upstream compiles the service core INTO `libsdrplay_api.so`
  (`EmbeddedService.cpp`), and the app feeds it UsbManager fds. That proves the core can run in-process.
- **It cannot run in-process in our Linux server.** Every radio is its own process (`vibeserver`,
  `vibeserver@<serial>`). If each process embedded a service, each would enumerate every RSP on the bus. They
  would race to upload firmware and to claim interfaces, and all of them would create the same global object
  names. A per-process name prefix (`ipc::setObjectNamePrefix` exists) would solve only the names, not the USB
  race. One service owns the USB devices and every VibeServer process is its client — the same shape as today,
  minus SDRplay's binary.
- **What "the official service hangs" taught us still holds:** the hangs were SDRplay's service, not the
  architecture. On the Lenovo's overnight A/B, OpenRSP's service had 0 hangs in 150 min of rapid tuning, and
  SDRplay's had 3 in 17 min.
- **Our unit, `vibeserver-rsp.service`:**
  - It is **unprivileged**: `vibeserver` in `plugdev`, with `CAP_SYS_NICE` for its SCHED_FIFO/RR stream
    threads. Upstream runs it as root, but it needs only the RSP's USB nodes (new udev lines in
    `99-vibeserver-sdr.rules`) and `/dev/shm`, whose objects it creates mode 0666. This was checked in a
    container: as `vibeserver` it serves Open/GetDevices to the same user and to root, and it unlinks
    everything on stop.
  - It is **started by the receiver, not by the boot.** A udev `SYSTEMD_WANTS` on any `1df7` device starts it
    at coldplug or at hotplug, so a Pi 2 serving an RTL-SDR never runs an RSP service. postinst starts it if an
    RSP is already plugged in, and try-restarts it on upgrade. It has no `[Install]` section and nothing enables
    it.
  - **`ConditionPathExists=!/opt/sdrplay_api/sdrplay_apiService`**: see §4.
  - Not `PartOf=vibeserver`, because a `stop` + `start` of the server would leave it dead. There is no
    `Conflicts=sdrplay.service` either: a start (an RSP plugged in) would stop an installed SDRplay service
    before our condition had been checked.
  - Hardening matches the radio units. It has `RestrictAddressFamilies=AF_UNIX AF_NETLINK` and
    `IPAddressDeny=any`, which is upstream's no-network promise enforced by systemd.

## 4. Coexistence with an installed SDRplay API (official or sdrplay-api-open)

Both SDRplay's installer and OpenRSP's own `sdrplay-api-open` package put their service at
**`/opt/sdrplay_api/sdrplay_apiService`** and their library at `/usr/local/lib/libsdrplay_api.so.3.15`, and run
`sdrplay.service`.

**Rule: one RSP service per machine, and an installed system stack wins.** One fact, asked in two places that
must stay in step:
- `vibeserver-rsp.service`: `ConditionPathExists=!/opt/sdrplay_api/sdrplay_apiService`. If it is installed, ours
  never starts.
- `sdrplay_source.cpp` `sdrplayCandidates()`: if it is installed, the **system library comes first** and the
  bundled one is a fallback (it speaks the same protocol to either service). If it is not installed, the
  **bundled library comes first**. The library in use is logged once per process:
  `sdrplay: API library … (bundled OpenRSP|system)`.

Why the system stack wins:
1. It already owns the receivers for every other program on the machine (OpenWebRX, SDR++).
2. Two services would fight over firmware upload and claims.
3. The owner installed it on purpose. The Lenovo and the Pi 500 run sdrplay-api-open today.

Taking it over silently would change other programs' driver under them.

| Machine | Service that runs | Library VibeServer loads |
|---|---|---|
| Nothing installed (the target case) | vibeserver-rsp (ours) | /usr/lib/vibeserver/openrsp/libsdrplay_api.so.3 |
| SDRplay official API installed | sdrplay.service (theirs) | /usr/local/lib/libsdrplay_api.so.3 (theirs) |
| sdrplay-api-open installed (Lenovo, Pi 500) | sdrplay.service (OpenRSP's) | /usr/local/lib/… (OpenRSP's) |
| System stack later removed | ours starts on the next RSP plug-in or reboot | bundled (decided per process at first use; a running server keeps its choice until restarted) |

- **No file conflicts.** We install nothing under `/opt/sdrplay_api`, `/usr/local`, `/usr/share/sdrplay_api` or
  `/etc`, so dpkg never sees two owners of one path.
- **The udev mode.** SDRplay's `66-sdrplay.rules` makes the RSPs 0666. Our `99-` rule narrows them to 0660
  `plugdev`, which their root-run service does not notice.
- **Edge case: SDRplay's API installed ON TOP while ours is running.** Their installer starts sdrplay.service
  while ours still holds the receiver and the object names. **Reboot** (or `systemctl stop vibeserver-rsp`)
  after installing it. Say so in INSTALL.md at release.
- **Edge case: stale root-owned objects.** If a crashed root service left objects in `/dev/shm`, our
  unprivileged service cannot unlink them (sticky `/dev/shm`), and it fails to start until a reboot clears
  them. It is rare; `systemctl status vibeserver-rsp` would show it. If it turns up, add an
  `ExecStartPre=+` cleanup limited to the eight `Glbl\sdrSrv*` names when no other service is active.

## 5. Per platform

### 5.1 Linux .deb (amd64, arm64, armhf) — BUILT on this branch
- `vendor/openrsp`: the unmodified tarball (e9ce7e26), then three opt-in switches (a19c6b44):
  - `SDRPLAY_LINUX_UDEV=OFF` — libudev feeds only a hotplug monitor that nothing in the service calls.
  - `SDRPLAY_LIBUSB_LIBRARY` / `_INCLUDE_DIR` — our static no-udev libusb 1.0.30. The cross toolchain files reset
    `PKG_CONFIG_LIBDIR`, and a service without libusb silently compiles the USB *stub*.
  - `SDRPLAY_STATIC_CXX_RUNTIME` — libstdc++/libgcc linked in.
- `vibeserver/CMakeLists.txt` builds it as an **ExternalProject with upstream's own CMake**, using the same
  toolchain file for the cross arches, `SDRPLAY_FIRMWARE_DIR=/usr/lib/vibeserver/openrsp/firmware`, and
  `CMAKE_PROJECT_INCLUDE=linux/openrsp-glibc-floor.cmake`. The last one is needed because static libstdc++ calls
  `arc4random@GLIBC_2.36`, exactly as it did for the server. It installs:
  - `/usr/lib/vibeserver/openrsp/{libsdrplay_api.so.3.15, libsdrplay_api.so.3 → , sdrplay_apiService, firmware/own/{fw_common,fw_rsp1,fw_rspduo}.bin + manifest.sha256}`
  - `/usr/lib/systemd/system/vibeserver-rsp.service`
  - `/usr/share/doc/vibeserver/{COPYING.openrsp, openrsp/README.openrsp, openrsp/VIBESDR-CHANGES}`
- **Headers.** Every build now compiles against `vendor/openrsp/include`, not SDRplay's staged headers, so the
  "RSP compiled out for want of a header" failure cannot recur. `build-debs-remote.sh` and
  `publish-apt-docker.sh` no longer stage headers. The three Dockerfiles drop `COPY sdrplay-inc/` and add
  `sdcc`, so **the Lenovo images rebuild on the next run** (needs network for apt).
- **Gates.** `VIBESERVER_STRICT_RADIOS` refuses a build without sdcc. `build-deb.sh` refuses a package if:
  - the firmware fails its manifest,
  - the service has no libusb in it (the stub),
  - the library lacks any of the 13 entry points we resolve,
  - COPYING.openrsp is missing.

  The existing gates also cover these files: every ELF is the package's architecture, GLIBC ≤ 2.35, and Depends
  are libc6 only.
- **Verified locally** (colima, bookworm arm64, 2 CPUs, nice):
  - The OpenRSP-only builds for arm64 (native) and armhf / amd64 (cross) link. NEEDED is libc/libpthread/librt/
    libm only, GLIBC ≤ 2.35, there are 101 exports, the service has libusb in it, and the firmware is
    byte-identical to upstream's release.
  - In a service smoke test as an unprivileged user: Open → Success, ApiVersion 3.15, GetDevices → 0 devices
    (no USB in the container), from two users, and objects unlinked on SIGTERM.
  - A **full arm64 package** (`vibeserver_11.0.9-0~openrsp1_arm64.deb`, test revision, never published) was
    built through a native copy of `build-deb.sh`. Every gate passed: `Depends: adduser, curl, gzip, libc6
    (>= 2.35)`; 4 ELF files, all aarch64, GLIBC ≤ 2.35; the firmware matches its manifest; 23 API exports
    including all 13 we resolve.
  - **That package was installed** into a clean `debian:bookworm-slim` container. As user `vibeserver`,
    `vibeserver --list-radios` logs `API library /usr/lib/vibeserver/openrsp/libsdrplay_api.so.3.15 (bundled
    OpenRSP)`. With the bundled service running, the API opens cleanly (no receiver in a container, so "No
    radios found"); without it, the library prints `shm_open: No such file or directory` and reports nothing.
  - Not run: the amd64 and armhf full packages (they build on the Lenovo), and anything with a receiver.
- `vibeserver-maintenance sdrplay-restart` restarts whichever of sdrplay / sdrplay_apiService /
  vibeserver-rsp is active. Diagnostics already collects `vibeserver*` units, which includes ours.

### 5.2 Mac VibeServer.app — BUILT (2026-10-22, branch `v12-rsp-platforms`)
OpenRSP's Darwin layer is complete (POSIX shm with the 31-character name limit handled, Darwin semaphores,
IOKit hotplug), and upstream reports streaming at 2/8/10 MS/s on Apple silicon.

What was built (steps 1–4 of the earlier plan):
1. **Build** — `vibeserver/mac/build-app.sh`, section "OpenRSP". arm64 at `MACOS_MINIMUM` (14.0) against
   `deps/prefix/lib/libusb-1.0.a` through our explicit-libusb switch (VIBESDR-CHANGES 2, frameworks added in
   5818cac7). It builds from a **staged copy** of vendor/openrsp with the pinned firmware dropped in as
   `firmware/own`, because OpenRSP's configure wants sdcc or prebuilt images and this Mac has no sdcc 4.2.0.
   - ★★ **Homebrew's sdcc 4.6.0 does NOT reproduce the released images** (measured: fw_common 0171e881… vs
     0508cc60…). A different compiler's firmware is firmware nobody has run on a receiver, so the Mac and Android
     ship the **pinned set in `vendor/openrsp-firmware/own`** (identical to openrsp_3.15.2-20, reproducible from
     vendor/openrsp/firmware/src with bookworm's sdcc 4.2.0 — see its README). Linux still builds them from source.
   - Gates in the script: the firmware matches its manifest (twice: source and bundle), the library exports all
     13 entry points we resolve, and the service has libusb in it (not the USB stub).
   - `VIBE_BUILD_JOBS` caps every compile in the script (unset = as before).
2. **Bundle** — `Contents/MacOS/vibeserver-rspd` (the service, our own name), `Contents/Frameworks/
   libsdrplay_api.so.3.15` + `.so.3` symlink, `Contents/Resources/openrsp/firmware/own/*`,
   `Contents/Resources/COPYING.openrsp`, `Resources/openrsp/{README.openrsp,VIBESDR-CHANGES}`
   (vibeserver/mac/README.openrsp is the Mac wording). ★ `notarise-and-release.sh` now also signs `*.so.*` —
   its `-name "*.so"` would have left libsdrplay_api.so.3.15 unsigned and failed notarisation.
3. **Supervise** — `vibeserver/mac/RspDriver.swift`. Started from `applicationDidFinishLaunching` BEFORE the
   auto-start's device count (with a 0.8 s settle and a refresh), only when `vs_sdrplay_run_bundled_service()`
   says so — i.e. the bundle has the service AND no system stack is installed (`/Library/LaunchDaemons/
   com.sdrplay.service.plist` or `/Library/SDRplayAPI/*/bin/sdrplay_apiService`). Env
   `SDRPLAY_OWN_FIRMWARE_DIR=<Resources>/openrsp/firmware/own`. Restarted 1 s after an unexpected exit; five exits
   in a minute = give up and log. A stale `vibeserver-rspd` of ours (crashed/force-quit app) is reaped first.
   SIGTERM on quit, after the server has stopped.
4. **Library choice** — `sdrplay_source.cpp` `__APPLE__` branch: candidates via
   `sdrplaylib::candidateOrder(macSystemStack(), bundled, system)` — system first when installed, bundled the
   fallback; bundled first otherwise. The bundle is found from the executable's real path (Simple mode = the
   app, Full mode = vibeserver-engine; both in Contents/MacOS). The root-owned check is replaced **for our own
   library only** by `insideBundleFrameworks` (its real path directly inside our Frameworks/). The log line says
   `(bundled OpenRSP)` or `(system)`.
   - `disable-library-validation` **stays**: SDRplay's own signed library is still a fallback.

The decision logic is one header, `android/app/src/main/cpp/vibe_sdrplay_lib.h`, and the Swift side calls into
it (`vs_sdrplay_system_stack`, `vs_sdrplay_run_bundled_service`) rather than copying it.

**Measured / verified on this Mac (it has SDRplay 3.15.1 installed and its service running):**
- `build-app.sh --no-copy` in the worktree: **gate PASSED**, 5 Mach-O all arm64 minos 14.0 — `vibeserver-rspd`
  (894 KB; IOKit, CoreFoundation, Security, libc++, libSystem) and `libsdrplay_api.so.3.15` (155 KB; libc++,
  libSystem; install name `@rpath/libsdrplay_api.so.3`; 101 exports).
- A harness `dlopen`ed the bundled library and resolved all 12 entry points (`GetErrorString(0)` = Success).
- **test-sdrplay-lib** (ctest + run-tests.sh), on a scratch root: both stacks' detection both ways (empty disk,
  versioned service, daemon plist only, library only), the supervisor rule, the library order, bundle-from-
  executable and the in-bundle check (including `..`, sibling directories, subdirectories).
- ✗ **Not run:** the bundled SERVICE. SDRplay's service is running on this Mac, the two would share the same
  shared-memory names, and I would not risk unlinking theirs. ✗ No RSP was attached. See "Open for Stuart".

### 5.3 Android app server + VibeServer Lite — BUILT (2026-10-22, branch `v12-rsp-platforms`)
Upstream fits our model exactly: `libsdrplay_api.so` with the **service embedded** and
`sdrplay_android_usb_attach(fd, deviceName)` for **UsbManager fds**. It **re-enumerates after the firmware
upload** — inside the first `GetDevices`, which waits up to 15 s for the new device (bcdDevice ≠ 0x0200) — so
the app must attach the NEW device during that wait. It forces bulk transfers. Requires API 26+.

What was built:
1. **Native build** — `android/app/openrsp/build-openrsp-android.sh`, run by `android/app/openrsp.gradle`, which
   BOTH apps apply (`ext.openRspAbis`: main app arm64-v8a + armeabi-v7a, Lite armeabi-v7a). Not via
   externalNativeBuild: that runs at the app's minSdk (24 / 21) and only packages what it links. API 26,
   `c++_static`, `ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON`, Release, against **`vendor/libusb-1.0.27`** (unmodified,
   every file checked against its SHA256SUMS first — CMake never downloads). From a staged copy with the pinned
   firmware, like the Mac. **The script refuses**: an arm64 build not at 0x4000/0x10000, any of the 17 entry points
   missing (13 API + 4 `sdrplay_android_*`), any exported `libusb_*`, any NEEDED outside
   libandroid/liblog/libm/libdl/libc. Inputs are declared, so the task is up to date when nothing moved.
   CPU: nice 19, `-j 2` (`VIBE_BUILD_JOBS`).
2. **Never linked, dlopen'd, only on API ≥ 26** — `android/app/src/main/cpp/CMakeLists.txt` adds
   `vendor/openrsp/include` + `VIBE_HAVE_SDRPLAY` only. Measured: `libvibelocalsdr.so` NEEDED has no
   libsdrplay_api.
3. **sdrplay_source.cpp Android branch** — `dlopen("libsdrplay_api.so")` (the app's lib dir, by name) only when
   `android_get_device_api_level() >= 26`; resolves the embedding calls; `apiOpen` starts the embedded service with
   the app's checked firmware dir before `Open`, and refuses with a reason if the app never prepared it.
   `SdrplaySource::androidSetFirmwareDir / androidSupported / androidAttach / androidDetach`, bound in
   `vibe_localsdr_jni.cpp` (`VibeLocalSDR.rsp*`). `LocalSdrShim::start` routes VID 0x1df7 to `startSdrplay(0, …)`:
   the fd is already the driver's.
4. **Kotlin** — `VibeRsp.kt` (shared with Lite: added to its SHARED list):
   - firmware copied from `assets/openrsp/firmware/own` to `filesDir/openrsp/firmware/own`, **every image checked
     against manifest.sha256** (MessageDigest) before the driver is told; a mismatch = no RSP.
   - `attach(conn, dev)` on both start paths (local listening, VibeServer start, and the crash/attach restore in
     VibeServerRestore). While a session is live, a receiver on its own HandlerThread handles ATTACHED (open +
     attach; if Android has not granted the re-enumerated device, `requestPermission` straight away so the driver's
     15 s wait is not missed), DETACHED (detach + close) and our permission result. `releaseAll` after the engine
     stops. `VibeServerRestore.onRadioAttached` (the activity's attach, which carries the grant) also calls it.
   - `isRsp` → `"sdrplay"` in the module's when-blocks (kind, radioKind, driver, model name from the PID) and in
     `isServableRadio`. **API < 26 is not offered at all**: `VibeRsp.supported` is false, and the RSP USB ids live
     only in **`res/xml-v26/device_filter.xml`** (Lite syncs both). `scripts/check-device-filter.mjs` (run-tests.sh)
     holds the two filters and `VibeRsp.RSP_PIDS` together.
   - The firmware re-enumeration looks like a blip to the server's own watch (DETACHED → gone, ATTACHED → back),
     well inside the blip window, so it does not stop a server.
5. **Gain model / hwinfo** — no Android-specific case: the app reads the RSP's caps from hwinfo exactly as from a
   Linux RSP server. ServerModeScreen gained the RSP rate list, ppm, and hides bias-T on an RSP1.
6. **Potato phones — measured on the debug APKs:**
   | | main app (arm64 + v7a) | Lite (v7a) |
   |---|---|---|
   | libsdrplay_api.so (stripped, as packaged) | 1,203,128 B arm64 (LOAD 0x4000) + 797,848 B v7a (LOAD 0x1000) | 797,848 B |
   | …compressed in the APK | 516,841 + 436,191 B | 436,191 B |
   | firmware assets | 10.9 KB (7.7 KB compressed) | same |
   | extra code in libvibelocalsdr (sdrplay_source real body vs stub, v7a -O3) | ~42 KB text | ~42 KB |
   - **APK delta ≈ 0.96 MB (main), ≈ 0.45 MB (Lite).** With legacy packaging the phone's own ABI is extracted on
     install: +1.2 MB (arm64) / +0.8 MB (v7a) on disk.
   - **Idle RAM: zero by construction** on a phone with no RSP — the library is not loaded, no thread, no receiver,
     no service until an RSP is attached (the service's ring buffers only while one streams). ✗ Not measured on a
     device (no permitted device was available: the Tab A9 was not paired, the XCover and Sony are off-limits).
7. **Licence before Play:** decided 10-21 (§2) — the Play AAB may carry it.

✗ **Unverified on hardware:** everything that needs an RSP on a phone — the firmware upload, the re-enumeration
re-attach, and **whether Android asks a SECOND time** for the re-enumerated device when the owner did not tick
"use by default" (if it does and the owner is slow, the first Start fails and a second Start works, because the
device is then already in firmware mode).

### 5.4 iOS / iPadOS — OUT
As decided 10-19: DriverKit is iPad-only, it needs per-maker vendor-ID entitlements, and a third-party GPL
component cannot go in an App Store build without its authors' exception. Revisit only if DriverKit reaches the
iPhone.

## 6. Risks and open questions

1. **i386 does not build (upstream).** `src/client/ClientAbi.h:232`
   `static_assert(sizeof(ServiceState) == 3144, "ServiceState size (ILP32)")` holds for armhf and Win32, where
   64-bit members are 8-byte aligned. **i386 SysV aligns them to 4 and gets 3132.** **Ask M9PSY** whether i386
   Linux is wanted. If it is, the asserts need an i386 case, or the shared structs need explicit `alignas(8)` on
   their 64-bit members. Since every process on the box would be ours, internal consistency is enough. Until then
   i386 keeps RSPs compiled out (as it always has). Kiko's and Danny's Atoms do not have RSPs.
2. **armhf has not streamed from real hardware anywhere**, upstream says ("Test ARM Linux … these builds pass
   their unit tests but have not yet streamed from real hardware"). The Pi 2 has no RSP. If Stuart can put the
   RSP1A on the Pi 2 for an hour, that is the only armhf proof available.
3. **The unprivileged service is our change in posture** (upstream is root). It works in a container. On real
   USB, the things to watch are the firmware upload after re-enumeration (the udev rule must apply to the
   post-firmware device too — same VID/PID, so yes), the claim, and SCHED_FIFO with `CAP_SYS_NICE`.
4. **Two services on one machine** (§4 edge case: SDRplay installed on top). Documented, not prevented.
5. **The upstream version number** stays "3.15.2-20". When OpenRSP releases again: drop in the new tarball as one
   commit, re-apply the three switches (or find them upstreamed — offer them to M9PSY, he accepts fixes),
   rebuild, and diff the firmware manifest against their deb.
6. **No public VCS upstream.** If openrsp.dev went away, our vendored copy is still the Corresponding Source for
   what we shipped, so nothing breaks.
7. **SDRplay's reaction.** Stuart's note (10-19): consider telling SDRplay before this reaches the app. Keep it
   quiet in all copy (§2).

## 7. Device tests for Stuart

Run them on the Lenovo first (amd64, RSP1A), then the Pi 500 (arm64). Both have **sdrplay-api-open 3.15.2-9
installed today**, so a straight upgrade exercises the **system-stack-wins** path. The bundled path needs the
system stack moved aside (reversibly, test B). Check the listener count first; these take the dial.

**A. Upgrade with the system stack in place (should change nothing).**
1. `sudo apt install ./vibeserver_<ver>+openrsp_amd64.deb`
2. `journalctl -u vibeserver -b | grep 'sdrplay: API library'` → `/usr/local/lib/libsdrplay_api.so.3.15 (system)`.
3. `systemctl status vibeserver-rsp` → "condition failed" / inactive. `systemctl status sdrplay` → active, as
   before.
4. The RSP opens, the AGC kick runs, and there is audio. `ls -l /dev/bus/usb/*/*` for the RSP → `crw-rw----
   root plugdev`.

**B. The bundled driver (the real test).** Move the system stack aside reversibly, without uninstalling, so
apt's backup and restore are not involved:
1. `sudo systemctl disable --now sdrplay && sudo mv /opt/sdrplay_api /opt/sdrplay_api.off`
2. Replug the RSP, or `sudo systemctl start vibeserver-rsp`; then `sudo systemctl restart vibeserver`. The
   server decides its library at first use.
3. `systemctl status vibeserver-rsp` → active, User=vibeserver. Its journal shows the firmware upload with no
   errors. `journalctl -u vibeserver | grep 'API library'` →
   `/usr/lib/vibeserver/openrsp/libsdrplay_api.so.3.15 (bundled OpenRSP)`.
4. **The regression test: DAB, then step rapidly down the channels to 7D (DAB Horizon).** It must produce no
   command timeouts, no `AlreadyInitialised`, no sdrplay-restart, and at most one re-initialised stall at the DAB
   rate change. Then FM → MW (the LNA clamp), and a 10-minute idle (the keepalive release/re-acquire).
5. Robustness checks:
   - `sudo systemctl restart vibeserver-rsp` while listening: VibeServer must recover by itself.
   - Unplug and replug the RSP: the service is still running, the device comes back, and the radio recovers.
   - The admin page's RSP-restart path.
6. **Reboot** with the RSP plugged in. The service must come up from udev before or with the server, and the
   radio must serve without help.
7. If you can, run the overnight stress script from 10-20 (FM sweeps, DAB stepping, DAB↔FM). Baseline:
   OpenRSP-system 150 min → 0 hangs, 1 stall, 3 gaps. Compare service CPU (`systemctl status` CPU time) with the
   1.1 %-of-a-core idle and 6 % streaming figures.
8. **Restore:** `sudo mv /opt/sdrplay_api.off /opt/sdrplay_api && sudo systemctl enable --now sdrplay && sudo
   systemctl restart vibeserver`. vibeserver-rsp stays running until a reboot; stop it with `sudo systemctl stop
   vibeserver-rsp` before re-enabling sdrplay.

**C. No-RSP box (Pi 2, armhf):**
- After the upgrade, `systemctl status vibeserver-rsp` → inactive (never started), with zero CPU cost.
- On a no-NEON box (if any), `/usr/lib/vibeserver/openrsp/sdrplay_apiService` must be absent after install.

**D. Fresh box (optional, the true target).** A clean Ubuntu 22.04 / bookworm VM or spare machine with no SDRplay
software, apt install, RSP plugged in → it serves with nothing else installed. Also confirm
`dpkg -s vibeserver | grep Depends` is libc6 + tools only.

## 8. RSP setup GUI — audit and what changed (2026-10-22)
Audited every RSP control across the app panel (local + remote), ServerModeScreen, the web client and the setup page.
One table now says what each model has: `android/app/src/main/cpp/vibe_rsp_caps.h`, read by `SdrplaySource::has*()`
and therefore by hwinfo `radio.*`, which every panel gates on. Per model:

| Model | FM/MW notch | DAB notch | Bias-T | HDR | AM notch | 24 MHz ref | Antenna choice |
|---|---|---|---|---|---|---|---|
| RSP1 | – | – | – | – | – | – | – |
| RSP1A / RSP1B | ✓ | ✓ | ✓ | – | – | – | – |
| RSP2 | ✓ | – | ✓ | – | – | ✓ | A / B / Hi-Z |
| RSPduo | ✓ | ✓ | ✓ | – | ✓ (tuner 1 Hi-Z) | ✓ | Tuner 1 50Ω / Tuner 1 Hi-Z / Tuner 2 50Ω (single-tuner mode; a change re-opens) |
| RSPdx / dx-R2 | ✓ | ✓ | ✓ | ✓ (below 2 MHz) | – | – | A / B / C |

LNA state, IF gain reduction / IF AGC, AGC target, sample rate (bandwidth follows it) were already complete and
per model (LNA count from the radio). Fixed:
- **RSP bias-T did nothing from the setup page or the config** (setBiasTee had no RSP branch); hwinfo `radio.biasTOn`
  read the dongle's flag; `rsp_control biast` was never persisted. The app panel had no RSP bias-T switch at all — now
  it has one, only where `radio.biasT`.
- **ppm** for an RSP: the server always applied it; app panel, web client, setup page and ServerModeScreen now show it.
- **Setup page**: `/vibeserver/hardware` now carries the radio's caps; HDR / AM notch / ext ref / the notches are drawn
  only on the models with them (live caps, else the model in the radio's name, else all as before); the AM notch was
  labelled "RSPdx family" though only the Duo has one; **the antenna card was never drawn** (nothing filled
  `r.antennas`) — now it is, with the port list; the sdrplay rate list offered 10 MS/s, which the server does not.
- Notch toggles were applied and saved BEFORE the "owner reserved / automatic" refusal — now refused first.
- `rspHdr` / `rspAmNotch` / `rspExtRef` were never folded into the radio's config entry (lost on restart).
- Duo: `antenna()` defaulted to "A" (not a Duo port); a short name that names exactly one port resolves, so a per-band
  rule `hf:Hi-Z` works on a Duo too (`sdrplaylib::resolvePort`).
- Not added (decided): AGC loop dynamics (attack/decay) stay wire-only — expert knobs nobody asked for; dual-tuner Duo
  (two radios from one Duo) — a different feature. HDR bandwidth stays at its default, as before.
- Tests: `test-rsp-caps` (50 checks, the whole table), `check-rsp-caps.mjs` (setup page's offline copy == the table,
  every caps key the server writes is read by the app panel and the web client, each app control gated on its key),
  `test-sdrplay-lib` (39), `check-device-filter.mjs`.
- Copy: the AGENTS.md grep list was checked — no tour card, About feature list, watch tip or website line says where an
  RSP's bias-T/ppm/notches are, so nothing misdirects. One quiet line was added to the V12 release notes.

## 9. Open for Stuart
1. **Sign + notarise the Mac app** from the worktree build — see the hand-off (never `/Applications`).
2. **Mac, both paths, with an RSP:** (a) as is (SDRplay 3.15.1 installed) → log `sdrplay: API library
   /Library/SDRplayAPI/… (system)`, no `vibeserver-rspd` process; (b) the bundled path needs SDRplay's service out of
   the way (e.g. `sudo launchctl bootout system/com.sdrplay.service` for the session, or a Mac without it) → `pgrep
   vibeserver-rspd`, log `(bundled OpenRSP)`, firmware upload in Console, audio. Quit → the service exits.
3. **Android with an RSP (Tab A9 or the Pixel class, Android 8+):** plug in, Start; watch `adb logcat -s VibeRsp
   VibeLocalSDR` for `firmware ready`, `attached`, then `re-attached` after the re-enumeration. Note whether a SECOND
   permission dialog appears (with and without "use by default" ticked). Then Lite on an ARMv7 Android 8+ box.
4. **Idle RAM on a 1 GB phone** with no RSP: should be unchanged (nothing loads) — worth one `dumpsys meminfo` A/B.
5. **Decide**: drop `disable-library-validation` once the bundled library is the only one you want on the Mac (it
   stays while SDRplay's signed library is the fallback).
6. **Decide**: whether a Mac with SDRplay installed should ever prefer ours (today: never — theirs wins, as on Linux).
7. Firmware provenance: if you want the Mac/Android images built in-tree rather than pinned, the build needs sdcc
   4.2.0 (a bookworm container step); 4.6.0 produces different images.

## 10. Commits on this branch
- `e9ce7e26` vendor: OpenRSP 3.15.2-20 source, unmodified
- `a19c6b44` vendor/openrsp: three opt-in build switches (VIBESDR-CHANGES)
- `8a449d5f` VibeServer Linux: build and package OpenRSP
- `711b4357` sdrplay_source: bundled library first unless a system SDRplay service is installed
- `7a2d733e` build images: sdcc; stop staging SDRplay's headers
- `5ec99ae9` Credits: OpenRSP inside M9PSY's entry
- `89d099b0` vibeserver-rsp started by the RSP's arrival (udev); export gate by name
- `5818cac7` vendor/openrsp: macOS frameworks for the explicit libusb
- this plan
