# VibeServer Portable on other single-board computers — plan

2026-10-25. Branch `worktree-agent-a92c52d618d3e4dee`. Prototype builder: `image/sbc/`.

Stuart asked: *"see if we can create VibeServer Portable ISO's for all the big popular SBC's. If we can bundle
multiple models together based on chip architecture such as say Pi3/Zero2W/4/5 as one ISO and 0/1/2 as another (2 with
neon 0/1 no neon detected on flashing). What I would like is the same onboarding as VibeServer Portable if possible so
if the board has wifi then create a captive wifi for the user to access. If LAN only then VibeServer-Setup.local:48000
should be available … Try and keep the ISO as close to Portable as we can, also may as well upgrade the server in
Portables ISO to 11.0.11 or whatever version we are on for the release build."*

## The short answer

- **The two Raspberry Pi groups already exist.** The current arm64 Portable image boots the Pi 3, 4, 5, Zero 2 W, 400,
  500 and the CM3/CM4/CM5. The armhf image boots every Pi, including the 0, 1 and 2. **But the armhf image cannot run
  VibeServer on a Pi 0 or Pi 1 today**, because of how an image is built (see "Pi 0/1/2" below). That is fixed on this
  branch and needs a release before it reaches a card.
- **Other boards cannot share one image across models the way Pis do.** Allwinner, Rockchip and Amlogic boards each
  need their own boot loader (U-Boot) on the card, written outside the partitions, and that boot loader picks the
  board's device tree. A card made for one board does not boot another. That holds for every OS (Armbian, DietPi,
  vendor images). **What can be shared is everything else.** One build per chip family makes the root filesystem
  once, and each board's card is that root with the board's own U-Boot. So there is one build per family and one
  download per board, at almost no extra cost per board.
- **Base OS for the non-Pi boards: Armbian minimal, not DietPi.** The Portable's Wi-Fi manager is NetworkManager
  from end to end, and DietPi uses ifupdown. DietPi's first boot also needs the internet, which a box whose only way in
  is its own setup hotspot does not have. Armbian is each board's own OS. It boots offline, and NetworkManager is a
  supported choice on it. The Soft66IP DietPi image stays as it is: it is an *ordinary* server image (ssh, then
  `vibeserver`), not a Portable.
- **Onboarding is unchanged.** The Portable's own scripts run on Armbian, with one fix made on this branch (the
  read-only seal). Boards with Wi-Fi raise the open "VibeServer" hotspot. Every board answers as
  `VibeServerSetup.local:48000` on Ethernet from the first boot.
- **Version.** Portable ships **11.0.1** (portable-v11.0.1, 2026-10-14). The current stable is **11.0.10-5**. There is
  no 11.0.11 yet; beta is 12.0.0~beta13. A rebuild is about an hour of machine time per image, but it should wait for
  the release that carries this branch's two package fixes. See "The Portable version bump".
- **Built:** an **Orange Pi One (H3)** Portable image, on the Lenovo, checked only under emulation. It has not been
  booted on a board. See "What was built".

## ★ The name: VibeServerSetup.local, not VibeServer-Setup.local

Portable answers as **`VibeServerSetup.local`** (hostname `vibeserversetup`; `SETUP_HOST` in
`vibeserver/linux/pocket/vibeserver-pocket`). It is the name in the 11.0.1 release notes, in the setup page and in
your own spec of 2026-10-10. Your message today says **`VibeServer-Setup.local`**, with a hyphen. **These are different
names.** mDNS ignores case but not hyphens, so `VibeServer-Setup.local` does not resolve today. I have kept
`VibeServerSetup.local` everywhere so the boards and the Pis match. **Decide:** keep it as it is, or change it on every
Portable (Pi too) in one release. Answering to both is possible with an extra avahi alias record, but it means two
names to document.

## Board families and images

"One build" means one customisation run. "Cards" means the downloads that build produces.

