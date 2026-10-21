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
| Mac VibeServer.app | **Planned (§5.2).** OpenRSP's macOS layer exists. Its installer is not out yet, and we need a supervisor in the app. |
| Android app server + VibeServer Lite | **Planned (§5.3).** OpenRSP's Android build embeds the service and takes UsbManager fds, which is exactly what we need. This is a V12 feature with Kotlin USB work, and device testing comes first. |
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
  - **The Google Play AAB of the Android app** is the open question. Most people treat Play as compatible with
    GPLv3, and many GPL apps ship there. But Stuart judged an exception worth writing for his own code, and
    OpenRSP should get the same care. **Ask M9PSY for the same additional permission** (App Store / TestFlight /
    Google Play, official binaries) **before** OpenRSP goes into the Play build. The GitHub APK and Lite (both
    sideloaded) do not need it.
- libusb (LGPL-2.1+) is already linked statically on Linux and the Mac, so nothing changes there.
- **Trade marks.** OpenRSP uses SDRplay's file names only where compatibility forces it. We go further and use
  none of their paths: everything lives under `/usr/lib/vibeserver/openrsp`, and the unit is called
  `vibeserver-rsp`. "SDRplay RSP" appears only to say which receiver this is.

### Quiet, as Stuart asked (2026-10-21)
**Do not advertise "driver-free SDRplay" anywhere.** No website copy, no store copy, no tour, no feature-list
line. We do not want to upset SDRplay. What goes in:
- **Credits** (done on this branch). OpenRSP sits inside M9PSY's existing entry in `AboutOverlay.tsx`
  `CREDITS` and in `README.md`'s credits table, with the GNU GPL v3 noted. The shipped-code table in README
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

### 5.2 Mac VibeServer.app — PLANNED
OpenRSP's Darwin layer is complete (POSIX shm with the 31-character name limit handled, Darwin semaphores,
IOKit hotplug), and upstream reports streaming at 2/8/10 MS/s on Apple silicon.

**Compile check done on this Mac (2026-10-21).** arm64, `CMAKE_OSX_DEPLOYMENT_TARGET=14.0` (our MACOS_MINIMUM),
built against `vibeserver/mac/deps/prefix/lib/libusb-1.0.a` through our explicit-libusb switch, which now adds
the IOKit/CoreFoundation/Security frameworks on APPLE (5818cac7):
- `libsdrplay_api.so.3.15` has 101 exports and links only libc++ and libSystem; its install name is
  `@rpath/libsdrplay_api.so.3`.
- `sdrplay_apiService` links the three frameworks + libc++ + libSystem, minos 14.0.

Nothing is wired into the app yet. Steps:
1. **Build.** In `mac/build-app.sh`, build OpenRSP with `-DCMAKE_OSX_ARCHITECTURES=arm64
   -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOS_MIN` against the deps prefix's static `libusb-1.0.a`. Our
   `SDRPLAY_LIBUSB_LIBRARY` interface target must add `-framework IOKit -framework CoreFoundation -framework
   Security` on APPLE: a small extension of VibeSDR change 2. Firmware is built with Homebrew `sdcc` (check it
   reproduces the manifest), or built once on Linux and checked against the manifest.
2. **Bundle.**
   - `Contents/MacOS/vibeserver-rspd` (the service; MacOS/ is where the notarise script signs executables).
   - `Contents/Frameworks/libsdrplay_api.so.3.15` (+ `.so.3` symlink).
   - `Contents/Resources/openrsp/firmware/own/*`.
   - Licence in `Contents/Resources/COPYING.openrsp`.

   Sign both binaries with our Developer ID inside the notarised app, as usual.
3. **Supervise from the app.** `VibeServerApp.swift` starts `vibeserver-rspd` as a child (env
   `SDRPLAY_OWN_FIRMWARE_DIR=<Resources>/openrsp/firmware/own` — non-code files must not sit in MacOS/), and
   restarts it on exit. It does this **only if no system stack is present**: the Mac fact is
   `/Library/LaunchDaemons/com.sdrplay.service.plist` or `/Library/SDRplayAPI/*/bin/sdrplay_apiService`. This
   Mac has SDRplay 3.15.1 installed, so test it both ways. It runs once for the app; Simple mode (in-process
   server) and Full mode (engine processes) are both clients. Stop it on quit.
4. **Library choice** (`sdrplay_source.cpp`, the `__APPLE__` branch).
   - With no system stack: `<bundle>/Contents/Frameworks/libsdrplay_api.so.3` first, found from the executable
     path.
   - The root-owned check cannot apply: an app in /Applications is owned by its user. Accept it only if its real
     path is inside our own bundle, so the threat the check addressed (a file planted in a user-writable search
     path) does not return.
   - `disable-library-validation` stays while SDRplay's own signed library is a fallback. If the bundled one
     becomes the only one, drop it and let library validation enforce our Team ID.
5. Wait for OpenRSP's macOS installer and Intel testing upstream. Upstream marks Intel untested; we ship arm64
   only, so that does not block us.

