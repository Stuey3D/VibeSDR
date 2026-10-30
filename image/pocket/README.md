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

## A new card checks for a VibeServer update once, during setup

From 11.0.16, a card checks for a newer VibeServer one time, during setup. A card carries the VibeServer it was built
with, so it can be weeks old. Stuart, 2026-10-28: *"Make the SBC cards check for a VibeServer update on first install
if connected to the internet … then the owner can choose auto updates thereafter."* This covers the Raspberry Pi cards
and the board cards (`image/sbc`), which share `customise.sh`.

1. The owner flashes the card and connects to it.
2. They set up the network on the setup page.
3. **Then the setup page runs one step: "Checking for a VibeServer update…".** It ends with one of these lines:
   - "Updated to VibeServer 11.0.x."
   - "VibeServer is up to date (11.0.x)."
   - "VibeServer cannot check for updates — proceeding with setup." (no internet)
   - "The update check did not finish — VibeServer 11.0.x is unchanged." (anything that failed)
   - "Update cancelled — VibeServer 11.0.x is unchanged."

   Setup then carries on.
4. **It never runs again by itself.** This holds for every outcome, including offline, failed, cancelled and a power
   cut. From then on, updates come from **Update VibeServer** (Admin › Maintenance) or from the schedule the owner
   chooses there.

**Cancel** is shown while the check and the download run. It stops the download, and the installed VibeServer is not
touched. Once dpkg has started, there is no Cancel: the page says *"This part cannot be cancelled"*. A press that
arrives just then gets *"Too late to cancel — the install has started and is finishing safely"*. A package is never
left half-installed.

How it works:

- **Armed by the card only.** `customise.sh` writes `/etc/vibeserver-pocket/first-update` when the package has the
  feature. A box that updated to 11.0.16 from an older package was set up long ago, so it never runs the check.