| # | Family (SoC) | Popular boards | Arch / NEON | Base OS | One card for several boards? | Smallest RAM | Onboard Wi-Fi | Status |
|---|---|---|---|---|---|---|---|---|
| 1 | **Raspberry Pi 64-bit** (BCM2837/2711/2712) | Pi 3, 3+, 3A+, Zero 2 W, 4, 400, 5, 500, CM3+/CM4/CM5 | arm64 | Raspberry Pi OS Lite 64-bit | **Yes, one card for all of them** (the Pi firmware picks the kernel and device tree) | 512 MB (3A+, Zero 2 W) | Yes (Compute Modules: wireless variants only) | **Exists** (portable-v11.0.1) |
| 2 | **Raspberry Pi 32-bit** (BCM2835/2836/2837) | Pi 0, 0 W, 1, 2 (and any Pi) | armhf; Pi 2 NEON, 0/1 none | Raspberry Pi OS Lite 32-bit | **Yes, one card**, with the build picked at boot (fixed on this branch) | 256 MB (Pi 1 A+), 512 MB (Zero) | Zero W only. Pi 0 (no W) has no Wi-Fi *and* no Ethernet, so it needs a USB adapter | **Exists**, but ARMv6 broken (see below) |
| 3 | **Allwinner 32-bit** (sunxi: H2+, H3, also A20) | Orange Pi One, PC, PC Plus, Lite, Zero, NanoPi NEO / NEO Air / M1, Banana Pi M2 Zero | armhf, NEON | Armbian minimal (Trixie) | **One build, one card per board** | 256 MB (Zero, NEO), 512 MB (One, Lite) | One, PC, NEO: none. Lite: RTL8189FTV. Zero: XR819. NEO Air / BPi M2 Zero: Broadcom | **Orange Pi One built (prototype)** |
| 4 | **Allwinner 64-bit** (sunxi64: H5, H6, H616, H618, A64) | Orange Pi Zero 3, Zero 2W, Zero 2, 3 LTS, PC2, NanoPi NEO2, Pine A64 | arm64 | Armbian minimal | One build, one card per board | 512 MB (Zero 2), 1 GB (Zero 3) | Zero 2/3/2W: UWE5622 (out-of-tree driver). 3 LTS: Broadcom | Planned |
| 5 | **Rockchip RK3588/RK3588S** | Orange Pi 5 / 5B / 5 Plus / 5 Pro, Radxa ROCK 5B / 5A / 5C, NanoPC-T6 | arm64 | Armbian minimal (vendor 6.1 or mainline) | One build, one card per board. These have SPI flash, so a later generic UEFI card is possible | 4 GB | 5B / 5 Pro: yes. 5, ROCK 5B: none | Planned |
| 6 | **Rockchip 64 older** (RK3328, RK3399, RK3566, RK3568) | Radxa ROCK 4, Zero 3W/3E, ROCK 3A, Orange Pi 3B, Pine64 Rock64 / RockPro64 / Quartz64, ODROID-M1 | arm64 | Armbian minimal | One build, one card per board | 1 GB | Zero 3W / OPi 3B: yes (AIC8800 / Broadcom); others none | Planned |
| 7 | **Amlogic** (meson64: S905X, S905X3, S922X) | Libre Computer Le Potato, Radxa Zero, ODROID-C4, ODROID-N2+ | arm64 | Armbian minimal | One build, one card per board | 1 GB (Le Potato 1 GB, Radxa Zero 1 GB) | Radxa Zero: yes. Le Potato, ODROID: none | Planned |
| 8 | **ODROID-XU4 / HC1** (Exynos 5422) | ODROID-XU4, HC1/HC2 | armhf, NEON | Armbian minimal | Its own card (one board) | 2 GB | None | Low priority |
| — | x86 mini PCs (Wyse 3040, N100 boxes, old netbooks) | — | amd64 / i386 | The ordinary apt install or the one-line installer | n/a, PCs boot any image | — | Varies | Not an image: the apt package already covers them. A "Portable for x86" would be an installer ISO, a separate project |
| ✗ | RISC-V (VisionFive 2, Milk-V) | — | riscv64 | — | — | — | — | No riscv64 VibeServer build; out of scope |

