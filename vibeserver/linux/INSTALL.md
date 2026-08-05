# VibeServer on Linux

Turns an SDR into a network receiver for the VibeSDR apps and any browser.
Tested on **Debian 13 (trixie)** on a Raspberry Pi 500 (arm64). Should work on any
Debian/Ubuntu/Raspberry Pi OS.

★ **64-bit only, in practice.** The DSP's NEON path is gated on `__aarch64__`, and a 32-bit
userland silently loses all of it — roughly **13× slower** on the same hardware. Use a 64-bit OS.

---

## Build and install (copy-paste)

```bash
# 1. Build tools and the libraries VibeServer links
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
                    libusb-1.0-0-dev librtlsdr-dev libopus-dev libncurses-dev

# 2. Get the source
git clone https://github.com/Stuey3D/VibeSDR.git
cd VibeSDR/vibeserver

# 3. Build (use all cores; on a Pi with 4 GB drop to -j2 if it runs out of memory)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 4. Make a .deb and install it
cd build && cpack
sudo apt install ./vibeserver_*.deb
```

That last step is `apt install ./…` — **with the `./`**. It tells apt this is a local file, so it
pulls the runtime dependencies from your normal repos. `dpkg -i` would fail on missing
dependencies and leave you to chase them.

### Then

```bash
vibeserver                          # opens the TUI: set the name, location, radio and options
sudo systemctl start vibeserver     # start serving
sudo systemctl status vibeserver    # check it came up
journalctl -u vibeserver -f         # watch the log
```

The service is **enabled but not started** by the install, on purpose: an unconfigured server that
starts itself and fails is a worse first impression than one that waits to be asked. It will start
automatically on every boot from then on.

★ **Log out and back in after installing.** The package adds you to the `plugdev` group so you can
open the radio without root, and group membership only takes effect on a new login.

---

## Where things go

| File | Purpose |
|---|---|
| `/usr/bin/vibeserver` | the binary — run it with no arguments for the TUI |
| `/etc/vibeserver/vibeserver.conf` | your settings. **Yours** — upgrades never overwrite it |
| `/usr/lib/systemd/system/vibeserver.service` | the service |
| `/usr/lib/udev/rules.d/99-vibeserver-sdr.rules` | USB access without root |

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
cd VibeSDR && git pull
cd vibeserver && cmake --build build -j$(nproc)
cd build && cpack && sudo apt install ./vibeserver_*.deb
```

Your config survives. The service restarts on the new binary.

## Before you start: the SDRplay RSP needs one extra download

★★★ **`apt install vibeserver` cannot install it for you, and this is not an oversight.** The
SDRplay API is SDRplay's own driver, distributed under their licence — we are not permitted to
redistribute it, so it can never be a `Depends:` line. RTL-SDR and Airspy HF+ need nothing extra;
only the RSP family does.

```sh
# https://www.sdrplay.com/downloads  →  "API/HW Driver for Linux (ARM64)"
chmod +x SDRplay_RSP_API-*.run && sudo ./SDRplay_RSP_API-*.run
systemctl status sdrplay_apiService     # should be active
```

★ Do this **before** starting VibeServer. Without it the server runs perfectly and reports no
radio, which reads as broken hardware rather than a missing driver — so the message it prints
names the download.

★ **Adding an RSP later?** Install the SDRplay API whenever you like, then
`sudo systemctl restart vibeserver` — the driver is loaded at runtime, so a restart is all it
takes. No reinstall, and your settings are untouched.
