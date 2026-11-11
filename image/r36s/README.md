# VibeSDR R36S handheld — the card image

One SD card that turns an R36S (RK3326; built and proven on Stuart's **EE clone**, ROCKNIX's "b" boards) into
"a proper portable radio just powered by VibeSDR": power on → VibeSDR splash with a progress bar → the receiver,
full screen, drawn on the GPU. The product spec is `docs/v12/R36S-HANDHELD-IMAGE.md` §0.1; the engine ↔ hardware
interface is §0.2.

## How to build

```sh
image/r36s/build.sh                                   # engine = newest arm64 vibeserver from apt.vibesdr.net beta
image/r36s/build.sh --deb ~/path/vibeserver_12.0.0~betaNN-1_arm64.deb    # a fresh local engine build
image/r36s/build.sh --out /some/dir                   # where the .img.gz lands (default ~/VibeSDR-r36s-build/out)
image/r36s/build.sh --mapdata ~/…/vibeserver-mapdata-detail_<v>_all.deb   # a given map pack (default: cache, else built)
DRY_RUN=1 image/r36s/build.sh                         # the plan + every script parses; builds nothing
image/r36s/build.sh --verify ~/…/vibesdr-r36s-<version>.img.gz           # re-run the offline checks on an image
```

Docker on the Mac (colima, arm64 — native, no QEMU), `--cpus 6`, about 10 minutes. Out: `vibesdr-r36s-<version>.img.gz`,
`.img.gz.sha256`, `.packages.txt` (every Debian package and version), `.SOURCES.txt` (GPL: where every binary came from),
`splash-preview.png`. `<version>` = the engine's version + the UTC date, e.g. `12.0.0-beta65-20261105`.

**The high-detail map pack is on the card** (Stuart, 2026-11-05: the handheld has no internet). `vibeserver` only
*Recommends* `vibeserver-mapdata-detail` (~221 MB: every layer past zoom 7) and the image installs with
`--no-install-recommends`, so the builder installs it explicitly: `--mapdata <deb>`, else the newest in
`~/VibeSDR-r36s-build/cache/mapdata`, else `scripts/build-mapdata-deb.sh` builds it from `assets/mapdata/v1`. It lands in
`/usr/lib/vibeserver/mapdata` beside the basic pack — the engine's first place to look (`vibe_mapdata.h`: the binary's
`../lib/vibeserver/mapdata`); `verify.sh` fails an image without it. The root partition is sized from what it holds
(+15 % +320 MiB free), so it grows by itself — about +260 MiB for the pack.

Inputs, all checked: the ROCKNIX 20261101 b image (downloaded once into `~/VibeSDR-r36s-build/cache`, sha256 pinned in
`build-image.sh`; `ROCKNIX_IMG=<file>` uses a local copy), Debian trixie (`SNAPSHOT=YYYYMMDDTHHMMSSZ` pins it to
snapshot.debian.org for a reproducible package set), the engine package (apt verifies the repository signature; a
`--deb` is taken as given), and this directory. Disk ids, filesystem UUIDs and the FAT serial derive from the version,
so two builds of the same version and inputs lay the card out identically.

## How to flash

```sh
diskutil list                                  # find the card: /dev/diskN (check the size!)
diskutil unmountDisk /dev/diskN
gunzip -c vibesdr-r36s-<version>.img.gz | sudo dd of=/dev/rdiskN bs=4m
diskutil eject /dev/diskN
```

The image is ~1.75 GB; the rest of the card becomes the `VIBESDR` drive on the first boot.

## What is on the card

