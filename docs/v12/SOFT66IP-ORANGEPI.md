# V12 — the Soft66IP (Orange Pi Zero, 256 MB) as a VibeServer

Asked 2026-10-24. Danny owns a **Soft66IP**: Kazunori Miura's RTL-SDR retrofit board with an HF upconverter, an RF amp
and a slot for an **Orange Pi Zero**. The Orange Pi Zero is an Allwinner H2+/H3 with four Cortex-A7 cores (ARMv7, NEON,
VFPv4), **256 MB of RAM**, 100 Mb Ethernet and XR819 Wi-Fi. Danny has **Armbian, kernel 6.18 (Ubuntu userland)** on it.
His screenshot reads "44 % of 231M" memory, 70 °C and a 15 GB card. Stuart asked whether our VibeServer Portable image
already works on it.

Nothing here was run on the board. This is research from our build files, the published apt metadata (read over HTTP)
and the measurements in our notes. §8 lists what is still unverified.

## 0. The short answer

- **The Portable image: no.** It is Raspberry Pi OS Lite. An Allwinner board cannot boot it (§1).
- **The apt package: yes, it should.** `curl -fsSL https://apt.vibesdr.net/install.sh | sudo bash` on Danny's Armbian
  installs the ordinary armhf VibeServer. Its build targets the same CPU core as the Pi 2 (§2). That is the route to try
  today (§7).
- **RAM: it fits, with less room than on a Pi.** OS + VibeServer measures about **200 MB on a Pi**, and that includes the
  Cloudflare tunnel. On this board, Armbian as Danny has it leaves room for VibeServer with the **tunnel off** and
  **DAB+ off**. Either one also fits once Armbian is slimmed down. Both together do not (§3).

## 1. Does the Portable image boot on an Orange Pi Zero? No

`image/pocket/build-image.sh` (default route) downloads `https://downloads.raspberrypi.com/raspios_lite_${A}_latest`
and customises it in a chroot. The `--pigen` route builds the same OS from `RPi-Distro/pi-gen`. Either way the card
carries:

- **The Raspberry Pi boot chain.** The Pi's GPU loads `bootcode.bin`/`start*.elf` from the FAT partition, then reads
  `config.txt` and loads a Pi kernel with a BCM283x/BCM2711 device tree. An H3's boot ROM looks for **U-Boot (SPL) at
  8 KB into the card**. The Pi image has nothing there, so the board does not start at all. It does not even get as far
  as a kernel panic.
- **A kernel built for Broadcom.** It has no sun8i drivers (no Allwinner clocks, MMC, EMAC, USB PHY or thermal), and it
  has no H2+/H3 device tree.
- **A Pi-only converge.** `vibeserver-pocket-converge` writes `/boot/firmware/config.txt` lines (`dtoverlay=disable-bt`,
  `gpu_mem=16`, `auto_initramfs=1`), uses Trixie's `rpi-swap` (`/etc/rpi/swap.conf.d/`) and disables `hciuart` and
  `dphys-swapfile`. `vibeserver-pocket` assumes brcmfmac Wi-Fi throughout (power-save, PMF/SAE and `tx failed` quirks).
  `vcgencmd` is already guarded by `which()`, and the config.txt step skips when no file is visible, so these parts fail
  harmlessly elsewhere. They do nothing useful on Allwinner, though.

You could not fix this by copying files onto the card. A port means a different base OS (§6).

## 2. Does the armhf apt package run on Armbian on this board? Yes, on the evidence

**Published metadata** (`https://apt.vibesdr.net/dists/stable/main/binary-armhf/Packages`, read 2026-10-24):

```
Version: 11.0.10-5        (beta suite: 12.0.0~beta10-1)
Depends: adduser, curl, gzip, libc6 (>= 2.35)
Recommends: ffmpeg, vibeserver-mapdata-detail
Installed-Size: 105173    (~103 MB on disk — both builds, see below)
```

