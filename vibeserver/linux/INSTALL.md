# VibeServer on Linux

Turns an SDR into a network receiver for the VibeSDR apps and any browser.
Runs on **Debian 12 or 13, Ubuntu 22.04 or newer, and Raspberry Pi OS** (bookworm or trixie, 32-
or 64-bit), and on distributions built on them. Tested on Debian 13 (trixie) on a Raspberry Pi 500
(arm64) and Ubuntu 24.04 on an Intel i5; installed and started in clean Debian 12, Debian 13,
Ubuntu 22.04 and Ubuntu 24.04 containers. **Debian 11 and Ubuntu 20.04 are too old** — the package
needs glibc 2.35, and apt cannot install it there.

★ Ubuntu 22.04 needs a package **newer than 11.0.5-1**. Earlier packages linked the distribution's `libhackrf0`
and asked for a newer one than 22.04 has; since then every radio and codec library is built into
VibeServer itself, so the only library the package names is the C library.

## Hardware

One package for every architecture — apt picks **arm64**, **amd64**, **armhf** (32-bit ARM) or
**i386** (32-bit Intel/AMD) to suit the machine. There is no separate "Lite" build on Linux.

- **Minimum confirmed hardware: Raspberry Pi 2** — quad-core ARMv7 at 0.9 GHz.
- **RAM:** 256 MB will run it; **512 MB recommended** for headroom.
- **32-bit ARM without NEON** — the ARMv6 Pi Zero, Zero W and Pi 1 — gets a scalar ARMv6 build
  from the same armhf package (postinst picks it by CPU), and runs with restrictions.
- **32-bit x86 needs SSE2** — any Pentium 4, Pentium M, Atom (the N270 netbooks included) or
  Athlon 64. A Pentium III or Athlon XP has none, and the package refuses to install there rather
  than fail at runtime. RSPs are not supported on i386.
- Older or lower-end hardware will most likely run with restrictions: on a small machine the
  setup page measures what it can carry and switches heavy features off.

---

## Install

One line, the same on every machine:

```bash
curl -fsSL https://apt.vibesdr.net/install.sh | sudo bash
```

It works out the system and the processor, checks the signing key's fingerprint
(`2B54962707D8119929E1FB2BFC384096D89F0AE7`) before trusting it, adds the repository as the steps
below do (naming this machine's own architecture, as apt.vibesdr.net's copy of them does), and
installs the right package. **Run it again at any time** — it upgrades
VibeServer and changes nothing else, and it repairs an older or duplicated repository line rather
than adding another. On a Mac it offers the notarised app instead (run it without `sudo` there).
The script is `scripts/install.sh` in this repository; every apt publish copies it to the site.

Options go after `bash -s --`, or as environment variables, which pass through `sudo`:

| | |
|---|---|
| `--auto-updates` / `VIBE_AUTO_UPDATES=1` | VibeServer updates itself (unattended-upgrades, **this repository only** — see below) |
| `--no-auto-updates` / `VIBE_AUTO_UPDATES=0` | turn that off again |
| `--dry-run` / `VIBE_DRY_RUN=1` | say what would be done, change nothing |

### Or by hand

Two commands to add the repository, then one to install. **`apt install` pulls in everything
VibeServer needs** — there is no list of libraries to chase, and nothing to build.

```bash
# 1. Trust the signing key and add the repository (once, ever)
curl -fsSL https://apt.vibesdr.net/KEY.gpg \
  | sudo gpg --dearmor -o /usr/share/keyrings/vibesdr.gpg
echo "deb [arch=arm64,amd64,armhf,i386 signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net stable main" \
  | sudo tee /etc/apt/sources.list.d/vibesdr.list

# 2. Install
sudo apt update
sudo apt install vibeserver
```

> **Naming the architectures is deliberate** — it lists exactly what we publish. ★ `armhf` joined
> on 2026-09-22 (32-bit Raspberry Pi OS on a Pi 2/3/4). An older line without it still works on a
> 64-bit machine, but a 32-bit Pi needs `armhf` in the list or apt will never see its package.
> `i386` joined on 2026-10-17 for the same reason: a 32-bit-only x86 machine (an Atom N270
> netbook) sees nothing without it.
>
> ★ It says `arm64,amd64`, not `arm64`. It was `arm64` alone from before x86 was supported, and
> that is not merely untidy on an Intel or AMD box — it tells apt to look for arm64 packages only,
> so `apt install vibeserver` reports no installation candidate on a machine we DO ship for.