**Order of popularity worth acting on**, from forum and seller volume: Pi (done) → Orange Pi Zero 3 / Zero 2W (cheap,
everywhere, H618) → Orange Pi One / PC / Zero (H3, the oldest cheap boards, and Nick's ask) → Le Potato → Orange Pi 5 /
ROCK 5B → the rest.

### Why one card per board on families 3-8, said plainly

- On a Pi, the GPU firmware on the FAT boot partition reads `config.txt` and loads the right kernel and device tree
  for whichever model it finds itself on. That is why one Pi card boots every Pi.
- On Allwinner, Rockchip and Amlogic boards, the SoC's boot ROM loads a **U-Boot SPL from a fixed raw offset of the
  card** (8 KiB on Allwinner, 32 KiB on Rockchip). That SPL sets up **this board's** DRAM and power chip, then U-Boot
  loads **this board's** device tree. A wrong SPL does not get as far as printing anything. A wrong device tree means
  no Ethernet, the wrong regulators, or worse.
- So every vendor and distribution ships one image per board. Armbian does (`dl.armbian.com/<board>/`), and so do
  DietPi and the vendors. **Neither DietPi nor Armbian offers a multi-board card** for these SoCs. Armbian's only
  generic image is `uefi-arm64`, for machines whose firmware is already UEFI.
- **The exception, later:** boards with **SPI flash firmware** (ROCK 5B, Orange Pi 5 family, ODROID-M1, Pine64 boards
  with Tow-Boot) can hold their own boot loader off the card. With EDK2/UEFI or U-Boot in SPI, one generic arm64 card
  could boot several of them. The owner would have to flash the SPI first, which is a step we cannot ask of a
  beginner. Not recommended for now.
- **What we do instead:** build the family's root once, then make each board's card from it. The kernel package is
  the same across a family (`linux-image-current-sunxi` covers every H2+/H3 board). What changes per board is the
  `linux-u-boot-<board>-current` package and its raw write (`/usr/lib/u-boot/platform_install.sh`), `BOARD=` in
  `/etc/armbian-release`, and the device-tree name in `armbianEnv.txt`. Today `image/sbc/build.sh` builds one board
  from that board's own Armbian image, which is the safe first step. The "re-target a finished root to the next
  board" step is the next piece of work (see the order below).

### Per-family caveats

- **RAM.** The measured baseline is a Pi 2 on Raspberry Pi OS Lite 32-bit with VibeServer at about 205 MB at rest:
  vibeserver ×2 76 MB, cloudflared 63 MB (only when the tunnel is on), NetworkManager 17 MB. A Portable adds avahi and
  NetworkManager, and its RAM overlay grows with every write to the root. **Floor for a Portable image: 512 MB.** The
  256 MB boards (Orange Pi Zero, NanoPi NEO, Pi 1 A+) "may work if restricted to narrow HF modes only, your mileage
  may vary", in your public wording. The Soft66IP DietPi image stays the better choice at 256 MB.
- **USB.** H3: each USB-A port has its own controller, which is good for two SDRs. The Orange Pi Zero has one USB-A
  port. Le Potato: four ports behind one hub, which is fine for one RTL-SDR or Airspy. RK3588: USB 3 ports are fine,
  but the USB 2 ports are the safer choice for an RTL-SDR until tested. **Power is the usual failure.** The H3 boards
  take 5 V on a 4.0/1.7 mm barrel or micro-USB, and an RTL-SDR plus a cheap supply browns out. Recommend a 5 V 2.5 A
  supply minimum.
- **Wi-Fi in access-point mode is the risk.** The Portable's Wi-Fi code was tuned on the Pi's Broadcom chip (brcmfmac):
  power save, PMF/SAE, the `iw scan dump` reading. Other chips have their own drivers and quirks:
  - Broadcom/Cypress (NanoPi NEO Air, Orange Pi 3 LTS, Banana Pi M2 Zero): the same driver as the Pi, so most likely
    to just work.
  - RTL8189FTV (Orange Pi Lite, PC Plus): out-of-tree driver in Armbian, AP mode reported to work.
  - XR819 (Orange Pi Zero): weak and unreliable in AP mode. Treat the Zero as Ethernet-only.
  - UWE5622 (Orange Pi Zero 2/3/2W): out-of-tree driver, AP mode exists, quality unknown.
  - AIC8800 (Radxa Zero 3W and others): out-of-tree driver, unknown.
  Each chip needs the same on-device check the Pi 3 A+ got: open setup hotspot, join home, join a phone hotspot, own
  hotspot, power save off. A board with a weak chip can still be set up on Ethernet, or with a known-good USB adapter.
  MediaTek mt76 (MT7610U/MT7612U) adapters are the safe recommendation, because their in-kernel driver supports AP mode.
- **Interface names.** Armbian's kernel names Ethernet `end0`/`eth0`, depending on the board. The Portable code reads
  device types from NetworkManager, never names, so this does not matter. netplan matches `e*`, `lan*`, `wan*`, as
  Armbian's own file does.

## Pi 0/1/2 on one card: what happens today, and the fix

The armhf package already carries **both builds**: `/usr/bin/vibeserver` (ARMv7 + NEON, the Pi 2's speed) and
`/usr/lib/vibeserver/armv6/vibeserver` (scalar ARMv6). Its `postinst` picks one by reading `/proc/cpuinfo` for
`neon`/`asimd`. That works on a box that installs the package itself.

**On a card image it picks for the wrong machine.** `image/pocket/build-image.sh` installs the package inside a chroot
on the **build host**. On the Mac, that is Docker's arm64 VM, whose cpuinfo says `asimd` (or QEMU's emulated CPU, which
has NEON), so the card always carries the NEON build. The "detected on flashing" step you described does not exist yet:
nothing on the card checks the CPU again. I have **not tried a Pi Zero**, but from the code, a Portable 32-bit card in
a Pi 0 W or Pi 1 would die with "Illegal instruction" on the first DSP call. There would be no setup page, so no
Update could fix it. The 11.0.1 release notes say a Pi 1 or Zero "may work". On this reading, VibeServer itself
cannot start there.

**Fix (this branch, commit 7cb83063): `vibeserver-pocket-cpu`**, a root one-shot that runs on a Portable before the
server and before the seal:

- **No NEON, writable root** (the first boot, before the seal): it installs the ARMv6 build over `/usr/bin/vibeserver`,
  as `postinst` would. The sealed root then carries it.
- **No NEON, sealed root** (a card moved into a Pi Zero later): it bind-mounts the ARMv6 build for that boot. Nothing
  is written and no RAM is spent on a copy. The next Update makes it permanent, because `postinst` then runs on the
  real CPU through `overlayroot-chroot`.
- The bundled SDRplay service is an ARMv7 build, so it stays down on these Pis, as `postinst` already does.
- **NEON present, no Portable marker, or no ARMv6 build: it does nothing.**

The converge enables it, so the image build and every Update get it. It has a test (`test_pocket_cpu.py`) and is
wired into `scripts/run-tests.sh`. **It reaches a card only through a release.** There is a second requirement: the
armhf image must be built from the **published** armhf package, because `image/pocket/build-deb.sh` deliberately leaves
the ARMv6 build out. `image/pocket/fetch-release-deb.sh` (new) does that, with the download checked against apt's
signed index.

The same build-host trap applies to every armhf board image. The Lenovo is x86, so its cpuinfo has *neither* word,
and `postinst` would install the **scalar** build on an H3. `image/sbc/build.sh` gives the chroot an ARMv7+NEON
cpuinfo during the install, then checks the result with `readelf`: `Tag_CPU_arch: v7` and `Tag_Advanced_SIMD_arch:
NEON`.

## Onboarding: what is reused, what is Pi-only, and what replaces it

The whole Portable is the vibeserver package's `vibeserver-pocket*` files plus the image's
`image/pocket/stage-pocket/00-pocket/files/customise.sh`. `image/sbc` runs **that same customise.sh** inside the
Armbian root, with an Armbian step before it and one after (`image/sbc/customise-armbian.sh`).

| Portable piece | Pi-only? | On Armbian |
|---|---|---|
| `vibeserver-pocket`, the Wi-Fi state machine: open setup hotspot, 1/2/3 strict order, own hotspot, wired mode, Wi-Fi off, captive DNS + `:80` redirect, join history, kept Wi-Fi log | No, it is nmcli + iw + nft | **Reused as is.** It needs NetworkManager, which Armbian minimal lacks, so `customise-armbian.sh pre` installs it and switches netplan's renderer to NetworkManager (Armbian's documented way). Ethernet keeps DHCP at boot |
| `VibeServerSetup.local` (hostname `vibeserversetup` until setup is done, then `vibepocket`, then the chosen name), avahi on every interface | No | **Reused as is.** avahi comes from the package list. A LAN-only board (Orange Pi One) is in "wired" mode, so the name answers on Ethernet from the first boot, and a USB Wi-Fi adapter plugged in later gets the hotspot |
| `pocket-packages.txt`, the converge (services on/off/masked, NM connectivity check, journald in RAM, avahi) | Mostly no | **Reused.** It already skips what is not there: no `config.txt` → "no config.txt visible here"; the `/boot/firmware` fstab rule does not match; units that do not exist are skipped. Nothing fails |
| `config.txt` headless block (`gpu_mem`, `disable-bt`, `audio=off`, splash) | **Yes** | Skipped by the converge, correctly. The equivalent Armbian switches (HDMI and audio off through device-tree overlays in `armbianEnv.txt`) are a later, per-family step |
| `vcgencmd display_power 0` | **Yes** | Skipped (no vcgencmd). HDMI stays powered on an H3 until the per-family step above |
| `raspi-config nonint do_wifi_country` | **Yes** | `vibeserver-pocket` already falls back to `iw reg set`, and repeats it at every boot |
| Read-only root: `overlayroot=tmpfs` via `cmdline.txt` (`vibeserver-pocket-seal`) | **Yes, in the file it writes** | **Fixed on this branch (commit 64fa6bff).** With no `cmdline.txt`, the seal writes `extraargs=overlayroot=tmpfs` into `/boot/armbianEnv.txt` (or `dietpiEnv.txt`) and needs `/boot/uInitrd`. It has a test. **11.0.10-5 does not have it yet**, so the prototype card runs with a writable root until a release carries it |
| overlayroot + initramfs | Pi OS packages it; so does Debian (`overlayroot`, from cloud-initramfs-tools) | Installed from Debian by the package list. Armbian's kernel hook rewraps the initramfs as `uInitrd`, and `customise-armbian.sh post` proves overlayroot is inside it |
| An Update through `overlayroot-chroot` (`vibeserver-maintenance`) | It binds `/boot/firmware` or `/boot` *if it is a mount point* | **Works as is.** On Armbian `/boot` is a directory of root, already inside the chroot |
| The settings partition `LABEL=vibedata`, 256 MB, after root | Layout only | **Same**, but it is partition **2** (Armbian has no FAT boot partition). `vibeserver-pocket-data` finds it by label. **Gap:** its "label lost → remake partition 3" repair looks for partition 3 only, so on Armbian it does nothing (safe, but no repair). It should be generalised to "the partition after root", with a test. Not done: it partitions disks, and it needs care and a real card |
| Pi OS's first-boot root resize (removed from `cmdline.txt`) | Yes | Armbian's switch: `/root/.no_rootfs_resize` |
| A locked login, SSH off | Pi OS specifics | **SSH off** (`ssh.service` disabled), because the first network is an open hotspot. root keeps **Armbian's default password, still flagged for Armbian's forced change at the first login** (`/root/.not_logged_in_yet`). No password of ours is made or stored. The converge masks the console gettys (HDMI and serial `ttyS0`), as on the Pi, so nobody logs in at all until the owner re-enables one |
| `/boot/firmware/vibeserver-reset-wifi` (forget Wi-Fi from a card reader) | Pi: FAT, readable on any computer | On Armbian `/boot` is ext4, which Windows and macOS cannot write. `/boot/vibeserver-reset-wifi` still works from a Linux PC. **Decide:** whether a small FAT partition is worth adding for this |
| High Detail Maps baked in | No | **Same** (177 MB, verified as the server does) |

