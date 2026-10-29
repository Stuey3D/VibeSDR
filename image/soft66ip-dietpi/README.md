# VibeServer for the Soft66IP / Orange Pi Zero (DietPi, experimental)

A ready-to-flash SD card image that turns a **Soft66IP** (Kazunori Miura's RTL-SDR board with a 50 MHz
upconverter and an Orange Pi Zero) into a **VibeServer**. It is for the **original Orange Pi Zero**
(Allwinner H2+, 256 MB or 512 MB). It is not for the Orange Pi Zero 2, 2W, 3 or 3W.

## ⚠ Experimental, and untested on the hardware

- **Nobody has booted this image on an Orange Pi Zero yet.** It was built and checked on a Linux PC
  (details at the end). The first person to flash it is the tester. Please tell us how it goes.
- VibeServer's minimum confirmed spec is a **Raspberry Pi 2** (quad-core ARMv7, 1 GB). The Orange Pi
  Zero has the same kind of processor but only 256 MB of memory. Below a Pi 2, **your mileage may vary
  and we cannot guarantee that it will run.** Narrow modes on HF with a handful of listeners is the
  realistic target.

## What is on the card

- **DietPi** (Debian 13 "Trixie", 32-bit ARM), set to finish its own setup by itself on the first boot.
  DietPi publishes no image for the original Orange Pi Zero, so this one was made the way DietPi
  documents for such boards. DietPi's own installer was run, as hardware model "Generic Allwinner H3",
  on the board's official **Armbian** image. That keeps Armbian's kernel and boot loader for this board.
- Nothing of VibeServer's yet. On the first boot a short script runs our ordinary one-line installer
  (`curl -fsSL https://apt.vibesdr.net/install.sh | bash`). From then on it is an ordinary apt
  VibeServer. It updates from the admin page's **Install updates** button, or with `apt upgrade`. No
  re-flash is needed.
- Settings chosen for 256 MB: the host name `vibeserver`, Ethernet on, Wi-Fi off and SSH on (Dropbear).
  Swap is zram, in RAM and never on the card. The CPU is capped at **1.008 GHz**, because the board
  runs hot in a closed box. The clock and the time zone are UTC, and DietPi's survey is off.
- **No passwords of ours.** The login passwords are DietPi's defaults until you change them at the
  first login (below). The VibeServer admin password is the one you type in the setup wizard.

## Flash it

1. Download `VibeServer-Soft66IP-OrangePiZero-DietPi-1.img.xz` and check it against its `.sha256`:
   - Linux: `sha256sum -c VibeServer-Soft66IP-OrangePiZero-DietPi-1.img.xz.sha256`
   - Mac: `shasum -a 256 -c VibeServer-Soft66IP-OrangePiZero-DietPi-1.img.xz.sha256`
2. Write it to a microSD card of 4 GB or more. **balenaEtcher** takes the `.img.xz` as it is. In
   **Raspberry Pi Imager**, choose *Choose OS → Use custom* and pick the `.img.xz`. Skip Imager's OS
   customisation, which is for Raspberry Pi OS only.
3. *Optional, for Wi-Fi instead of Ethernet:* before the first boot, open the card's root partition.
   In `/boot/dietpi.txt`, set `AUTO_SETUP_NET_WIFI_ENABLED=1`, and put your network in
   `/boot/dietpi-wifi.txt`. The Orange Pi Zero's XR819 Wi-Fi is weak, so Ethernet is the recommended
   way.

## First boot

1. Put the card in the Soft66IP. **Connect Ethernet** to a network with internet access, then power it
   up. The RTL-SDR can stay plugged in.
2. **Wait 10–20 minutes**, or longer on a slow connection. On its own, DietPi updates the system,
   grows the filesystem to fill the card, sets itself up, then installs VibeServer. Leave the power on.
3. Find the box's address: look for `vibeserver` in your router's list of devices. Then log in:

   ```
   ssh root@<address>          (password: dietpi)
   ```

   If the first-boot setup is still running, the login says so. Let it finish, then log in again.
4. **DietPi asks you to choose new passwords** for `root` and `dietpi` at this first login. Do it. Any
   password you choose is yours alone; none is stored in the image.
5. Set up VibeServer:

   ```
   vibeserver
   ```

   The wizard asks for a name, the location, the radio (the RTL-SDR) and **an admin password**. When
   you finish, it starts the server.
6. Open **`http://<address>:48000`** in a browser and finish setup there (48000 is VibeServer's first
   port; the wizard prints the exact address). Once it has a name, the server also answers as
   `<name>.local` on most networks.

If VibeServer did not install (no internet during the first boot, for example), run the installer
again by hand: `curl -fsSL https://apt.vibesdr.net/install.sh | sudo bash`. The first-boot log is
in `/root/vibeserver-firstboot.log`.

## Recommended settings for 256 MB

In the browser setup page, for the RTL-SDR:

- **Span (sample rate): 1.024 MS/s** to start. Try 2.048 MS/s for FM stereo only once the board's
  temperature is seen to settle. Rate costs CPU, and CPU makes heat.
- **Run benchmark.** It tells you what this board can run.
- **Tunnel / listing: off** for the first run. It is off on a new server. The tunnel uses about
  60 MB, which on this board decides what else fits. Listeners on your own network, or through a port
  forward, need no tunnel.
