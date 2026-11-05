# R36S handheld receiver image — research (2026-11-04)

**Status:** research (2026-11-04) + DAY ONE ON THE DEVICE (2026-11-05, §0) + the product spec (§0.1). Build next.
**Goal:** an SD card for the R36S (RK3326) that boots straight into the VibeSDR web client in a full-screen
webview drawn on the GPU, with `vibeserver` running locally on `127.0.0.1:48000` and the radio (RTL-SDR V4,
Airspy HF+ or Airspy Mini) on the USB-C OTG port.
**The question that decides the project:** can a browser engine draw our waterfall on the Mali-G31 on this device?

Labels used below: **(checked)** means I read it in the source, a package index or the binary myself, today.
**(source)** means a project page or forum says it and I could not check it further. **(unverified)** means it
is my reading or estimate. Every item in "Only the device can tell us" is in that last group until the unit says otherwise.


> **2026-11-05: THE DEVICE ARRIVED AND DAY ONE IS DONE — see §0 (results) and §0.1 (the product, Stuart's spec).
> The web client is the UI: on Panfrost the live waterfall costs half a core. Everything below §0.1 is the original
> research; where §0 contradicts it, §0 is what the hardware said.**

## 0. Day one on Stuart's unit (2026-11-05) — what the hardware said

**The board.** An **EE clone** (K36-type), not a genuine R36S: the stock card ran ArkOS on Rockchip's 4.4 BSP kernel with
`rf3536k3ka.dtb` (`rockchip,rk3326-evb-lp3-v12-linux`), joypad driver **`play_joystick`**. Stock card backed up whole
(`~/R36S-stock-card.img`, 15 938 355 200 bytes; copies of the DTB and extlinux in `~/VibeSDR-test-audio/r36s-stock/`).
**The card:** 16 GB sold as 64 GB — the controller reports 16 GB honestly; the stock GPT fits it (15.73 GB). Read 24 MB/s,
write ~8.5 MB/s.

| | Measured | For the image |
|---|---|---|
| ROCKNIX 20261101 **B** image (EE clones) | boots, mainline kernel **7.1.2** | use ROCKNIX's U-Boot + KERNEL + DTBs |
| Panel | **Sitronix ST7703** MIPI-DSI 4-lane, 640×480, 30 MHz → **80.046 Hz** default; modes to 120 Hz work | display **60 Hz**; our drawing capped at **30 fps** (Stuart) |
| Display overlay | works ONLY with ROCKNIX's own generated `mipi-panel.dtbo` — stock DTB in `/flash/stock`, **no** overlay pre-placed (`002-generate_dtbo` is a no-op if one exists; a panel-only `importpanel.py` overlay broke the EE-clone audio tuning) | ship the overlay ROCKNIX generated for this board, or let first boot generate it from `stock/` |
| Audio | card `rk817_ext`; amp `simple-audio-amplifier` (enable high); **silent until `Playback Mux` (numid=6) → SPK** | mux follows the `Headphones Jack` control: **SPK default, HP when a plug is in** |
| Backlight | `/sys/class/backlight/backlight`, PWM 25 µs, 0–255, stock default 200 | slider 0–255 (floor above 0); **default 120** |
| CPU | RK3326 4×A35; ROCKNIX OPPs **1008/1296 only**; stock table 408–1296 (1416 disabled; 1512 only at 1.35 V = overclock) | **schedutil** (Stuart) + the stock **408–1296** steps |
| RAM | 747 MB (730 MB to Linux) | fine: Cog ≈ 340 MB + engine |
| GPU | Mali-G31; ROCKNIX loads ARM's `mali_kbase`; **`panfrost.ko` is built** | **Panfrost + Mesa 25.0.7** (proven below) |
| Joypad | `r36s_Gamepad` (event3), udev tags it **ID_INPUT_TABLET_PAD** → libmanette skips it | udev rule clearing `ID_INPUT_TABLET{,_PAD}`; Cog `--gamepad=manette` |
| Wi-Fi | **none** (only EVB leftovers in the DTB) | none needed |
| USB device mode | **none** on either port (Vol- disk mode, C–C and A–C cables) — OTG is host-only, as the radio needs | files + updates by **card swap** |
| USB host (OTG) | a USB-C Ethernet adapter works (SSH for development) | radio on OTG |

**The browser test** (Debian trixie container in `/storage/vibetest`, entered with `unshare -m` + `pivot_root` so WebKit's
bubblewrap sandbox stays ON; Cog 0.18.4 / WPE 2.48.3 on the live waterfall from a VibeServer):

| | Software GL (Mesa, no GPU driver) | **Panfrost** (`rmmod mali_kbase; modprobe panfrost`, `cog -P drm --platform-params=renderer=gles`) |
|---|---|---|
| WPEWebProcess | 234 % (2.3 cores) | **54.5 %** |
| cog | ~7 % | 6.8 % |
| whole device idle | 30 % | **75 %** |
| web process RSS | ~390 MB | ~340 MB |
DRM fdinfo confirmed Panfrost buffers in the web process. At full rate; the 30 fps cap lowers it further.
Gotchas found: Sway tiled Cog beside EmulationStation (fullscreen it, or no compositor — Cog on DRM); the renderer crashed
until `gstreamer1.0-plugins-base/-good` were present (appsink/appsrc/autoaudiosink); the **landing page's buttons do not
take gamepad navigation** (client fix; the handheld skips the landing page anyway).

## 0.1 The product (Stuart, 2026-11-05) — "a proper portable radio just powered by VibeSDR"

**Power on.** A VibeSDR splash from the first instant (the bootloader's logo, then an early splash with a **progress
bar**, then the page's own matching screen while the engine starts) — never a ROCKNIX logo or Linux text. **No "start
audio" splash**: autoplay is allowed, the receiver opens straight into the waterfall.

**Radios (hot-swap).** No radio → full screen **"Please connect an SDR"**. Plug one in → the waterfall starts and the menu
offers only what that radio can do (`vibe_radio_caps.h`: no DAB / ADS-B on an Airspy HF+). Unplug during use → its settings
are saved, the screen goes back to "Please connect an SDR". **Every radio has its own memory, by serial** (frequency/station,
mode, gain, decoders, view), **saved every ~30 s in use** and on unplug, power-off and low battery — so a pulled radio, a
switch-off or a flat battery resumes exactly where it was. **Hold Start + Select while powering on** = that radio's settings
and last station back to defaults (the splash says so).

**Audio.** Speaker by default, headphones when a plug is in (automatic, both ways). The volume buttons drive the **master**
volume with a VibeSDR volume overlay — a system service, so it works on every screen including "Please connect an SDR".

**Power.** **Short press of the power button = screen off, audio keeps playing**; long press = clean power-off (settings
saved, card flushed). **Battery: warning at 10 %, clean automatic shutdown at 3 %** (settings saved first) — a flat
battery must never corrupt the card.

**The menu's "This device" section.** Brightness (default **120/255**), volume, battery and charging, the clock (manual UTC
set + 0.1 s nudge; set from a decoded MSF/DCF77/WWV(B)/JJY minute — two consecutive minutes must agree; align to FT8 DT),
the card's space, power off / restart.

**The handheld main menu (Stuart 2026-11-05 — supersedes a "This device" section).** Three buttons, **R36S HARDWARE**,
**SDR HARDWARE** and **DISPLAY**. Pressing one expands its options **in place, directly under the button** (an accordion — not a new page,
not a nested sub-menu); pressing it again, or B / back, collapses them into the button. One level only: "no multilayered
menu but the options are still hidden", so the main menu stays uncluttered. Opening one collapses the others.
- **R36S HARDWARE** = volume, battery, the clock tools, card space, IMPORT, EXPORT DIAGNOSTICS, the
  **Performance** overlay toggle with its figures in full, **Developer: SSH**, power off, restart.
- **SDR HARDWARE** = the plugged-in radio's own controls (gain / AGC / bias-T / direct sampling / ppm / antenna …) from its
  caps — what the radio menu shows today for that driver.
- **DISPLAY** = the client's existing look options (control look, transparency, waterfall palette / contrast, display
  font …) plus the handheld's own: **brightness** (moved here from R36S HARDWARE), the drawing frame rate (**30 fps**
  default / 60), and an auto-dim / screen-off-after timeout (**off** by default; the screen goes off through the
  service's `screen-off` action, audio keeps playing, any button wakes it).
- Gamepad: the D-pad moves through the buttons and the expanded options; **A** expands / activates; **B** collapses.

  The DISPLAY accordion also carries the **waterfall rate: 5 / 10 / 20 real frames a second** (below).

**No server furniture on the handheld (Stuart 2026-11-05).** The engine is local, so everything about a SERVER goes from
the handheld UI: the data-rate / link meter, KB/s, ping, buffer readouts, bandwidth or compression settings, the
uncompressed-audio option, listener counts, reconnect banners about the server, admin, chat, the landing page. Anything
about the RADIO or the DSP stays. (AGENTS.md: no dead controls.)

**Waterfall and spectrum pacing (Stuart 2026-11-05).** The waterfall runs at **5, 10 or 20 REAL frames a second** —
every row is a real FFT row from the engine, **no interpolated / synthesised waterfall rows** on the handheld. The
**spectrum trace still interpolates** (a smooth trace between real frames), and frames keep the existing **fixed output
clock / even pacing** — rows arrive evenly, never in bursts, no refresh holds.

**Performance overlay (Stuart 2026-11-05).** No admin window on the handheld; instead the Performance row's toggle shows a
SteamOS-style strip at a screen edge: CPU % (box), CPU temperature, battery % + time-remaining estimate, RAM used, client
fps, the engine's DSP load (the figures the admin health cards already get). Plain DOM text, updated ≤ 2×/s, no canvas;
remembered across radios.

**The card.** exFAT data partition **`VIBESDR`**, grown to the KERNEL-reported size on first boot after a one-time
capacity check: `Recordings/`, `Gallery/<DECODER>/` (SSTV, WEFAX, RTTY, NAVTEX, FT8, …), `import/` (bookmark backups,
imported at start and from an IMPORT key, moved to `import/done/` with a result `.txt`), `export/`, **`Diagnostic logs/`** (EXPORT DIAGNOSTICS on the handheld writes the same scrubbed vibeserver-diagnostics file there, named by UTC time — Stuart 2026-11-05), `README.txt`. Names
"YYYY-MM-DD HHMM - <kHz> kHz <MODE>.ext" in **UTC**, FAT-safe. **Every write is a temp file + rename**; fsck at boot; mount
`flush`. Audio recordings: the page hands the WAV to the local engine (Cog has no downloads).

**The engine: a "handheld" profile** of the same vibeserver: 127.0.0.1 only, no tunnel / listing / mDNS / admin / limits /
chat / shared tuner / background decoding / DX scanner; every hardware control unlocked; the FULL decoder set the radio can
run (IQ ones too — DAB, ISM, ADS-B). arm64 build (the Pi 500's package).

**ONE IMAGE, ONE CARD WRITE (Stuart 2026-11-05):** everything below ships together. Test what runs after Linux is up on the device over SSH (the /storage/vibetest container, Panfrost) BEFORE the image exists; only the pre-Linux parts (U-Boot logo, splash/progress, Start+Select, first-boot partitioning) wait for the one write.

**Build stages (all in the one image):** (1) bootable image: splash → waterfall on a plugged-in radio, with the hardware fixes above; (2) the
handheld profile in the engine incl. hot-swap and per-radio memory; (3) the `VIBESDR` partition and card rules; (4) the
"This device" controls, power button, battery and clock.

## 0.2 The interface between the engine and the hardware service (2026-11-05)

Two halves meet here: **`vibe-hwd`** (the image's root hardware service: backlight, ALSA master volume, the
speaker/headphone mux, battery, the power button, the clock) and **`vibeserver --handheld`** (the engine, not root).
The engine is the client; the web page never talks to the service directly — the engine proxies "This device" and relays
the service's events to the page as a `device` message on the spectrum socket.

**Transport.** Unix socket **`/run/vibe-hwd.sock`** (path overridable by env **`VIBE_HWD_SOCK`**), newline-delimited JSON,
one request per line, **one reply line per request**, in order. After the engine sends `{"subscribe":true}` the same
connection also carries **unsolicited event lines** (they have an `"event"` key; replies never do).

**Requests → replies.**

| Request | Reply |
|---|---|
| `{"get":"state"}` | `{"ok":true,"brightness":120,"maxBrightness":255,"volume":60,"muted":false,"output":"speaker"\|"headphones","battery":{"pct":83,"charging":false,"status":"Discharging"},"screen":"on"\|"off"}` |
| `{"set":"brightness","value":N}` | `{"ok":true}` or `{"ok":false,"error":"…"}` |
| `{"set":"volume","value":0..100}` | same |
| `{"set":"time","utcMs":1793900000000}` | same (the service is root; the engine is not) |
| `{"action":"poweroff"\|"reboot"\|"screen-off"\|"screen-on"}` | same |
| `{"subscribe":true}` | `{"ok":true}`, then events on this connection |
| `{"get":"ssh"}` | `{"ok":true,"ssh":false,"passwordSet":false}` |
| `{"set":"ssh","on":true\|false}` | `{"ok":true}`; switching ON with no password set → `{"ok":false,"error":"no password set"}` |
| `{"set":"ssh-password","password":"…"}` | `{"ok":true}`; under 8 characters → `{"ok":false,"error":"password too short"}` |

**SSH (Stuart 2026-11-05): OFF on a fresh card**, no default password. "Developer: SSH" in "This device" first asks
the user to set a password, then switches it on. The password is **never logged, never echoed back and never stored by the
engine** — the engine relays the one request line and forgets it; only the service (root) keeps the hash.

**Events** (after `subscribe`):

- `{"event":"volume","volume":N,"muted":B}` — the volume buttons; the page shows its volume overlay.
- `{"event":"output","output":"headphones"}` — or `"speaker"`.
- `{"event":"battery","pct":N,"charging":B}`
- `{"event":"battery_low","pct":10}` — the page shows the 10 % banner.
- `{"event":"shutdown","reason":"battery"|"power-button"|"request","inSec":5}` — the engine MUST save every radio's memory
  and flush, then send **`{"ack":"shutdown"}`** on the same connection. The service waits **≤ 5 s** for the ack, then shuts
  down anyway.
- `{"event":"screen","screen":"off"}` — or `"on"`.

**Files the two halves share.**
- `/run/vibe-handheld/reset-radio` — written by the boot check when Start + Select are held (absent = normal). The engine
  reads it once at start: the radio that connects next loses its memory (back to defaults) and the page shows a one-time
  "Radio settings reset" toast. The engine deletes the flag when it has acted on it (it needs write access to
  `/run/vibe-handheld/`; if it cannot delete it, it remembers in memory for that run only).
- `VIBESERVER_DATA_DIR/handheld/radio-<serial>.json` — the engine's per-radio memory (private to the engine).
- `VIBESERVER_CARD_DIR` (the exFAT `VIBESDR` mount) — `Recordings/`, `Gallery/<DECODER>/`, `import/`, `import/done/`,
  `export/`, `Diagnostic logs/`. The engine creates the folders it writes on first use; every write is temp + rename.

## 0.3 Starting the engine, and what the page asks of Cog (2026-11-05)

**The engine** (`vibeserver/handheld.cpp`, the supervisor; the radio process is the same binary):

```
User=vibeserver   (NOT root — the hardware service is the only root part)
Environment=VIBESERVER_DATA_DIR=/var/lib/vibeserver        # per-radio memory in handheld/ (ext4, not the card)
Environment=VIBESERVER_CARD_DIR=/media/VIBESDR             # the exFAT card; unset = no card (no recordings, no gallery files)
Environment=VIBE_HWD_SOCK=/run/vibe-hwd.sock               # the default; only needed to move it
ExecStart=/usr/bin/vibeserver --handheld                   # or "profile":"handheld" in VIBESERVER_CONFIG's file
```

- It listens on **127.0.0.1:48000** (`--port N` moves it); the radio process takes the next port on loopback and is reached
  through a hand-off socket in `$VIBESERVER_DATA_DIR/handheld/run/`.
- The user needs: write to `$VIBESERVER_DATA_DIR` and the card mount, USB access to the radios (the package's udev rules,
  plugdev), connect to `/run/vibe-hwd.sock`, and write to `/run/vibe-handheld/` (to remove the Start+Select flag — if it
  cannot, it says so and acts on the flag for that run only). `VIBE_HANDHELD_RESET_FLAG` moves the flag (tests).
- **No root, no maintenance helper:** EXPORT DIAGNOSTICS runs `/usr/lib/vibeserver/vibeserver-diagnostics` as this user
  (python3 needed; what it cannot read it says so) — `VIBESERVER_DIAG_SCRIPT` moves it.
- SIGTERM = save every radio's memory, stop the radio, exit 0. The service's `shutdown` event is ACKed after the memory is
  on disk and the data dir + card are `syncfs`'d (19 ms in the test); the radio process is stopped after the ack.
- The radio process logs "handheld profile" at start; the supervisor logs every plug, unplug, save and reset.

**Cog:** `cog -P drm --platform-params=renderer=gles --gamepad=manette 'http://127.0.0.1:48000/?handheld=1'`, with
autoplay allowed (WPE's default policy allows it; the page needs no gesture). `?handheld` alone does nothing — the page
asks `/handheld/session`, which only the handheld engine answers. Cog needs no downloads, no file picker, no storage
quota beyond localStorage (the look and the DISPLAY choices; everything per radio is the engine's).
- `&wf20=interp` = the measured fallback for SPECTRUM RATE 20 (10 real frames + repeated rows) — **default is 20 real**;
  only switch it after measuring on the device.

**The page's routes on the engine** (`/handheld/*`, never handed to the radio): `session`, `memory`, `events` (long-poll),
`device` (GET state; POST one of: brightness 1–255, volume 0–100, ssh on/off, ssh-password ≥ 8, poweroff / reboot /
screen-off / screen-on), `clock` (`utcMs` or `deltaMs`), `perf`, `card`, `recordings[/<name>]` (POST a WAV, GET with
Range, DELETE), `import` + `import/result`, `export?kind=bookmarks|dxlog`, `diagnostics`. Device events reach the page
through `events`, not the spectrum socket — the page must hear the volume buttons and the battery on the "Please connect
an SDR" screen, where there is no spectrum socket.

---

## 1. The short answer

**Recommended stack:**

| Layer | Choice |
|---|---|
| Base OS | **Debian 13 (trixie) arm64**, built with debootstrap in Docker on the Mac (native arm64, no emulation) |
| Kernel, device trees, panel driver, joypad driver | **ROCKNIX's RK3326 kernel**: mainline Linux 7.1.2 plus ROCKNIX's RK3326 patches, its `generic-dsi` panel driver and its R36S/clone DTS files |
| Bootloader | **ROCKNIX's RK3326 U-Boot**: mainline U-Boot v2025.10 (`rk3326-handheld_defconfig`) packed with Rockchip's rkbin DDR/miniloader/BL31 blobs |
| GPU driver | **Mesa Panfrost** from trixie (Mesa 25.0.7): EGL + GBM + GLES 3.1 on the DRM device |
| Browser | **WPE WebKit 2.48.3 + Cog 0.18.4** from trixie, Cog's **DRM platform** with `renderer=gles` — no X, no Wayland |
| Our side | the `vibeserver` arm64 `.deb` plus the Portable's boot machinery from `image/pocket` and `image/sbc` |

**Why trixie and not bookworm (the brief said Debian 12).** Bookworm's WPE is **2.38.6** and its Mesa is **22.3.6**
(checked: `apt-cache policy` in `debian:bookworm` arm64). 2.38 predates WebKit's switch to Skia (2.46), the release that
made the **2D canvas GPU-accelerated by default** (source: WPE 2.46 release notes). Our waterfall draws on WebGL and is
then copied onto a 2D canvas every frame (section 2), so the accelerated 2D canvas is the part that matters most.
bookworm-backports has a newer Mesa (25.0.7) but no newer WPE. Trixie has WPE **2.48.3**, Cog **0.18.4** and Mesa **25.0.7** (checked).
Our `.deb` keeps a glibc floor of 2.35 (`vibeserver/linux/glibc_floor.c`, enforced by `check-deb-depends.cmake`). Trixie
ships glibc 2.41, so the package installs there unchanged. The Armbian Portable cards in `image/sbc/` are already built
on Trixie with the same package. dArkOS's own Makefile now defaults to `DEBIAN_CODE_NAME ?= trixie` (checked). Our earlier
memory note said dArkOS is "Debian 12 base", which is no longer true.

**Runner-up: dArkOS / ArkOS's stack (Rockchip BSP 4.4 kernel + the libmali blob).** It is not the first choice for four reasons:
- It ships a single libmali build for the G31, `libmali-bifrost-g31-rxp0-gbm.so` (checked: `utils.sh` in
  christianhaitian/dArkOS). That is a **GBM-only** build of an old Rockchip driver drop, and every GL, EGL, GBM and
  wayland-egl name is symlinked to it.
- WPE moves frames between its web process and UI process as dma-bufs through EGL. Whether that old blob has the EGL
  extensions WPE needs is **unverified**. WPE's own hardware table lists Rockchip only with Panfrost on the RK3399 (source).
- The kernel is 4.4, a branch of christianhaitian/linux (`odroidgoA-4.4.y` / `rg351`). Debian 13's systemd and Mesa
  expect a newer kernel. Building WPE against the blob instead of Mesa would also mean fighting Debian's packaging
  (`libgbm1`, `libegl1`).
- Its real strength is panel coverage on clones: years of community DTBs.

The **fallback inside the recommended stack** is better than switching distro. ROCKNIX also ships a libmali (g29p1, via
the `mali-bifrost` kbase driver) on the same mainline kernel: `GRAPHIC_DRIVERS="mali panfrost"` in
`projects/ROCKNIX/devices/RK3326/options` (checked). If Panfrost fails on the WebGL path, that libmali on the same kernel
is the next thing to try.

**Verdict on the GPU path:** **probably yes, and the risk is in specific places.**
- Panfrost on the Mali-G31 is a mature, upstream GLES 3.1 driver (Mesa's own table, checked).
- ROCKNIX ships it as an option on exactly this SoC (checked).
- WPE 2.48 contains WebGL 1/2 and Skia GPU canvas code (checked in the binary).

Nobody has published "Cog DRM + Panfrost + G31" working. WPE's support table lists Panfrost only on RK3399 and only with
Cog's Wayland platform, not DRM (source). So three things stay open until the unit arrives:
- whether Cog's DRM platform behaves on the RK3326 display controller;
- whether the WebGL-to-2D-canvas copy stays on the GPU;
- whether the frame rate is good enough.

Section 8 is a one-hour test that settles all three.

---

## 2. What our web client asks of the browser (from this repository)

- **The waterfall is WebGL 1**, not 2D canvas. `web/client/src/wfgl.ts` asks for `getContext('webgl')` (falling back to
  `experimental-webgl`). It draws a ring texture and a palette LUT through a fragment shader. If no context can be made,
  `waterfall.ts` (constructor, around line 329) **silently falls back to the 2D `putImageData` path**. That fallback runs on the CPU.
  On four A35 cores it will work, but it is the slow path. On a device where WebGL is missing, it would look like a
  "slow waterfall" rather than an error. ★ Day one must confirm which path is running (section 8, test C3).
- **The GL output is copied onto a 2D canvas every frame**: `ctx.drawImage(this.glCanvas, …)` in `waterfall.ts` (around
  line 1124). The spectrum trace, markers and axes are ordinary 2D canvas drawing on the same visible canvas.
  - With an **accelerated** 2D canvas (WebKit 2.46+ with Skia GPU), that copy is a texture draw on the GPU.
  - With an **unaccelerated** canvas, it becomes a read-back from the GPU into CPU memory every frame. That is a known cost on Mali.

  So both the **WebGL** path and the **accelerated 2D canvas** path matter, and bookworm's 2.38 has only the first.
  If the copy turns out to be the bottleneck, a client-side option is to show the GL canvas as its own layer under a
  transparent 2D overlay. That is a client change for later, not needed to start.
- **2D canvas effects:** `shadowBlur` halos (peak hold, sideband edges, needle layers) are costly on any CPU-painted canvas.
  The `#noglow` measurement switch in `waterfall.ts` turns them off for an A/B test. `#nowf` and `#noaudio` (main.ts) isolate the other costs.
- **Audio:** an `AudioContext` at 48 kHz with an **AudioWorklet** sink (`audio.ts`), Opus decoded either by WebCodecs
  `AudioDecoder` or by the WASM `opus-decoder` in a Worker. WebCodecs is `[SecureContext]`, and `http://127.0.0.1` counts
  as a secure context, so WPE's WebCodecs may be used. The WPE 2.48 binary contains `WebCodecsAudioDecoder` (checked).
  Whether its Opus decoding works through GStreamer here is **unverified**. Either path works. In WebKit, WebAudio output
  goes through GStreamer, so the image needs `gstreamer1.0-alsa` and the base and good plugin sets.
- **Gamepad:** `web/client/src/gamepad.ts` already names "an Anbernic / R36S-type console running the page in a
  full-screen shell". It uses the standard Gamepad API and its standard button layout, covering pads with and without
  sticks. The R36S has two sticks, so the sticks layout applies; HOLD START switches between layouts.

---

## 3. GPU path — decision table

| | **Mainline + Mesa Panfrost** (recommended) | **Mainline + libmali g29p1** (ROCKNIX's other option) | **BSP 4.4 + libmali rxp0-gbm** (dArkOS / ArkOS) |
|---|---|---|---|
| Kernel | ROCKNIX 7.1.2 + RK3326 patches (checked) | same kernel, plus the out-of-tree `mali-bifrost` kbase module | christianhaitian/linux 4.4 (checked) |
| GLES | 3.1, Mesa table; the conformance footnote on that row is the G52's submission, not the G31's (checked) | 3.2 (Arm blob) | 3.2 (Arm blob) |
| EGL on GBM (needed by Cog DRM and WPE) | yes, standard Mesa | ROCKNIX builds it with wrappers; exact EGL platform set **unverified** | the build is named "-gbm"; extension set **unverified** |
| dma-buf between WPE processes | standard Mesa, the path WPE is developed against | **unverified** | **unverified**, the main doubt |
| Debian packaging | stock `libgl1-mesa-dri`, `libgbm1`, `libegl-mesa0` | blob overrides Debian's GL libraries | blob overrides Debian's GL libraries |
| Vulkan | PanVK: Mesa lists Vulkan 1.3 for Bifrost v7, **not conformant**. Mesa says experimental GPUs refuse to load unless `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` (checked: docs/drivers/panfrost.rst on Mesa main). Whether v7 is still in that experimental group is **unverified**. | Arm blob Vulkan (ROCKNIX pulls `vulkan-wsi-layer`) | — |
| Does Vulkan matter to us? | **No.** WPE draws with GLES/EGL (Skia's GL back end, WebGL through ANGLE on GLES). Vulkan helps nothing here. | No | No |
| Display panel | mainline DSI + ROCKNIX ST7703 R36S mode, or `generic-dsi` + `mipi-panel.dtbo` for clones | same | the widest clone coverage (community DTBs) |
| Verdict | **Start here** | first fallback if Panfrost misdraws WebGL | last resort, mostly as a source of panel data |

---

## 4. Which distros run the R36S, and with what

### ROCKNIX (formerly JELOS) — the source to borrow from
Repository: `github.com/ROCKNIX/distribution`, branch `next`, pushed 2026-11-03 (checked).
- **Device options** `projects/ROCKNIX/devices/RK3326/options` (checked):
  - target `cortex-a35`, `msdos` partition table, U-Boot with `UBOOT_FIRMWARE="rkbin"`;
  - `MALI_FAMILY="bifrost-g31"`, `GRAPHIC_DRIVERS="mali panfrost"`, Wayland with sway;
  - `ADDITIONAL_PACKAGES="device-switch libmali generic-dsi"`;
  - console on `ttyS2,1500000`.
- **Kernel** `projects/ROCKNIX/packages/linux/package.mk` (checked): `RK3326 → PKG_VERSION="7.1.2"` from kernel.org.
  For RK3326 it copies `panel-generic-dsi.c` into `drivers/gpu/drm/panel/` and adds the `rocknix-joypad` patch dir.
- **RK3326 patches** `projects/ROCKNIX/devices/RK3326/patches/linux/` (checked) include:
  - `001-panel-updates.patch`, which adds a `gameconsole,r36s-panel` mode to `panel-sitronix-st7703.c`: **640×480**,
    50 MHz pixel clock, 4 lanes, RGB888, 52×70 mm. That mode is not in mainline yet;
  - `004-input-drivers.patch`, plus a polled-input driver for the joypad;
  - dwc2 OTG fixes (`003`, `004`, `010`, `035`, `038`);
  - `039-drm-rockchip-vop-add-afbc-support-for-px30`;
  - a whole `px30s` series (`023`–`034`).
- **The PX30S series matters.** It suggests that newer boards use the RK3326S/PX30S variant. If Stuart's unit is one of
  those, those patches are needed (**unverified** which variant he has).
- **DTS** `projects/ROCKNIX/devices/RK3326/linux/dts/rockchip/` (checked): `rk3326-gameconsole-r36s.dts`,
  `rk3326-gameconsole-r33s.dts`, `rk3326-gameconsole-eeclone.dts`, `rk3326-gameconsole-r3xs.dtsi`,
  `rk3326-device-group-r36.dtsi`, the Odroid Go 2/3, RG351M/V, RGB10/RGB10X, XU10, XU-Mini-M, GKD Pixel 2 and others.
  - The R36S joypad is `compatible = "rocknix-singleadc-joypad"`, name `r36s_Gamepad`, vendor `0x0001`, product `0x1188`.
    It is ADC sticks through an analogue mux plus GPIO buttons, a ROCKNIX in-kernel driver.
  - **Mainline alone has no R36S DTS and no R36S joypad driver.** Mainline has `rk3326-odroid-go2*`, `-go3`, `rg351m/v` and
    `gameforce-chi` only (checked: torvalds/linux `arch/arm64/boot/dts/rockchip`). That is why we borrow ROCKNIX's kernel, not plain mainline.
- **Clone panels**: ROCKNIX's `generic-dsi` driver reads the panel description from a DT overlay. `importpanel.py`
  (`packages/linux-drivers/generic-dsi/scripts/`, checked) extracts it from a **stock vendor DTB** (`dsi@ff450000/panel@0`)
  into `mipi-panel.dtbo`. This is how an unknown clone panel is supported without writing a driver.
- **Board detection**: U-Boot reads a hardware ID from the SARADC (`uboot.hwid_adc` on the kernel command line). `boot.scr`
  then picks the DTB, with a legacy U-Boot variant for some boards (checked: `devices/RK3326/bootloader/update.sh`).
- **ROCKNIX's device pages say** "libmali (GLES 3.2) & Panfrost (GL 3.1/GLES 3.1)" for the RK3326 clones (source: rocknix.org device page via search).

### dArkOS — Debian-based ArkOS (christianhaitian/dArkOS)
- Builds a Debian root with chroot. The default suite is now **trixie** (Makefile, checked). It has a 64-bit + 32-bit
  user space for PortMaster.
- Kernel: christianhaitian/linux **4.4** (`odroidgoA-4.4.y` for rgb10/rk2020, `rg351` for the rest), `build_kernel.sh` (checked).
- GPU: the `libmali-bifrost-g31-rxp0-gbm.so` blob, symlinked over every GL name (checked, `build_deps.sh` + `utils.sh`).
- R36S builds come from the `rg351mp` target and community forks. dArkOSen (djparentx/dArkOSen-R36S) ships patched DTBs
  and a **DTB selector in /boot**. For a model not listed, its notes say: copy your original DTBs and rename them
  `rg351mp-uboot.dtb` and `rk3326-r36s-linux.dtb` (source: its release notes).

### ArkOS (and ArkOS-for-clones forks: AeolusUX/ArkOS-R3XS, lcdyk0517/arkos4clone)
- Same BSP 4.4 + libmali family. These are where most of the **clone panel knowledge** lives.
- arkos4clone renamed panels to "Type" numbers: Panel 1 = Type 1, Panel 8 = Type 2, Panel 4 = Type 3, Panel 7 = Type 5
  (source: arkos4clone 20251022 notes). Panel numbers in forum posts are therefore ambiguous; always go by the DTB.

### Arch R (archr-linux/Arch-R)
- Arch Linux ARM for the same devices. It has separate "original" and "clone" images and a `mipi-panel.dtbo` overlay
  generator, the same idea as ROCKNIX.
- The project page and a search summary disagreed on its kernel (mainline 6.12 LTS vs "Rockchip BSP 6.6.89"), so treat
  its details as **unverified**. It is useful as a second opinion on clone panels.

### Armbian / plain Debian
- Armbian has no R36S target. Mainline device trees cover only the Odroid Go Advance family, and a plain mainline kernel
  would have no R36S panel mode and no joypad. Plain Armbian is not a starting point.

### How a user finds out which board and panel they have
1. **Before anything else, image the stock card whole** (`dd` or a card imager). It holds the only copy of the vendor DTB
   for that exact panel. Copy the BOOT partition's `*.dtb` files somewhere safe.
2. **Clone or original:**
   - BOOT DTB names such as `rk3326-evb-lp3-v12`, `rf3536k4ka` or `rf3536k3ka`, or no DTB at all, mean an
     EmuELEC-style clone, not a standard R36S (source: handhelds.wiki R36S guides, techtactician).
   - The board model is printed under the battery (six back screws).
3. **Panel:** run the stock DTB through the handhelds.wiki "Unofficial Handheld DTB Analysis Tool" or ArkOS4Clone's DTB
   identification tool (source). For our image: ROCKNIX's `importpanel.py` on the stock DTB makes the overlay directly.

---

## 5. Browser engine

### WPE WebKit + Cog on DRM/KMS (recommended)
- **Versions** (checked, arm64 package indexes, 2026-11-04):

  | Suite | WPE WebKit | Cog | Mesa | libmanette |
  |---|---|---|---|---|
  | bookworm (12) | 2.38.6 (`libwpewebkit-1.1-0`) | 0.16.1 | 22.3.6 (25.0.7 in backports) | 0.2.6 |
  | **trixie (13)** | **2.48.3** (`libwpewebkit-2.0-1`) | **0.18.4** | **25.0.7** | 0.2.12 |
  | forky/sid | 2.54.2 | 0.18.5 | 26.2.6 | 0.2.13 |

- **Cog 0.18.4 on trixie** ships `libcogplatform-drm.so`, `-wl.so` and `-headless.so` (checked, `dpkg -c`). It also has a
  gamepad backend: `libcogcore` contains `gamepad-manette` / "gamepad implementation" strings, and Debian's cog depends
  on `libmanette-0.2-0` (checked).
- **DRM platform parameters** (Cog docs, `docs/platform-drm.md`):
  - `--platform=drm --platform-params=renderer=gles,rotation=N`. `renderer=gles` is needed for rotation; `rotation` is
    quarter turns counter-clockwise.
  - `cursor` and `pointer` options; `disable-atomic-modesetting` in the config file;
  - `COG_PLATFORM_DRM_VIDEO_MODE=640x480`.

  ROCKNIX's R36S mode is already 640×480 landscape, so **no rotation is expected on an original R36S** (unverified on clones).
- **WPE 2.48.3 binary** (checked by searching its strings):
  - `WebGLRenderingContext` and `WebGL2RenderingContext` (ANGLE inside);
  - Skia GPU canvas code (`CanvasUsesAcceleratedDrawing`, `SkiaGPUWorker`);
  - environment switches: `WEBKIT_SKIA_ENABLE_CPU_RENDERING`, `WEBKIT_SKIA_CPU_PAINTING_THREADS`,
    `WEBKIT_SKIA_GPU_PAINTING_THREADS`, `WEBKIT_SHOW_FPS`, `WEBKIT_FORCE_VBLANK_TIMER`, `WEBKIT_WEBGL_DISABLE_GBM`,
    `WEBKIT_DISABLE_MEMORY_PRESSURE_MONITOR`;
  - `WebCodecsAudioDecoder`;
  - Gamepad API classes.

  Its installed size is **117 MB**. The library alone is 116 MB on disk; RAM is a separate matter (section 7).
- **2D canvas and layer painting** (source: WPE 2.46 notes and Igalia's 2.46 graphics post): Skia replaced Cairo.
  The 2D canvas is accelerated by default through the `enable-2d-canvas-acceleration` setting, even when layer tiles are
  painted on the CPU. On embedded SoCs, Igalia found that CPU tiles plus a GPU canvas is often the fastest mix, and that
  is the default there. The setting may still leave a canvas unaccelerated (by size, or `willReadFrequently`).
  ★ We never set `willReadFrequently` on the visible canvas. The `getImageData` at `waterfall.ts:433` is on the 2D-path
  ring only, during a resize. That should be confirmed if the 2D path is ever the one running.
- **Gamepad:** WebKit's Linux gamepad provider is libmanette. **libmanette maps pads through SDL's controller database**,
  so whether it treats the ROCKNIX `r36s_Gamepad` evdev device (vendor 0x0001, product 0x1188) as a standard-layout pad
  is **unverified**. If it does not, there are two fixes:
  - (a) add an SDL-style mapping for that ID, through libmanette's user mapping store (path **unverified**);
  - (b) a small evdev→uinput remapper that presents the buttons as a well-known pad (the same trick as ArkOS's
    `gptokeyb`). This one is simplest and most predictable.
- **Audio:** WebAudio → GStreamer → `alsasink`. ALSA directly is fine because the webview is the only thing making sound.
  The R36S codec is the RK817 PMIC's. ROCKNIX supports it on mainline (source). How the speaker and headphone are
  switched (mixer controls, jack detection) is **unverified**.
- **Security:** trixie's `libwpewebkit-2.0-1` is still **2.48.3-1** with no `+deb13u` update. Over the same period trixie's
  WebKitGTK moved to 2.54.0~deb13u3 (checked). So WPE does not appear to get security updates in trixie (**unverified** whether
  that is Debian policy).
  - The kiosk only loads `http://127.0.0.1:48000`, which limits the exposure.
  - Pages that reach out (map tiles, station logos) widen it. Consider locking Cog to the local origin.
  - Moving to WPE 2.54 (a backport build, or the sid package) later gives the built-in **WPEPlatform DRM**, which no longer
    needs Cog's platform plug-ins (source: wpewebkit.org supported-hardware page: "since 2.54 … DRM/KMS … built-in").
    That build is heavy and is left for later.

### Alternatives
| | Display stack | RAM (estimate, **unverified**) | Notes |
|---|---|---|---|
| **WPE 2.48 + Cog DRM** | DRM/KMS direct | ~200–300 MB for UI + web + network processes on our page | no compositor; smallest |
| WebKitGTK 6 (2.54 in trixie) under **cage** (wlroots kiosk compositor, 0.2.0 in trixie) | Wayland | WPE + GTK4 + cage, roughly +60–120 MB | more moving parts, same engine; GTK4's GL renderer is happy on GLES 3.1 |
| Chromium 156 (trixie) under **cage** | Wayland (Debian's Chromium is built for X11/Wayland; a DRM-only "Ozone DRM" build is not what Debian ships, **unverified**) | ~350–500 MB (browser + GPU + renderer + utility processes) | best WebGL/canvas maturity, but the RAM is a poor fit for 1 GB next to the server and decoders |

---

## 6. Build plan, step by step

All work happens in Docker on the M5 Pro, natively arm64, one build at a time (standing rule).

**Stage 0 — before the unit arrives (cheap)**
1. Prototype the **web client in Cog on a Pi** running Pi OS (vc4/v3d DRM; Cog DRM is listed for the Pi 3 on WPE's
   hardware page). This shakes out WPE-specific client bugs (AudioWorklet, WebCodecs vs WASM Opus, gamepad, layout at
   640×480) before the Mali question. ★ Ask Stuart before using any Pi of his. Never the Pi 2 without asking.
2. Lay out a `image/r36s/` builder modelled on `image/sbc/` (`build.sh`, `run-in-docker.sh`, `DRY_RUN=1`, `boot-test.sh`).

**Stage 1 — day one on the device (no build of ours)**
3. Image the stock card. Then flash an **unmodified ROCKNIX RK3326 release** on a second card. It proves our kernel,
   panel, joypad and bootloader source on this exact unit. Black screen: use ROCKNIX's panel overlay route with the stock DTB.

**Stage 2 — the first card of ours**
4. **Root file system:** in a `debian:trixie` arm64 container, use `debootstrap --variant=minbase trixie` into a
   directory. Install:
   - `systemd`, `udev`, `dbus`, `kmod`, `firmware-*` as needed, `libgl1-mesa-dri`, `libegl-mesa0`, `libgbm1`;
   - `cog`, `libwpewebkit-2.0-1`, `gstreamer1.0-alsa`, `gstreamer1.0-plugins-base`, `gstreamer1.0-plugins-good`, `alsa-utils`;
   - the test tools: `kmscube`, `glmark2-es2-drm`, `mesa-utils-bin` (`eglinfo`), `libdrm-tests` (`modetest`), `evtest`;
   - then `vibeserver` (apt.vibesdr.net or a local `.deb`, as `customise.sh` does).

   Use `--no-install-recommends` throughout, as on the Portable.
5. **Kernel, DTBs and modules:**
   - Day one: lift `Image`, the RK3326 DTBs, `overlays/` and `/usr/lib/modules/<ver>` out of the ROCKNIX release image
     (same kernel as Stage 1).
   - Product: build ROCKNIX's recipe ourselves, pinned to a commit:
     - kernel.org 7.1.2;
     - the RK3326 patch set;
     - the `rocknix-joypad` patches;
     - `panel-generic-dsi.c`;
     - `projects/ROCKNIX/devices/RK3326/linux/dts/`;
     - their `linux.aarch64.conf`.

     That is a native arm64 kernel build in Docker (tens of minutes; still a "build", so one at a time).
   - The kernel is GPL: the card must point to the exact source (ROCKNIX commit + our config). The rkbin blobs are Rockchip's
     redistributable binaries.
6. **Bootloader:** reuse ROCKNIX's packed `uboot.bin`, or rebuild it the same way: mainline U-Boot v2025.10,
   `rk3326-handheld_defconfig`, rkbin `rk3326_ddr_333MHz_v2.12.bin`, `rk3326_miniloader_v1.40.bin`, `rk3326_bl31_v1.37.elf`,
   packed by their `bootloader/rkhelper` (checked). Also reuse their `boot.scr` (SARADC board detection → DTB +
   `overlays/mipi-panel.dtbo`). ROCKNIX also carries a **UART5 variant** and a **legacy U-Boot** for some boards
   (`update.sh` picks `a_uboot.bin` / `b_uboot.bin` / `.uart5`). Which one an R36S needs is **unverified**, so take
   ROCKNIX's choice for the board.
7. **Card layout** (from ROCKNIX's `rkhelper` and `distributions/ROCKNIX/options`, checked):

   | Where | What |
   |---|---|
   | sector 0 | MBR (`msdos`) |
   | 32 KiB (sector 64) | `idbloader.img` (DDR init + miniloader), the start of `uboot.bin` |
   | 8 MiB (sector 16384) | `uboot.img` (`uboot.bin` + 16320 sectors) |
   | 12 MiB (sector 24576) | `trust.img` (BL31) (`uboot.bin` + 24512 sectors) |
   | 16 MiB (sector 32768) | **p1 FAT32 BOOT**: `Image`, `*.dtb`, `overlays/`, `boot.scr` / `extlinux/` |
   | after p1 | **p2 ext4 root**, read-only via overlayroot as on the Portable |
   | after p2 | **p3 small settings/data partition** (the Portable's `vibeserver-pocket-data`) |

   ROCKNIX writes the whole loader in one aligned `dd` at 32 KiB (`update.sh`). Our builder does the same onto the image file.
8. **Kiosk service:** a `vibeserver-kiosk.service` running Cog as an unprivileged user with `/dev/dri`, `/dev/input`
   and audio group access. It runs after `vibeserver.service`, with `Restart=always`, Cog's DRM platform,
   `renderer=gles`, `--gamepad=` the manette backend (exact option spelling **unverified**, see `cog --help`), and the URL
   `http://127.0.0.1:48000/`. No getty on tty1.
9. **Boot test:** extend `image/sbc/boot-test.sh` (systemd-nspawn) to check the units, the front door on `:48000` and
   "no sshd". The GPU and panel can only be tested on the device.

**What can be reused from our Portable tooling** (`image/pocket`, `image/sbc`, `vibeserver/linux/pocket`):
- `customise.sh` — run inside the root. Its marker, first-boot config, apt key, locked login and the single "check for a
  VibeServer update during setup" all apply unchanged.
- `vibeserver-pocket-converge` + `pocket-packages.txt` — the package list every Update tops up. Add a R36S list for
  cog/WPE/gstreamer, read only when the R36S marker exists, so that ordinary Portables never pull a browser.
- `vibeserver-pocket-data` (+ `.service`) — mounts the settings partition, so the root can stay read-only and pulling
  the power cannot corrupt the card (on a handheld that is a battery running flat).
- `vibeserver-pocket-seal` (+ `.service`) — overlayroot at first boot. Its `After=` already names
  `armbian-firstrun`; add our own first-run.
- `vibeserver-pocket` — the Wi-Fi state machine. The R36S has no Wi-Fi, so it starts in its "wired" / no-Wi-Fi state
  and already watches for a **USB Wi-Fi dongle appearing later**. That is exactly the "share it on a hub" case.
- `vibeserver-pocket-cpu` — not needed: arm64 only, NEON always present.
- `image/sbc/build.sh` / `run-in-docker.sh` / `boot-test.sh` — the shape of the builder: board table, DRY_RUN, an
  emulated boot check that fails if sshd is running.

---

## 7. Fit with our server on 4× A35 / 1 GB

**CPU (estimates, unverified until measured):**
- The Cortex-A35 is an in-order ARMv8 core. Per clock it is somewhat above the Pi 2's Cortex-A7, and the RK3326 runs it
  at ~1.3 GHz (1.5 GHz overclocked) against the Pi 2's 900 MHz.
- So the server alone has roughly **1.5–2× a Pi 2**. The Pi 2 is our minimum confirmed spec.
- The webview is the new cost: the main JavaScript thread for the spectrum trace and UI, Skia painting threads, and the
  WebGL work going to the GPU.
- Plan for the browser to take **one to one-and-a-half cores**, which leaves about two and a half for vibeserver and
  decoders. Thread priority stays Network > Audio > Spectrum > Decoders, and the kiosk process sits below the server
  (`Nice=` on the kiosk unit).

**RAM (estimates):**

| | MB |
|---|---|
| kernel, systemd, journald (in RAM), udev, dbus | 60–80 |
| vibeserver front door + one radio (the Pi 2 measured 76 MB for both, `docs/POCKET-VIBESERVER-PI3A.md`) | ~80 |
| vibeserver-pocket (Python) + nmcli monitor, if kept | ~15 |
| WPE: Cog/UI + WebProcess + NetworkProcess on our page | 200–300 |
| GPU buffers (640×480, a few frame buffers; shared with system RAM) | < 20 |
| **Total** | **~380–500 of 1024** |

No cloudflared by default (tunnel off), plus zram as on the Portable. It fits, with room for DAB+ (ffmpeg) or one decoder.
`WEBKIT_DISABLE_MEMORY_PRESSURE_MONITOR` should **not** be set. WebKit's own pressure handling is useful on 1 GB.

**Radios and rates (realistic first targets, unverified):**

| Radio | USB load | Suggested |
|---|---|---|
| **Airspy HF+** | 192–768 kS/s × 4 bytes ≈ 0.8–3 MB/s | **192–384 kS/s** first, 768 kS/s if headroom. The lightest and the best demo |
| **RTL-SDR V4** | 1.2 MS/s ≈ 2.4 MB/s; 2.4 MS/s ≈ 4.8 MB/s | **1.024–1.2 MS/s** for WFM stereo; try 2.4 MS/s once measured |
| **Airspy Mini** | 3 MS/s ≈ 9–12 MB/s | tight with the browser; 6 MS/s out |

★ **The OTG port's USB controller is a DesignWare dwc2.** On the Raspberry Pi 1 and Zero, that controller family in host
mode costs a lot of CPU in interrupts. ROCKNIX carries several dwc2 fixes for this SoC. Its host-mode cost at 5–12 MB/s
is a real risk and is measured on day one (test E2). The RTL V4 and the HF+ also draw their power from the OTG port's 5 V;
whether it delivers enough with the radio and a hub is **unverified**.

**Package compatibility:** the arm64 `.deb` is built on bookworm with a glibc 2.35 floor. Trixie (2.41) runs it, as the
Armbian Trixie Portable cards in `image/sbc/` already do.

---

## 8. Only the device can tell us — the day-one tests

Get a shell first. The unit has no Wi-Fi, so use one of:
- **(a)** a USB-C OTG hub with power pass-through + a USB Ethernet adapter (needed later anyway for sharing);
- **(b)** a supported USB Wi-Fi dongle;
- **(c)** the UART at `ttyS2`, 1 500 000 baud (pads on the board).

Run the tests in this order and write every number down.

**A. Identity (stock card, before flashing anything)**
- A1. Image the whole stock card. Copy BOOT's `*.dtb` and `boot.ini`/`boot.scr`. Photograph the board number under the battery.
- A2. Is it a clone (DTB names, section 4)? Which panel (DTB analysis tool)?
- A3. On a ROCKNIX card: `cat /proc/device-tree/model`, `cat /proc/cmdline` (look for `uboot.hwid_adc=`) and
  `cat /proc/cpuinfo` (RK3326 or PX30S).

**B. Kernel, display and GPU (ROCKNIX first, then our trixie card)**
- B1. `dmesg | grep -iE 'panfrost|dsi|panel|st7703'` — panel probed, Panfrost bound (`mali-g31`).
- B2. `ls -l /dev/dri` — `card0` and `renderD128` present.
- B3. `modetest -M rockchip -c` — the DSI connector is connected at 640×480. Also `modetest -p` for the plane list (note
  whether there is a cursor plane, an overlay plane and AFBC).
- B4. `eglinfo -B` (or `EGL_PLATFORM=gbm eglinfo`) — the GBM platform reports Mesa / Panfrost / Mali-G31, OpenGL ES 3.1.
- B5. `kmscube` — spinning cube, smooth, no tearing. Then `kmscube -M rgba` and `-A` (atomic).
- B6. `glmark2-es2-drm` — completes all scenes; record the score and any scene that misdraws.
- B7. Temperature and clocks during B6: `cat /sys/class/thermal/thermal_zone*/temp`, `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq`.

**C. The browser (our trixie card)**
- C1. `cog --platform=drm --platform-params=renderer=gles webkit://gpu`. Read: the GL/EGL vendor and renderer strings,
  whether accelerated compositing is on, the dma-buf format and modifier, and the 2D canvas acceleration state.
  - Fails to start: retry with `disable-atomic-modesetting` in the config.
  - Wrong orientation: try `rotation=1/3`.
- C2. `WEBKIT_SHOW_FPS=1 cog … http://127.0.0.1:48000/` with a radio attached. Waterfall running:
  - record the frame rate shown;
  - `top -H` per thread (`WPEWebProcess`, its Skia painting threads, `vibeserver`'s threads);
  - `free -m`;
  - `ps -o rss,comm`.
- C3. **Which waterfall path?** If WebGL failed, `waterfall.ts` fell back to 2D without saying so. Load
  `webkit://gpu`, or a WebGL test page, to confirm WebGL is available. Then compare with WebGL switched off in Cog's
  WebKit settings (`cog --help-websettings`, look for `enable-webgl`); the difference is the GPU's contribution.
- C4. A/B the painting mode: default vs `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`, and Skia painting threads 1 vs 2
  (`WEBKIT_SKIA_CPU_PAINTING_THREADS`). Keep the faster one.
- C5. A/B the client's own costs: the same page with `#noglow`, then `#nowf`, then `#noaudio`. Each shows what its part costs.
- C6. Leave it running 30 minutes: memory steady? No growth in `/proc/<pid>/fd` for Cog. A Tinker Board 2S user reported
  Cog DRM with a Mali blob leaking `/dev/dri/card0` descriptors until it crashed (source: Arm community forum); check
  that it does not happen here.

**D. Input and audio**
- D1. `evtest` — find `r36s_Gamepad` and note the button and axis codes.
- D2. In Cog with the manette gamepad backend: press SELECT on our page (the controller guide should open). Check that
  the D-pad moves the highlight and that both sticks register. If nothing happens, the device has no libmanette mapping
  (section 5): try a mapping, else the uinput remapper.
- D3. `aplay -l`, then **at low volume** `speaker-test -c2 -t sine -l1` on the speaker and on the headphones. Check the
  WebAudio output through GStreamer. Note whether WebCodecs or the WASM Opus decoder was used (the client logs which).

**E. USB, power and radio**
- E1. `lsusb` and `dmesg` with each radio on the OTG port (direct, then through the hub).
- E2. Throughput and CPU:
  - `rtl_test -s 1200000`, then `-s 2400000`, for 60 s: lost bytes? Watch `%hi`/`%si` in `top`.
  - Then the same for each radio through vibeserver: no audio drops at the rates in section 7.
- E3. Battery: full charge → kiosk running with the RTL V4 → time to shutdown; also note whether the OTG 5 V holds.
- E4. Heat after 30 minutes and its effect on the RTL V4's frequency (TCXO drift on a warm case).

---

## 9. Open risks and unknowns

1. **Panel on Stuart's exact unit:** clones vary. Mitigation: the stock DTB → ROCKNIX `importpanel.py` → `mipi-panel.dtbo`.
   This is why A1 comes first.
2. **RK3326 vs RK3326S/PX30S board:** ROCKNIX carries a PX30S patch series. We need whichever kernel ROCKNIX boots on this unit.
3. **Cog's DRM platform on the PX30 display controller with Panfrost:** not listed anywhere as tested. Fallback order:
   - Cog's Wayland platform under `cage` (ROCKNIX itself runs sway on this SoC);
   - then WebKitGTK under cage.
4. **The WebGL → 2D canvas copy:** if the copy turns out to be the bottleneck (C2/C4), layer the GL canvas in the client.
5. **libmanette and the R36S pad:** mapping unknown. A uinput remapper is the sure fix.
6. **dwc2 host-mode CPU cost** at Airspy Mini rates, and the OTG power budget.
7. **WPE security updates in trixie** appear stalled at 2.48.3. Lock the kiosk to the local origin; plan a 2.54 build later.
8. **Kernel source tracking:** ROCKNIX moves fast (7.1.2 today). Pin a commit, record it in the image manifest, and
   publish the matching source as the GPL requires.
9. **The 640×480 layout and pad-first UI** are client work and separate from this image. `gamepad.ts` is already written
   with this class of device in mind.
10. **Audio routing** (speaker/headphone switch, volume keys) on the RK817 under mainline — copy what ROCKNIX does, once seen.

---

## Sources

- ROCKNIX distribution, branch `next` (read through the GitHub API, 2026-11-04):
  - `projects/ROCKNIX/devices/RK3326/options`;
  - `projects/ROCKNIX/packages/linux/package.mk`;
  - `projects/ROCKNIX/devices/RK3326/patches/linux/` (`001-panel-updates.patch` and the rest);
  - `projects/ROCKNIX/devices/RK3326/linux/dts/rockchip/`;
  - `projects/ROCKNIX/devices/RK3326/bootloader/update.sh`;
  - `projects/ROCKNIX/devices/RK3326/packages/u-boot/package.mk`;
  - `projects/ROCKNIX/bootloader/rkhelper`;
  - `distributions/ROCKNIX/options`;
  - `projects/ROCKNIX/packages/graphics/libmali/package.mk`;
  - `projects/ROCKNIX/packages/linux-drivers/generic-dsi/`.
- ROCKNIX device pages: https://rocknix.org/devices/unbranded/game-console-r36max , https://rocknix.org/devices/anbernic/rg351pmv/
- christianhaitian/dArkOS: `README.md`, `Makefile`, `build_kernel.sh`, `build_deps.sh`, `utils.sh`, `bootstrap_rootfs.sh`;
  christianhaitian/rk3326_core_builds `mali/aarch64/`.
- dArkOSen-R36S wiki and releases: https://github.com/djparentx/dArkOSen-R36S/wiki , https://github.com/djparentx/dArkOSen-R36S/releases
- Arch R: https://github.com/archr-linux/Arch-R
- Mesa Panfrost docs (main): https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/docs/drivers/panfrost.rst
- Linux mainline: `drivers/gpu/drm/panel/panel-sitronix-st7703.c`, `arch/arm64/boot/dts/rockchip/` (torvalds/linux).
- WPE: https://wpewebkit.org/about/supported-hardware.html , https://wpewebkit.org/release/wpewebkit-2.46.0.html ,
  https://wpewebkit.org/blog/2024-wpewebkit-2.46.html , https://blogs.igalia.com/carlosgc/?p=1029 ,
  https://people.igalia.com/aperez/Documentation/wpe-webkit/property.Settings.enable-2d-canvas-acceleration.html
- Cog DRM platform: https://github.com/Igalia/cog/blob/master/docs/platform-drm.md ; Cog FAQ https://wpewebkit.org/about/faq.html
- Cog DRM + Mali blob descriptor leak report: https://community.arm.com/support-forums/f/graphics-gaming-and-vr-forum/56686/difficulties-with-midgard-t860-r18p0-driver-on-asus-tinkerboard-2
- Panfrost G31 history: https://www.cnx-software.com/2020/06/09/gnome-renders-on-arm-mali-g31-bifrost-gpu-with-fully-open-source-code/
- R36S identification: https://handhelds.wiki/R36S_Guides , https://handhelds.wiki/R36S_Clones ,
  https://techtactician.com/r36s-handheld-full-starter-guide/ , https://newreleases.io/project/github/lcdyk0517/arkos4clone/release/20251022
- Package versions and binary contents: `apt-cache policy`, `apt-get download` + `dpkg -c` in `debian:bookworm`,
  `debian:trixie-slim` and `debian:sid-slim` arm64 containers on the Mac, 2026-11-04.
- This repository: `web/client/src/wfgl.ts`, `web/client/src/waterfall.ts`, `web/client/src/audio.ts`,
  `web/client/src/gamepad.ts`, `vibeserver/linux/glibc_floor.c`, `vibeserver/linux/pocket/`, `image/pocket/`,
  `image/sbc/`, `docs/POCKET-VIBESERVER-PI3A.md`.