| Where | What |
|---|---|
| 32 KiB … 16 MiB | ROCKNIX's loader `b_uboot.bin.uart5` (U-Boot v2025.10 + rkbin), byte-identical to what ROCKNIX's own updater wrote on Stuart's unit |
| p1 FAT32 `VIBEBOOT`, 128 MiB | `KERNEL`, `rk3326-gameconsole-eeclone.dtb`, `boot.scr` (ROCKNIX's), `extlinux/extlinux.conf` (ours), `overlays/mipi-panel.dtbo` (60 Hz default), `overlays/vibe-r36s.dtbo`, `overlays/mipi-panel.rocknix-80hz.dtbo` (the untouched original), `stock/rf3536k3ka.dtb`, `SOURCES.txt`, `README.txt` |
| p2 ext4 `vibe-root`, ~1.6 GB (with the map pack) | Debian 13 trixie arm64, **mounted read-only**, a RAM overlay on top |
| p3 ext4 `vibestate`, 256 MiB | what must survive a power cut: the engine's data (`/var/lib/vibeserver`), the browser's storage, `hwd.json` (brightness, volume), the clock, the card-check result |
| p4 exFAT `VIBESDR`, the rest | made on the first boot: `Recordings/`, `Gallery/`, `import/` (+ `done/`), `export/`, `Diagnostic logs/`, `README.txt` |

## Boot chain — and why this one

**U-Boot → ROCKNIX `boot.scr` → our `extlinux.conf` → ROCKNIX's KERNEL → our Debian root, directly.**

ROCKNIX's `KERNEL` is an arm64 `Image` with ROCKNIX's initramfs **built in** (`CONFIG_INITRAMFS_SOURCE`); that initramfs
mounts `/flash/SYSTEM` (a squashfs) and `/storage`. Two ways to boot our root were open:

1. **Reuse ROCKNIX's initramfs, ship our root as `SYSTEM`.** Rejected: that init is ROCKNIX's whole boot policy —
   it grows `/storage` to 100 % on first boot (which would eat the space VIBESDR needs), runs ROCKNIX's update and
   reset hooks, and expects ROCKNIX's layout. We would be steering someone else's distribution from outside.
2. **Boot Debian directly — chosen.** The extlinux line passes `rdinit=/vibesdr-no-initramfs`: the kernel unpacks its
   built-in initramfs, finds no such file, skips it, and mounts `root=PARTUUID=<id>-02` itself onto `/root` of that
   initramfs (`init/main.c`: `prepare_namespace()` — the SD driver, ext4 and overlayfs are all built in).
   ★★ ROCKNIX's initramfs has no `/root`, so a 1 KB `vibe-initrd` (`/dev`, `/dev/console`, `/root`) is loaded beside it
   and unpacked on top. Without it: "VFS: Cannot open root device … error -2", a panic, and a reboot 1 s later
   (`CONFIG_PANIC_TIMEOUT=1`) — the first card's boot loop (2026-11-06). `boot.scr` puts it at 0x0c000000, above the KERNEL.
   `ro` keeps the root read-only; `init=/usr/lib/vibesdr-r36s/overlay-init` lays a tmpfs overlay on top and execs
   systemd. Cost: the unused initramfs stays in RAM (a few MB). Gain: no ROCKNIX userspace at all, and one less
   moving part than an initramfs of our own.

The **kernel binary, DTB, modules (`/usr/lib/modules/7.1.2`) and firmware** are ROCKNIX's from the same release, so they
match. `mali_kbase` is blacklisted; `panfrost` loads (Mesa 25.0.7). The loader cannot be changed without a U-Boot
build and an unproven card, so it is used exactly as ROCKNIX wrote it on this unit.

**Read-only root, and why.** A battery can die mid-write. The ext4 root is never written on the device (kernel `ro`
+ a RAM overlay — the Portable's overlayroot idea without an initramfs), so it cannot be corrupted. What must
persist lives on p3 (journalled ext4, `commit=5`, fsck'd every boot by `vibe-state` — a bounded wait, never fstab: a missing or damaged one costs that boot's saved settings, never the radio)
and on p4 (exFAT, fsck'd every boot; the engine writes temp + rename). Developer hot-fix: `mount -o remount,rw
/media/root-ro`, edit, `sync`, reboot.

## Splash

- **U-Boot shows nothing.** ROCKNIX's mainline U-Boot has no PX30 VOP/DSI video driver (checked: no `rk_vop`, no `bmp`
  command; `boot.bmp` there is only the bootflow-menu logo), so the panel stays dark until the kernel's panel driver
  lights it. Showing a logo from power-on needs a U-Boot with a PX30 display driver — Stuart's decision (below).
- **Kernel quiet:** no `console=tty0` (serial only), `quiet loglevel=3`, `vt.global_cursor_default=0`, no getty on tty1.
- **`vibe-splash`** (ours, ~300 lines of stdlib Python, not Plymouth): starts as one of the first units, draws the
  pre-rendered artwork (`art/make-splash.py`: the app icon + wordmark, 640×480) straight into a DRM dumb buffer, a
  **real** progress bar fed by the boot units (state 12 % → boot keys 22 % → card 35 % → engine started 50 % → engine
  answering 75 % → browser 100 %), and messages ("Radio settings reset", the card check, a fake card). At the hand-off
  it drops the DRM master but keeps its picture on screen; Cog becomes master and the splash exits only when the CRTC
  shows Cog's framebuffer — no black gap. Cog's own first frames use the splash's background colour (`--bg-color`).
  Why not Plymouth: its bar guesses from the previous boot's duration (a read-only root cannot keep it), it is a daemon
  plus a theme engine, and without an initramfs it starts no earlier than this.

## Display, GPU, input, audio

- **Panel: ROCKNIX's 80 Hz, unchanged.** ✗ Both 60 Hz modes failed on the device (2026-11-06): the 23.04 MHz one put the
  picture ~80 px left, the 22.92 MHz one drew the left half and columns of noise. The 80 Hz default is centred and clean,
  and the page draws at 30 fps regardless. The history: the overlay lists twelve 640×480 modes and marks 80.046 Hz
  `default=1`; Cog takes the PREFERRED mode and cannot pick by refresh. `board/panel-60hz.py` (unused now) moved it to the
  22.92 MHz mode (732 × 521 → 60.10 Hz) — the same bytes otherwise, same length. ★ It keeps the 80 Hz mode's horizontal
  timing (640,46,2,44); the 23.04 MHz mode (horizontal 640,46,20,44) put the picture ~80 px left on the panel.
- **Cog:** `cog -P drm --platform-params=renderer=gles --gamepad=manette` — WebKit's media defaults untouched: the page keeps its start / audio screen and the **A press on the pad** starts the audio (Stuart, 2026-11-05)
  as user `vibe-kiosk`, `Restart=always`, `Nice=5` (below the engine). WebKit sandbox ON (bubblewrap).
- **Gamepad:** `70-vibe-r36s-gamepad.rules` clears `ID_INPUT_TABLET{,_PAD}` on `r36s_Gamepad` (libmanette skips tablet pads).
- **Audio:** GStreamer → ALSA directly (`gstreamer1.0-alsa`; the browser is the only thing making sound). `vibe-hwd` sets
  `Playback Mux` from the jack switch (SPK by default — the codec is silent until it is set).
- **CPU:** `board/vibe-r36s.dts` restores the stock 408 / 600 / 816 / 1008 / 1104 / 1200 / 1248 / 1296 MHz steps with
  the vendor tree's voltages, disables 1416; `schedutil` (kernel parameter + `vibe-cpu.service`, which also caps at 1296).
- **Backlight:** boot level 120 in the overlay; `vibe-hwd` restores the owner's level.

## Services

| Unit | Does |
|---|---|
| `vibe-splash` | the boot screen (above) |
| `vibe-clock` | restores the clock at boot (RTC + saved offset, never earlier than the last save or the build), saves it at power-off |
| `vibe-cpu` | schedutil, max 1296 MHz |
| `vibe-state` | finds p3 (≤ 10 s), fsck (-p, then -y), mounts it, binds `/var/lib/vibeserver` + the browser's data, mounts VIBEBOOT read-only; every failure carries on in RAM |
| `vibe-hwd` | the hardware service, `/run/vibe-hwd.sock` (§0.2; SSH requests: "not supported") |
| `vibe-bootkeys` | Start + Select held at power-on → `/run/vibe-handheld/reset-radio` + "Radio settings reset" |
| `vibe-card` | first boot: capacity check → p4 → exFAT `VIBESDR`; every boot: `fsck.exfat -p`, mount at `/media/VIBESDR`, folders |
| `vibeserver-handheld` | `vibeserver --handheld` as `vibeserver`, `VIBESERVER_DATA_DIR=/var/lib/vibeserver`, `VIBESERVER_CARD_DIR=/media/VIBESDR`, `VIBE_HWD_SOCK=/run/vibe-hwd.sock` |
| `vibe-kiosk` | waits for the engine to answer on `http://127.0.0.1:48000/`, hands the panel over, runs Cog |

Masked: the package's network-server units (`vibeserver`, `-radios`, `-maintenance`, `-pocket*`), `getty@tty1`,
apt timers and every network service (`systemd-networkd`, `-resolved`, `-timesyncd`). Not on the card: cloudflared (dpkg path-exclude), avahi, apt
sources, and **no SSH at all** (Stuart 2026-11-05: "we don't need SSH" — no server package, no unit, no keys). Nothing
listens on any port but the engine on 127.0.0.1; `verify.sh` checks every enabled socket unit and service. Debugging
is the card's `Diagnostic logs/` export (or the serial console on `ttyS2`, which has no login: root is locked).

### vibe-hwd

Implements §0.2 exactly (newline JSON, one reply per request, events after `subscribe`, socket `root:vibeserver 0660`).
Volume buttons (adc-keys, grabbed) → ALSA `Master Playback Volume` on alsamixer's perceptual curve, ±5 per press/repeat,
`volume` events. Power button (grabbed; logind ignores it): short = screen off/on (`bl_power` 4 + brightness 0 — the
LEDs; DPMS needs the DRM master, which is Cog), held 2 s = `shutdown` event → ≤ 5 s for every subscriber's
`{"ack":"shutdown"}` → `sync` → the splash says "Switching off" → `systemctl poweroff` (which stops the engine, unmounts
the card, saves the clock). Any pad button wakes the screen. Battery polled every 20 s (5 s under 15 %): `battery`
on change, `battery_low` once at 10 %, the same clean shutdown at 3 % (two readings in a row). `set time` → clock +
RTC + saved floor. `get ssh` / `set ssh` / `set ssh-password` answer `{"ok":false,"error":"not supported"}` — a
password is never read or kept. Brightness and volume persist (`hwd.json`).

### The capacity check (first boot)

The **kernel's** size of the card is the claim; the check is the proof; p4 ends where the proof ends. 96 × 1 MiB
samples spread over the free space (bit-reversed order, so a time-out still spans the card), plus an **anchor** at the
start and **probes** at anchor + 2^k (1 GiB … 1 TiB): a fake that drops address bits wraps at a power of two, every probe
past the wrap lands on the anchor, and the first probe that reads back wrong gives the real size. Non-destructive,
f3probe-style: each sample is read (backup) before its write, all are read back, then every backup is written back in
**reverse** order — since an aliased read hits the same physical block as an aliased write, the reverse restore leaves
every block, ours included, as it was. ~200 MB of I/O, capped at 180 s. A bad card → a plain message on the splash
for 20 s ("Part of this card failed a check. It says Y, but only the first X can be trusted. VibeSDR itself needs S, leaving F for your files." — the figures add up as shown), then p4 sized to what works.
**Known limit:** a wrap at a size that is not a power of two (aliases between the samples) is not seen; such cards
usually also drop writes, which is seen. A power cut *during* the ~1 min check on a fake card is the one case that can
damage the card.

## Tests

- `test/capacity-test.py rootfs/usr/lib/vibesdr-r36s/vibe-card` — the check against simulated honest / wrap / drop
  cards, verdicts and restore (Mac, no hardware).
- `test/card-loop-test.sh <img>` — first + second boot of `vibe-card` on a loop device, a dirty exFAT, the `flush`
  option (needs a kernel with exFAT: the R36S, not colima).
- `test/device-hwd-test.sh` + `test/fake-keys.py` + `test/hwd-client.py` — `vibe-hwd` on the R36S against the real
  backlight / ALSA / battery, with uinput stand-ins for the buttons and an injected jack switch.
- **The image's own root, on the R36S under ROCKNIX** (nothing written to the card's boot area): unpack the build's
  root tree to `/storage/vibe-r36s-root` and run inside it with `test/enter-root.sh <cmd>` (pivot_root in its own mount
  namespace — WebKit's bubblewrap sandbox stays ON). First `systemctl stop essway sway input powerstate; rmmod
  mali_kbase; modprobe panfrost`; a reboot restores ROCKNIX. Then:
  `vibe-splash --measure` (the panel's refresh by vblank timing), `test/kiosk-test.sh URL` (splash → hand-off → Cog as
  `vibe-kiosk`: CPU, memory, Panfrost), `test/gamepad-test.sh [URL]` (the pads the page sees + an A press as the audio
  gesture), `test/bootkeys-test.sh`, `test/hwd-root-test.sh`, `test/card-loop-test.sh` (no argument: a synthetic
  4 GiB loop file). Under ROCKNIX the device nodes carry ROCKNIX's group ids; kiosk-test.sh adds them for the test.
- `verify.sh` — the offline checks every build runs (partition table, loader bytes, boot files, overlays applied with
  fdtoverlay, units enabled/masked, no SSH, no passwords, nothing listening but the engine, `systemd-analyze verify`).

## Decisions for Stuart

See `docs/v12/R36S-HANDHELD-IMAGE.md` §0 "Image build".

## Debug log to the card (no SSH)

Add ` vibe.debuglog` to the end of the `APPEND` line in `extlinux/extlinux.conf` on **VIBEBOOT** (any computer can
edit it). On the next boot `vibe-debuglog` writes `Diagnostic logs/debuglog-<boot id>.txt` on VIBESDR every 15 s:
CPU clocks and temperatures, memory and pressure, `top`, failed units, the state and restart count of every VibeSDR
unit, the GPU / input / sound devices and mixer, USB devices, the journal and the kernel log. With no VIBESDR mounted
it writes to VIBEBOOT instead. Remove the word to stop it — the card is not a log disk. Use it when the receiver
itself will not come up and EXPORT DIAGNOSTICS cannot be reached.