That is the whole installation. Every library VibeServer uses — libusb, librtlsdr, the Airspy and
HackRF drivers, libopus, ncurses, the C++ runtime — is **built into it**, so it does not depend on
your distribution's versions of any of them. The package needs only the C library and three
everyday tools (`curl`, `gzip`, `adduser`), which apt fetches if they are missing. DAB+ and DRM audio
are decoded by FAAD2, built in, and xHE-AAC (some DRM stations) by **libxaac**, built in too (Apache-2.0 —
see `/usr/share/doc/vibeserver/NOTICE.libxaac`), so there is nothing extra to install. If the machine
has `ffmpeg` installed, the setup page also offers it as an alternative DAB+ decoder.

★ **The SDRplay RSP driver is included too** on amd64, arm64 and armhf, as its own small service
(`vibeserver-rsp`). If SDRplay's own API is already installed, VibeServer uses that instead. See the
section at the end.

### Prefer a file?

A standalone `.deb` is published with each release. Install it with **`apt install ./…`** — with
the `./` — so apt still resolves the dependencies for you:

```bash
sudo apt install ./vibeserver_2.0.0-3_arm64.deb
```

★ Use `apt install ./file.deb`, never `dpkg -i file.deb`. `dpkg` does not resolve dependencies: it
fails part-installed and leaves you to work out what was missing, which is exactly the experience
this package exists to avoid.

### Then

```bash
vibeserver          # a short wizard: name, location, radio, admin password
```

When you finish the wizard it **enables and starts the service for you** — there is no separate
`systemctl start` to run. Then open the receiver in a browser:

```
http://<the address the wizard printed>:48000
```

★ **Log out and back in before running `vibeserver`.** The package adds you to the `plugdev` group
so the radio can be opened without root, and group membership only takes effect on a new login.
Skip it and the wizard reports no radio.

```bash
sudo systemctl status vibeserver     # check it came up
journalctl -u vibeserver -f          # watch the log
```

### ★ The waterfall looks quiet at first, deliberately

A new install starts the radio at **minimum gain, in manual mode**. An unknown antenna on an
unknown band can overload a front end the instant it is switched on, and a receiver that starts
safe and sounds quiet is much easier to recover from than one that starts hot and distorts
everything.

Open the menu and bring the gain up until the noise floor lifts. Whatever you set is remembered,
and upgrades never change a gain you have touched.

## Where things go

| File | Purpose |
|---|---|
| `/usr/bin/vibeserver` | the binary — run it with no arguments for the TUI |
| `/etc/vibeserver/vibeserver.conf` | your settings. **Yours** — upgrades never overwrite it |
| `/usr/lib/systemd/system/vibeserver.service` | the service |
| `/usr/lib/udev/rules.d/99-vibeserver-sdr.rules` | USB access without root |
| `/usr/lib/vibeserver/vibeserver-maintenance` | run as root by systemd for the admin page's Reboot / Restart / Update buttons |
| `/var/lib/vibeserver/` | state: ban list, spectrogram, EiBi schedule, country and network data |

Uninstall with `sudo apt remove vibeserver` (keeps your config) or
`sudo apt purge vibeserver` (removes it).

---

## If it cannot find your radio

```bash
lsusb                    # is the radio actually enumerated?
groups                   # are you in plugdev? (log out and back in if not)
journalctl -u vibeserver -n 50
```

**"could not open … (is another program using it?)"** means the device was found but could not be
claimed. Either something else has it — OpenWebRX, another VibeServer, a stray `rtl_test` — or the
udev rules have not taken effect yet. Try:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

and unplug/replug the radio.

---

## Updating

```bash
sudo apt update && sudo apt upgrade
```

Your settings survive, and the service restarts on the new binary. That is all — the repository is
the update channel, so there is nothing to download by hand and nothing to rebuild. The admin
page's **Install updates** button does the same, and so does running the one-line installer again.

### Automatic updates (opt-in)

```bash
curl -fsSL https://apt.vibesdr.net/install.sh | sudo VIBE_AUTO_UPDATES=1 bash
```

This writes one file, `/etc/apt/apt.conf.d/52vibesdr-auto-updates`, for unattended-upgrades:

