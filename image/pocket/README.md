# Pocket VibeServer image

This builds a Raspberry Pi OS **Lite** card that boots as a pocket VibeServer. On the first boot it
raises an open "VibeServer" hotspot and opens the setup page in the phone's captive sheet. After
setup it joins your saved networks in strict order, or falls back to its own secured hotspot. The
design, the boot state machine and the on-device checklist are in
[`docs/POCKET-VIBESERVER-PI3A.md`](../../docs/POCKET-VIBESERVER-PI3A.md).

Both architectures come from **one** customisation script,
`stage-pocket/00-pocket/files/customise.sh`, run inside the target root:

| Image | Base | VibeServer build |
|---|---|---|
| `vibeserver-pocket-arm64-<date>.img.xz` | Raspberry Pi OS Lite 64-bit | arm64 (`Dockerfile.build`, native) |
| `vibeserver-pocket-armhf-<date>.img.xz` | Raspberry Pi OS Lite 32-bit | armhf (cross-compiled on the Mac's arm64) |

## Requirements

- Docker (colima on the Mac). `build-deb.sh` uses the release build images:
  `vibeserver-build:bookworm-arm64` and `vibeserver-cross:bookworm-armhf`.
- `tools/cloudflared-desktop/bin/` in this checkout, or in the main checkout if this is a worktree.
- About **5 GB free on the Mac** while one image builds. The work files live in `out/`, not in the
  Docker VM. Each finished `.img.xz` is about 1.0–1.2 GB (the baked maps are already compressed).
- **One build at a time.** Every container runs with `--cpus 2` and `nice`, and `xz` uses 2
  threads. Never run two of these at once.

## Exact steps

From the repository root:

```bash
# 1. The packages, from this checkout (BUILD ONLY — nothing is signed, indexed or pushed).
#    ~10 min each on an M4; one at a time.
image/pocket/build-deb.sh arm64
image/pocket/build-deb.sh armhf
ls image/pocket/out/debs/        # vibeserver_<ver>-900_arm64.deb, vibeserver_<ver>-900_armhf.deb

# 2. The images (default route: official Raspberry Pi OS Lite, customised). One at a time.
image/pocket/build-image.sh arm64
image/pocket/build-image.sh armhf
ls image/pocket/out/*.img.xz

# 3. Optional: check an image's checksum
(cd image/pocket/out && shasum -a 256 -c vibeserver-pocket-arm64-*.img.xz.sha256)
```

Options (environment variables):

- `POCKET_COUNTRY=GB` sets the Wi-Fi country, which decides the legal channels. Setup can change it
  later.
- `POCKET_DEB_REV=900` sets the Debian revision of the local package. It stays above published
  revisions of the same version, so a test box's "Install updates" cannot swap it for a package
  without the pocket code.

### The pi-gen route (from scratch, reproducible)

```bash
image/pocket/build-image.sh arm64 --pigen     # pi-gen "arm64" branch, Trixie
image/pocket/build-image.sh armhf --pigen     # pi-gen "master" branch (32-bit)
```

This clones pi-gen into `out/work/pi-gen-<arch>` and copies `stage-pocket` in. It then runs
`stage0 stage1 stage2 stage-pocket`, with only `stage-pocket` exporting an image. It runs pi-gen's
own `build-docker.sh` with `PIGEN_DOCKER_OPTS="--cpus 2"`. **It needs about 15 GB free in the Docker
VM**, because pi-gen keeps a root filesystem per stage. The Mac's colima VM had 5 GB free on
2026-10-10, so this route is for a Linux build host or a bigger VM. The login it creates (`vibe`)
gets a random password that is not kept. SSH is off.

## What the customisation does

`customise.sh` makes the following changes, and nothing else:

- **Adds** `dnsmasq-base` (hotspot DHCP and the captive DNS rule; it has no service of its own),
  `nftables` (the `:80` redirect; its service stays off) and `iw`. NetworkManager is Lite's default
  and is checked, not installed. `python3` is already on Lite.
- **Installs** VibeServer from `out/debs/`, falling back to apt.vibesdr.net. It adds the apt
  repository so the box updates like any other VibeServer.
- **Writes** the pocket marker `/etc/vibeserver-pocket/enabled`, `pocket.conf`, and the first-boot
  `/etc/vibeserver/config.json`: a Full-mode front door with no radio and no password.
- **Enables** `vibeserver`, `vibeserver-radios`, `vibeserver-maintenance.path`, `vibeserver-pocket`
  and `NetworkManager`. `postinst` cannot enable them in a chroot.
- **Disables** Bluetooth (`hciuart`, `bluetooth`, `dtoverlay=disable-bt`), ModemManager,
  triggerhappy, the apt daily timers, `man-db.timer` and `e2scrub_all.timer`, and masks udisks2.
  It turns the KMS display driver, onboard audio (`dtparam=audio=off`) and the boot splash off, and sets
  `gpu_mem=16`: the box is headless. `vibeserver-pocket` powers the display output down at every boot
  (`vcgencmd display_power 0`) and blocks Bluetooth, so a box updated over apt gets that too (power + EMI).
- **Sets** the hostname to `vibepocket` and the Wi-Fi country. It creates a locked login so first
  boot never asks for a user.
- **Power-cut hardening.**
  - Installs `overlayroot` and builds its initramfs for every kernel on the image.
  - Removes ` resize` from `cmdline.txt`, so Pi OS will not grow root into the settings partition.
  - Mounts the boot partition read-only.
  - Puts journald in RAM (16 MB cap) and uses zram swap only.
  - Enables `vibeserver-pocket-data` (the settings partition) and `vibeserver-pocket-seal`. The seal
    makes root read-only on the first boot.
- **The High Detail Maps.** `build-image.sh` reads `DETAIL_URL` / `DETAIL_BYTES` from `vibe_mapgl.h`
  and fetches the file once into `out/cache/`. It verifies the file as the server's installer does
  (exact size + `PMTiles` magic) and installs it to
  `/usr/lib/vibeserver/mapgl-detail/` on the read-only root.

## Card layout (made by `build-image.sh`)

| Partition | Size | |
|---|---|---|
| 1 boot | 512 MB | read-only |
| 2 root | used + 1 GB (arm64 3.6 GB, armhf 3.4 GB on 2026-10-10) | read-only after the first boot |
| 3 `vibedata` | 256 MB, never grown | the settings (`data=journal`) |
| rest of the card | unallocated | kept for a future gallery partition |

The `.img` is cut after partition 3, at **4.4 GB raw (arm64) / 4.2 GB (armhf)**. **Minimum card:
8 GB. Typical: 32 GB.**

## Flashing

Raspberry Pi Imager → **Choose OS → Use custom** → the `.img.xz` → the card. For the first-boot
hotspot test, answer **No** to OS customisation. If Imager sets a Wi-Fi network, the box joins it
directly and setup is at `http://vibepocket.local:48000/` on that network.

## Status (2026-10-10)

Both images were **built on the Mac** by the default route, one after the other:

| Image | .xz | Raw | Base |
|---|---|---|---|
| `vibeserver-pocket-arm64-20261010.img.xz` | 1.18 GB | 4.4 GB | `2026-10-06-raspios-trixie-arm64-lite` (sha256 checked) |
| `vibeserver-pocket-armhf-20261010.img.xz` | 1.03 GB | 4.2 GB | `2026-10-06-raspios-trixie-armhf-lite` (sha256 checked) |

Neither has been **booted** yet. The `--pigen` route is written but was not run, because the Docker
VM had no disk to spare. Flash with **"leave it powered for the first 3 minutes"**: it seals its
root and reboots once.