## The Portable version bump

- **Portable today:** `portable-v11.0.1`, images `vibeserver-pocket-{arm64,armhf}-20261014`, carrying **11.0.1-1** from
  apt.
- **Stable today:** **11.0.10-5** (`apt.vibesdr.net` stable; `project(vibeserver VERSION 11.0.10)` on main). There is
  **no 11.0.11**. Beta is 12.0.0~beta13.
- **What a rebuild involves:**
  1. **Get the published packages.** `image/pocket/fetch-release-deb.sh arm64` and `… armhf` (new on this branch)
     fetch the published package through apt's signed index into `image/pocket/out/debs/`. That is the same package
     every apt server gets, and for armhf it is the only one that carries the ARMv6 build. Delete any rev-900
     `build-deb.sh` package there first: customise.sh installs the newest by version.
  2. `image/pocket/build-image.sh arm64`, then `… armhf`, **one at a time**, about 5 GB free each, on the Mac
     (colima) as before. It fetches the newest Raspberry Pi OS Lite, checks it, customises, shrinks and compresses.
  3. Check: boot each card, go through setup, and run **Install updates** once.
- **My recommendation: do not rebuild on 11.0.10-5. Rebuild on the next release (11.0.11), with this branch's two
  package fixes in it.** On 11.0.10-5 the armhf card still cannot run on a Pi 0/1, and the H3/Armbian cards cannot seal
  their root. The fixes are small, have tests, are inert without the Portable marker, and only touch
  `vibeserver/linux/pocket/` plus three CMake lines. Without them, "0/1/2 on one card" stays untrue.
  - Already-flashed Portables get both through Install updates. Neither needs a new card.
- I have not rebuilt the Pi images. The Lenovo was busy with an i386 package build when I started, and the Pi images
  need the release above to be worth flashing.

## What was built (prototype)

`image/sbc/`:

