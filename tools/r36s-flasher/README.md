# VibeSDR Flasher (R36S)

Makes VibeSDR cards for the R36S — `docs/v12/R36S-FLASHER.md` is the spec. macOS first; Go engine, Wails window.

**This build is a LOCAL TEST** (`release.LocalTest = true`): the image comes from `~/Desktop/VibeSDR-Flasher/` — the
newest `vibesdr-r36s-*.img.gz` there, checked against its `.img.gz.sha256`. The GitHub path (releases tagged `r36s-`)
is in `internal/release` for the published build. Simple mode, **Update** and **Advanced** are complete.

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

## Advanced (0.4.0)

- **Make a VibeSDR card — choose the drivers** (`internal/job/advanced.go`, `PrepareAdvanced`), checked before any card
  is touched (the window shows "Using …" or why not):
  - **a saved backup or image** — this app's backups (listed from `~/Documents/VibeSDR Backups`) or any R36S card image,
    `.img` / `.iso` / `.bin`, or `.gz` / `.xz` / `.zip` around one (`card.OpenSource` unpacks only as far as the boot
    partitions). Its drivers are read exactly as Simple mode reads a card's;
  - **generic ROCKNIX drivers** — the image as built (the EE clone device tree, the panel overlay, our `vibe-r36s.dtbo`);
  - **your own device tree** — a `.dtb` made for ROCKNIX (+ optional `.dtbo` panel overlays), copied to `/own` on the
    boot partition, our `vibe-r36s.dtbo` kept. Refused in one sentence: not a device tree, not an RK3326 (the fakes in
    R36S shells), or a stock vendor (Rockchip 4.4) DTB we do not know — one we know uses OUR device tree for that board.
  - onto **any** card, blank ones included; the card's backup is Whole / Boot / **Don't back up** (Advanced only; chosen
    for a card with no readable volumes, in red on the confirm when the card is not empty).
- **Put a backup back on a card** (`runRestore`): checked all the way through first (its `.sha256` when there is one),
  refused for a smaller card, the card optionally backed up, written, read back, ejected. An image that is not a whole
  number of sectors is padded (a raw card takes whole sectors only).
- Tests: `internal/job/advanced_test.go` (gz/xz/zip/plain backups, VibeSDR backup with the debug log, unknown board,
  own DTB + overlay, the refusals, restore incl. sector-only) and `frontend/test-ui.mjs` (every Advanced flow's job).

## Build, test, sign

```sh
go test ./...                       # engine end to end on image FILES (the image ~/R36S-images/LATEST names, or VIBE_TEST_IMAGE)
tools/r36s-flasher/build-mac.sh     # tests → universal build → Developer ID → notarise → staple → ~/Desktop/VibeSDR-Flasher
```

Wails v2: `go install github.com/wailsapp/wails/v2/cmd/wails@v2.17.1`. go-diskfs (MIT) reads/writes FAT inside image
files; pgzip (MIT) for the backups.