- **DAB+: nothing to install.** VibeServer decodes DAB+ audio itself (FAAD2, built in). Run the
  benchmark first to see whether this board keeps up with a DAB multiplex, and untick DAB under
  *Modes and decoders* if it does not.
- **Advanced RDS: off.** It is a CPU cost that a Pi 2 also runs without. It only runs while a
  listener has its panel (ADV RDS) open.
- **CPU governor:** VibeServer holds the clock at its top, which is 1.008 GHz here. If the box runs
  too hot, choose *default* to let it slow down when idle.
- Keep cooling in mind: a heatsink on the H2+ and some vents in the case. Check the temperature with
  `cat /sys/class/thermal/thermal_zone0/temp` (in thousandths of °C).

## HF through the upconverter

The Soft66IP's HF path is described as a **50 MHz upconverter**: an HF signal at *f* reaches the
RTL-SDR at 50 MHz + *f*. **We have not confirmed this**, nor which antenna socket feeds which path,
nor whether the board switches or sums the two. If you can, please tell us.

- In setup, set **Converter LO (MHz) = 50** and **Highest frequency it passes (MHz) = 30**. The dial
  then shows true HF frequencies, everywhere: to listeners, in bookmarks, in the band plan and in the
  directory.
- **Calibrate:** tune a known carrier, such as a time signal on 10.000 MHz or a broadcaster on a
  published frequency. Correct the LO by the error you see. The board's oscillator will not be
  exactly 50.000 MHz.
- If HF only sounds right with the converter at 0 and **Automatic direct sampling** on, the board feeds
  HF to the RTL's direct-sampling input instead. Tell us either way.
- The converter setting applies to the whole radio. To use the VHF/UHF socket, set the LO back to 0.

## Please send us

From an ssh login, once it is running:

```bash
uname -m; dpkg --print-architecture; . /etc/os-release; echo "$PRETTY_NAME"
grep -m1 -ow neon /proc/cpuinfo; free -m; zramctl
cat /sys/class/thermal/thermal_zone0/temp
ps -eo rss,comm --sort=-rss | head -15
```

Run it **once idle**, and **again while listening** (and with DAB+ playing, if you tried it). These
numbers replace our estimates. Post them in the VibeSDR Discord or in an issue at
<https://github.com/Stuey3D/VibeSDR/issues>.

## Credits, licences and source code

This image is built from other people's free software. **It is not made or endorsed by DietPi or by
Armbian.**

- **DietPi** — <https://dietpi.com>, licensed **GPLv2**. Source: <https://github.com/MichaIng/DietPi>.
  This image used DietPi v10.8.2, installed by `dietpi-installer` from branch `master` at
  commit `65a12a6442f8b41f5de1a982a4c271cd50f85fcf`.
- **Armbian** — <https://www.armbian.com>. The base image was
  `Armbian_community_26.11.0-trunk.108_Orangepizero_trixie_current_6.18.57_minimal.img.xz`, verified against Armbian's checksum and its signature by key
  `DF00 FAF1 C577 104B 50BF 1D00 93D6 889F 9F0E 78D5`. Armbian's build framework (GPLv2) is
  <https://github.com/armbian/build>. This image was built from commit `fac57a02f1c6ff2221a3f3099a1094dcdf41be26` (recorded in its `/etc/armbian-release`). It is a community-maintained nightly build, "trunk", and is the only current Armbian image for this board.
- **Linux kernel** (GPLv2): Armbian package `linux-image-current-sunxi` 26.11.0-trunk.108. Source: Linux 6.18.57 from <https://kernel.org>,
  with Armbian's sunxi patches from the armbian/build repository above (`patch/kernel/archive/sunxi-6.18` at that commit).
- **U-Boot** (GPLv2): Armbian package `linux-u-boot-orangepizero-current` 26.11.0-trunk.108, U-Boot 2026.07. Source: <https://source.denx.de/u-boot/u-boot>,
  with Armbian's patches from armbian/build.
- **Debian** packages (various free licences, many GPL). Source for every package:
  <https://deb.debian.org/debian> (`apt source <package>`), or <https://snapshot.debian.org> for the
  exact version.
- **VibeServer** is installed from <https://apt.vibesdr.net> at first boot. It is not part of this
  image.

If you cannot find the corresponding source for any GPL component of this image at those links, ask
in an issue at <https://github.com/Stuey3D/VibeSDR/issues> and we will provide it.

The build script, the `dietpi.txt` values and the first-boot script are in the VibeSDR repository,
under `image/soft66ip-dietpi/`, on the `v12-soft66ip-image` branch.

## What was checked, and what was not

Checked on the build machine:

- The Armbian base image matched its published checksum and Armbian's signature.
- After DietPi's installer had run, the area outside the partition, which holds the boot loader, was
  byte-for-byte the same as Armbian's. That area is the MBR boot code and U-Boot. Only the
  partition's size entry changed.
- Every `dietpi.txt` value was read back. The first-boot script passes `bash -n` and ShellCheck.
- On a throwaway copy of the finished image, booted as an emulated ARM container, the first-boot script
  ran for real. It installed VibeServer 11.0.10-5 (armhf) from the stable channel, and the
  installed binary started under emulation, then stopped as expected at "no SDR found".

**Not checked:** booting on a real Orange Pi Zero; DietPi's first boot on the board; the RTL-SDR,
Ethernet, Wi-Fi, temperatures and memory use on the board; the Soft66IP's converter and ports.
