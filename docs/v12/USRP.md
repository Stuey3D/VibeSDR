# Ettus USRP B200 / B210 / B200mini in VibeServer (V12, 2026-11-02)

Branch `v12-usrp` (from `v12-experimental`). Asked for by a VibeSDR user, _zer0_gravity_, about the AliExpress "B210"
boards. **Status: added, needs a tester.** Nobody on the VibeSDR side owns a USRP, so the driver has been written from
Ettus's documentation and UHD's published C++ API, and has never been run against a radio.

## RESUME HERE

* Everything below is committed on `v12-usrp`, nothing pushed, merged or published.
* Built and tested here: **Mac arm64** (dev build, `test-usrp-caps` 71/71 with UHD linked, `--list-radios` clean; the
  full `.app` from `build-app.sh --no-copy` — the B2xx driver in both binaries, `Resources/uhd-images` present, the
  macOS-14 gate PASSED for libuhd.a and every Boost archive; not signed, notarised or installed) and
  **Linux arm64** (the real package recipe, `Dockerfile.build` + `build-deb.sh`, native in colima — see "Sizes").
  **Linux amd64**: UHD, Boost and `usrp_source.cpp` cross-built for x86-64 from the same `build-uhd.sh` and the
  no-device test run under emulation (see "Sizes"); the full amd64 *package* has not been built, because that happens
  on the Lenovo, which is a live server. The first real amd64 package comes out of the normal publish route.