- `boards.txt`: the boards, families, architectures, NEON, Wi-Fi chip, RAM and Armbian channel.
- `build.sh <board> <Armbian…minimal.img.xz> <out>`. It checks the checksum and **Armbian's signature** (the same key
  check as the Soft66IP build), grows the image, and chroots in under QEMU. Then it runs `customise-armbian.sh pre` →
  the Pi Portable's `customise.sh` → `customise-armbian.sh post`. It shrinks root to used + 1 GB, adds
  `vibedata` (256 MB) as partition 2 and compresses with `xz -T2`. It then checks the result:
  - the **boot region** (MBR boot code and U-Boot, everything before the partition) is **byte-identical** to
    Armbian's;
  - the root UUID is unchanged (`armbianEnv.txt` boots by UUID);
  - the units are enabled or disabled as intended;
  - `/usr/bin/vibeserver` is the ARMv7+NEON build;
  - the board's own U-Boot package is present.
  `DRY_RUN=1` prints the plan without root.
- `run-in-docker.sh`: the same build in a throwaway privileged Debian container: 2 CPUs, `nice -n 19`, and one build at
  a time (it refuses to start if another is running). Nothing is installed on the host. The QEMU binfmt entry exists
  for the run only. **No AppArmor or other security change** was needed: unlike DietPi's installer, a plain chroot
  never runs `chpasswd`.

**Orange Pi One build:** see the result section at the end of this file.

## Recommended order, effort and risk

| Step | What | Effort | Risk |
|---|---|---|---|
| 1 | **Orange Pi One (H3) prototype.** Built. Nick tests on Monday: Ethernet → `vibeserversetup.local:48000` → setup → radio → Install updates once | Done; a day of fixes after his report | Medium. First boot of an Armbian Portable: NetworkManager under netplan, avahi on Ethernet, first-boot timing. Root stays writable until step 2 |
| 2 | **Release 11.0.11** with `vibeserver-pocket-cpu` + the U-Boot seal (this branch, rebased on main) | Small: two scripts, a unit, tests | Low. Inert without the marker. Already-flashed Portables get it through Update |
| 3 | **Rebuild both Pi Portable images on 11.0.11** (`fetch-release-deb.sh` + `build-image.sh`) and test a **Pi Zero W** and a **Pi 2** with the armhf card | Half a day of machine time + a Pi Zero test | Low to medium. The Pi Zero has never run a Portable; 512 MB and a single core make it the slowest Portable by far |
| 4 | **Family re-targeting in `image/sbc`**: one sunxi root → Orange Pi PC, Lite, Zero, NanoPi NEO cards (swap the `linux-u-boot-*` package and its raw write, `BOARD=`, the device tree) | 1-2 days | Medium. Each card still needs one real boot before release |
| 5 | **Orange Pi Zero 3 / Zero 2W (H618, arm64)**: the most-sold cheap boards now. Needs the UWE5622 Wi-Fi in AP mode tested | 1 day + hardware | **High** for the Wi-Fi (out-of-tree driver). Ethernet onboarding is low risk |
| 6 | **Le Potato**, then **Orange Pi 5 / ROCK 5B** | ~1 day each + hardware | Medium. No onboard Wi-Fi, so Ethernet onboarding only, the lower-risk path |
| 7 | Per-family "headless" switches (HDMI and audio off through Armbian overlays); generalise `vibeserver-pocket-data`'s repair to "the partition after root" | 1 day | Low |

**Honest overall:** the shared Portable code moves well, and almost nothing in it is Pi-specific. The real cost is
**hardware testing per board**, and above all **per Wi-Fi chip**. Every family needs one person with one board for one
evening before its card is public. And under your public-image rule ("full parity"), no card should ship ahead of the
packages and apps.

## Stuart's decisions (2026-10-25, ~01:15)

1. **The name stays `VibeServerSetup.local`**, with no hyphen. Nothing about it changes.
2. **11.0.11, a stable VibeServer release, carries both fixes**: `vibeserver-pocket-cpu` and the U-Boot seal. Both Pi
   cards and the board cards are built from the **published** 11.0.11 package (`image/pocket/fetch-release-deb.sh`).
3. **Enable SSH** on the admin page, in 11.0.11:
   - default off;
   - the owner types the password;
   - admin-gated;
   - survives the read-only seal.
   Built as follows:
   - the account is the image's own `vibe` login (in sudo; root keeps Debian's no-password-login rule);
   - the password goes to `chpasswd` on stdin;
   - only the system's **hash** is kept, on the settings partition, along with the box's host keys;
   - every boot puts it back and starts sshd;
   - the switch is not offered while the open setup hotspot is up.
4. **Boards with 256 MB stay on the DietPi ordinary-server image** (Orange Pi Zero, NanoPi NEO). **A Portable card
   needs 512 MB or more**; `image/sbc/build.sh` refuses a smaller board and says which image to use instead.
5. **A small FAT partition** for the forget-the-Wi-Fi file:
   - **Board cards** get partition 3, `VIBESETUP`, 16 MB of FAT16, with a `README-VibeServer.txt`.
   - **Pi cards** get no second one. Their boot partition already *is* FAT and every computer can write to it.
   - **Fixed in passing:** on a Pi Portable that partition is mounted read-only. From the code, the file could never
     be deleted after a reset, so the box forgot its Wi-Fi at *every* boot. It is now remounted for the one delete.
   - The file counts in every old place too, and as Notepad's `.txt`.
   - The settings-partition repair now uses "the partition after root" (3 on a Pi, 2 on a board).
6. **The most popular boards are built after 11.0.11 is on apt stable**, one at a time on the Lenovo, each boot-tested
   under emulation:
   - both Pi cards;
   - Orange Pi One, PC and Lite;
   - Orange Pi Zero 3 and Zero 2W;
   - Le Potato;
   - Orange Pi 5;
   - ROCK 5B;
   - Radxa ZERO 3.
   Any board whose Armbian image cannot be verified is skipped.
7. **Download names:** `vibeserver-portable-<board>-<date>`, beside the Pi's `vibeserver-pocket-<arch>-<date>`.

