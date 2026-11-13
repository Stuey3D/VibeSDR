# VibeSDR Flasher (R36S)

Makes VibeSDR cards for the R36S — `docs/v12/R36S-FLASHER.md` is the spec. macOS first; Go engine, Wails window.

**This build is a LOCAL TEST** (`release.LocalTest = true`): the image comes from `~/Desktop/VibeSDR-Flasher/` — the
newest `vibesdr-r36s-*.img.gz` there, checked against its `.img.gz.sha256`. The GitHub path (releases tagged `r36s-`)
is in `internal/release` for the published build. Simple mode is complete; **Update** and **Advanced** say "Coming soon".

## Simple mode, step by step

1. **The card** — removable physical disks only (`internal/disks`): the Mac's startup disk, fixed internal drives, disk
   images and virtual disks are never offered. ★ The MacBook's own SD slot reports itself *internal* — it is removable
   media, so it is offered (a plain "external disks" filter would hide it).
2. **The backup**, to a folder the person chooses — whole card (every byte, gzip) or boot only (the first 16 MiB and the
   boot partition) — then **read back** and compared (SHA-256) before the card is touched. A README and a `.sha256` go
   beside it.
3. **The drivers** (`internal/card`):
   - a **VibeSDR card** keeps its boot menu's choice (device tree, overlays) and `vibe.debuglog`; the files are the new
     image's own (built for its kernel) unless it lacks one, which is then copied from the card;
   - a **stock card** is recognised only by its vendor DTB **byte for byte** (today the EE clone's `rf3536k3ka.dtb`) and
     gets OUR device tree — a vendor (Rockchip BSP 4.4) DTB is never copied into the image;
   - anything else **stops before writing** with one sentence; the backup is safe and the card unchanged.
4. **The write** — the image as published, its boot partition replaced by the prepared one, onto `/dev/rdiskN` opened
   exclusively (so macOS cannot remount what was just written), then **read back** and compared, then ejected.

Writing a card needs root: the window runs the same binary as `--helper job.json` through `osascript … with
administrator privileges` (one password prompt). The helper only ever sees files in `~/Library/Caches/VibeSDR Flasher`
(macOS keeps root's hands off the Desktop and Documents); the window copies the image in first and moves the backup to
the chosen folder afterwards.

## Build, test, sign

```sh
go test ./...                       # engine end to end on image FILES (needs ~/R36S-images/…beta74….img.gz or VIBE_TEST_IMAGE)
tools/r36s-flasher/build-mac.sh     # tests → universal build → Developer ID → notarise → staple → ~/Desktop/VibeSDR-Flasher
```

Wails v2: `go install github.com/wailsapp/wails/v2/cmd/wails@v2.17.1`. go-diskfs (MIT) reads/writes FAT inside image
files; pgzip (MIT) for the backups.