| Requirement | The package | Armbian on an H3 |
|---|---|---|
| C library | `libc6 (>= 2.35)`. Every SDR library is built in (libusb, librtlsdr V4L, libopus, static libstdc++), so dpkg-shlibdeps cannot add more (`linux/check-deb-depends.cmake` fails the build if it tries) | Bookworm 2.36, Noble 2.39, newer Ubuntu higher ✓ |
| ISA | `/usr/bin/vibeserver` is ARMv7, `-marm` (ARM mode, the measured Pi 2 speed), `-mfpu=neon-vfpv4` (CMakeLists.txt, armv7 branch) | Cortex-A7 = **the same core as the Pi 2 v1.1** (BCM2836). NEON, VFPv4, idiv ✓ |
| NEON fallback | `postinst` installs the scalar ARMv6 build if `/proc/cpuinfo` has neither `neon` nor `asimd` | The H3 reports `neon`, so it gets the NEON build ✓ |
| Hard-float ABI | armhf | Armbian armhf is hard-float ✓ |
| systemd unit | `vibeserver.service`: sandboxed, `DeviceAllow=char-usb_device`. `/dev/vcio` is allowed but optional | Nothing Pi-specific is needed ✓ |
| RTL kernel driver | `vibeserver-blacklist-dvb.conf` blacklists `dvb_usb_rtl28xxu` and friends | The same modules exist on Armbian. The blacklist takes effect after a **reboot** (or `rmmod`) |
| plugdev, udev rule | `99-vibeserver-sdr.rules`, group plugdev | Standard on Debian/Ubuntu ✓ |
| mDNS | Our own responder, built into the server (no avahi needed on an ordinary install) | ✓ |
| Maintenance helper | `vibeserver-maintenance` runs apt from the admin page. It only behaves differently under overlayroot (Portable) | Ordinary apt box ✓ |
| Converge | Does **nothing** without `/etc/vibeserver-pocket/enabled` | Not a Portable, so it is inert ✓ |
| ffmpeg (DAB+ audio) | Recommends | Pulled in by a default apt install. On disk only (~a few hundred MB on Ubuntu), and in RAM only while DAB+ plays |
| `vibeserver-mapdata-detail` | Recommends, but **not in the repository** | apt ignores a missing Recommends ✓ (worth tidying) |
| CPU governor | `vibeserver-governor` sets `performance` unless the owner chose otherwise | It writes `scaling_governor` only, so a `scaling_max_freq` cap still holds (see §4) |

**`install.sh` on Armbian.** Armbian keeps the base distribution's `ID`/`ID_LIKE` (`ubuntu`/`debian`), so the distro test
passes. `dpkg --print-architecture` gives `armhf`, the NEON test passes, and glibc ≥ 2.35 passes. It then writes our
one-line source, checks the key fingerprint and runs `apt-get install vibeserver`. The script itself is live at
`https://apt.vibesdr.net/install.sh` (HTTP 200, 2026-10-24).

★ **Fixed on this branch** (2d103cb1): the installer's closing line tested `neon` only. On a 64-bit kernel with a
32-bit userland, which reports `asimd`, it told the owner SDRplay was unsupported, although the NEON build (which has
SDRplay) was the one installed. It now uses the same `neon|asimd` test as the arch check and `postinst`. This is
cosmetic on Danny's board (32-bit kernel, `neon`), and it does not change what gets installed.

★ **One unknown: Ubuntu's armhf future.** "Armbian 6.18 (Ubuntu, rolling)" may be a newer Ubuntu than 24.04. Our
package needs only libc6 from it, but **check that the release still publishes armhf** before recommending it. Armbian
**Bookworm or Trixie (Debian) minimal** is the safer base. Debian keeps armhf in Trixie.

## 3. 256 MB: what fits

### The measured baselines (Pi 2, 32-bit Pi OS Lite)

These figures come from our notes in `memory/`. They are the source of Stuart's "an OS and VibeServer clocks in at
around 200 MB".

| What | Figure | Source |
|---|---|---|
| **Whole Pi 2 at rest, rc36, 0 listeners, tunnel ON** | 225 MB used incl. ~20 MB of an ssh session ⇒ **~205 MB**, swap 0 | `minimum_confirmed_spec.md` (2026-10-10). Stuart: "runs at UNDER 200 MB RAM in use" |
| …of which vibeserver ×2 (front door + radio process) | **76 MB RSS** | same |
| …cloudflared ×2 (the tunnel) | **63 MB RSS** | same; the Portable doc: "Personal = cloudflared actually STOPPED (~60 MB back)" |
| …NetworkManager | 17 MB | same |
| ⇒ the OS's own share, by subtraction | **~50 MB** (+17 NM) | |
| Whole Pi 2 **playing DAB+** (whole-multiplex label scan on) | **176 MB** used of 920. VibeServer 60 MB (peak 64) + ffmpeg 37 MB | `lite_minimum_spec.md` (2026-09-19, older build, no tunnel) |
| Danny's Orange Pi Zero, Armbian, **before VibeServer** | **44 % of 231 MB ≈ 102 MB** used | his screenshot |