### The H3 cards: one card per board, each from that board's own image

The Orange Pi One's Armbian image is a *stable* 26.8.1 build (kernel 6.18.44). The PC's and the Lite's exist only as
*community trunk* 26.11.0 builds (kernel 6.18.57). Moving one finished root onto another board's U-Boot would put a
26.8.1 system under a 26.11 boot loader, with that board's support packages taken from somewhere other than its own
signed image. Building each card from **its own** signed Armbian image costs about 25 minutes per board and is the
version Armbian itself tested. So "one build" for the H3 family means one recipe (the same customisation, the same
checks) run three times, not one root copied three ways.

## Result: the Orange Pi One build (2026-10-25)

**`vibeserver-portable-orangepione-20261024.img.xz`**: 779 MB xz, 4.08 GB raw. A **8 GB card minimum**, as for the Pi.
It is in the worktree's `image/sbc/out/` (git-ignored), with its `.sha256`, manifest, package list, partition table and
the boot-test result. sha256 `96b504103146f33285adba4baf5207b78b1c8f082fd7b9b88469a3a1dfb63a7a`. **Not published.**

- **Base:** `Armbian_26.8.1_Orangepione_trixie_current_6.18.44_minimal.img.xz`. This is Armbian's *stable*
  (supported) image for the board, not a community nightly. The checksum and Armbian's signature (key
  `DF00 FAF1 C577 104B 50BF 1D00 93D6 889F 9F0E 78D5`) were both checked.
- **On it:**
  - vibeserver **11.0.10-5** armhf from apt stable: the **ARMv7+NEON** build, checked with readelf;
  - NetworkManager 1.52.1;
  - avahi 0.8;
  - overlayroot (Debian);
  - ffmpeg 7.1;
  - the High Detail Maps;
  - Armbian's kernel 6.18.44 and U-Boot `linux-u-boot-orangepione-current` 26.8.1.
- **Layout:** partition 1 root (`/boot` inside it) from sector 8192, 3.8 GB. Partition 2 `vibedata`, 256 MB. The
  boot region (MBR boot code and U-Boot) is **byte-identical to Armbian's**, and the root UUID is unchanged.
- **Build checks, all passed:**
  - the marker, the hostname `vibeserversetup` and the first-boot config;
  - enabled: vibeserver, vibeserver-pocket, -data, -seal, NetworkManager and avahi;
  - off: systemd-networkd and sshd;
  - netplan uses the NetworkManager renderer;
  - Armbian's root resize is off;
  - nmcli, iw, dnsmasq, nft and avahi are present, and so is overlayroot-chroot;
  - root still carries Armbian's forced-change flag;
  - the board's own U-Boot package is installed.
  On a Linux PC it takes about 25 minutes on 2 CPUs.
- **Emulated boot** (`boot-test.sh`: systemd-nspawn + QEMU, private network, systemd's sandbox switched off **on the
  throwaway copy only**, because a container nested in Docker cannot set it up):
  - the front door answered **HTTP 200 on :48000**;
  - hostname `vibeserversetup`;
  - vibeserver, vibeserver-pocket and vibeserver-pocket-data active. The pocket service reported "no Wi-Fi device
    (yet) — answering on any wired link", which is the LAN-only onboarding;
  - the settings partition mounted (91 KB used of 230 MB);
  - ssh.service inactive and **nothing listening on TCP 22**. Only systemd's local AF_UNIX ssh sockets exist, which
    the network cannot reach.
  - **NetworkManager did not start under emulation.** QEMU's user mode has no netlink ("Cannot open netlink socket:
    Protocol not supported"), so NetworkManager, and with it Ethernet DHCP and the name on the network, **can only be
    proved on the board.**
- **Expected on the board, from the 11.0.10-5 package:** the seal logs "no cmdline.txt" and the root stays
  **writable**. That is safe, but there is no power-cut hardening until 11.0.11 carries the U-Boot seal; the next
  Update then seals it.
- **Found while building**, each fixed and committed:
  1. the Pi `customise.sh` deletes `/tmp/pocket-files` at its end, which took the Armbian post-step script with it;
  2. "enabled" was tested with `-e`, which follows systemctl's absolute links into the build host's tree;
  3. on the host-side checks, `grep -q` under pipefail was swapped for counting;
  4. the boot test's first runs met nspawn's D-Bus needs and the nested-container sandbox.
- **Also seen, to check on the board:**
  - Armbian minimal runs **systemd-resolved** (Raspberry Pi OS Lite does not). It answers LLMNR on 5355. If it also
    answers mDNS it could compete with avahi for `.local`, so if the name does not resolve on the board, set
    `MulticastDNS=no`.
  - `fake-hwclock-load` fails (harmless).
  - HDMI stays powered. Armbian's switch is a device-tree overlay (later step).

### For Nick (Monday)

Flash with balenaEtcher or Raspberry Pi Imager (Use custom), Ethernet in, power on, and wait about 3 minutes. Then
open **http://vibeserversetup.local:48000** from a phone or PC on the same network, choose the admin password, then
run normal setup. Please send:

- whether the name resolved, or whether the box had to be found by its IP;
- the admin page's **Download diagnostics** file. A Portable has no login: SSH is off and the console gettys are
  masked, so the file is the way to see the box;
- whether Install updates works once.

## 11.0.11 cards: status at the pause (2026-10-25, ~13:20 UTC)

Built from the **published 11.0.11-1** packages on the Lenovo, in `~/vibe-sbc-out/11.0.11/` there. **Not uploaded.**
Every card passed its **emulated** boot test (systemd-nspawn under QEMU; no real hardware):

