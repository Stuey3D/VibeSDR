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
| `run-in-docker.sh` | Runs `build.sh` in a throwaway privileged Debian container: 2 CPUs, niced, one build at a time |
| `customise-armbian.sh` | The Armbian half (`pre` / `post`) around the Pi image's own `customise.sh` |

## Build (on the Lenovo, or any Linux Docker host)

```bash
# 1. The board's Armbian Trixie minimal image, with its .sha and .asc (Armbian's signature is checked)
B=orangepione
U=$(curl -fsSI https://dl.armbian.com/$B/Trixie_current_minimal | sed -n 's/^location: *//Ip' | tr -d '\r')
mkdir -p base && for x in "" .sha .asc; do curl -fsSL -o "base/$(basename "$U")$x" "$U$x"; done

# 2. Build (about 15 minutes for an armhf board on 2 CPUs; needs ~6 GB free beside the output)
sudo image/sbc/run-in-docker.sh $B base/$(basename "$U") out
ls out/   # vibeserver-portable-<board>-<date>.img.xz + .sha256, .manifest.txt, .packages.txt, .partition-table.txt
```

Options: `VIBESERVER_DEB=/path/to.deb` (a branch build instead of apt.vibesdr.net stable), `COUNTRY=GB`, `NO_MAPS=1`,
`KEEP_WORK=1`.

## Card layout

| Partition | Size | |
|---|---|---|
| 1 root (ext4, `/boot` inside it) | used + 1 GB | read-only after the first boot once the seal supports U-Boot boards (this branch; 11.0.10-5 does not) |
| 2 `vibedata` | 256 MB, never grown | the settings |
| before partition 1 | 4 MiB | Armbian's U-Boot, **byte-identical** to the base image (checked) |

## Status

Prototype. The Orange Pi One image was built and checked under emulation only. Nobody has booted it on a board yet.
See the end of the plan doc.