★ The board shows **231 MB** of 256 because of the kernel image and the CMA/reserved regions. Armbian's 102 MB idle is
about **35–50 MB heavier** than the OS share on Pi OS Lite. Likely reasons, all unconfirmed: the Ubuntu userland,
`armbian-ramlog` (/var/log in a RAM disk), unattended-upgrades' Python, an interactive login session and the XR819
driver. A `ps` list from Danny would settle it (§7, step 1).

### What the code says about rate

VibeServer's RAM barely moves with the sample rate. The RTL capture buffers are `rtlBufLenForRate()` = 32 ms of u8 I/Q,
clamped to 16–256 KB, × librtlsdr's 15 buffers:

| Rate | One buffer | 15 buffers |
|---|---|---|
| 1.024 MS/s | 64 KB | ~1.0 MB |
| 1.2 MS/s | 77 KB | ~1.1 MB |
| 2.048 MS/s | 131 KB | ~2.0 MB |
| 2.4 MS/s | 154 KB | ~2.3 MB |

The DSP stages downstream hold a few blocks of the same order. Between 1.024 and 2.4 MS/s the difference is **a few MB**
(estimated from the code, not measured on the board). **Rate costs CPU, not RAM.** These cost little resident memory:

- The aircraft data (`vibe_adsb_data.h`) is **mmap'd read-only**, so it is page cache and the kernel can drop it.
- The served web page (~1–2 MB) sits in the binary's read-only data. It is file-backed and shared.
- The baked maps are files on disk.

The real RAM swings are:

| Feature | RAM | Notes |
|---|---|---|
| **Cloudflare tunnel** (listing to the world) | **~60 MB** | The biggest single item. Off = LAN, or a port forward |
| **DAB+** | **~37 MB ffmpeg + DAB pipeline** | ffmpeg runs only while DAB+ audio plays. DAB MP2 needs no ffmpeg |
| Each listener | a few MB (Opus encoder + queues) | Unmeasured. Keep it to a handful |
| Weekly ASN/GeoIP rebuild | a transient peak while parsing (~9 MB download, ~43 MB of text for the ASN set) | Unmeasured on 256 MB. Worth watching in `free -m` the first time it runs |
| Advanced RDS, waterfall fps, ADS-B, decoders | mostly **CPU** | ADS-B needs ≥ 2.4 MS/s (see `adsb_v12.md`). The Pi 2 runs it |

### The budget on 231 MB

| Setup | Estimate | Left of 231 MB |
|---|---|---|
| Armbian as Danny has it (102) + VibeServer (76) | ~178 MB | **~53 MB** ✓ |
| …+ DAB+ playing (+~40) | ~220 MB | ~10 MB, marginal: zram takes the strain, at a CPU cost |
| …+ the tunnel (+63) | ~241 MB | **over**: only zram keeps it alive ✗ |
| **Slimmed Armbian (~55–65)** + VibeServer | ~135 MB | **~95 MB** ✓ |
| Slimmed + tunnel ("the 200 MB" figure) | ~200 MB | ~30 MB ✓, tight |
| Slimmed + DAB+ | ~175 MB | ~55 MB ✓ |
| Slimmed + tunnel + DAB+ | ~240 MB | over ✗ |

Armbian enables zram by default (`armbian-zram-config`, typically 50 % of RAM). That is the right swap for this board:
nothing goes to the card. zram is headroom for idle pages, though, not a budget to plan for. A real-time audio path that
pages through zram costs CPU on four 1 GHz A7s, and the box is already thermally marginal (§4). Stuart's Android
observation is the same thing: "the CPU/RAM breathing" on phones with zRAM.

### The feature set that fits