- the front door answered HTTP 200 on :48000;
- hostname `vibeserversetup`;
- package 11.0.11-1;
- ~~sshd off~~ — **WRONG** (corrected 2026-10-25 evening): every board card's report said `ssh active` and
  `0.0.0.0:22 LISTEN`. The test printed it and did not fail on it. See "Nick's real Orange Pi One" below;
- on every board card, the read-only seal wrote `extraargs=overlayroot=tmpfs` and rebooted on the first boot.

| Card | File | Bytes | sha256 |
|---|---|---|---|
| Raspberry Pi 64-bit | vibeserver-pocket-arm64-20261025.img.xz | 1498203228 | 59ad757be1c02905c5f647d18361902aab80fe33ac6862729196bbefb2ae6ead |
| Raspberry Pi 32-bit (0/1/2) | vibeserver-pocket-armhf-20261025.img.xz | 1496787716 | cbe396b07038f81ca774fb7068152ba2198ae651a1386dd663d6ac36a129dce9 |
| Orange Pi One | vibeserver-portable-orangepione-20261025.img.xz | 882404220 | 3880f58538367a0a6447e0504d4e9857896c5f651abb58c15c9b627db07e5358 |
| Orange Pi PC | vibeserver-portable-orangepipc-20261025.img.xz | 874609656 | 2236f45cf585e7f96d535180767e37f15a26dea34d876a8305097d992af0e5c6 |
| Orange Pi Lite | vibeserver-portable-orangepilite-20261025.img.xz | 836773872 | 75789cb6a131f04e17c15a90acae8a5e875c3299bcffb12d030d9995958158c7 |
| Orange Pi Zero 3 | vibeserver-portable-orangepizero3-20261025.img.xz | 1205320656 | 1b731295e1824c0e969ac30e1d2c1b9cc7b71bf92e7696c55ec41f21a9608b8f |
| Orange Pi Zero 2W | vibeserver-portable-orangepizero2w-20261025.img.xz | 1223363792 | 497582c8b3faba24adfa3d73421fc3c766a2f9e02efecd004b38c855f4bf95b2 |
| Le Potato | vibeserver-portable-lepotato-20261025.img.xz | 1178912252 | e0cafa2ed432f59e2af1429580f10eca1ec1ff5cf6a79aa5bbc73c24872255f3 |

**The three Rockchip cards, built 2026-10-25 15:25–15:54 on the M5 Pro Mac** (colima; `build-all.sh 11.0.11-1 …`), in
`~/vibe-sbc-out/11.0.11/` on the Mac — the other eight stay on the Lenovo. **Not uploaded.** Same emulated boot test, passed:

| Card | File | Bytes | sha256 |
|---|---|---|---|
| Orange Pi 5 | vibeserver-portable-orangepi5-20261025.img.xz | 1144316764 | 0c6a761d4811f10f4a3de39d40884b516c38686f240edee36785bb70981788fd |
| Radxa ROCK 5B | vibeserver-portable-rock-5b-20261025.img.xz | 1160933200 | fbbd461f59e9c92414f005b2f9bad7a80940f1017df66a54849a6d88dd72b513 |
| Radxa ZERO 3 | vibeserver-portable-radxa-zero3-20261025.img.xz | 1222087540 | f79b425d3107299e0550b78f5d471f5e7a815fc6f1eb3be6f9138cdb3e050ee3 |