- **The root helper does the work.** The page asks for the maintenance action `first-update`, which runs
  `first_update` in `vibeserver-maintenance`. It is the `update` action with four additions:
  - a quick look first, at apt.vibesdr.net only, into a throwaway list directory in `/run`. When nothing is newer,
    nothing is written anywhere;
  - time limits: 90 s for the look, 600 s for the indexes, 900 s for the download;
  - the cancel;
  - a record of the run.

  Everything else is the same as `update`. It installs `vibeserver` alone with `--only-upgrade`, at the exact version
  it found, so it never touches the OS and never goes down a version. It uses `apt_do`, which works through
  `overlayroot-chroot` on a sealed card. It runs `pocket_converge`, and it **reboots on a sealed card** (the read-only
  root's own rule). Without a seal, the package's own try-restart restarts VibeServer.
- **Once means once.** The record `/var/lib/vibeserver-pocket/first-update` is on the settings partition. It is
  written **before** anything touches the network, and the helper refuses to run while the record exists. The steps
  are: `state=checking | downloading | installing`, then `updated | uptodate | offline | failed | cancelled`, followed
  by `from=`, `to=` and `why=`. Admin status carries it as `updates.first`, so a page reloaded during a run follows
  that run.
- **Cancel** is one empty file, `/var/lib/vibeserver/first-update.cancel`. The daemon writes it into its own
  directory, and the helper only checks that it exists. The helper then signals **apt-get and its download methods
  only**, never `overlayroot-chroot`, which must finish its own clean-up and remount the real root read-only. The last
  point at which a Cancel is honoured is the moment just before dpkg starts.
- **The restart never strands the page.** The step is the whole page while it runs, so nothing typed can be lost.
  After an update the page waits for a **new server instance**, the same proof the Save uses, and then carries on.
  This covers the reboot of a sealed card, after which the box comes back on the same network under the same setup
  name.
- **Never on the open setup hotspot.** There is no internet there, and the network is not set up yet (step 2).
- **Beta.** A card built from a `~beta` package also gets `vibesdr-beta.list`, the same file as the admin page's
  **Receive beta updates** tick. Its check therefore looks in the beta suite, the page shows the box ticked, and
  unticking it is the existing way back to stable.
- Tests:
  - `vibeserver/linux/test_first_update_docker.sh`, 47 checks. Real apt and dpkg against a local repository, in a
    Debian bookworm container with no network.
  - `scripts/test-setup-first-update.mjs`, 27 checks. The page's own functions against a fake server.

## Updating a Portable: no new card needed

**A Portable updates fully through the admin page's Install updates. A new image is only for a brand-new
box (or a card that has died).** Stuart, 2026-10-13: "after initial setup we need to make sure that
VibeServer Portables can be fully updated without needing a full ISO again".

The vibeserver package carries the Portable's operating-system setup as well as the server:

- `/usr/lib/vibeserver/vibeserver-pocket-converge` applies everything the image does to the OS that
  should follow a release. It covers the services on, off and masked, NetworkManager's connectivity
  check, avahi on every interface, journald in RAM, zram only, the `config.txt` lines, the read-only
  `/boot` in fstab and the settings partition's mount points. It writes a file only when the content
  differs and touches a unit only when its state differs, so a second run changes nothing. It does
  **nothing** without `/etc/vibeserver-pocket/enabled`, so ordinary installs are untouched.
- `/usr/lib/vibeserver/pocket-packages.txt` lists the system packages a Portable needs. They are
  never Depends or Recommends, so an ordinary install does not pull them.

**What one Update does on a Portable:**

1. `vibeserver-maintenance update` runs `apt-get install --only-upgrade vibeserver` inside
   `overlayroot-chroot`, so it writes the real root. The boot partition is mounted inside the chroot too.
2. The package's `postinst` runs the converge.
3. Any package in `pocket-packages.txt` that the box lacks is installed (`--no-install-recommends`).
   The list is read inside the chroot, so it is the new release's list.
4. The converge runs again. This time it sees the new packages and the boot partition.
5. The box reboots, and every change takes effect. Nothing is restarted before then, so networking
   never drops mid-update.

"Update all packages" does the same after its system upgrade.

**Beta updates on a Portable.** The *Receive beta updates* tick-box (`beta-on` / `beta-off`) writes or removes
`/etc/apt/sources.list.d/vibesdr-beta.list` in the real root through `overlayroot-chroot` and in the live overlay,
so the tick survives a reboot and shows at once. *Back to stable* (`update-stable`) installs the newest stable
version the same way as an Update, then reboots. See `vibeserver/linux/INSTALL.md`, "Beta updates".

**Two kinds of step:**

- **Enforced every time.** These are the product: our own units enabled, our own drop-in files, and
  avahi answering on every interface.
- **Done once and recorded** in `/etc/vibeserver-pocket/converge.done`. These switch off one of the
  OS's things (Bluetooth, a getty, udisks2…) or add a `config.txt` or fstab line. An owner who
  changes one back (for example, unmasks `getty@tty1` for an HDMI console, or raises `gpu_mem`)
  keeps that change through every update. A step that a new release adds still reaches every box,
  once. Delete a line from the ledger to have that step done again.

At start, `vibeserver-pocket` runs `vibeserver-pocket-converge --check` and logs anything that is
not converged, such as a package an Update could not install. It only reports. The live root is the
RAM overlay, so a change made there would vanish at the next boot.

**Image-only: done once, by `customise.sh`, and why:**

| Step | Why it is not repeated |
|---|---|
| The marker `/etc/vibeserver-pocket/enabled` | It is what makes a box a Portable |
| `pocket.conf` | It records the country chosen at build time. The daemon has the same defaults in code, and setup's choices live on the settings partition |
| First-boot `config.json` | From the first boot on it lives on the settings partition |
| Hostname, Wi-Fi country | `vibeserver-pocket` and setup own them at runtime |
| The apt key | Fetching it needs the network, and a box the update reached already has a working key. A missing `vibesdr.list` is restored by the converge |
| The explicit `update-initramfs` | On a running box, the `overlayroot` package's trigger and every kernel package rebuild it |
| Removing ` resize` from `cmdline.txt` | Pi OS resizes only on the first boot |
| The locked `vibe` login, `userconfig` off | First boot only |
| The settings partition | `build-image.sh` makes it, and `vibeserver-pocket-data` re-makes it if it is lost |

**Lag of one release for the update helper itself.** The `update` that installs a new
`vibeserver-maintenance` is run by the old copy. So a change to the helper (such as installing the
package list) first acts on the update *after* it. This does not matter for boxes flashed from the
11.0.0 image, because they already have every package and `config.txt` line that release has.

## What the customisation does

`customise.sh` runs inside the image's root. In order:

- **Checks** NetworkManager (Lite's default; never installed over the top). It installs
  `ca-certificates`, `curl`, `gnupg` and `python3`, then adds the apt repository and its key.
- **Installs** VibeServer from `out/debs/`, falling back to apt.vibesdr.net. It refuses a package
  without `vibeserver-pocket` or `vibeserver-pocket-converge`.
- **Writes** the pocket marker `/etc/vibeserver-pocket/enabled`, `pocket.conf`, and the first-boot
  `/etc/vibeserver/config.json`: a Full-mode front door with no radio and no password.
- **Installs the package's list**, `pocket-packages.txt`: `dnsmasq-base` (hotspot DHCP and the
  captive DNS rule), `nftables` (the `:80` redirect; its service stays off), `iw`, `rfkill`,
  `avahi-daemon`, `ffmpeg` (DAB+ audio), `overlayroot` and `initramfs-tools`. These are the same
  packages that every Update checks for.
- **Runs `vibeserver-pocket-converge`**, which is the same step every Update runs, then
  `--check`. The build stops if anything did not converge. The converge does the following:
  - **Enables** `vibeserver`, `vibeserver-radios`, `vibeserver-maintenance.path`,
    `vibeserver-pocket`, `vibeserver-pocket-data`, `vibeserver-pocket-seal`, `NetworkManager` and
    `avahi-daemon`. `postinst` cannot enable them in a chroot.
  - **Disables** Bluetooth (`hciuart`, `bluetooth`), ModemManager, triggerhappy, the apt daily
    timers, `man-db.timer`, `e2scrub_all.timer`, `nftables.service`, `dphys-swapfile` and
    `fake-hwclock`. It masks the console gettys and udisks2.
  - In `config.txt`, it turns the KMS display driver, onboard audio (`dtparam=audio=off`), Bluetooth
    (`dtoverlay=disable-bt`) and the boot splash off. It sets `gpu_mem=16` and `auto_initramfs=1`.
    The box is headless. `vibeserver-pocket` also powers the display output down at every boot
    (`vcgencmd display_power 0`) and blocks Bluetooth (power + EMI).
  - Writes NetworkManager's connectivity check and switches avahi's interface limits off.
  - Puts journald in RAM (16 MB cap), uses zram swap only and mounts the boot partition read-only.
- **Sets** the hostname to `vibeserversetup` (`vibeserver-pocket` retires it to `vibepocket` once
  setup is finished) and the Wi-Fi country. It creates a locked login so first boot never asks for a
  user.
- **Power-cut hardening.** It builds the `overlayroot` initramfs for every kernel on the image and
  removes ` resize` from `cmdline.txt`, so Pi OS will not grow root into the settings partition.
  The seal (`vibeserver-pocket-seal`) makes root read-only on the first boot.
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
directly and setup is at `http://VibeServerSetup.local/` (or `:48000`) on that network. Once set up, the box answers
only to the name chosen in setup, at `http://<that name>.local:48000/` (port 80 is released; 2026-10-25).

## Status (2026-10-10)

Both images were **built on the Mac** by the default route, one after the other:

| Image | .xz | Raw | Base |
|---|---|---|---|
| `vibeserver-pocket-arm64-20261010.img.xz` | 1.18 GB | 4.4 GB | `2026-10-06-raspios-trixie-arm64-lite` (sha256 checked) |
| `vibeserver-pocket-armhf-20261010.img.xz` | 1.03 GB | 4.2 GB | `2026-10-06-raspios-trixie-armhf-lite` (sha256 checked) |

Neither has been **booted** yet. The `--pigen` route is written but was not run, because the Docker
VM had no disk to spare. Flash with **"leave it powered for the first 3 minutes"**: it seals its
root and reboots once.
