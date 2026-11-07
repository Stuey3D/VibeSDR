# R36S card tool — make a VibeSDR card from the card the handheld already boots

Status: **plan** (Stuart, 2026-11-07: "the best way of doing it and the most plug and play way too").
Context: the R36S image (`image/r36s/`, docs/v12/R36S-HANDHELD-IMAGE.md) runs so well on Stuart's unit that other R36S
and clone owners will want it. The clones differ in exactly one layer — the **device tree** (panel, buttons, speaker
amplifier, battery, RAM settings). Everything above it (kernel, engine, native UI) is the same. Every clone ships with
its maker's working device tree **on the card in the box**. This tool takes it from there.

## 1. The flow (Stuart's design)

1. The user downloads **VibeSDR Card Maker** (Mac and Windows first, Linux after).
2. They insert the R36S's **current, working** SD card.
3. The tool **reads the boot partition** and extracts the drivers (§3).
4. The tool **integrates** them into the VibeSDR image (§4).
5. **Backup, with confirmation:** the whole original card is saved to a compressed image file. It can be declined —
   e.g. not enough disk space (the tool says how much it needs). **A small "drivers backup" (the boot partition's
   files, a few MB) is ALWAYS kept**, even when the full backup is declined.
6. After the backup (or the decline), the card is **erased and written** with VibeSDR + that unit's drivers, then
   **read back and verified**.
7. The card goes back in the R36S and boots straight to the receiver with a correct picture.

A **Restore** button writes a saved backup back to a card — the way home to the stock firmware.

## 2. What is on the user's card (what we read)

- An ArkOS / stock-style card has a **FAT boot partition** (readable on any computer without admin rights) holding:
  the vendor kernel, one or more `.dtb` files (often several, e.g. `rk3326-r35s-linux.dtb`, `rk3326-r33s-linux.dtb`),
  and the boot script (`boot.ini`, `extlinux.conf` or `boot.scr`) that says **which** DTB this unit boots.
- Stuart's unit: the stock card image is `~/R36S-stock-card.img` (16 GB raw, 2026-11-05); its DTB is
  `image/r36s/board/rf3536k3ka.dtb` and is the reference for this work.

## 3. Extraction

- Copy the **entire boot partition** (files + the boot script) into the drivers backup.
- Find the **DTB the unit actually boots** from the boot script; if there are several and no script names one, keep
  them all and let identification (§5) choose.
- From that DTB read the **facts**, never the DTB as a whole (§4 says why):
  - the panel: `dsi@ff450000/panel@0` — its timings and init sequence (`panel-init-sequence`, `display-timings`) —
    the same extraction as ROCKNIX's `importpanel.py` (packages/linux-drivers/generic-dsi/scripts/);
  - `model` / `compatible` (board identification, §5);
  - the joypad GPIO / ADC map, the speaker-amplifier enable pin, the battery's design capacity and curve, the RAM
    (`ddr_timing`, `dmc-fsp`) — recorded even where we cannot use them yet.

## 4. Integration — why the facts, not the file

- The stock DTB was written for the **vendor kernel** (Rockchip BSP 4.4 / 5.10). VibeSDR runs **ROCKNIX's mainline
  7.1** kernel, whose bindings differ for much of the same hardware — the stock DTB cannot be booted as-is.
- So, as ROCKNIX does (and as Stuart's own image already does):
  - the base is the matching **ROCKNIX mainline DTB** for the board (§5);
  - the panel becomes `overlays/mipi-panel.dtbo` for ROCKNIX's `generic-dsi` driver — **generated on the computer**,
    so the first boot already has the right picture (no regenerate-and-reboot on the device);
  - VibeSDR's own overlay goes on top as now (`vibe-r36s.dtbo`: CPU steps, backlight; the four clock-only display
    modes are added to the panel overlay as in `build-image.sh`);
  - the user's whole boot partition is also copied to `VIBEBOOT/stock/`, so the device can regenerate the panel
    overlay itself, and so it is never lost.

## 5. Identification — board, then chips, then the person

Each step can only narrow the list; most units should need none of the last one.

| What | How |
|---|---|
| **Board family / revision** | The DTB's `model`/`compatible`, and on the device the bootloader's board-ID ADC (`uboot.hwid_adc=b,508` on Stuart's unit) — ROCKNIX picks its RK3326 trees by this. |
| **Panel** | From the stock DTB (above). On the device, research: many DSI panel controllers (the ST7703 family included) answer the DCS **read ID** commands (0xDA/0xDB/0xDC) over the link even before the video timing is right — an automatic ID where the panel answers. |
| **Power chip, fuel gauge, codec, amplifier** | Probe the known I²C addresses at boot; a chip that answers is fitted. |
| **Buttons / sticks** | Per board; where clones differ, a one-time "press each button" check (the pad test already exists). |
| **Speaker amplifier** | Per board; otherwise one test tone — "did you hear it?". |
| **Battery capacity** | Not detectable — learnt: the fuel gauge calibrates over a few cycles (the time-left estimate improves by itself). |
| **RAM** | Measured by the bootloader (730 MB vs 1 GB: see the memory note — parked). |

## 6. The safety net on the device — the PANEL FINDER

For a lost card, a DTB the tool could not read, or an image flashed directly:
- the image carries **every known R36S-family panel** (ROCKNIX's trees + the community's numbered panels, plus any
  made from a stock DTB in `stock/`);
- first boot without a chosen panel, or a key combination held at power-on, starts it on the most likely panel (by
  board ID; the stock DTB's first);
- a full-screen test card — a big number, colour bars, a white frame at the very edge (a shifted picture shows) — and
  **beeps the panel number** on the speaker, so a black screen still says where it is;
- **D-pad right / left** = next / previous panel; **the bottom button** = "that's the one" (saved to the boot
  partition, then normal start); **no press for ~20 s** = the next panel by itself, so a user with a black screen just
  waits until the picture appears and presses the button;
- each try needs a reboot (~10–15 s: the panel is set up by the kernel at boot) — minutes, once.

## 7. Writing safely

- Only **removable** disks are offered; the card's name and size are shown; erasing needs a confirmation.
- Read back and **verify** after writing.
- **Fake-capacity cards** (Stuart's "64 GB" was 16 GB): warn before writing (a quick capacity probe), as well as the
  image's own first-boot probe.
- The full backup is compressed (most of a card is empty); the space needed is shown before asking.

## 8. Build choices (open)

- One app, a small privileged helper for raw disk access. Research first: build on **Raspberry Pi Imager**'s open
  source (Apache-2.0; it already solves raw writing safely on all three systems) or write our own (Rust/Tauri or Qt).
- The panel extraction as a library both the tool and the device use (one implementation).
- Where the images come from: the tool downloads the current VibeSDR R36S image (signed), or takes a local file.

## 9. Order of work

1. **The panel set and ROCKNIX's mechanism** — collect ROCKNIX's RK3326 trees + the community numbered panels; read
   ROCKNIX's first-boot `002-generate_dtbo` and its board-ID table.
2. **The device side**: board-ID base DTB selection, `stock/` → overlay generation on first boot, the **panel finder**.
   (Useful on its own: the image can go to other owners with instructions before the tool exists.)
3. **The extraction library** (ours — `importpanel.py` is read for inspiration, the extraction written in our code).
4. **Card Maker** — Mac + Windows: read → extract → integrate → back up (confirm / decline) → write → verify; Restore.
5. Testers with other clones: their stock DTB + a photo of the screen; each one widens the known panel set.