**On Armbian as it stands:** one RTL at 1.024–2.048 MS/s. WFM stereo with standard RDS; NFM, AM and SSB on HF through the
upconverter. Waterfall at 10 fps. A handful of listeners on the LAN. One decoder at a time. **Tunnel off** (LAN or a port
forward). **DAB+ off**, or tried and watched. **Advanced RDS off.** The Pi 2 already runs without it; it is a CPU limit,
not RAM.

**On a slimmed Armbian or DietPi:** the above **plus either** the tunnel (listing to the world) **or** DAB+, not both.

**Trade-offs, plainly:**

- **The tunnel is ~60 MB of RAM for "shared to the world".** On this board that is the choice that decides what else
  fits.
- **RAM↔CPU:** zram turns a RAM shortage into CPU work, and that CPU work raises the temperature.
- **Rate↔CPU:** a lower rate saves CPU and almost no RAM. 1.024 MS/s is the benchmark's floor (RTLs struggle below
  ~1 MHz).
- **Advanced RDS** is the accuracy trade-off that is already off on the Pi 2. It stays off here.

**Is there a low-memory profile to reuse?** Not a RAM one. `"lite"` in the shim marks a 32-bit ARM build and shows the
Lite-only setup choices. `vibe_thread_layout.h` sets `VIBE_DSP_THREADS=1`/`VIBE_DAB_SPLIT=1` on small 32-bit hosts. The
first-setup **benchmark** switches red features off, but it measures **CPU only**. `vibe_hwinfo.h` already reads
`MemTotal` (`ramMb`) and nothing acts on it. That is the gap (§6, item 1).

## 4. CPU and heat

- **The core.** Cortex-A7, the same as the Pi 2. Armbian runs the H2+/H3 Orange Pi Zero at up to **1.2 GHz** (DVFS),
  against the Pi 2's 900 MHz. That is ~10–30 % more clock per core. Two things give some of it back: the Orange Pi Zero
  has a **16-bit DDR3 bus**, and USB 2.0 on the same SoC. **Expect Pi 2 class.**
- **What the Pi 2 does** (`armhf_one_vibeserver.md`, `pi2_*`): WFM stereo at 2.4 MS/s for one listener ≈ **1.7 of
  4 cores**. DAB MP2/DAB+ in real time with `VIBE_DAB_SPLIT=1`. It does not run Advanced RDS alongside audio. ADS-B at
  2.4 MS/s works (~370 msg/s at fixed gain).
- **Heat is the real limit.** The board reads 70 °C at idle in a closed box. H2+/H3 boards are known to run hot. Under a
  ~1.7-core load it will reach the kernel's passive trip and step down the operating points, and the throttle trips are
  unverified for Danny's kernel. Throttling mid-stream is what causes buffer drops. Our `vibeserver-governor` sets
  `performance`, which holds the top clock. On this board, **cap the clock** (`scaling_max_freq` = 1008000) instead of
  letting the kernel throttle unpredictably, and add a heatsink and vents.
- **Realistic rates:** **1.024 MS/s** to start. **2.048 MS/s** for WFM stereo once the temperature is seen to settle
  below the trip. **2.4 MS/s** only for ADS-B, and only with cooling. The setup benchmark is the arbiter: run it on the
  board.

## 5. The hardware: upconverter, ports, switching

What the two rtl-sdr.com articles say:

- The Kickstarter article says the board has "a **50 MHz upconverter** and RF amplifier", room for an Orange Pi Zero and
  an RTL-SDR, OpenWebRX preloaded, and a script to choose between OpenWebRX and HDSDR (presumably rtl_tcp).
- The earlier article (Soft66IP with rtl_tcp) gives 3 kHz–1.7 GHz, with 15–24 MHz "covered by oversampling". It also says
  the Soft66IP is the Soft66Q's design minus its HF filter. **The Soft66Q is direct-sampling.**

**Not stated anywhere I could read:** the exact LO, which SMA feeds which path, whether the paths are switched or summed,
and any GPIO control. "50 MHz upconverter" most likely means an LO of 50 MHz, so HF appears at 50 MHz + f. That is
**unconfirmed**, and so is whether this version samples HF directly instead.

**What VibeServer already has:**

- **A converter LO setting**, per radio, on the setup page (`#hwConverter`: "Converter LO (MHz)", a down-converter
  tick-box and "Highest frequency it passes"). It is applied at the single tuner boundary (`tuneHw()` in
  `local_sdr_shim.cpp`), so listeners, bookmarks, the band plan and the directory all see true RF. For an LO of 50 MHz,
  enter **LO 50, Highest 30**. The dial then offers HF only.
