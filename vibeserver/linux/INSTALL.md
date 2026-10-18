# VibeServer on Linux

Turns an SDR into a network receiver for the VibeSDR apps and any browser.
Runs on **Debian 12 or 13, Ubuntu 24.04 or newer, and Raspberry Pi OS** (bookworm or trixie, 32-
or 64-bit), and on distributions built on them. Tested on Debian 13 (trixie) on a Raspberry Pi 500
(arm64) and Ubuntu 24.04 on an Intel i5. **Ubuntu 22.04 and Debian 11 are too old** — their
libraries predate the ones the package is built against (on 22.04 it is `libhackrf0`, measured
2026-10-18), and apt cannot install it there.

## Hardware

One package for every architecture — apt picks **arm64**, **amd64**, **armhf** (32-bit ARM) or
**i386** (32-bit Intel/AMD) to suit the machine. There is no separate "Lite" build on Linux.

- **Minimum confirmed hardware: Raspberry Pi 2** — quad-core ARMv7 at 0.9 GHz.
- **RAM:** 256 MB will run it; **512 MB recommended** for headroom.
- **32-bit ARM without NEON** — the ARMv6 Pi Zero, Zero W and Pi 1 — gets a scalar ARMv6 build
  from the same armhf package (postinst picks it by CPU), and runs with restrictions.
- **32-bit x86 needs SSE2** — any Pentium 4, Pentium M, Atom (the N270 netbooks included) or
  Athlon 64. A Pentium III or Athlon XP has none, and the package refuses to install there rather
  than fail at runtime. SDRplay publish no 32-bit x86 driver, so RSPs are not offered on i386.
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


That is the whole installation. Every runtime dependency — libusb, librtlsdr, libopus, ncurses,
curl, gzip — is declared by the package and fetched by apt automatically.

★ **The one exception is the SDRplay RSP driver**, and it is not an oversight: SDRplay distribute
it under their own licence and we are not permitted to redistribute it, so it can never be a
`Depends:` line. RTL-SDR and Airspy HF+ need nothing extra. See the section at the end.

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

## Before you start: the SDRplay RSP needs one extra download

★★★ **`apt install vibeserver` cannot install it for you, and this is not an oversight.** The
SDRplay API is SDRplay's own driver, distributed under their licence — we are not permitted to
redistribute it, so it can never be a `Depends:` line. RTL-SDR and Airspy HF+ need nothing extra;
only the RSP family does.

On a headless box, download it from the command line — there is no browser to use:

```sh
curl -fL -o SDRplay_RSP_API-Linux-3.15.run \
  "https://sdrplay.com/download/hardware-api-linux/?wpdmdl=1906"
chmod +x SDRplay_RSP_API-Linux-3.15.run
sudo ./SDRplay_RSP_API-Linux-3.15.run
```

★ It is INTERACTIVE: it pages a licence (space to scroll), then asks twice for `y`. One file
covers every architecture — it carries `armhf`, `amd64` and `arm64` inside, so there is no separate
Pi download, despite the website's wording.

★★ **THE OLD `sdrplay.com/software/….run` LINKS ARE DEAD.** SDRplay moved their downloads behind a
WordPress download manager in 2026 and every `SDRplay_RSP_API-Linux-3.15.x.run` address now 404s —
including the one this page used to give (Stuart hit it on 2026-09-11). The address above is their
current one and, importantly for a headless box, it resolves under `curl` with no browser and no
User-Agent games: it redirects to SharePoint with a short-lived token that curl follows by itself.
★ `wpdmdl=1906` is a download-manager file id, stable for this file but almost certainly new when
they publish the next version. If it fails, the page to read is <https://www.sdrplay.com/api/>.
★ Verified 2026-09-11: 499 KB, "SDRplay RSP API Install Package V3.15 (installer v2)".

★★ **Then reboot.** The installer asks for it, and it means it: the service and the USB rules both
need to come up cleanly, and a replug only does half of that.

```sh
sudo reboot
# then, once it is back:
systemctl status sdrplay        # should be active (running)
```

★ The unit is **`sdrplay`**. It was `sdrplay_apiService` in older API versions, and this document
said so for a while — checked against a real 3.15 install on 2026-08-07, which is the only way
that sort of thing gets found.

★ Do this **before** starting VibeServer. Without it the server runs perfectly and reports no
radio, which reads as broken hardware rather than a missing driver — so the message it prints
names the download.

★ **Adding an RSP later?** Install the SDRplay API whenever you like, then
`sudo systemctl restart vibeserver` — the driver is loaded at runtime, so a restart is all it
takes. No reinstall, and your settings are untouched.

---

## Building from source (developers only)

You do **not** need this to run VibeServer — the package above is the supported route. This is for
working on the code.

```bash
sudo apt install -y build-essential cmake git pkg-config \
                    libusb-1.0-0-dev librtlsdr-dev libopus-dev libncurses-dev
git clone https://github.com/Stuey3D/VibeSDR.git
cd VibeSDR/vibeserver
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cd build && cpack && sudo apt install ./vibeserver_*.deb
```