* **Next:** a volunteer with a B210 (or a B200mini) runs the test list at the bottom. Nothing else is blocked.
* **`v12-usrp-options` (2026-11-02, on top):** the owner's radio options (reference clock / time source, DC-offset and
  IQ-balance correction, analogue RX bandwidth), the web client's RADIO AGC switch, the **board's own firmware / FPGA
  image** section (upload, or copy from this computer's own UHD images), and plain words for a clone's failed FPGA
  load and for a radio another program holds. See "Owner options" and "Board's own image" below. Built and tested on
  the Mac (dev build with UHD: `test-usrp-caps` 160/160; header-only 158/158; `check-usrp-options.mjs`); untested on a
  radio, like the rest.
* Open follow-ups (small, none blocking): measure the 250 kHz DC guard on real hardware; measure the CPU per rate on a
  Pi 4 / Pi 5 and set the arm64 default from that.

## What it is, in one paragraph

A seventh capture source, `driver "usrp"`, beside RTL, SDRplay, Airspy HF+, Airspy R2/Mini, HackRF and Icom. UHD 4.9.0.1
is compiled with **every device but the B2xx turned off**, linked statically into `vibeserver`, and the B2xx firmware
and FPGA images are fetched **at build time**, SHA-256-checked against UHD's own `images/manifest.txt`, and shipped in
the package. The owner installs nothing: no `uhd-host`, no `uhd_images_downloader`, no SoapySDR. Plug the radio into a
USB 3 port, it appears in the Setup page's radio list like any other, and the app and web page draw its controls from
`hwinfo` with no USRP constants of their own.

## Where it builds (Stuart, 2026-11-02: high-end only)

| Host | USRP support | Why |
|---|---|---|
| Linux **amd64** | yes | |
| Linux **arm64** (Pi 4 / Pi 5 class, USB 3) | yes | |
| **Mac** (arm64) | yes | in the app's Simple mode and Full mode |
| Linux armhf (Pi 2), ARMv6 | **no** | a Pi 2 has no USB 3 and cannot carry a B2xx's rates; UHD is not built there |
| Linux i386 | **no** | out of scope (Stuart) |
| Android, Lite, iOS | **no** | see "Android (later)"; the iOS App Store library stays GPL-free and UHD-free |

Off those three hosts `usrp_source.h` compiles to an **inline stub** that reports zero devices, so a USRP is never
listed, offered or half-drawn there (AGENTS.md: no dead control). `build_ios.sh`, the Android CMake and Lite are
untouched — they include the header and get the stub.

**It needs USB 3 and a high-end ARM or x86-64 host.** On a USB 2 port it still runs, with the rate list capped at
8 MS/s, and the Setup page and app say so; a B210 on USB 2 (or with both channels in use) may need its own 6 V supply.

## What works (as written — untested on hardware)

| | |
|---|---|
| Radios | B200, B210, B200mini, B205mini (and B206mini, which UHD maps to the B205mini image); NI-branded B200/B210 |
| Enumeration | our own claim-free libusb descriptor walk (2500:0020–0023, 3923:7813/7814) — never `uhd::device::find`, which loads firmware into every B2xx it meets. Listed **last**, after the Icom, so no existing `--radio N` index moves. Name "Ettus USRP B200/B210 (serial)" before opening (one PID covers both), the real model once open |
| Open | `uhd::device::find` by serial with our firmware path → the product from the EEPROM → our FPGA image for it → `multi_usrp::make` with both paths explicit (absolute, so UHD searches nothing). Our directory is also compiled into libuhd as its images directory, so an owner's own UHD install of another release is never used |
| Tuning | 50 MHz – 6 GHz, the range the device reports (`get_rx_freq_range`); a 250 kHz DC guard (the HackRF's — the AD9361 is zero-IF) |
| Rates | 1, 2, 2.4, 3.2, 4, 5, 8 MS/s on USB 2; + 10, 12.5, 16 MS/s on USB 3. Default for a new radio: 2.4 MS/s on arm64, 4 MS/s on x86-64 / Mac. The default is set by the host, the list never is (the owner can always choose) |
| Gain | ONE element, the device's own range (`get_rx_gain_range`: 0–76 dB in 1 dB steps), on the generic slider; per-band ceilings, per-band lock and a starting gain from the Setup page |
| AGC | the AD9361's own, as AUTO (negative gain) — only when the radio accepted `set_rx_agc` at open; otherwise no AUTO is offered. Not VibeAGC. A switch for it in the app's panel and the web client's **RADIO AGC** row |
| Owner options | reference clock + time source, DC-offset / IQ-balance correction, analogue bandwidth — Setup page, per radio, each only where the radio reports it (below) |
| RX ports | `RX2` and `TX/RX`; on a B210 `RX2 (A)`, `TX/RX (A)`, `RX2 (B)`, `TX/RX (B)` (switching channel rebuilds the stream). Through the generic `{type:"antenna"}` message and `antennaPort`, the owner's antenna lock, and the per-band aerial list ("0-400MHz RX2, 400MHz+ TX/RX"). **VibeServer never creates a TX stream: choosing TX/RX only receives on it.** |
| Not offered (it has none) | bias-T, direct sampling, a listener's tuner IF filter (the analogue filter follows the span, or the owner's fixed figure), the RTL digital AGC, PPM, the NDB skimmer (50 MHz floor) |
| Watchdog | the HackRF's escalation: restart the stream, then close + reopen by index with gain and RX port put back. UHD overflows ('O') counted as USB holes (`usbDrops`) |
| Lend / take back | release closes the radio, reacquire reopens it — like every other desktop radio |
| Samples | `fc32` on the host, `sc16` on the wire: **UHD's own converter** (SSE2 / NEON with a generic fallback, inside libuhd) does int16 → float, so there is no new hot loop of ours to vectorise. The AD9361's DC-offset and IQ-balance tracking are on (UHD's default) unless the owner switches them off |
| Threads | our receive thread takes the IQ rung (`vibeIqThread`) — Network > Audio > Spectrum > Decoders is unchanged. UHD's own libusb event thread runs at the default priority (not reachable from outside UHD) |

## Owner options (Setup page → the radio's tab → Radio options)

Per radio, in `config.json` (`usrpClock`, `usrpTime`, `usrpDcOffset`, `usrpIqBalance`, `usrpRxBw`), set only from the
Setup page (admin) — no listener message reaches them. Applied before the radio opens and again, live, on a save.
**Each control is drawn only from what the running radio reported**; with the radio not running the card says when
they appear and the saved values are kept.

| Option | Appears when | Values | Notes |
|---|---|---|---|
| Reference clock | `get_clock_sources` gives at least two of internal / external / gpsdo | internal (default), external (10 MHz on REF), GPSDO | **GPSDO only with a GPSDO fitted** — the `gps_locked` mboard sensor exists (UHD lists "gpsdo" on every full-size B200/B210 whether a board is fitted or not). Lock shown from `ref_locked` (not on internal) |
| Time source (PPS) | `get_time_sources` likewise | internal, external, GPSDO | timestamps only; a mini cannot take external for both clock and time (one SMA) — refused in words, time stays internal |
| DC-offset correction | the radio has the `dc_offset/enable` property | on (default) / off | `set_rx_dc_offset(bool)` |
| IQ-balance correction | the radio has `iq_balance/enable` | on (default) / off | `set_rx_iq_balance(bool)` |
| Analogue bandwidth | `get_rx_bandwidth_range` is at least 1 MHz wide (0.2–56 MHz on an AD9361) | AUTO (follows the sample rate) or a fixed figure from the radio's range | AUTO is what UHD does |

A saved choice the radio does not have (a GPSDO on a board without one, a bandwidth outside its range) is **refused
in words** — `optionsNote` in hwinfo, shown on the Setup card, and in the log — and the radio's default kept. The
save itself refuses only a bad shape (an unknown name, an impossible bandwidth). hwinfo's `radio` object carries
`clockSources`, `timeSources`, `clock`, `time`, `refLocked`, `gpsdo`, `gpsLocked`, `dcOffset(On)`, `iqBalance(On)`,
`rxBwRange`, `rxBwChoices`, `rxBwAuto`, `rxBwHz`, `optionsNote` — each only where it applies. The app and the web
client show the reference read-only when it is not internal.

## Board's own image (Setup page → the radio's tab → Board's own firmware / FPGA image)

For a clone that is not Ettus's design. **We still ship no third-party image** (no stated licence). The owner either
**uploads** the file their seller provides, or presses **Use this computer's UHD images…**, which — only then, never
at open — looks in `/usr/share/uhd/images`, `/usr/share/uhd/*/images`, `/usr/local/share/uhd/images`,
`/opt/homebrew/share/uhd/images` and the service's `UHD_IMAGES_DIR`, lists the B2xx files there (size, date, its kind,
and whether it is byte-for-byte the same as ours), and copies the one chosen (with the FX3 firmware from the same
folder only when that differs from ours). An old genuine-board image in a system folder can carry another UHD
release's FPGA compatibility number, which is why it is never searched silently.

* Written into the override folder (`/etc/vibeserver/uhd-images` — /etc/vibeserver belongs to the daemon and is in both
  units' `ReadWritePaths`; the Mac: `~/Library/Application Support/VibeServer/uhd-images`) under **UHD's own file name**,
  chosen from a closed list (`usrp_b210_fpga.bin`, `usrp_b200_fpga.bin`, `usrp_b200mini_fpga.bin`,
  `usrp_b205mini_fpga.bin`, `usrp_b200_fw.hex`) — never a path from the client. Streamed to a temporary file
  (`O_EXCL|O_NOFOLLOW`), fsync'd, renamed; a refused or short upload leaves the old file as it was.
* **Checked before it is kept:** an FPGA image must be a Xilinx bitstream — the sync word `AA 99 55 66` within the
  first 256 bytes, 1–16 MB — and is named by family: **7-series (Kintex/Artix)** when the bus-width detect
  `00 00 00 BB 11 22 00 44` comes before the sync word (a clone board's), **Spartan-6** otherwise (a genuine B2xx's).
  The firmware must be Intel HEX (UHD loads nothing else); a Cypress `.img` (`CY…`) is recognised and refused in words.
  A zip, a saved web page or the firmware under the FPGA's name are refused in words.
* After a change the radio process running the USRP reopens it at once; if the radio was not running (the clone that
  would not load), the front door saves the file and the radio's own restart picks it up within seconds.
* Endpoint: `/vibeserver/usrp-images` (admin): `GET` list (`?scan=1` adds this computer's UHD images), `POST
  ?op=put&name=…` (raw body), `?op=remove&name=…`, `?op=adopt&from=<index>&name=…` (`from` indexes the server's own scan).

**Words for the owner** (in the radio's status, shown on its Setup card): a failed FPGA load in the clone way
("fx3 is in state 5", an FPGA load timeout) → *"This board needs its maker's FPGA image — upload usrp_b210_fpga.bin in
Setup → this radio"* (the file follows the product); nothing answering, or a BUSY / claim failure → *"Another program
is using this USRP — stop it first (OpenWebRX+ / owrx_connector, a SoapyRemote server, GNU Radio or uhd_fft)"*.

## Plug and play — what ships and where

| | Linux (`.deb`) | Mac (`.app`) |
|---|---|---|
| driver | static in `/usr/bin/vibeserver` | static in `vibeserver-engine` and the app binary |
| images | `/usr/share/vibeserver/uhd-images/` | `Contents/Resources/uhd-images/` |
| owner override (a clone's own FPGA image — uploaded from the Setup page, or copied from this computer's UHD images) | `/etc/vibeserver/uhd-images/` | `~/Library/Application Support/VibeServer/uhd-images/` |
| licences | `/usr/share/doc/vibeserver/LICENSE.uhd`, `LICENSE.uhd-fpga`, `LICENSE_1_0.txt.boost`, `IMAGES.txt` | beside the images |
| USB access | `99-vibeserver-sdr.rules` (plugdev) | — |

The override directory is empty on every install. It is searched **per file**, before ours: an owner who drops a
third-party `usrp_b210_fpga.bin` there still gets our firmware. `VIBE_UHD_IMAGES_DIR` in the environment beats both
(tests, developers).

### Pins

| | Version | Source | Licence |
|---|---|---|---|
| UHD | 4.9.0.1 (tag → commit 9ec1f582) | GitHub tag tarball, SHA-256 `0be26a13…a7bc` | GPL-3.0-or-later |
| Boost | 1.83.0 | archives.boost.io, SHA-256 `6478edfe…3b8e` | BSL-1.0 |
| B2xx FPGA images | `uhd-c37b318` | files.ettus.com, per UHD 4.9.0.1 manifest | LGPL-3.0 (Ettus's `fpga/usrp3`) |
| B2xx FX3 firmware | `uhd-7f7d016` | files.ettus.com, per manifest | GPL-3.0-or-later (built with Cypress's FX3 SDK) |

★ UHD's banner at first use reads `UHD_4.9.0.0-0-unknown` — the v4.9.0.1 tag did not move UHD's own CMake version
numbers; the source IS the 4.9.0.1 tag (SHA-256 above). Not a mismatch to chase.

One edit to UHD's *build list*, none to its code: Boost.System is named in UHD's component list (the static library
links its CMake target; it is header-only since Boost 1.69). UHD's `FindLIBUSB` calls `pkg_check_modules`
unconditionally, so pkg-config is blinded rather than disabled. Both are in `vibeserver/linux/build-uhd.sh`.

The image files, as shipped (uncompressed):

| File | Size | Radio |
|---|---|---|
| `usrp_b200_fw.hex` | 514 346 | every B2xx (FX3 firmware) |
| `usrp_b200_fpga.bin` | 2 456 408 | B200 (Spartan-6 LX75) |
| `usrp_b210_fpga.bin` | 4 224 356 | B210 (Spartan-6 LX150) |
| `usrp_b200mini_fpga.bin` | 2 455 856 | B200mini |
| `usrp_b205mini_fpga.bin` | 4 223 252 | B205mini, B206mini |
| **total** | **13.9 MB** | (2.5 MB as Ettus's zips; the `.deb`'s xz does about the same) |

**Redistribution.** The FPGA images are built from Ettus's LGPL-3.0 `fpga/usrp3` tree and the FX3 firmware from
GPL-3.0-or-later sources with Cypress's FX3 SDK; Ettus publishes them for exactly this use, and we ship them unmodified
with their licence texts and the exact source tag (`IMAGES.txt`). The corresponding source is UHD's own repository at
`v4.9.0.1`. ★ The FX3 firmware contains Cypress SDK library code under Cypress's own licence, which permits
redistribution of firmware built with it for their parts; we redistribute Ettus's binary as Ettus does.

## AliExpress "B210" clones — what is plug and play

| Board | FPGA | Plug and play? |
|---|---|---|
| Full-size "B210" clones that copy Ettus's design (Spartan-6 **XC6SLX150**, AD9361, Cypress CYUSB3014) | same part as a real B210 | **Should be** — same FPGA part, same FX3, so our stock images are the right bitstream. Not confirmed by anyone here; this is what a tester's report settles |
| "B200mini"-style clones (Spartan-6 LX75, AD9364) | same part as a B200mini | **Should be**, on the same reasoning |
| **Kintex-7 "USRP_B210" clone** (a tester's, serial 30AA038) | 7-series, NOT the Spartan-6 | **Replace `usrp_b210_fpga.bin` only** (the seller's image, a 7-series bitstream; the FX3 firmware stays Ettus's — the seller's own instructions). Works with stock UHD: the tester runs it today under OpenWebRX+ with Debian's UHD. Upload it in Setup, or use "Use this computer's UHD images" |
| **LibreSDR "B210 mini" / "B220 mini"** (Artix-7 **XC7A100T** / **XC7A200T**, AD9361, FX3) — the common AliExpress/eBay "B210 mini" | Artix-7, NOT the Spartan-6 the stock image is built for | **No.** They enumerate as 2500:0020 and identify as a B210 (the stock firmware is fine), but the stock B210 FPGA image cannot load into an Artix-7 — UHD reports `fx3 is in state 5`. They need a third-party `usrp_b210_fpga.bin` built for their FPGA, which we do **not** ship (no stated licence, no build we can point to). The owner uploads it in Setup (or drops it into the override directory above), and VibeServer then uses it with our firmware. The XC7A100T and XC7A200T boards need different images |

So: genuine Ettus/NI B200, B210, B200mini, B205mini — plug and play. Spartan-6 copies — expected to be. Artix-7 /
Kintex-7 boards — one file from their seller, uploaded on the Setup page; nothing else to install.

When an image will not load, the error reaches the owner in words through the radio's status — for the clone
failure, which file to upload and where; otherwise UHD's own message, prefixed "USRP:" — never "download the images".

## CPU and USB

* The B2xx's master clock runs 220 kHz – 61.44 MHz (UHD's AD9361 limits); UHD picks it to suit the rate, and the FPGA
  decimates, so any rate in our list is native — no host-side resampling to reach it.
* USB 2 carries 53 MB/s by UHD's figure (≈13 MS/s at 4 bytes per complex sample, overflows well before that on a
  shared bus) — hence the 8 MS/s cap. USB 3 carries 500 MB/s; our 16 MS/s cap is the host's limit, not the link's.
* Our DSP costs scale with the rate. On a Pi 4 / Pi 5 the default is 2.4 MS/s (what every other radio here is tuned
  around); x86-64 and Apple silicon start at 4 MS/s. **Not measured on a B2xx** — the per-rate CPU is to be measured
  on a Pi 4 / Pi 5 by the first tester, and the arm64 default set from that.
* Overflows are counted (`usbDrops` in the status probe). Rising overflows at a given rate = lower the rate, or move to
  a USB 3 port that is not shared with a disk.

## Sizes

| | Without UHD | With UHD | Growth |
|---|---|---|---|
| Mac arm64 `vibeserver` (stripped) | 14.9 MB | 19.7 MB | **+4.7 MB** (two copies in the `.app` — the engine and the app binary — so ≈ +9.5 MB, plus 13.9 MB of images) |
| Linux arm64 `vibeserver` (stripped) | 19.6 MB | 25.9 MB | **+6.2 MB** |
| Linux arm64 `.deb` (gzip data) | — | 66.4 MB | **≈ +4–5 MB** (images 2.0 MB as xz / ~2.5 MB gzip, the binary ≈ +1.5–2 MB compressed) |
| Linux amd64 (cross-built `test-usrp-caps`, stripped, static libstdc++) | 0.25 MB (stub) | 8.2 MB | **≈ +6–8 MB** expected on the server binary (not yet measured on a real amd64 package) |
| `libuhd.a` (archive, unstripped) | | 21.6 MB (Mac) / 36.2 MB (arm64) / 35.3 MB (amd64) | only the B2xx driver is kept in the link |

Runtime RAM was not measured (no radio); UHD's B2xx transport allocates its USB frame buffers at open (a few MB).

Build time: Boost + UHD add about two minutes to the build image's deps layer on the M5 Pro (8 jobs), once per image.

## Code map

| File | What |
|---|---|
| `vibeserver/linux/build-uhd.sh` | the one builder for Linux and the Mac: Boost subset, UHD (B2xx only, static), images pinned to the manifest |
| `vibeserver/linux/build-deps.sh`, `mac/build-deps.sh` | run it (amd64 / arm64 / Mac only) |
| `vibeserver/linux/Dockerfile.build`, `Dockerfile.cross-arm64` | `python3-mako` + `unzip` (host tools for UHD's configure and the image zips) |
| `vibeserver/CMakeLists.txt` | UHD block: host-class gate, whole-archive link, images + licences installed, strict on release builds |
| `vibeserver/mac/build-app.sh` | the Swift app's link line (`VIBE_UHD_LINK`) and `Resources/uhd-images` |
| `android/app/src/main/cpp/usrp_source.{h,cpp}` | the source (UHD as a library; the header's inline stub elsewhere) |
| `android/app/src/main/cpp/vibe_usrp_caps.h` | every decision: ids, rates, default, gain list, ports + labels, image resolution, the owner options (what is offered, what is refused), image names / sizes / the bitstream classifier, the clone and busy wording, the hwinfo shape |
| `android/app/src/main/cpp/vibe_usrp_images.h` | the owner's image folder: atomic write, list, remove; this computer's UHD images (scan, compare, adopt) |
| `android/app/src/main/cpp/local_sdr_shim.cpp` | `startUsrp` and every "name every source" site |
| `vibeserver/radios.cpp`, `main.cpp`, `vibeserver_api.cpp`, `sdr_presence.h` | the radio list, start by serial / index / discovery, the Mac Simple-mode list, bus + claimed counts |
| `vibe_setup_page.h`, `LocalHardwarePanel.tsx`, `web/client/src/main.ts` | Setup page, app panel, web client — all branch on `driver === "usrp"` |
| `vibeserver/test-usrp-caps.cpp` | the test (run-tests.sh header-only against the stub; CMake target with UHD) |
| `scripts/check-usrp-options.mjs` | the Setup card, app panel and web client drawn from fake hwinfo |

## Android (later) — research only

Linux and the Mac remain the build target. This is what an Android version would take, framed (Stuart) around the
realistic host: **a docked USB 3 flagship running VibeServer Lite** — a Samsung on DeX, a recent Pixel with USB 3 and
display out — with a powered dock solving the radio's power.

**1. UHD enumerates devices itself.** UHD's USB transport (`host/lib/transport/libusb1_base.cpp`,
`libusb1_device_handle`) builds its device list with `libusb_get_device_list` and opens by `libusb_open`. On a
non-rooted phone that finds nothing: an app may not walk `/dev/bus/usb`; it is handed one already-open descriptor by
`UsbManager.openDevice()`. Our Android radios all take that route — Kotlin asks permission and opens the device, the
native side wraps a `dup()` of the fd with `libusb_wrap_sys_device` (`hackrf_source.cpp`, `airspy_source.cpp`,
`airspyhf_source.cpp`; libusb is initialised with `LIBUSB_OPTION_NO_DEVICE_DISCOVERY`). For UHD that means a patch:
a `usb_device_handle` that wraps a given fd instead of coming from the device list, a `get_device_list()` that returns
just that one, and `b200_find()` / `b200_impl` taught to take it (they re-run the device list after the firmware load).
Contained — perhaps 200–300 lines in two UHD files — but it is a fork of UHD's transport we would carry from release
to release, which the desktop does not need.

**2. The firmware load re-enumerates the radio.** A cold B2xx comes up on its FX3 bootloader; UHD uploads
`usrp_b200_fw.hex` and the device **disconnects and reconnects** (same 2500:00xx id). On Android that is a new
`UsbDevice`: the fd we hold dies, and permission is per device instance — so it is a **second permission prompt** unless
the app is the default handler for that id (`device_filter.xml` + the user ticking "always open with VibeSDR Lite" on
the first one, after which the re-attach is granted through the attach intent with no dialogue). The flow has to
expect the disconnect: load firmware on fd #1, wait for `ACTION_USB_DEVICE_ATTACHED`, take fd #2, then load the FPGA.

**3. Power.** UHD's manual: USB 3 bus power is enough "in most cases"; Ettus recommend an external supply for a B210
using both channels, and on USB 2. A phone's OTG port is not a USB 3 host port's 900 mA in practice, and a B210 at full
tilt is more than most phones will give. **A powered USB 3 hub or dock is the minimum** — which the docked-flagship
setup already has.

**4. USB 3 on the phone.** Many current flagships have USB 3 on the port (Samsung S-series with DeX, Pixel Pro models);
plenty of mid-range phones are USB 2 only, and some docks are USB 2 internally. The app must **detect a USB 2 link and
say so plainly instead of failing**: the FX3 reports the negotiated speed (UHD's `get_usb_speed`, the same figure our
desktop source publishes as `usb`), and libusb's `libusb_get_device_speed` works on a wrapped fd. On USB 2 the honest
behaviour is the desktop's — run, cap at 8 MS/s, and show "This port is USB 2: a USRP needs a USB 3 port or dock for
its full rates (and may need its own power)".

**5. APK size.** arm64-v8a only (no 32-bit): about +4.5–5 MB of stripped native code, plus 13.9 MB of images (≈2.5 MB
compressed in the APK; they must be extracted to app storage once, because UHD loads them by path). **≈ +7–8 MB
download.** That is too much to put in every phone's VibeSDR app for a radio almost nobody has.

**Minimum:** arm64, a USB 3 SuperSpeed port, a powered USB 3 dock/hub, Android 10+.

**Recommendation:** if it is done at all, **VibeServer Lite only, arm64 only**, behind the docked-flagship framing —
with the images either as a Play asset pack / on-demand download delivered by us (still "nothing the owner installs
by hand", and still no runtime fetch from Ettus) or simply in the Lite APK if +8 MB is acceptable there. Not in the
main VibeSDR app. Do it after the desktop driver has met a real radio, so the Android work starts from a driver that
is known to be right.

## What a tester must check (a volunteer with a B210 or B200mini)

Linux amd64 or arm64 (Pi 4 / Pi 5) with the `v12-usrp` beta package, or the Mac app. **USB 3 port, radio's own
power supply if it has one.**

1. **Nothing to install.** Fresh machine, no UHD on it. `vibeserver --list-radios` (Linux) or the Mac app's radio list
   shows "Ettus USRP B200/B210 (serial)". Report the line.
2. **Firmware + FPGA load from our images.** Start the radio. The log says `USRP B210 (serial) open — USB 3, 2.400 MS/s,
   RX2, 2 channel(s), images from /usr/share/vibeserver/uhd-images` (Mac: `…/Resources/uhd-images`). Report the whole
   line, and any UHD warning above it. Unplug + replug, start again (firmware already loaded path).
3. **A clone?** Say which board (photo of the FPGA marking). If it fails with `fx3 is in state 5`, it is an Artix-7
   LibreSDR-type board: put its seller's `usrp_b210_fpga.bin` in `/etc/vibeserver/uhd-images/` and try again.
4. **Tuning:** a strong FM broadcast station, a 2 m / 70 cm repeater, airband, 1090 MHz ADS-B. Frequency right to
   within a few hundred Hz? Any DC spike **inside** the audio channel (it should sit 250 kHz away)?
5. **Gain:** slider 0 → 76 dB moves the noise floor smoothly; the AGC switch (app) turns the AD9361 AGC on and the
   slider takes it back. Per-band ceiling and lock in the Setup page hold.
6. **RX ports:** switch RX2 ↔ TX/RX with an aerial on one only — the signal follows. On a B210, `RX2 (B)` with the
   aerial on channel B. Confirm **nothing transmits** (a nearby receiver hears nothing when TX/RX is chosen).
7. **Rates:** each offered rate; watch `usbDrops` in the status probe (`curl -s localhost:<port>/vibeserver/status`)
   and the audio. Report the highest rate that holds with no drops, and the CPU (`top`) at 2.4 / 4 / 8 / 16 MS/s —
   on a Pi this sets the arm64 default.
8. **USB 2:** the same radio on a USB 2 port: the log line says USB 2, the list stops at 8 MS/s, the app's note says so.
9. **Lend and take back** ("release when idle"), **unplug while running** (it should say so, then recover on replug),
   **DAB** (2.048 MS/s) and **ADS-B** (rate boost) on it.
10. **Setup page:** the USRP's tab shows the experimental note, starting gain / ceilings / lock, and the aerial card
    with its ports once running; no bias-T, no direct sampling, no NDB skimmer.
11. **Radio options (Setup, radio running):** report which appear. A B210 with no GPSDO: Reference clock offers
    Internal / External only — **no GPSDO**; with one fitted, GPSDO appears with its lock state. Feed 10 MHz into REF,
    choose External, save: "Now: External … locked" (and "NOT locked" with the cable pulled). Untick DC-offset
    correction: the DC spike at the centre of the waterfall appears; tick it: gone. Untick IQ balance: mirror images
    of strong signals rise. Analogue bandwidth: AUTO, then 1 MHz at 4 MS/s — the waterfall's edges dim; back to AUTO.
    Restart the server: every choice is still there.
12. **Radio AGC (web client):** the radio menu shows **RADIO AGC** (not on any other radio); ON = the radio sets its
    own gain, the gain slider takes it back by hand. Same as the app's switch.
13. **A clone's own FPGA image:** with the stock image the clone should fail with "This board needs its maker's FPGA
    image — upload usrp_b210_fpga.bin in Setup → this radio". Either upload the seller's `usrp_b210_fpga.bin` (it is
    listed as "7-series (Kintex/Artix) image — for clone boards"), or press **Use this computer's UHD images…** and pick
    the one OpenWebRX+ uses (it shows "differs from VibeServer's"). The radio opens within seconds. Remove it: back to
    our image (and the clone failure). Try uploading a zip or the wrong file: refused in words, nothing changed.
14. **Another program holding it:** start OpenWebRX+ (or `uhd_fft`) on the radio first, then VibeServer: its status
    says "Another program is using this USRP — stop it first …". Stop OpenWebRX+: VibeServer takes it.

Report: the log from start to first audio (`journalctl -u 'vibeserver@*' -b`), and `/vibeserver/hardware` for the radio.