- **Automatic direct sampling**, next to the converter on the same page. Off by default. If the Soft66IP actually feeds
  HF to the RTL's direct-sampling input (Q branch), this is the setting, not the converter.
- **The converter is all-or-nothing per radio.** One LO, and the pass-band narrows the dial. Reaching the VHF/UHF port
  means setting LO back to 0. There is **no "converter below 30 MHz only, straight through above"** mode. If both ports
  are summed into the dongle, that mode would be the natural feature. Under the "one radio" rule it is a converter
  setting that works on any driver, so it is acceptable. Build it only once Danny confirms the topology.
- **An antenna switch** exists (`vibe_antswitch.h`): **MQTT only** (Tasmota / Home-Assistant relays), still gated off
  on main and V12. **No GPIO backend.** If the board has a relay on an Orange Pi GPIO, that would be a new backend, a
  local `gpiod` line. Do not start it until the hardware is known.

**A Soft66IP preset button** beside "Ham It Up" / "SpyVerter" is cheap. Add it **only once the LO is measured.** A
button that types a wrong number is worse than none.

## 6. Options, ranked

| | Route | Effort | What we maintain | Licensing | Update without re-flash |
|---|---|---|---|---|---|
| **(a)** | **Armbian (Debian minimal preferred) + `install.sh`** — an ordinary VibeServer | **None to try.** Small follow-ups (below) | Nothing new. Same armhf package | We ship nothing of Armbian's | ✓ apt / admin **Install updates**, exactly like a Pi in a loft |
| (a′) | DietPi + `install.sh` | None | Nothing | We ship nothing | ✓ (Debian underneath). DietPi's own tooling owns the OS, so never mix it with the Portable converge |
| (b1) | **Armbian Portable image by customisation**: download Armbian's published minimal image for `orangepizero`, chroot (qemu-arm, as `build-image.sh` does for armhf) and run our `customise.sh` | Medium: days, plus board testing | A second image line. A board branch in the converge for `armbianEnv.txt`, `armbian-zram-config` and Allwinner Wi-Fi | **Redistributing** a modified Armbian image means meeting GPL source obligations for its kernel and U-Boot (point at Armbian's sources/tags; a written offer). **Never copy Armbian code into our tree** | ✓ once the converge knows the board. That is the 2026-10-13 gate |
| (b2) | Armbian **build framework** (`armbian/build`, `userpatches/customize-image.sh` calling our `customise.sh`) | Medium–high: reproducible, but a heavy build host (Docker, ~20+ GB) | The same as (b1), plus a pinned framework version | The framework is GPL-2.0; using it as a tool is fine | ✓ the same as (b1) |
| (c) | A DietPi-based Portable image | High | Our converge would fight `dietpi-*` | — | Possible, but awkward |

**Recommendation: (a) now.** It needs no work from us. It keeps "full update without re-flash" trivially, because it is
an ordinary apt server. It is also what Stuart's rule for ordinary servers expects: the **OS owns the network**, and we
never offer to manage it. A Soft66IP sits on Ethernet in a box, so it needs none of the Portable's hotspot machinery.

**(b1) only if** Soft66IP owners want a flash-and-go card. Even then, the XR819 is a poor fit for the Portable's own
hotspot and captive page: xradio's AP mode has a weak reputation, and every Wi-Fi quirk in `vibeserver-pocket` is
brcmfmac. An Ethernet-first "Portable lite" without the captive hotspot would be the realistic scope. That is a product
decision for Stuart.

### Things we would build (none started)

1. **RAM awareness in setup.** The server already reports `ramMb`. Below ~400 MB, the setup page would show the RAM
   cost beside **Enable tunnel** (~60 MB) and DAB+ (~40 MB) and default both off. The benchmark would gain a RAM row
   (RSS read from `/proc/self/status` while each scenario runs), so its verdict covers RAM as well as CPU. This also
   helps 512 MB Pi 3A+ Portables and old Android boxes.
2. **The trimmed-Armbian recipe** (a doc, not code): what to disable, measured on a real board.
3. **A Soft66IP converter preset**, once the LO is measured. **"Converter below X MHz only"**, if the ports are summed.
   A GPIO antenna-switch backend, if there is a relay.
