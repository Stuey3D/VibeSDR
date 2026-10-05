# VibeServer on Windows: port plan

*Research and plan only. Nothing here is built yet. Written 2026-10-05 for Stuart.*
*Labels used throughout: **[verified]** means checked against a source or our own code. **[unverified]**
means believed true but not yet tested. **[decide]** means Stuart has to choose.*

---

## 0. Summary

- **The earlier brief exists, but only as a memory note.** It is not a `BRIEF-*.md` file. The agreed
  approach from 2026-08-21 is **UsbDk**, a filter driver that *borrows* the dongle from whatever
  driver Windows bound to it and gives it back when VibeServer exits (§1).
- **The research supports the idea but finds serious risks in the product.** UsbDk works the way
  the brief says. However, its last release was March 2020. It installs a filter on *every* USB hub
  in the machine. libusb's own wiki tells people not to use it. There is an open report of it making
  Windows 11 24H2 unbootable, and an open report of keyboard and mouse loss after install and
  uninstall. It has no ARM64 build (§2.3).
- **Recommendation (§3): use whatever driver is already there, and offer two Zadig-free ways to
  set up a dongle on stock Windows.**
  1. If the dongle already has WinUSB, libusbK or libusb0 (SDR#, SDR++ and Zadig users), **use it as
     it is**. No install and no prompt. This already works with stock libusb.
  2. On a stock dongle, **"Give this dongle to VibeServer"** binds Windows' *own* Microsoft-signed
     WinUSB (the inbox `winusb.inf`, "WinUsb Device") to that one dongle. It shows one admin (UAC)
     prompt, uses no third-party kernel code, and has a "give it back" button. This is the step
     Zadig does, automated and scoped to one dongle. Unlike Zadig (libwdi) it plants no self-signed
     root certificate.
  3. **"Borrow" through UsbDk**, the method in the brief, becomes opt-in for people who also watch TV
     on the same dongle. It ships only if Milestone 1 shows it is stable on current Windows 11.
- **The BDA (TV-tuner driver) path is not viable** (§2.1). Realtek never published its sample
  interface. Windows often installs no TV driver at all. The V4's tuner is unlikely to be handled.
- **The rest of the port is a packaging and portability job**, as the August note predicted. The
  DSP already builds on x86 with SSE2. The real work is in sockets, processes and the platform reads
  (§4).
- **Milestone 1 (§5) ends with a downloadable test `.exe` on GitHub.** It is a probe that answers
  the driver question on a real PC, plus a first RTL-only `vibeserver.exe`.

---

## 1. Where the earlier brief is

### 1.1 Found: Claude memory, not a brief file
`/Users/stuey3d/.claude/projects/-Users-stuey3d-VibeSDR/memory/x86_vibeserver_feasibility.md`, section
**"▶ WINDOWS — AGREED APPROACH (2026-08-21), NOT STARTED"** (verbatim):

> Stuart has a Windows box to test on. ★★★ **NO EV CERTIFICATE** — he declined the £250-400/yr, and
> "the unknown developer thing is pretty much the norm for all SDR software anyway", so SmartScreen is
> accepted and attestation-signed WinUSB INFs are OFF the table.
> ★★★ **THE PLAN IS UsbDk** (pre-signed MSI, ships its own Microsoft signing, costs us nothing):
> libusb has a UsbDk backend (`LIBUSB_OPTION_USE_USBDK` before init). It is a FILTER driver — the
> device is BORROWED while VibeServer runs and handed back on exit, so unlike Zadig the DVB-T driver
> is not permanently replaced and a dongle used for television keeps working. No device picker either
> (Zadig's real hazard is choosing the wrong row and unbinding your mouse).
> ★★ Stuart: a bundled dependency installer is NORMAL on Windows (".net installer or other installer
> for a needed asset") — **"as long as we are clear in our wizard with what we are doing and explain
> how the driver will behave then that is fine"**. So the wizard must SAY what UsbDk does and that it
> is temporary.
> ▶ **UNPROVEN AND MUST BE TESTED FIRST**: libusb's UsbDk backend is less travelled than its WinUSB
> one — needs librtlsdr's async bulk path streaming for real, and the V4 on HF. If it will not carry
> librtlsdr, the fallback is a WinUSB INF, which is where the certificate cost comes back.

It is cross-referenced from `memory/next_android_vibeserver_finalise.md:48`:
*"▶ **Windows is designed but not started** — see [[x86_vibeserver_feasibility]] for the UsbDk plan."*

The same file records the x86 facts this plan builds on:
- `vibedsp` compiles unmodified on x86_64.
- SSE2 kernels shipped in 3.2.1. WFM stereo at 1.92 MS/s costs **5.93 % of a core** with SSE2,
  against 13.97 % scalar, on an i5-11300H.
- amd64 `.deb`s are live on apt.vibesdr.net.

### 1.2 Searched, and not found anywhere else
- `briefs/BRIEF-*.md` (80 files) and `docs/`: no mention of Zadig, WinUSB, UsbDk, libwdi or BDA.
  The only Windows mentions are Edge/Opus (`BRIEF-vibeserver-remote-audio-controls.md`) and "the web
  client already covers Windows" (`BRIEF-website-redesign.md`).
- The repo: these terms appear only in vendored `libusb.h` and `libhackrf/hackrf.c`.
- `~/Downloads`, `~/Desktop`, `~/Documents`: no Windows or Zadig brief.
- Session transcripts: the five sessions the coordinator named, and every one of the 219 transcripts
  (including subagent transcripts), were scanned. In the five named sessions every "zadig" or
  "UsbDk" hit was a false match inside base64 image data. **The oldest surviving transcript is from
  2026-09-06**, so the 2026-08-20/21 conversation that produced the plan has been pruned. The memory
  note above is the only surviving record.

---

## 2. Technical research: talking to an RTL2832U on Windows

### Background: what "stock Windows" actually means [verified + unverified]
- RTL dongles are USB composite devices. The SDR endpoint is **interface 0**: Zadig's "Bulk-In,
  Interface (Interface 0)", hardware ID `USB\VID_0BDA&PID_2838&MI_00`. rtl-sdr.com's quick-start
  guide also accepts the rows "RTL2832UHIDIR" or "RTL2832U". On Windows 11 the guide says you may
  need to untick "Ignore Hubs or Composite Parents" [verified, rtl-sdr.com].
- On plug-in, Windows "will either fail or install Windows DVB-T TV drivers" [verified, rtl-sdr.com].
  So the stock state is **one of two things**: (a1) *no driver* (an unknown "Bulk-In, Interface"
  device), or (a2) a Realtek BDA driver (`RTL2832UBDA.sys`), from Windows Update or the dongle's CD.
  Which one is more common on a fresh Windows 11 PC **[unverified]**. This must be measured on
  Stuart's PC in Milestone 1.
- The **Zadig state (b)** is interface 0 bound to WinUSB (by far the most common), libusbK, or
  libusb-win32 (libusb0.sys).

### 2.1 The BDA / kernel-streaming path (use the TV driver as it is)
- A BDA driver exposes a DirectShow/KS **tuner and demodulator graph**. Its output is a demodulated
  **MPEG transport stream**, not I/Q. The raw-I/Q mode exists in the chip and Realtek used it for
  DAB/FM in its own software. However, "Realtek never published the raw-IQ interface". It is
  reached only through the reverse-engineered librtlsdr [verified, gophertrunk / Osmocom history].
- There is evidence that the Realtek driver *does* carry samples internally. Realtek's own FM player
  "requires Realtek drivers. It does not work with libusb based driver" [verified, onetransistor.eu
  2017]. So a private sample path exists inside that driver. No documentation, SDK or open project
  that uses it was found.
- Fatal problems for us:
  - It only exists in state (a2). In state (a1) there is no driver to talk through.
  - It depends on a closed driver from around 2012, which tunes the tuner itself. Whether it can
    drive the R828D in the RTL-SDR Blog V4 **[unverified, very unlikely]**.
  - All of our tuner control (librtlsdr register writes, the V4's HF upconversion, the bias-T,
    direct sampling, `vibe_r82xx_if.h`) would have to be rebuilt over an undocumented interface.
- **Verdict: not viable.** It works for neither (a1) nor (b), and only possibly for (a2) after
  reverse-engineering.

### 2.2 Vendor control requests through the BDA driver's handle (the "dual" idea)
- A user-mode app can only send a USB control transfer through a driver that exposes a pass-through
  IOCTL or KS property for it. WinUSB does this (`WinUsb_ControlTransfer`). A BDA minidriver does
  not, unless the vendor added a private property set. No such interface is documented for
  RTL2832UBDA.sys. **[unverified: none found]**
- Windows also gives exclusive ownership of an interface to one function driver. You cannot open
  "WinUSB alongside" a bound BDA driver on the same interface.
- **Verdict: not viable** without reverse-engineering a private interface. It has the same
  coverage problems as §2.1.

### 2.3 UsbDk: the brief's "borrow" method
**How it works** [verified, UsbDk Software Development Manual rev 1.3, 2017-04-19]:
- UsbDk.sys is "both USB filter driver and generic USB device driver". On install it is registered
  as a USB filter. It "creates filter instances for **USB hubs only**", sees every enumeration, and
  for a device being redirected it patches the device IDs so PnP sees "a generic USB device" and
  marks the PDO raw. **UsbDk.sys then becomes the driver**.
- Redirect is: *GetDeviceList → Redirect (VID, PID, serial) → reset device …. Stop redirect → reset
  device*.
- The manual's claims, verbatim:
  - "Device capture process is totally dynamic, i.e. no INF files and no self-signing needed, any
    device can be captured."
  - "UsbDk co-exists with original device driver, when the device is not captured original driver is
    loaded by the system automatically."
  - "If user mode client terminates unexpectedly for any reason system reverts to original device
    driver immediately."
  - "Being USB filter driver UsbDk doesn't require WHQL-ing."
- Install: "separate MSI packages for Intel x86 and AMD x64". Install "may require system reboot" in
  some cases. Uninstall "never requires system reboot".
- **libusb integration** [verified, libusb Windows wiki]:
  - It is a runtime option from libusb 1.0.22: `libusb_set_option(ctx, LIBUSB_OPTION_USE_USBDK)`.
  - `libusb_set_option(NULL, …)` adds the option to the defaults for every context created later
    [verified, libusb API docs]. That matters because librtlsdr creates its own context inside
    `rtlsdr_open`. PR #942 fixed backend options not being applied when no default context existed
    [verified]. We already build libusb **1.0.30** for the Mac (`vibeserver/mac/deps/src`).

**So it does exactly what Stuart remembers.** It works in states (a1), (a2) and (b), because it
captures the device whatever driver is bound. No Zadig and no certificate are needed.

**Against it, all found 2026-10-05:**
| Risk | Evidence |
|---|---|
| **Effectively unmaintained** | Newest MSI is **1.0.22, 2020-03-13** (spice-space.org). GitHub tags v1.00-21 and v1.00-22 (2023-2024) are build fixes with no installers attached. [verified] |
| **libusb discourages it** | "use of usbdk is also discouraged as it seems to have some stability issue. Please use WinUSB driver instead." (libusb wiki) [verified] |
| **Whole-machine blast radius** | It filters *every* USB hub, so a fault affects the keyboard, mouse and boot disk, not just the dongle. |
| **Windows 11 24H2: unbootable** | daynix/UsbDk **#134** (2025-02-09): installing 1.0.22 on 24H2 26100.3037 made the system unbootable. Removal left USB input dead, and System Restore was the only fix. Open, no maintainer reply, a single report. [verified, not reproduced] |
| **Input loss after install/uninstall** | #120 (2023, open): mouse and keyboard dead even after `UsbDkController -u`. Similar older reports (#13, #70) cleared after a reboot. [verified] |
| **StartRedirect hangs** | #70 and #35 ("hangs when detaching a Bluetooth device"). [verified, as reported] |
| **No ARM64** | Only x86/x64 MSIs. Windows-on-Snapdragon laptops are excluded. [verified] |
| **Signature on current Windows 11 with Secure Boot / HVCI** | A third-party page says it is signed with a "valid Microsoft cross-certificate", and SPICE/virt-viewer still ship it. Whether 1.0.22 loads under 24H2/25H2 with Memory Integrity on is **[unverified, Milestone 1 must test]**. |
| **Re-bind race on every close** | Our own history: on Linux the DVB driver re-binds the dongle every time VibeServer closes it and races our reopen. That is why `vibeserver/linux/vibeserver-blacklist-dvb.conf` exists (Pi 500, 2026-09-09: "chip type detection failed -110"). Under UsbDk, each `rtlsdr_close` (rate rebuild, DAB entry, idle release) is a **device reset that hands the dongle back to the TV driver**. The fix is design: hold the redirect for the life of the radio process. **[unverified]** |
| **Admin at run time** | Install needs admin (MSI). Whether a non-admin process may *redirect* is **[unverified]**. It matters if VibeServer runs as a normal user from the tray. |
| **Who else ships it** | SPICE/virt-viewer and GIMX. **No SDR project was found using UsbDk for RTL-SDR**, so we would be first. [verified: none found] |

**Verdict: technically the right shape, and the only option that truly borrows the dongle. It is
too risky as the default** for a mass, non-expert Windows audience. Keep it as an opt-in mode,
gated on Milestone 1 (§5) passing on Windows 11 24H2/25H2 with Memory Integrity both on and off.

### 2.4 Automated WinUSB binding (what Zadig does, without Zadig)
Two ways to do it:
- **libwdi (Zadig's engine)**: it builds an INF, self-signs the catalog with a certificate generated
  on the machine, and installs that certificate into **Trusted Root and Trusted Publishers**. The
  private key is then destroyed [verified, libwdi FAQ]. It works on Windows 10 and 11 (that is how
  Zadig works). Planting a root certificate is a security smell and an antivirus flag.
- **The inbox WinUSB, no custom INF at all**: Microsoft documents a Windows 8+ path in Device
  Manager: *Update driver → Browse → Let me pick → Universal Serial Bus devices → **WinUsb
  Device***. It uses Windows' own Microsoft-signed `winusb.inf`, so no certificate of ours is
  involved [verified, learn.microsoft.com "WinUSB Installation for Developers"]. The same steps can
  be done in code with SetupAPI: build the class driver list for `USBDevice`, select "WinUsb Device",
  then call `DiInstallDevice` on `…&MI_00`. Then set `DeviceInterfaceGUIDs` under the device's
  `Device Parameters`, which Microsoft says is needed for app access. The doc warns: "If Universal
  Serial Bus devices doesn't appear in the list of device classes, then you need to install the
  driver by using a custom INF" **[unverified: does it appear for a node currently in the BDA/MEDIA
  class? Milestone 1 must test]**.
- In both cases: one **UAC prompt**, normally no reboot **[unverified]**. It is **a persistent swap**.
  The TV driver no longer loads on that dongle until it is given back. Giving it back means
  uninstalling the device with its driver package, or rolling back, then rescanning. We provide that
  as a button.
- Works in states (a1) and (a2). In (b) it is not needed. Works on ARM64, because the inbox WinUSB
  exists there.
- Like the Linux DVB blacklist, a permanent bind **removes the re-bind race** (§2.3).

**Verdict: the recommended default** for a stock dongle. It does not borrow, but it is
Microsoft-signed, limited to one dongle, reversible, and the same end state as the Zadig step every
SDR# and SDR++ user already has.

### 2.5 A properly signed WinUSB INF package
Microsoft-signed (attestation) INFs need a Hardware Dev Center account, which needs an EV
certificate. That is off the table (§1.1). **Not needed** if §2.4's inbox route works.

### 2.6 Other options considered
- **Airspy HF+, Airspy, HackRF** are WCID devices. Windows auto-installs WinUSB from their Microsoft
  OS descriptors, so no Zadig is needed [verified, Airspy / libwdi WCID wiki]. stock libusb works on
  them.
- **SDRplay** uses its own API service on Windows (`sdrplay_api.dll`, `C:\Program Files\SDRplay\API`).
  We already `dlopen` it on Linux, so on Windows it becomes `LoadLibrary`. The user installs
  SDRplay's API, as they already must on Linux and Mac.
- **What the rest of the SDR world does on Windows today**: SDR#, SDR++, HDSDR's ExtIO_RTL,
  SDRangel and rtl_tcp builds all use librtlsdr over libusb's WinUSB backend, plus **a Zadig step**.
  SDR++'s setup proposes "installing WinUSB" [verified, sdrstore.eu guide; rtl-sdr.com]. No mainstream
  SDR app was found that avoids the driver swap. Doing that would be a real point of difference.

### 2.7 Matrix
| Approach | (a1) no driver | (a2) Realtek BDA | (b) Zadig WinUSB/libusbK/libusb0 | Admin | Reboot | Our cert | ARM64 | Risk |
|---|---|---|---|---|---|---|---|---|
| Use existing WinUSB/libusbK/libusb0 | — | — | **yes** | no | no | no | yes | none |
| Inbox WinUSB bind (§2.4) | **yes** | **yes**\* | not needed | once (UAC) | no\* | **no** | yes | low; persistent swap with a give-back button |
| libwdi bind | yes | yes | not needed | once | no\* | self-signed root | yes | low-medium (root cert) |
| UsbDk borrow (§2.3) | yes | yes | yes | install yes; run \* | sometimes | no | **no** | **high** (machine-wide filter, unmaintained, #134) |
| BDA / KS (§2.1–2.2) | no | private API only | no | — | — | — | — | not viable |

\* = [unverified], to be measured in Milestone 1.

---

## 3. Ranked recommendation for the driver

1. **Always: use what is already there.** If `…&MI_00` is on `WinUSB`, `libusbK` or `libusb0`,
   open it with stock libusb (WinUSB backend; libusbK/libusb0 through `libusbK.dll`). Show **"RTL-SDR
   found — using its WinUSB driver (set up by Zadig/SDR#)"**. No action is needed, and SDR# keeps
   working with it.
2. **Default for a stock dongle: "Give this dongle to VibeServer"**. This is the inbox WinUSB bind
   (§2.4): one UAC prompt and a clear wizard sentence, for example: *"Windows has a TV-tuner driver on
   this dongle (or none). VibeServer will switch this one dongle to Windows' own WinUSB driver. Your
   other USB devices are not touched. You can give it back to Windows any time from Settings."*
   Then a **"Give back to Windows"** button.
3. **Opt-in "Borrow it without changing the driver (UsbDk)"**. Offer it only after Milestone 1
   passes and only for users who need the TV driver kept. The wizard must say it installs a
   system-wide USB filter (Stuart's August rule: be clear about what it does). The radio process
   holds the redirect for its whole life.
4. Not pursued: BDA/KS and vendor IOCTLs (§2.1–2.2), and attestation-signed INFs (§2.5).

**Why not make UsbDk the default, as agreed in August?** In August it was unproven. Today's evidence
is an unmaintained 2020 binary, libusb's own warning, an open unbootable-24H2 report, open reports of
keyboard loss after uninstall, and no ARM64. A Windows audience is mostly not experts. One bricked
PC from a radio server's helper driver would hurt more than a reversible, Microsoft-signed bind on
one dongle. **[decide]** If Stuart still wants borrow-first, Milestone 1 measures exactly this and
the decision can be made on evidence.

### 3.1 Detection: what VibeServer shows
Enumerate with SetupAPI / CfgMgr32. This needs no admin and does not open the device.
- `SetupDiGetClassDevs(NULL, "USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT)`. For each device, read
  `SPDRP_HARDWAREID` and match `USB\VID_xxxx&PID_yyyy` against librtlsdr's `known_devices` table.
  Look at both the composite parent and the `&MI_00` child.
- Read **`DEVPKEY_Device_Service`** (`CM_Get_DevNode_PropertyW` or `SetupDiGetDevicePropertyW`)
  [verified, MS docs] to get the bound service. Also read `DEVPKEY_Device_DriverDesc` and
  `DEVPKEY_Device_ProblemCode` (28 = no driver).
- Map the result to a message:
  | Service on MI_00 | Show |
  |---|---|
  | `WinUSB` | "RTL-SDR found — using WinUSB (Zadig/SDR#). Ready." |
  | `libusbK` / `libusb0` | "RTL-SDR found — using libusbK/libusb-win32. Ready." |
  | none / problem 28 | "RTL-SDR found — needs one-time permission" → §3 option 2 |
  | Realtek BDA (`RTL2832UBDA`, `RTL2832UUSB` or similar **[unverified names]**) | "RTL-SDR found — Windows is using it as a TV tuner" → option 2 or 3 |
  | UsbDk service present and redirect mode enabled | "RTL-SDR found — borrowed via UsbDk" |
- This plugs into the existing **`sdr_presence.h`** logic ("the bus is the witness"). The SetupAPI
  walk is the Windows version of the descriptor walk there, and it also sees held devices. Stuart's
  rule still applies: never say "unplugged" when the dongle is merely held or lent.
- VID:PIDs: `0BDA:2838` and `0BDA:2832` are most common. librtlsdr's `known_devices[]` also lists
  about 40 more, from vendors `0413, 0458, 0ccd, 1554, 15f4, 185b, 1b80, 1d19, 1f4d`. Take the table
  from **our bundled librtlsdr fork** at build time rather than copying it by hand ("one rule, two
  readers").

---

## 4. Port plan for the rest

### 4.1 Toolchain: **llvm-mingw (clang, MinGW-w64), cross-built on the Mac or in Docker** [decide]
- Reasons:
  - The tree is GCC/clang-shaped: `-O3 -ffp-contract=fast`, `__attribute__`, `-march` handling and
    the SSE2/NEON intrinsics compile unchanged.
  - MinGW includes **winpthreads**, so `vibe_thread.h`'s `pthread_*` calls mostly compile as they
    are.
  - It fits the existing "build on the Mac, ship the artefact" pipeline.
  - **llvm-mingw also targets aarch64-w64-mingw32**, so an ARM64 Windows build reuses the NEON path.
- MSVC would mean rewriting compiler flags, the attributes and some intrinsic spellings, and gives no
  benefit for a headless server.
- Also add a **GitHub Actions `windows-latest` job**. It can be a test-only runner that builds
  natively and runs the DSP and DAB unit tests on real Windows. "A new compiler is a new test rig"
  (memory, August). The repo already has `.github/workflows/codeql.yml`.

### 4.2 POSIX survey: server sources
This counts **lines matching each pattern, per file**, across the sources `vibeserver/CMakeLists.txt`
compiles into `vibeserver` (`vibeserver/*.cpp|h` plus the shared `android/app/src/main/cpp` core,
decoders and vibedsp, about 114,500 lines). Counted 2026-10-05.

| API | Where (lines) | Windows answer | Size |
|---|---|---|---|
| `fork` / `exec*` / `waitpid` / `kill` | main.cpp (fork 2, exec 5, waitpid 3, kill 4); proc.cpp (fork 3, exec 1); vibe_dab_aacdec.h (fork 1, exec 1) | `CreateProcessW` with an argv→command-line quoter; `WaitForSingleObject`; `TerminateProcess`. `proc.h`'s no-shell design ports cleanly. | M |
| `prctl(PR_SET_PDEATHSIG)` / `parent_watch` | vibe_thread.h 6, main.cpp 5, rtl_tcp_server.cpp 3, parent_watch.* 3 | **Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`**. This is stronger than PDEATHSIG, and also fixes the "engine orphaned itself" class of bug on this platform. | S |
| Signals (`signal`/`sigaction`/SIGPIPE/SIGTERM) | main.cpp 7, plus 1–2 each in vibe_dab_aacdec.h, vibe_decoder_host.h, sdrplay_source.cpp, local_sdr_shim.cpp | `SetConsoleCtrlHandler`, and the service control handler. SIGPIPE does not exist (Winsock returns an error instead). | S |
| **FD passing** (`SCM_RIGHTS`, `sendmsg`/`recvmsg`, `AF_UNIX`, `socketpair`) | fd_passing.cpp 12, net_shim.h 2, local_sdr_shim.h 1 | **The biggest design item.** Windows has AF_UNIX (Win10 1803+) but **no SCM_RIGHTS**. Use `WSADuplicateSocketW(sock, childPid)` → send the `WSAPROTOCOL_INFOW` blob over a named pipe or AF_UNIX → `WSASocketW(FROM_PROTOCOL_INFO)` in the radio process. MSG_PEEK on the front door still works. Keep the same `sendFdTo`/`fdAccept` API. | M |
| Sockets: `close()` on sockets, `fcntl(O_NONBLOCK)`, `poll`/`select`, `setsockopt`, `MSG_NOSIGNAL`/`SO_REUSEPORT`, `errno` | `close(` 117 in local_sdr_shim.cpp (mixed sockets and files), plus 3–14 in about 12 other files; fcntl 7 in net_shim.cpp; poll/select 15 across 8 files; setsockopt 20; MSG_* 4 | A `vs_sock` layer: `closesocket`, `ioctlsocket(FIONBIO)`, `WSAPoll`, `WSAGetLastError`, `SO_EXCLUSIVEADDRUSE`, `WSAStartup`. **The 117 `close(` calls in the shim must each be classed as socket or file.** That is mechanical, but each one has to be judged. | **L** |
| `/proc`, `/sys`, `/dev` reads | vibe_admin.h 13, directory.cpp 11, vibe_vcio.h 10 (Pi only), local_sdr_shim.cpp 8, tui.cpp 7, vibe_benchmark.h 6, vibe_hwinfo.h 4, vibe_health.h 3, proc/main/radios/mapgl 1–2 | CPU: `GetSystemTimes`. Memory: `GlobalMemoryStatusEx`. Clock and throttle (the "throttle snail"): `CallNtPowerInformation(ProcessorInformation)`. Self path: `GetModuleFileNameW`. **Temperature has no reliable non-admin API, so remove that readout on Windows** (the "no dead controls" rule). | M |
| `mmap` | local_sdr_shim.cpp 2 | `CreateFileMapping`/`MapViewOfFile`, or a plain read. | S |
| `dlopen`/`dlsym` | sdrplay_source.cpp 6, local_sdr_shim.cpp 1, airspyhf_source.h 1 | `LoadLibraryW`/`GetProcAddress`, plus the SDRplay API path lookup. | S |
| `getifaddrs` | local_sdr_shim.cpp 2, main.cpp 1 | `GetAdaptersAddresses`. | S |
| `pthread_*`, `setpriority`/`sched_*` | vibe_thread.h 13 + 6, pipeline.cpp 2, shim 2, mpxmeasure 1 | winpthreads compiles these; replace priority with `SetThreadPriority` + **MMCSS** (`AvSetMmThreadCharacteristicsW("Pro Audio")`) for the USB reader thread (the v6 lesson: USB *input* starvation). Names: `SetThreadDescription`. | S |
| `popen` / `system` | tui 3, directory 2, shim 2, radiodns 1–2, proc.h 1, main 1, dab_aacdec 1, benchmarks 2, net_shim 1 | Route through `vibeproc::run` (CreateProcess). Note: **Windows 10 1803+ ships `curl.exe`** in System32, so the existing curl-to-a-file design survives. **`gunzip` does not exist**: link zlib or miniz for asndb/eibi. | M |
| Filesystem (`stat`, `mkdir`, `opendir`, `realpath`, `unlink`, …) | main.cpp 15, vibe_mapgl.h 9, config 7, dab_service 6, shim 6, 2–4 in about 9 others | Prefer `std::filesystem` (C++17 is already the standard). Paths: `%ProgramData%\VibeServer` for config and maps; UTF-8 ↔ wide at the boundary. | M |
| `isatty`/`ioctl` | vibe_vcio.h 5 (Pi only), shim 2, main 1 | `_isatty`; drop the Pi-only code. | S |
| `getuid`/`getpwnam` | main.cpp 1 | Drop it (there is no service user to drop to). | S |
| `clock_gettime`/`usleep` | 1–2 in 7 files | `std::chrono` (mostly already used); MinGW provides `clock_gettime`. | S |
| ncurses TUI (`tui.cpp`) | the whole setup wizard | **Do not port.** On Windows the setup is the existing web setup page (`vibe_setup_page.h`) opened by the tray app. | S |
| `vibe_dab_aacdec.h`: ffmpeg as a child process | about 30 lines | Either use **Media Foundation's AAC decoder** (the platform decoder, like AudioToolbox on the Mac; HE-AAC support **[unverified]** for DAB+'s 960-sample frames), or find `ffmpeg.exe` if installed. Start with ffmpeg-if-present and add MF later. | M |
| mDNS responder (`mdns_responder.cpp`) | binds UDP 5353 | Windows' own `Dnscache` already owns 5353. Use **`DnsServiceRegister`** (Win10 1809+) instead. **[unverified coexistence]** | S–M |
| `haveServiceManager()` = systemd check | main.cpp | On Windows the front door supervises its radio processes itself (already supported for "no systemd"), inside a Job Object. | S |

### 4.3 Radio libraries
| Library | Windows | Size |
|---|---|---|
| libusb 1.0.30 | Builds with MinGW. WinUSB backend by default; UsbDk only as an option (§3). | S |
| **librtlsdr (our fork, with V4 support)** | Builds with MinGW against libusb. The upstream Windows builds are the same code. Make sure our fork keeps `rtlsdr_open` working by index as well as the Android `open_sys_dev`. | S |
| libairspyhf / libairspy / libhackrf (vendored C) | Build as they are against libusb; the devices auto-bind WinUSB (WCID). | S |
| SDRplay API 3.15 | User-installed Windows service + `sdrplay_api.dll`; `LoadLibrary` from `%ProgramFiles%\SDRplay\API\x64`. Headers come from the same build-machine staging as Linux (they are SDRplay's to license). | S |
| libopus 1.6.1 | Builds with MinGW. | S |
| kissfft / pffft / ft8_lib | Portable C. | S |

### 4.4 Web UI, ports, firewall
- The web UI is served by the server, so there is no change.
- **The Windows Defender Firewall prompt** appears the first time the server listens on the network.
  The installer adds an inbound rule for `vibeserver.exe`, limited to Private profiles by default
  [decide: Public too?], with `netsh advfirewall` or `INetFwPolicy2`. That avoids a confusing
  prompt, or a silently blocked LAN when the user clicks "Cancel".
- Sleep: call `SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_CONTINUOUS)` while listeners are
  connected. A laptop serving radio should not doze off mid-stream. [decide: also while idle?]

### 4.5 Process model: tray app first, service later [decide]
- **v1: a per-user tray app** (small Win32 `Shell_NotifyIcon`, like the Mac menu-bar app). It starts
  the front door, has an "Open VibeServer" item that opens the browser to the setup page, and
  "Start with Windows" via `HKCU\…\Run`. It needs no admin at run time; WinUSB access does not need
  admin [unverified for UsbDk redirect].
- **v2: an optional Windows Service** (LocalSystem or a virtual service account) for headless or
  always-on boxes. It is chosen in the installer. libusb and WinUSB work from session 0.
- Both use the same `vibeserver.exe`. The tray app is a thin control surface, like the Mac app over
  `vibeserver_core`.

### 4.6 Installer [decide]
- **Milestone 1: a portable ZIP**, with no installer.
- **Release: NSIS** (`makensis` runs on macOS and Linux, so it can be cross-built in the same
  pipeline) or **Inno Setup** (Windows-only, run in the GitHub Actions Windows job).
- The installer handles: Program Files layout, firewall rule, Start menu, optional service, optional
  "prepare RTL dongles" step (§3), and uninstall that **offers to give dongles back to Windows**.
- **MSIX is unsuitable.** It cannot install drivers or services cleanly, and its sandbox fights
  USB access.
- WiX/MSI is possible later (`wixl` from msitools builds an MSI on Linux) if enterprise deployment
  ever matters.

### 4.6b Microsoft Store — Stuart's preferred distribution (2026-10-05: "I would like a store app if i could")
Store apps are signed by Microsoft — no SmartScreen warning, no Smart App Control block, no certificate to buy — with
one-click install and automatic updates. To settle FIRST, ideally with a real Store test submission once Milestone 1's
build exists (all **[unverified]** today):
1. **Account cost** — believed free for individual developers now; confirm the current terms.
2. **The driver bind from a Store app** — binding the in-box WinUSB needs admin once; packaged (MSIX) apps are limited
   on elevation and drivers. Options: a restricted capability with a justification Microsoft reviews, or a small
   separate elevated helper. Dongles already on WinUSB (Zadig / SDR# / SDR++ users) need no bind at all.
3. **Running in the background** — MSIX can carry a Windows service, but that is also a restricted capability.
4. Whether the Store's unpackaged-installer (MSI/EXE) route helps — it is believed to still require the installer to be
   signed with a CA certificate of our own, which would undo the point.
Likely shape: the Store as the main way in, the GitHub download (signed via SignPath, §4.7) alongside.

### 4.7 Code signing [decide]: a risk for SOME new PCs, decided before a public release
- **SmartScreen** shows "Windows protected your PC" and allows *More info → Run anyway*. It is
  accepted in August as the SDR norm.
- **Smart App Control (Windows 11)** is different, but it applies only to machines where it is ON. Where it is on, it
  **blocks unsigned executables outright, with no override** [verified, Microsoft Q&A and multiple GitHub issues].
  It exists only on clean installs of Windows 11 22H2 or later, starts in an evaluation mode and then decides on its
  own whether to stay on; machines upgraded from Windows 10, and machines where it has been switched off, never
  enforce it. **Everyone else gets the SmartScreen "Run anyway" above** — which is what Stuart sees on his own PC
  (2026-10-05: "I've been able to run apps without a certificate on windows with a Run Anyway prompt").
  How large the SAC-on share of SDR users is, is **[unverified]**: ask the first Windows testers what Windows
  Security → App & browser control → Smart App Control says, and decide on signing from that. Unsigned test builds
  (Milestone 1) are unaffected on Stuart's PC if his setting is Off. This was not known in August.
- ★★ **STUART'S DECISION (2026-10-05): sign, but not for ~£120 a year on a free app.** Recommended route:
  **SignPath Foundation (free for open source)**. The VibeSDR repo is **public and GPL-3.0** (checked with `gh repo
  view`), which meets its licence requirement. As recalled, to be **[verified against their current terms before
  applying]**: artefacts must come from public CI (GitHub Actions — the Windows build lives there anyway); a short
  code-signing policy on the project page; two-factor on maintainers' accounts. ✓ No closed-source snag: **we do not ship the
  SDRplay API — it is the user's own separate install** (Stuart, 2026-10-05), loaded at runtime, as on the other
  platforms. Stuart: "signpath seems like a good fit".
  Free fallback to check: the Microsoft Store individual developer account (believed free now — unverified — but a
  driver-switching server may not fit the Store's packaging rules).
- Cheaper routes than the declined EV certificate (all paid; kept for reference):
  - **SignPath Foundation**: free OV signing for open-source projects. Whether VibeServer qualifies
    under its licence is **[decide]**.
  - **Azure Artifact Signing (Trusted Signing), about US$10/month**: trusted by both SmartScreen and
    SAC. However, **individuals: US/Canada only**; **organisations: EU and UK included** [verified,
    learn.microsoft.com / Microsoft Q&A]. This would work if Stuart has, or forms, a UK company.
  - An OV certificate from a CA, about £100–250 a year (now on a hardware or cloud token).
- Defender false positives are likely: an unsigned exe that bundles cloudflared and changes a USB
  driver looks suspicious to heuristics. Submit each release to Microsoft's WDSI portal.

### 4.8 Auto-update
- apt does this on Linux. On Windows, the server checks a signed JSON manifest (hosted beside
  apt.vibesdr.net or on GitHub Releases), downloads the installer, verifies its hash and signature,
  and runs it silently between listeners. It must respect the "do not take a dial someone is
  listening on" rule by checking the listener count first.
- Optional: publish to **winget** (`winget-pkgs` manifest) for `winget install VibeServer`.

### 4.9 Cloudflare tunnel
- `cloudflared` publishes **`cloudflared-windows-amd64.exe`** and `-386.exe`, plus MSIs.
  The latest release is 2026.9.3 (2026-09-24) [verified, GitHub]. No Windows ARM64 exe was listed,
  so it would run under x64 emulation on ARM64 **[unverified]**.
- Ship it beside `vibeserver.exe`. `directory.cpp: cloudflaredPath()` already looks "beside the
  executable", so it only needs the `.exe` suffix and `GetModuleFileNameW`.
- `--protocol http2` and the Quick Tunnel log parsing are unchanged. Run it inside the same Job
  Object.

### 4.10 Effort summary
| Area | Size |
|---|---|
| Driver: detection (SetupAPI) + inbox-WinUSB bind/unbind + give-back | M |
| Driver: optional UsbDk mode (libusb option + hold-for-life + wizard) | S–M (plus testing) |
| Sockets compatibility layer + the 117-call audit in the shim | **L** |
| FD passing → `WSADuplicateSocket` | M |
| Processes (CreateProcess, Job Objects, ctrl handler) | M |
| Platform reads (/proc and /sys equivalents, throttle, hwinfo) | M |
| Filesystem/paths/UTF-8 | M |
| DAB+ AAC (ffmpeg-if-present, then Media Foundation) | M |
| mDNS via DnsServiceRegister | S–M |
| Build: llvm-mingw cross + GitHub Actions Windows test job | M |
| Tray app | M |
| Installer (NSIS/Inno) + firewall + service option | M |
| Signing + Defender submissions | S of work; cost and **[decide]** |
| Auto-update | M |
| **Total** | roughly **4–7 focused weeks** to a release candidate. Unverified: the shim audit dominates. |

---

## 5. Staged plan

### Milestone 1: prove the driver question on a real PC, ending in a downloadable test `.exe`
Deliverable: **`vibeserver-windows-x64-test1.zip` attached to a GitHub *pre-release***. There is no
Windows `.exe` on GitHub today. It contains:
1. **`vibe-usbprobe.exe`**, a standalone console tool (llvm-mingw, static libusb 1.0.30 and our
   librtlsdr fork):
   - `list`: the SetupAPI walk. Shows each RTL node (parent and MI_00), its VID:PID, bound service,
     problem code, and the line from §3.1 that VibeServer would show.
   - `stream [--usbdk]`: open with librtlsdr, stream **60 s at 2.4 MS/s**, and report dropped and
     short buffers. With the V4: also stream on HF (upconversion) and toggle the bias-T. Uses the
     WinUSB backend, or the UsbDk backend if UsbDk is installed.
   - `bind` / `giveback`: the inbox-WinUSB bind on MI_00, and its reversal (UAC prompt).
   - `cycle N`: open, stream 5 s, close, N times. This is the re-bind-race test (§2.3).
2. **`vibeserver.exe`, RTL-only first build.** Front door and one radio, web UI, no tunnel, no DAB+
   AAC, no TUI. Enough to listen to FM from a browser on another PC.

**Test matrix on Stuart's Windows box** (Windows 11, version noted; Memory Integrity on, then off):
- Dongles: V3 and V4.
- States: (a1) freshly plugged with no driver; (a2) Realtek BDA if it can be obtained; (b) Zadig
  WinUSB; (b') libusbK.
- Methods: existing driver / inbox bind / UsbDk 1.0.22.

**Go/no-go:**
- Inbox bind: "Universal Serial Bus devices → WinUsb Device" can be selected in code from both (a1)
  and (a2); `stream` has no drops at 2.4 MS/s; `giveback` restores the original state; no reboot.
  → **GO for the default.**
- UsbDk: installs and boots on 24H2/25H2 with Memory Integrity **on**; `stream` and `cycle 50` are
  clean; uninstall leaves the keyboard and mouse working. → **GO for the opt-in borrow mode.**
  Otherwise drop it.
- Record admin/reboot behaviour and the exact service names seen (they fill in the
  **[unverified]** rows above).

### Milestone 2: full server parity on x64
The sockets layer and shim audit, FD passing, multi-radio front door, Airspy HF+/SDRplay, DAB+
(ffmpeg-if-present), the tunnel, and the same unit tests passing on GitHub Actions Windows. It still
ships as a ZIP pre-release.

### Milestone 3: product
Tray app, the detection UI from §3.1 in the web setup page, NSIS/Inno installer with firewall rule
and the dongle step, signing (per the §4.7 decision), auto-update, and the website/`AboutOverlay`
copy (per AGENTS.md, anything that says where things are must be updated). Add an ARM64 build if
llvm-mingw and cloudflared allow.

### Milestone 4: optional service mode, winget, Media Foundation AAC.

---

## 6. Open questions for Stuart
1. **Default driver path:** accept "Give this dongle to VibeServer" (inbox WinUSB, persistent but
   reversible) as the default, with UsbDk borrow as opt-in? Or insist on borrow-first if Milestone 1
   passes?
2. **Signing (before a public release, not for testing):** on the subset of Windows 11 PCs where Smart App Control
   is ON, an unsigned app is blocked with *no override*; everyone else gets SmartScreen's "Run anyway". Once early
   testers tell us how common SAC-on is: a UK company (which makes Azure Artifact Signing about US$10/month
   available), SignPath (if the licence qualifies), an OV certificate — or accept that SAC-on machines cannot run it?
   ★ ANSWERED 2026-10-05: sign, free route preferred — SignPath Foundation (repo is public GPL-3.0); see §4.7.
3. **Is VibeServer open source** for SignPath's purposes? Under which licence?
4. **Tray app first, service later**, or is a headless service needed from day one?
5. **Firewall rule:** Private networks only by default, or Public too?
6. **DAB+ on Windows:** is "works if you install ffmpeg" acceptable for the first release?
7. **Test PC details:** Windows 11 version, Memory Integrity state, and whether any Realtek TV driver
   is installed today. Is it a PC Stuart minds having a test USB filter driver (UsbDk) installed on?
   It is worth a restore point first.
8. **ARM64 Windows:** worth a build in Milestone 3?

---

## Sources
- UsbDk: [GitHub](https://github.com/daynix/UsbDk) · [releases](https://github.com/daynix/UsbDk/releases) ·
  [MSI downloads (spice-space)](https://www.spice-space.org/download/windows/usbdk/) ·
  [Software Development Manual rev 1.3](https://www.spice-space.org/download/windows/usbdk/UsbDk_Software_Development_Manual.pdf) ·
  issues [#134](https://github.com/daynix/UsbDk/issues/134), [#120](https://github.com/daynix/UsbDk/issues/120),
  [#13](https://github.com/daynix/UsbDk/issues/13), [#70](https://github.com/daynix/UsbDk/issues/70),
  [#35](https://github.com/daynix/UsbDk/issues/35)
- libusb: [Windows wiki](https://github.com/libusb/libusb/wiki/Windows) ·
  [contexts/options](https://libusb.sourceforge.io/api-1.0/libusb_contexts.html) · [PR #942](https://github.com/libusb/libusb/pull/942)
- libwdi: [FAQ](https://github.com/pbatard/libwdi/wiki/FAQ) · [WCID devices](https://github.com/pbatard/libwdi/wiki/WCID-Devices)
- Microsoft: [WinUSB installation](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb-installation) ·
  [DEVPKEY_Device_Service](https://learn.microsoft.com/en-in/windows-hardware/drivers/install/devpkey-device-service) ·
  [Code signing options](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options) ·
  [Smart App Control FAQ](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/smart-app-control-frequently-asked-questions) ·
  [SAC vs individual developers (Q&A)](https://learn.microsoft.com/en-us/answers/questions/5524360/smart-app-control-is-unfriendly-to-individual-deve) ·
  [Artifact Signing individual-country limits (Q&A)](https://learn.microsoft.com/en-us/answers/questions/5810735/cant-create-a-new-trusted-signing-individual-ident)
- RTL-SDR on Windows: [rtl-sdr.com Quick Start](https://www.rtl-sdr.com/rtl-sdr-quick-start-guide/) ·
  [onetransistor: Realtek FM player needs the Realtek driver](https://www.onetransistor.eu/2017/08/fmplayer-realtek-rtl2832u.html) ·
  [gophertrunk RTL2832U](https://gophertrunk.org/reference/rtl2832u/) ·
  [sdrstore.eu Windows guide](https://www.sdrstore.eu/rtl-sdr-setup-guide-windows-sdrsharp-sdrplusplus-zadig-drivers-first-signal/)
- Others: [cloudflared releases](https://github.com/cloudflare/cloudflared/releases) ·
  [SDRplay API (Windows)](https://sdrplay.com/download/hardware-api-windows/) · [Airspy downloads](https://airspy.com/download/)
- In-repo: `vibeserver/CMakeLists.txt`, `vibeserver/main.cpp` (superviseRadios, selfExePath),
  `android/app/src/main/cpp/fd_passing.h`, `vibeserver/proc.h`, `vibeserver/sdr_presence.h`,
  `vibeserver/linux/vibeserver-blacklist-dvb.conf`, `android/app/src/main/cpp/vibe_dab_aacdec.h`,
  `vibeserver/directory.cpp` (cloudflaredPath).