### 5.3 Android app server + VibeServer Lite — PLANNED (V12, headline but QUIET)
Upstream fits our model exactly: `libsdrplay_api.so` with the **service embedded** and
`sdrplay_android_usb_attach(fd, deviceName)` for **UsbManager fds** — answering question (1) from 10-19. It
**re-enumerates after the firmware upload**: "the app then gets a NEW device and a new fd; re-attaching the same
deviceName replaces the old entry" — question (2), so expect a second attach. It forces bulk transfers (phones
drop isochronous packets), and gets 8 MS/s gap-free on a Pixel 8a. Requires API 26+.

Steps:
1. **Native build: proved on this Mac (2026-10-21, NDK 27.1, SDK cmake 3.22.1 + ninja, `-DANDROID_PLATFORM=android-26
   -DANDROID_STL=c++_static`, `SDRPLAY_ANDROID_LIBUSB_SRC` = a local libusb 1.0.27 tree, SHA-256-checked — never
   let CMake download at build time).**
   - **Both ABIs build.** NEEDED is libandroid/liblog/libm/libdl/libc only. The library exports the 23 plain
     `sdrplay_api_*` functions plus the eight `sdrplay_android_*` embedding calls (usb_attach/detach/count,
     service_start/stop/running, client_mode, status_json), and **no `libusb_*`** (upstream hides its libusb).
   - Stripped, it is **1.20 MB (arm64) and 0.80 MB (armeabi-v7a)**.
   - ★★★ **NDK 27.1 linked it 4 KB-aligned (LOAD 0x1000) by default.** Only with
     **`-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON`** did arm64 come out at 0x4000. That flag (or
     `-Wl,-z,max-page-size=16384`) is mandatory, and the build script must refuse an unaligned arm64 build,
     the way the sdr-kit scripts do (play_16kb_page_size). Note that memory's "r27 aligns by default" did not
     hold here.
2. **Never linked, dlopen'd, and only on API ≥ 26.** Lite's minSdk is 21 (the main app's comes from
   `rootProject.ext.minSdkVersion`; check it, as it is likely below 26 too).
   The library needs `ASharedMemory_create` (API 26), so it must not be a DT_NEEDED of libvibelocalsdr. Below 26
   the RSP is simply not offered: "a control that only works on one radio should not be there" applies to an OS
   version too.
3. **The Android branch of `sdrplay_source.cpp`.** `dlopen("libsdrplay_api.so")` from the app's own lib dir; the
   root-owned check does not apply, because the app's own lib dir is trusted by the platform. Before `Open`:
   `sdrplay_android_service_start(<filesDir>/openrsp/firmware)`, with the firmware copied from assets on first
   run and checked against the manifest. Define `VIBE_HAVE_SDRPLAY` in `android/app/src/main/cpp/CMakeLists.txt`
   with `vendor/openrsp/include`.
4. **Kotlin.**
   - Add `1df7` + the 7 PIDs to `res/xml/device_filter.xml`.
   - Add `isRsp(dev)` → driver `"sdrplay"` in the three when-blocks of `VibeLocalSdrModule.kt` (the "three-way
     fact" comment there now becomes five-way).
   - Request permission and pass `connection.fileDescriptor` + `dev.deviceName` to `sdrplay_android_usb_attach`.
   - On `USB_DEVICE_ATTACHED` for the re-enumerated RSP, attach the new fd again (and detach on DETACHED).
   - Measure whether Android asks a **second permission** after re-enumeration. The device-filter intent with
     "always open" should grant it silently, but confirm on the XCover and the Tab A9.
5. **The gain model** is already the RSP's (LNA states + IF gain reduction) in `LocalHardwarePanel` /
   `radioCaps` from hwinfo, since VibeServer Linux serves RSPs today. Check that the app's hwinfo path needs no
   Android-specific case (app has no hardcoded settings).
6. **Potato phones.** Measure the APK size and RAM cost on the 1 GB armeabi-v7a class. The library is about
   0.8 MB per ABI, and the service's ring buffers are allocated only while an RSP streams.
7. **Licence before Play:** see §2. The AAB waits for M9PSY's App Store/Play permission. The GitHub APK and Lite
   can ship as soon as steps 1–6 pass.

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

## 8. Commits on this branch
- `e9ce7e26` vendor: OpenRSP 3.15.2-20 source, unmodified
- `a19c6b44` vendor/openrsp: three opt-in build switches (VIBESDR-CHANGES)
- `8a449d5f` VibeServer Linux: build and package OpenRSP
- `711b4357` sdrplay_source: bundled library first unless a system SDRplay service is installed
- `7a2d733e` build images: sdcc; stop staging SDRplay's headers
- `5ec99ae9` Credits: OpenRSP inside M9PSY's entry
- `89d099b0` vibeserver-rsp started by the RSP's arrival (udev); export gate by name
- `5818cac7` vendor/openrsp: macOS frameworks for the explicit libusb
- this plan