4. **Tidy:** `Recommends: vibeserver-mapdata-detail` names a package that is not in the repository.
5. **(b1) only if Stuart wants it:** a board abstraction in `vibeserver-pocket-converge` (Pi `config.txt` vs
   `armbianEnv.txt`, `rpi-swap` vs `armbian-zram-config`), plus `build-image.sh <board>`.

## 7. What Danny could try today (route a)

Over ssh on the Orange Pi Zero, on Ethernet:

```bash
# 1. What we are working with — send us the output
uname -m; dpkg --print-architecture; . /etc/os-release; echo "$PRETTY_NAME"
grep -m1 -ow neon /proc/cpuinfo; free -m; zramctl
cat /sys/class/thermal/thermal_zone0/temp
ps -eo rss,comm --sort=-rss | head -15          # what is using the 102 MB already

# 2. If the card is the Soft66IP's own OpenWebRX/rtl_tcp image (not a fresh Armbian), stop whatever
#    holds the dongle first, e.g.:  sudo systemctl disable --now openwebrx rtl_tcp 2>/dev/null

# 3. Install VibeServer (the same one-liner as every Debian/Ubuntu box)
curl -fsSL https://apt.vibesdr.net/install.sh | sudo bash
sudo reboot                                       # loads the DVB blacklist and the plugdev group

# 4. Keep it cool and predictable: cap at 1.0 GHz rather than throttling under load
echo 1008000 | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq
#    (to keep it: MAX_SPEED=1008000 in /etc/default/cpufrequtils on Armbian)

# 5. Set it up
vibeserver                                        # the wizard: name, location, RTL-SDR, admin password
```

In setup, use these settings:

- **Sample rate 1.024 MS/s**, Advanced RDS **off**, tunnel **off** for the first run.
- **Run the benchmark.**
- For HF, set **Converter LO 50, Highest 30**. Then tune a known carrier, such as a time signal on 10.000 MHz or a
  broadcaster, and adjust the LO by the error you see. The crystal will not be exactly 50.000. If HF only sounds right
  with the converter at 0 and direct sampling on, the board samples HF directly. Tell us either way.

While listening, send `free -m`, the temperature and `ps -eo rss,comm --sort=-rss | head`. Do the same again with DAB+
playing, if wanted. Those numbers replace every estimate in §3.

Slimming Armbian is optional and worth measuring before and after:

- Skip unattended-upgrades on a dedicated box (VibeServer updates through its admin page).
- Turn the Wi-Fi radio off when on Ethernet.
- Check what `armbian-ramlog` and the login session hold.
- A fresh **Armbian Bookworm/Trixie minimal** image is the cleanest start.

## 8. Unverified

- Everything above is **untested on an Orange Pi Zero**. Our armhf package has never been installed on real ARMv7
  hardware other than the Pi 2/3: the Mac cannot execute AArch32, so it is cross-built.
- The Soft66IP's LO, port topology, switching and any direct sampling.
- The userland under Danny's Armbian (which Ubuntu release, and whether it still has armhf), and what makes up its
  102 MB idle.
- VibeServer's RSS on 256 MB: per listener, at each rate, during the weekly ASN/GeoIP rebuild, and with V12's ADS-B and
  background decoders. The figures in §3 come from Pi measurements (rc36 / Sept builds) and code reading.
- The H2+/H3 thermal trips and DVFS table on Armbian's 6.18 kernel. The "1.2 GHz" is Armbian's usual ceiling for this
  board.
- Whether the directory lists a port-forwarded server without the tunnel.
- Stuart's public wording for anything below a Pi 2 still applies: *"your mileage may vary and I cannot guarantee that
  it will run"*. The minimum confirmed spec remains the Pi 2 (900 MHz quad ARMv7, 1 GB).

Sources: [Soft66IP: network-connected RTL-SDR with rtl_tcp](https://www.rtl-sdr.com/soft66ip-network-connected-rtl-sdr-rtl_tcp/)
· [Soft66IP: an OpenWebRX RTL-SDR internet server on Kickstarter](https://www.rtl-sdr.com/soft66ip-an-openwebrx-rtl-sdr-internet-server-now-crowd-funding-on-kickstarter/)
