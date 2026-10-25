# VibeServer Portable for other boards (Armbian) — prototype

This builds a **VibeServer Portable** card for a board that is not a Raspberry Pi, from that board's official
**Armbian** minimal image. The box it makes is the same Portable as the Pi image (`image/pocket/`):

- the same first-boot config;
- the same open "VibeServer" setup hotspot on a board with Wi-Fi;
- the same `VibeServerSetup.local:48000` on Ethernet from the first boot;
- the same setup page, the same Install updates and the same baked High Detail Maps.

The plan, which board families exist, and why each board needs its own card: [`docs/SBC-IMAGES-PLAN.md`](../../docs/SBC-IMAGES-PLAN.md).

## Files

| File | |
|---|---|
| `boards.txt` | The boards: Armbian id, family, arch, NEON, Wi-Fi chip, RAM, Armbian channel, status |
| `build.sh` | The builder (Linux, root). `DRY_RUN=1 build.sh <board>` prints the plan without root |
| `run-in-docker.sh` | Runs `build.sh` (or `boot-test.sh`) in a throwaway privileged Debian container: 2 CPUs, niced, one build at a time |
| `fetch-armbian.sh` | Downloads a board's current Armbian Trixie minimal image with its `.sha` and `.asc` |
| `boot-test.sh` | Boots a finished card under emulation and checks the Portable comes up |
| `customise-armbian.sh` | The Armbian half (`pre` / `post`) around the Pi image's own `customise.sh` |

## Build (on the Lenovo, or any Linux Docker host)

```bash
# 1. The board's Armbian Trixie minimal image, with its .sha and .asc (Armbian's signature is checked by build.sh)
B=orangepione
IMG=$(image/sbc/fetch-armbian.sh $B base)          # prints base/Armbian_…_minimal.img.xz

# 2. Build (about 25 minutes for an armhf board on 2 CPUs; needs ~7 GB free beside the output)
sudo image/sbc/run-in-docker.sh $B "$IMG" out
ls out/   # vibeserver-portable-<board>-<date>.img.xz + .sha256, .manifest.txt, .packages.txt, .partition-table.txt

# 3. Boot it under emulation (systemd-nspawn + QEMU): units, hostname, the front door on :48000
sudo image/sbc/run-in-docker.sh boot-test out/vibeserver-portable-$B-<date>.img.xz out
```

`fetch-armbian.sh` goes through dl.armbian.com's own redirector for the image, the `.sha` and the `.asc`, because a
community (trunk) image lives on an expiring GitHub asset URL.

Options: `VIBESERVER_DEB=/path/to.deb` (a branch build instead of apt.vibesdr.net stable), `COUNTRY=GB`, `NO_MAPS=1`,
`KEEP_WORK=1`. A board under 512 MB is refused: the 256 MB boards stay on the DietPi ordinary-server image.

## Card layout

| Partition | Size | |
|---|---|---|
| before partition 1 | 4 MiB | Armbian's U-Boot, **byte-identical** to the base image (checked) |
| 1 root (ext4, `/boot` inside it) | used + 1 GB | read-only after the first boot, from 11.0.11 (the U-Boot seal) |
| 2 `vibedata` | 256 MB, never grown | the settings |
| 3 `VIBESETUP` (FAT16) | 16 MB | for any computer: drop `vibeserver-reset-wifi` here to forget the Wi-Fi (11.0.11); `README-VibeServer.txt` says how |

## Status

The Orange Pi One card was built and checked under emulation only (11.0.10-5, before the FAT partition). Nobody has
booted it on a board yet. The rest of the boards in `boards.txt` are dry-run only until 11.0.11 is on apt stable.
See the end of the plan doc.
