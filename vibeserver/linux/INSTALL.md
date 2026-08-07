# VibeServer on Linux

Turns an SDR into a network receiver for the VibeSDR apps and any browser.
Tested on **Debian 13 (trixie)** on a Raspberry Pi 500 (arm64). Should work on any
Debian/Ubuntu/Raspberry Pi OS.

★ **64-bit only, in practice.** The DSP's NEON path is gated on `__aarch64__`, and a 32-bit
userland silently loses all of it — roughly **13× slower** on the same hardware. Use a 64-bit OS.

---

## Install

Two commands to add the repository, then one to install. **`apt install` pulls in everything
VibeServer needs** — there is no list of libraries to chase, and nothing to build.

```bash
# 1. Trust the signing key and add the repository (once, ever)
curl -fsSL https://apt.vibesdr.net/KEY.gpg \
  | sudo gpg --dearmor -o /usr/share/keyrings/vibesdr.gpg
echo "deb [signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net stable main" \
  | sudo tee /etc/apt/sources.list.d/vibesdr.list

# 2. Install
sudo apt update
sudo apt install vibeserver
```

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
the update channel, so there is nothing to download by hand and nothing to rebuild.

## Before you start: the SDRplay RSP needs one extra download

★★★ **`apt install vibeserver` cannot install it for you, and this is not an oversight.** The
SDRplay API is SDRplay's own driver, distributed under their licence — we are not permitted to
redistribute it, so it can never be a `Depends:` line. RTL-SDR and Airspy HF+ need nothing extra;
only the RSP family does.

On a headless box, download it from the command line — there is no browser to use:

```sh
curl -fLO https://www.sdrplay.com/software/SDRplay_RSP_API-Linux-3.15.2.run
chmod +x SDRplay_RSP_API-Linux-3.15.2.run
sudo ./SDRplay_RSP_API-Linux-3.15.2.run
```

★ It is INTERACTIVE: it pages a licence (space to scroll), then asks twice for `y`. One file
covers every architecture including ARM64 — there is no separate Pi download, despite the
website's wording.

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