The GPT blocker was not a short table: once the file is cut below the old backup header, libfdisk discards the GPT
and reads the protective MBR as DOS, so `sfdisk --relocate` did nothing. `sgdisk -e` repairs it, and the backup area
is first-lba (2048 on Armbian's Rockchip images), not 33 (96a9b01d). **All eleven 11.0.11 cards are now built.**
★ The pause itself: the ROCK 5B build stopped at the pause emptied the Lenovo's /dev through build.sh's cleanup
(fixed 12e201b3; the Lenovo was rebooted).

## Nick's real Orange Pi One (2026-10-25 evening): four faults, the cards withdrawn, the fixes

Nick (G4IRX) flashed the 11.0.11 Orange Pi One card onto a real board (Allwinner H3, **no Wi-Fi**, Ethernet). The nine
Armbian board cards were taken down from the `sbc-portable-11.0.11` pre-release the same evening (the notes carry a
withdrawal notice); the two Raspberry Pi cards stayed. The fixes are on branch `sbc-fixes` (from `main`):

| # | What Nick saw | Cause (evidence) | Fix | Commit |
|---|---|---|---|---|
| 1 | "ssh is running. I connected to root@vibeserversetup.local, pw 1234" | Nothing enabled sshd — **Armbian started it**: `armbian-firstrun` runs `service ssh restart` while `OPENSSHD_REGENERATE_HOST_KEYS=true` (its default), `armbian-firstlogin` runs `systemctl restart ssh.service`. `armbian-firstrun.service` is Type=simple, so the seal's `After=` did not wait for it: the seal could reboot under it and its self-disable land in RAM, so it ran on every boot. Read off the built card (mounted read-only); the boot tests had printed `ssh active`, sshd on 0.0.0.0:22. Every card also had the same SSH host keys, and Armbian's `PermitRootLogin yes`. | Debian's `sshd_not_to_be_run`; ssh.socket + sshd@ masked; first-run key regeneration off; host keys deleted; ssh generator masked; `PermitRootLogin no`; the seal waits for the first run; the owner's Enable SSH removes / restores the file. build.sh checks every way sshd can start; boot-test.sh fails on any. | d6d50d90 |
| 2 | "vibeserversetup.local resolves to a link local ipv6, then a public ipv6, but no ipv4 … 'site not found'"; `http://<IPv4>:48000/` worked | The server listened on **IPv4 only** (`net::listen`: AF_INET, INADDR_ANY). Likely also avahi's default `publish-a-on-ipv6=no` (an answer sent over IPv6 carries no A record). | One dual-stack socket (AF_INET6, IPV6_V6ONLY=0), IPv4 fallback; IPv4 clients read exactly as before (`::ffff:` unwrapped where first read). IPv6 classes: fe80::/10 + fc00::/7 LAN, a global address LAN inside the box's own prefixes; brute-force lockout per /64 for remote IPv6. avahi: both addresses in every answer (converge). | 15d356cc, 672820d9 |
| 3 | `http://vibeserversetup.local/` (no port) — "site not found" | The :80 redirect named the **Wi-Fi** interface and was IPv4 only; a wired box had none. | :80 → the front door on every link, IPv4 + IPv6, while the box is being set up; removed when the setup name retires. | 672820d9 |
| 4 | "Wi-Fi not saved — Give this box's own hotspot a name" | The page counted an untouched Wi-Fi card as unsaved. | Already fixed: d2464c1c ("Disable Wi-Fi / no Wi-Fi present"). | d2464c1c |
| — | (Stuart) USB Wi-Fi dongles added later | A radio appearing on a set-up box with nothing saved raised the OPEN setup hotspot on a live server. | A Wi-Fi hardware record; a new radio on a set-up box is left alone, the setup page asks "New Wi-Fi hardware detected — configure it now?" (Configure / Not now). No open hotspot on a set-up box unless the owner reset its Wi-Fi. | 18a5a37b, f5a3c5c1 |

**Stuart's answers, 2026-10-25 late evening** (both done on `sbc-fixes`):
- *"release the port the less open ports the better"* — once set up, port 80 is released on every interface; the
  2026-10-10 rule that kept the Wi-Fi redirect on the home network (http://vibepocket.local/) is reversed. The Wi-Fi
  redirect exists only while the box's OWN hotspot is on the air (a joining phone's captive check needs it).
- *"realistically the only .local address open after setup should be the one the user sets"* — on a Portable the server
  runs no mDNS responder; it hands its name to vibeserver-pocket, which makes it the HOSTNAME that avahi publishes (IPv4
  + IPv6, every interface) with the `_vibesdr._tcp` service. avahi asks `vibeserver-pocket avahi-gate` before every
  start (an ExecCondition drop-in, converge-enforced): the right hostname is set before avahi publishes anything, and the
  start is skipped when there is nothing to publish (mDNS off). `vibepocket.local` is never published. For the 3 minutes
  after a Save the new name answers by an avahi alias beside the setup name (the page sends the owner there at once).
  systemd-resolved's LLMNR (5355, Armbian) is switched off too. The boot test asserts: vibeserversetup.local only,
  from avahi alone, no service, no alias, nothing else on 5353, nothing on 5355.

Nick also saw **DAB struggling**; he suspects his power supply. Not investigated — note only.

**Proved here:** each fix by tests (pocket Python suites, test-ipclass, test-pocket, test-dualstack-clients against the
real binary over 127.0.0.1, [::1], the Mac's own ULA and LAN IPv4, the setup-page tests); the new sshd checks against
the shipped card's root and against it after the new section (chroot on an overlay); `sshd -T` says
`permitrootlogin no`; the nft scripts with real nftables; the avahi file with avahi 0.8.
**Reproduced under emulation:** the withdrawn orangepione card, booted by the new boot-test.sh (2026-10-25 22:30): on the
second boot `armbian-firstrun` was still **enabled** (the seal had rebooted under it), `ssh.service` disabled but
**active**, sshd on `0.0.0.0:22` and `*:22` — the test fails it. (A first version of the check, reading the state the
moment the front door answered, passed the same card: the first run had not reached `service ssh restart` yet. The test
now waits for the first run to finish and fails if it is still enabled.)
**Not proved without hardware:** NetworkManager, DHCP and avahi on a real board; a real USB dongle's sysfs ids and
NetworkManager's `managed no` on it; IPv6 NAT on Armbian's sunxi kernel (the redirect falls back to IPv4 alone).

### Release notes for the fixed cards (draft — not published)

> **VibeServer Portable cards for other boards — fixed (replaces the withdrawn 11.0.11 board cards)**
>
> Thanks to Nick (G4IRX), who ran the Orange Pi One card on a real board. The first board cards had four faults, all
> fixed on these:
>
> - **SSH was switched on, with Armbian's default root password.** Armbian's own first-boot script started it. SSH is
>   now off and cannot start until you switch it on in the admin page (Enable SSH: login `vibe`, your own password —
>   root can never log in over SSH). Each box makes its own SSH keys. **If you flashed a withdrawn board card, re-flash
>   it with one of these.**
> - **The box now answers on IPv6 as well as IPv4.** On some networks `vibeserversetup.local` gave only IPv6 addresses
>   and the page did not open.
> - **`http://vibeserversetup.local/` works on Ethernet**, with no port number, until setup is finished
>   (`http://vibeserversetup.local:48000/` still works).
> - **A box with no Wi-Fi can finish setup** (there is a "Disable Wi-Fi / no Wi-Fi present" tick if the box gets it
>   wrong).
> - **Once set up, the box answers to one name — the one you chose** (`http://<name>.local:48000/`); the setup name
>   goes, and port 80 is released.
> - **A USB Wi-Fi adapter plugged in after setup** is left switched off until you choose: the setup page asks "New Wi-Fi
>   hardware detected — configure it now?".
>
> These cards are still **tested under emulation only** apart from the Orange Pi One. If anything goes wrong, install
> the board's own OS and add VibeServer with APT instead.