- **If the machine was not already updating itself**, the file limits unattended-upgrades to this
  repository (`origin=VibeSDR,label=VibeSDR,site=apt.vibesdr.net`) and nothing else — it clears the
  Debian/Ubuntu origin lists, so turning it on for VibeServer cannot quietly start upgrading the
  operating system. If you later want automatic OS updates too, delete the file first.
- **If it already was**, our origin is added to the ones it had, and nothing else changes.

`--no-auto-updates` (or deleting the file) undoes it.

### Beta updates (opt-in) — and going back

From 11.0.10-4 the admin page's **Maintenance** section has a tick-box, **Receive beta updates (V12)**.

- **Tick it** and the server adds one file, `/etc/apt/sources.list.d/vibesdr-beta.list`, naming the `beta`
  suite of the same repository with the same key, then runs `apt update` straight away. The line under
  the buttons then names the newest beta, e.g. *"12.0.0~beta1-1 available (beta)"*. Nothing is installed
  until you press **Update VibeServer**.
- **Untick it** and that file is removed and `apt update` runs again. On a server running a beta, the
  line then reads *"11.0.10-4 available — back to stable"* and the button becomes **Back to stable
  (11.0.10-4)**. Pressing it installs the newest stable version (a downgrade, asked for by name, with
  `--allow-downgrades`); your settings are kept, and ticking the box again brings the beta back.
- The tick shows what is **on the machine**: it is ticked exactly when that file exists, however it got
  there.
- **Automatic updates and betas.** A scheduled update (the admin page's own schedule, or
  unattended-upgrades from `VIBE_AUTO_UPDATES=1`) installs a beta only on a server whose owner ticked
  the box, because only those servers have the beta suite in their sources. Nothing automatic ever goes
  **back**: apt never downgrades by itself, and only a person pressing **Back to stable** does.

By hand, the tick is:

```bash
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net beta main" \
  | sudo tee /etc/apt/sources.list.d/vibesdr-beta.list
sudo apt update && sudo apt install vibeserver
```

and going back is:

```bash
sudo rm /etc/apt/sources.list.d/vibesdr-beta.list && sudo apt update
apt-cache madison vibeserver                       # the newest "stable/main" line is the one to go back to
sudo apt install --allow-downgrades vibeserver=<that version>
```

★ Settings survive the round trip. 11.0.10-4 onwards keeps every `config.json` key it does not know
(the ones a V12 beta adds) when it saves, so going back to 11 and then forward to the beta again loses
nothing. An 11.x release older than 11.0.10-4 would drop them on its next save.

## SDRplay RSP receivers

★ **Nothing extra to install.** On amd64, arm64 and armhf the package includes an RSP driver, run as
its own service, `vibeserver-rsp`, which starts when an RSP is plugged in. RTL-SDR, Airspy and
HackRF need nothing extra either.

★ **Already have SDRplay's API installed?** Keep it. If `/opt/sdrplay_api/sdrplay_apiService` exists,
`vibeserver-rsp` stays out of the way and VibeServer uses SDRplay's driver (their `sdrplay` service),
exactly as before. Only one RSP service ever runs on a machine. To use the bundled driver instead,
uninstall SDRplay's API and reboot.

★ RSPs are not supported on i386, or on the ARMv6 build used by the Pi Zero, Zero W and Pi 1.

```sh
systemctl status vibeserver-rsp    # with an RSP plugged in: active (running)
```

---

## Building from source (developers only)

You do **not** need this to run VibeServer — the package above is the supported route. This is for
working on the code.

```bash
sudo apt install -y build-essential cmake git pkg-config curl bzip2 libncurses-dev
git clone https://github.com/Stuey3D/VibeSDR.git
cd VibeSDR/vibeserver
linux/build-deps.sh "$HOME/vibe-deps"      # libusb, libopus, librtlsdr from pinned source
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVIBE_LINUX_DEPS="$HOME/vibe-deps"
cmake --build build -j$(nproc)
cd build && cpack && sudo apt install ./vibeserver_*.deb
```

★ `build-deps.sh` builds the same pinned libraries the published package links, statically — so
your build depends on nothing but the C library, like the real one. Leave out `-DVIBE_LINUX_DEPS`
and CMake links your distribution's `libusb-1.0-0-dev librtlsdr-dev libopus-dev` instead (install
those), which is fine for working on the code but makes a package tied to your distribution.
Release packages are built in `vibeserver/linux/Dockerfile.build` (bookworm), which refuses a
package whose `Depends:` names any library but `libc6`.
