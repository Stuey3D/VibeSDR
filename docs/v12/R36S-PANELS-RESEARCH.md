# R36S panels and ROCKNIX's device-tree mechanism — research

Status: **research** (2026-11-07). This is step 1 of §9 in [R36S-CARD-TOOL.md](R36S-CARD-TOOL.md). It feeds §5
(identification) and §6 (the on-device PANEL FINDER). Nothing here was built or flashed.

**How sure each claim is.** **(read)** means I read it in the source at the commit given. **(decoded)** means I parsed
the DTB/DTBO files myself (Python `fdt` 0.3.x in a scratch venv; no dtc on this Mac). **(source)** means a web page
says it and I could not check it further. **(unverified)** means it is my reading or inference.

Scratch clones are in `~/R36S-debug/research/` (outside the repo):

| Clone | Ref | Licence |
|---|---|---|
| `distribution/`: ROCKNIX/distribution, branch `next` (sparse) | `041a615bdc20cc989d55ee26f258c27e35135ad6` (2026-11-07) | repo GPL-2.0 (LICENSE.md); DTS files `(GPL-2.0+ OR MIT)` |
| `overlay_server/`: stolen/overlay_server, the source of `rocknix_dtbo.py` | `04e5e55b82d30c5c03d060f44cb6d7cd8840f531` (2026-03-18). ROCKNIX's `generic-dsi/package.mk` pins this exact commit. | **MIT** (Danil Zagoskin) |
| `arkos4clone/`: lcdyk0517/arkos4clone, `main` (sparse) | `0e7d6060cfccae3b87d3b26688b051e5829ff78e` (2026-11-06) | repo **MIT** |
| `stolen_overlays/`: release `panel_overlays` of stolen/r.nix-distribution (now stolen/rnix) | assets 2024-07 → 2025-04 | none stated |
| `linux/`: torvalds/linux master (sparse: drm/panel, dw-mipi-dsi, include) | `d48723711bae9b8cdc56719c448ba42fef28cbee` (2026-11-06) | GPL-2.0 |

---

## 0. The headline findings

1. **Stuart's panel is not an ST7703.** The stock DTB *says* `compatible = "sitronix,st7703", "simple-panel-dsi"`.
   But its init sequence starts `FF 30 / FF 52 / FF 01 / E3 00`. That is the **NewVision NV3051D** page-select
   preamble, byte for byte the start of mainline `panel-newvision-nv3051d.c` (read + decoded). Vendor BSP DTBs keep a
   copy-pasted `compatible` because `simple-panel-dsi` ignores it. **Never identify a panel by its `compatible`.**
   Use the init sequence. The "Panel: Sitronix ST7703" line in R36S-HANDHELD-IMAGE.md §0 should be corrected.
2. **Stuart's unit = arkos4clone "Clone Type 1 Panel 2"** (decoded). Its 151-command init sequence is identical to
   that DTB's. The timing is identical (640×480, 30 MHz, 46/2/44 + 16/2/14 = 80.05 Hz). The panel reset is identical
   (GPIO3_B7, active low). The LCD-power pin is identical (GPIO0_B5). The joystick-mux enable pin is identical (12).
   - The same panel is also in "Clone Type 1 Panel 5" and "Clone Type 2 Panel 2", wired to different pins there.
3. **The board-ID ADC does not tell clones apart.** On ROCKNIX's B image, every ADC reading in 490–540 boots
   `rk3326-gameconsole-eeclone.dtb` (Stuart: 508). Everything that varies between clones comes from the
   **stock DTB → overlay** step: panel, panel pins, joypad tweaks, headphone detect, speaker amplifier.
   The ADC picks the *family*, never the *variant*.
4. **ROCKNIX's first boot does more than the panel.** The generator is **`rocknix_dtbo.py`** (MIT, stolen/overlay_server).
   `importpanel.py` is not it. That is why a panel-only overlay broke audio on Stuart's unit. Its panel half is the
   same algorithm as `importpanel.py`.
5. **The `generic-dsi` driver can already read from the panel.** An `I … read=N` line does a DSI read during panel
   power-up and prints the bytes to dmesg. It also has a `descfile=` parameter that loads a description from
   `/lib/firmware` instead of the DT (read). Both are useful for the panel finder (§6).
6. **About 14 distinct init sequences cover every R36S-family DTB in arkos4clone.** ROCKNIX's tree plus stolen's
   overlay set already match 7 of them exactly (decoded, §3.3).

---

## 1. ROCKNIX's RK3326 boot-time DTB selection

### 1.1 Where the ID comes from (read)
- U-Boot (mainline v2025.10, `rk3326-handheld_defconfig`, `CONFIG_TARGET_ODROID_GO2=y`) runs
  `board/hardkernel/odroid_go2/go2.c`: `adc_channel_single_shot("saradc@ff288000", 0, &adc_info)`. That is **SARADC
  channel 0**, 10-bit. Upstream U-Boot matches it against OGA 856 / OGA v1.1 677 / OGS 85, ±50.
- ROCKNIX's patch `devices/RK3326/packages/u-boot/patches/0001-add-support-for-k36-clone-with-emmc-rk3326.patch`
  (Danil Zagoskin, 2025-06-21) adds a GENERIC fallback. It also adds `env_set_ulong("hwid_adc", adc_info)`, so the
  **raw value** reaches the boot script.
- The boot script prefixes the image letter and passes it to the kernel: `uboot.hwid_adc=b,508` means the B image
  (mainline U-Boot) read 508. The legacy (hardkernel) U-Boot has no `hwid_adc`. The script then maps `${hwrev}` to a
  pseudo-ADC value and passes `a,<hwrev>`.
- ROCKNIX's `update.sh` reads the letter back from `/proc/cmdline` to choose `a_`/`b_` `boot.scr` and U-Boot.

### 1.2 The table (read: `devices/RK3326/packages/u-boot/config/{a,b}_boot.ini`, built into `{a,b}_boot.scr` by mkimage)

**B image** (`b_boot.ini`, mainline U-Boot; Stuart's):

| Test, in order | DTB |
|---|---|
| PX30S silicon (`DDR_GRF_CON1[15:14] == 0b11` at 0xff630004, read *before* the ADC "because the pin floats") | `rk3326s-gkd-pixel2.dtb` |
| 140–190 | `rk3326-powkiddy-rgb10x.dtb` |
| 1000–1050 | `rk3326-magicx-xu-mini-m.dtb` |
| **490–540** | **`rk3326-gameconsole-eeclone.dtb`** |
| anything else | `rk3326-odroid-go2.dtb` ("Unexpected ADC value … falling back to OGA dtb") |

**A image** (`a_boot.ini`, legacy U-Boot):

| Range | DTB |
|---|---|
| 60–110 | odroid-go3 |
| 140–190 | gameconsole-r33s |
| 450–495 | gameforce-chi |
| < 541 (the rest) | anbernic-rg351v |
| 652–702 | GPIO C22/D9 decide: powkiddy-rgb10 / odroid-go2-v11 / anbernic-rg351m |
| 831–881 | odroid-go2 |
| 1000–1050 | magicx-xu10 |
| else | odroid-go2 |

- Both scripts finish with `sysboot … /extlinux/extlinux.conf`. ROCKNIX's `extlinux.conf` has `FDTDIR /` (which uses
  `${fdtfile}`) and `FDTOVERLAYS /overlays/mipi-panel.dtbo`. The overlay file may be absent; the README says "that's ok".
  `APPEND` ends `uboot.hwid_adc=${hwid_adc}`.
- **Boards not in either table** get a generated `extlinux.conf.<name>` with a fixed `FDT` line (`package.mk`,
  `generate_custom_extlinux_conf_files`). The user renames it to `extlinux.conf`. **`rk3326-gameconsole-r36s` (the
  original R36S) is one of these**: it is not in either `.ini` (read). How R36S owners are told to do this was not
  confirmed.
- **`rocknix,device_switch`** (read: `rk3326-device-group-r36.dtsi`, `packages/apps/device-switch/scripts/RK3326/device-switch`)
  lets r33s / r36s / rgb10x users switch DTB from the UI. It rewrites the `FDT` line in `/flash/extlinux/extlinux.conf`
  and keeps an `.orig`.
- **What this means for us:** our `extlinux.conf` already names the eeclone DTB directly and passes
  `uboot.hwid_adc=${hwid_adc}`. That stays correct for EE clones. To support other families on one card, we need
  either ROCKNIX's `boot.scr` choosing `${fdtfile}` (then `FDT /${fdtfile}`, not a fixed path), or our own small table.

### 1.3 The RK3326 DTBs ROCKNIX ships (read: `projects/ROCKNIX/config.xml`, `<RK3326>`)
- **A image:** anbernic-rg351m, anbernic-rg351v, gameconsole-r33s, **gameconsole-r36s** (compatible
  `gameconsole,r35s` + `gameconsole,r36s`), magicx-xu10, odroid-go2, odroid-go2-v11, odroid-go3, powkiddy-rgb10.
- **B image:** powkiddy-rgb10x, powkiddy-rgb20s, magicx-xu10, magicx-xu-mini-m, **gameconsole-eeclone** (model
  "Generic EE clone"), batlexp-g350, rk3326s-gkd-pixel2.
- The DTS directory also has `rk3326-gameconsole-r3xs.dtsi` (the R33S/R36S base), the `rk3326-panel-elida-kd35t133.dtsi`
  generic-dsi replacement, and `retrogame_joypad_s2_f1.dtsi`.
- **No R36Max / R36H / R36 Pro / R36S Plus DTB.** "R36Max" is only a ROCKNIX device *page*. The ADC range sends it
  to eeclone, plus an overlay (unverified).
- There is no rg351p/rg351mp DTB by name; stolen's `rg351mp-v2` overlay exists (below).

---

## 2. ROCKNIX's first-boot panel overlay

### 2.1 What runs, and when (read)
- `packages/hardware/quirks/devices/Generic EE clone/002-generate_dtbo` is a device quirk. The directory name equals
  the eeclone DT's `model`. That quirks are selected by `QUIRK_DEVICE` = the DT model is my reading (unverified).
- Steps:
  1. **No-op if `/flash/overlays/mipi-panel.dtbo` exists.** That file is the whole "regenerate?" decision. There is no
     hash or version check; deleting the file forces a new run.
  2. Stock DTB: `/flash/stock/*.dtb`. If there is none, it mounts the partition labelled **`EMUELEC`** (the EE clones'
     stock firmware, still on the unit) and copies its `*.dtb`. If neither exists, it gives up.
  3. `STOCKDTB=$(ls /flash/stock/*.dtb … | head -1)`. **The alphabetically first DTB wins.** A stock card with several
     DTBs can pick the wrong one. Our tool must put exactly one there, or the one the boot script names.
  4. `/usr/libexec/generic-dsi/rocknix_dtbo.py STOCKDTB -o …/overlays/mipi-panel.dtbo`. No option flags are passed.
  5. rsync to `/flash`, then **`/usr/share/bootloader/update.sh`**, which **reinstalls U-Boot**. It picks
     `b_uboot.bin.uart5` if the console is on UART5 (`ff178000`). The EE clones' debug UART is UART5, and the eeclone
     DTS turns `uart5` on and `uart2` off. Then `boot.scr` is refreshed, and finally `reboot`.
- `config/stock/README.txt`: "On some devices (EE clones) you can put your stock .dtb file to this dir if the wizard
  could not locate it automatically."
- The web version of the same generator is `https://rocknix.gosk.in/dtbo/` (source: rocknix.org EE-clones page). Its
  server is `overlay_server.py` in the same MIT repo.

### 2.2 What `rocknix_dtbo.py` reads from the stock DTB (read, `make_dtbo`)
- **Panel:** `__symbols__/dsi` + `/panel@0`, i.e. `/dsi@ff450000/panel@0` (RK3326; RK3566 is `/dsi@fe060000/panel@0`).
  It reads:
  - `prepare-`, `reset-`, `init-`, `enable-delay-ms` (default 50 each);
  - `dsi,format` (0..3 = rgb888 / rgb666 / rgb666_packed / rgb565), `dsi,lanes`, `dsi,flags`;
  - `display-timings` (every `timing*`: clock-frequency, h/v active, front porch, sync length, back porch; `native-mode`);
  - `width-mm` / `height-mm` (falls back to a 3.5″ diagonal);
  - `panel-init-sequence`;
  - `reset-gpios`;
  - `power-supply` → that regulator's `gpio`;
  - `enable-gpios`.
- **Board, besides the panel:**
  - `/adc-keys`: present and enabled → `status=okay`. Absent or disabled → disable it in the overlay, and lift the
    volume keys from `/play_joystick` `key-gpios[14..15]` + `/pinctrl/buttons/gpio-key-pin` into a `gpio-keys` node.
  - **Joypad** (`&joypad`): un-inverts the left stick by default. With adc-keys absent it switches to the
    "My Mini" four-ADC layout.
  - `/rk817-sound` `hp-det-gpio` + `/pinctrl/headphone/hp-det` (pull direction kept).
  - The amplifier from `spk-con-gpio`, or from the codec's `spk-ctl-gpios`: enables `rk817-sound-amplified` +
    `audio-amplifier` `enable-gpios` + pinctrl `spk-amp-enable-h`. Otherwise it uses `rk817-sound-simple`.
- **Option flags** (the CLI's second argument, dash-separated; the wizard passes none):

  | Flag | Effect |
  |---|---|
  | `Dno` | skip the stock native mode |
  | `DR90` / `DR180` / `DR270` | panel rotation |
  | `JPk36` / `JPmm` | force the K36 or My Mini joypad |
  | `LSi` / `RSi` | invert the left / right stick |
  | `HPi` | invert headphone detect |
  | `SRs` | force simple audio routing (no amplifier) |

- **Quick path:** if the stock root `compatible[0]` contains `odroidgo3` (the original R36S stock DTBs), it returns
  after the panel description alone. The original R36S needs no pin or audio fix-ups on ROCKNIX's r36s tree.
- The result is a DT overlay:
  - `fragment@0` targets `/` and writes `dsi@ff450000/panel@0 { compatible = "rocknix,generic-dsi"; panel_description = …; reset-gpios; enable-gpios? }`,
    plus `pinctrl/gpio-lcd/lcd-rst` and `vcc18-lcd0`;
  - further fragments carry adc-keys, `&joypad`, and audio;
  - `__fixups__` resolve against the base DTB's labels (`gpio0..3`, `pcfg_pull_none/up/down`, `joypad`).
  
  This is exactly the shape of `image/r36s/board/mipi-panel.rocknix.dtbo` (decoded: four fragments; fixups `gpio0/2/3`,
  `pcfg_*`, `joypad`). Stuart's overlay has 12 `M` lines, which fits `rocknix_dtbo.py`'s 11-rate list
  (`importpanel.py` adds 54.8766 Hz, making 13).
  - I was **not** allowed to run the script here to prove a byte-identical rebuild. Running it is a one-line check for
    Stuart (`rocknix_dtbo.py rf3536k3ka.dtb -o x.dtbo; cmp`).

### 2.3 `importpanel.py` (read: `packages/linux-drivers/generic-dsi/scripts/importpanel.py`, no SPDX header, so under ROCKNIX's GPL-2.0)
- Panel only: it tries `dsi@ff450000/panel@0`, then `dsi@fe060000/panel@0`. Outputs: `pdesc` (text with comments),
  `dts`, or `dtbo` (a single `fragment@0`, `target-path = "/dsi@ff450000/panel@0"`).
- It needs `-D <inches>` if width/height-mm are missing. Otherwise it computes the same as the panel half of
  `rocknix_dtbo.py`.
- Package comment: "importpanel.py still here because it's simple, documented in wiki and performs just one task well".

### 2.4 The `panel_description` format (read: `packages/linux-drivers/generic-dsi/sources/panel-generic-dsi.c`)
A DT string list, one line per entry (or a firmware file, see `descfile` below). `#` starts a comment. The first
character picks the line type, and the rest is `key=value` pairs parsed by the kernel's `next_arg`/`get_options`.

| Line | Keys | Meaning |
|---|---|---|
| `G` | `size=W,H` | physical size in mm (goes to the connector's `width_mm` / `height_mm`) |
| | `delays=P,R,I,E,Y` | ms: **prepare** (after the enable GPIO, before reset) · **reset** (reset held) · **init** (after reset released, before the init sequence) · **enable** (after display-on + exit-sleep) · **ready** (parsed, currently unused — its `msleep` is commented out). Defaults 5,1,25,120,50; at most 5 values are read. |
| | `format=` | `rgb888` / `rgb666` / `rgb666_packed` / `rgb565` |
| | `lanes=` | DSI data lanes |
| | `flags=0x…` | `dsi->mode_flags` (mainline `MIPI_DSI_*` bits) |
| `M` | `clock=` | pixel clock in **kHz** (a mode is kept only if > 5000 and both actives > 0) |
| | `horizontal=a,fp,sync,bp` | active, front porch, sync length, back porch (px) |
| | `vertical=a,fp,sync,bp` | the same, in lines |
| | `default=1` | marks the DRM **preferred** mode |
| `I` | `seq=<hex>` | a raw DCS write of the whole buffer (`mipi_dsi_dcs_write_buffer`; byte 0 is the command) |
| | `dcs=<hex> data=<hex>` | the alternative form: `mipi_dsi_dcs_write(cmd, data)` |
| | `wait=<hex>` | ms to sleep after the command. **Hex**, parsed by `simple_strtoul(…, 16)`. So `wait=200` means 0x200 = 512 ms, not 200. Harmless but surprising; our generator must keep the same convention. |
| | `read=<hex n>` | instead of writing, **`mipi_dsi_generic_read(seq bytes, n≤8)`**; the bytes are logged with `dev_info` ("read[i]: xx") |

- Every mode gets `NHSYNC | NVSYNC` (the driver comment says all tested panels accept it).
- The probe defaults are `VIDEO | VIDEO_BURST | LPM | NO_EOT_PACKET | CLOCK_NON_CONTINUOUS`, 1 lane, RGB888. `G` overrides them.
- **Flags:** the generators take the stock `dsi,flags` and OR in `0x400`. Stock `0xa03` = VIDEO|BURST|NO_EOT(0x200)|LPM(0x800)
  (the BSP 4.4 bit layout matches mainline here, unverified for every bit). Adding 0x400 = CLOCK_NON_CONTINUOUS gives
  **`0xe03`**.
- **Power-up order** (`generic_panel_prepare`):
  1. vdd, then iovcc;
  2. enable-gpio on, then the *prepare* delay;
  3. reset on, the *reset* delay, reset off;
  4. the *init* delay;
  5. every `I` line;
  6. display-on (0x29) then exit-sleep (0x11) — note the order;
  7. the *enable* delay.
- Unprepare sends display-off and enter-sleep.
- `panel_description` lines with a `#` comment are fine. pyfdt cannot store empty strings, so the generators drop
  blank lines.
- **`descfile=`** (module parameter): if set, the description is read with `request_firmware()` instead of the DT.
  The driver is **built in** on RK3326 (`obj-y` added in `packages/linux/package.mk`), so on our kernel it would be
  `panel_generic_dsi.descfile=<name>` on the command line. The file must be readable at panel probe, i.e. inside the
  initramfs (our `vibe-initrd`) under `/lib/firmware/` (unverified that it loads that early — test it).
- The kernel also has the in-tree `CONFIG_DRM_PANEL_NEWVISION_NV3051D=y` and `CONFIG_DRM_PANEL_SITRONIX_ST7703=y` (read:
  `devices/RK3326/linux/linux.aarch64.conf`). ROCKNIX's `001-panel-updates.patch` adds `gameconsole,r36s-panel`,
  `anbernic,rg351v-panel-v2` and `magicx,xu10-panel` modes to the ST7703 driver.

---

## 3. The community's numbered panels

### 3.1 Naming (read: arkos4clone `Changelog.md` + `dtbTools/internal/console/config.go`)
**The "Panel 1 = Type 1, Panel 8 = Type 2, Panel 4 = Type 3, Panel 7 = Type 5" mapping in our docs is out of date.**
arkos4clone renamed again on 2025-09-05 and 2025-09-15. Today the names are two-dimensional:

- **"Clone Type N"** = the **board** wiring. 20250915: "Clone device Type is now distinguished by the GPIO multiplexed
  with the joystick ADC". In the DTBs, Type 1 has `amux-en` 12 and Type 2 has `amux-en` 11.
- **"Panel M"** = the panel within that board type.
- **"GameConsole R36S Panel 0–4"** = genuine-R36S boards (reset GPIO3_C0, mux 11/8/13).
- 20250915 renames:
  - Type 3 Panel 1 → **Type 1 Panel 2**;
  - Type 3 Panel 2 → Type 1 Panel 3;
  - Type 3 Panel 3 → Type 1 Panel 4;
  - Type 5 → Type 1 Panel 5;
  - new: Type 1 Panel 6 and Type 2 Panel 2.
- 20250905 renames:
  - Type 4 → Type 3 Panel 3;
  - "R36S Panel 4" → "Panel 4 Type 1";
  - "R36S V22" → "Panel 4 Type 2";
  - V30 removed ("motherboard numbering is no longer used").
- **Forum "Panel N" numbers are therefore meaningless without a date.** Go by the init sequence (§3.3). Older names
  (Clone Type 1–5, SoySauce panel 1–5, R36S v21/v22/v30) still appear in issues.
- In the repo, each variant is a directory `boot/dArkOS/consoles/<name>/`. It holds a BSP 4.4 `*.dtb`, a `5.10/` DTB
  (jokubasver's 5.10 kernel) and a `boot.ini`.
- Identification tools:
  - `https://lcdyk0517.github.io/dtbTools.html` (README) and the older `https://r36s.dpdns.org/dtbTools.html`.
    Both match a stock DTB to a variant.
  - The repo's `dtbTools/` (Go) is the on-device DTB selector.

### 3.2 The R36S-family DTBs, decoded (BSP 4.4 DTBs from arkos4clone; all `simple-panel-dsi`, 4 lanes, stock flags 0xa03)
"Seq" is the first 8 hex digits of the SHA-1 of the raw `panel-init-sequence`. Identical seq = identical init.

| Variant (arkos4clone dir) | Controller (by init preamble) | Res | Native mode (clock, h fp/sync/bp, v fp/sync/bp, rate) | Reset / power pins | Seq |
|---|---|---|---|---|---|
| clone type1 panel1 (+ amp, + invert) | ST7703 (`B9 F1 12 83`) | 640×480 | 30 MHz 150/50/150, 20/6/12, 58.50 Hz | GPIO3_B7 / en GPIO0_B5 | 94162d76 |
| **clone type1 panel2** = **Stuart** | **NV3051D** (`FF 30 52 01`) | 640×480 | 30 MHz 46/2/44, 16/2/14, **80.05 Hz** | GPIO3_B7 / GPIO0_B5 | **ca87b989** |
| clone type1 panel3 | NV3051D | 640×480 | 30 MHz, same timing, 80.05 Hz | GPIO3_B7 / GPIO0_B5 | 535b74d6 |
| clone type1 panel4 | ST7703-like (`B9 F1 12 87`; the last byte differs from ST7703's 83 — unverified chip) | 640×480 | 32 MHz 150/120/120, 20/6/12, 59.98 Hz | GPIO3_B7 / GPIO0_B5 | 1335be8a |
| clone type1 panel5 (+ invert) | NV3051D | 640×480 | 80.05 Hz | **GPIO3_D3 / GPIO3_A3** | ca87b989 |
| clone type1 panel6 | ST7703 (sleep-out first, then `B9 F1 12 83`) | 640×480 | 30 MHz 120/120/120, 16/4/12, 58.59 Hz | GPIO3_B7 / GPIO0_B5 | c6715f54 |
| clone type2 panel1 (+ amp, + rk809) | ST7703 | 640×480 | 58.50 Hz | GPIO3_D3 / GPIO3_A3 | 94162d76 |
| clone type2 panel2 | NV3051D | 640×480 | 80.05 Hz | GPIO3_D3 / GPIO3_A3 | ca87b989 |
| origin panel0 (also r33s, xu10, rg351v panel2) | ST7703 | 640×480 | 50 MHz 450/46/450, 17/5/13, 61.22 Hz | GPIO3_C0 | 4962c634 |
| origin panel1 | unknown: `E0 AB BA / E1 BA AB` unlock. The same key is in mainline `panel-boe-th101mb31ig002-28a.c`, which does not name its controller. | 640×480 | 29 MHz 80/20/80, 20/4/12, 68.54 Hz | GPIO3_C0 | cc3ecb6e |
| origin panel2 | NV3051D | 640×480 | 29 MHz 46/2/44, 16/2/14, 77.38 Hz | GPIO3_C0 | 18ab78d4 |
| origin panel3 | NV3051D | 640×480 | 77.38 Hz | GPIO3_C0 | fea2ddd5 |
| origin panel4 type1 (also r36h) | NV3051D | 640×480 | 77.38 Hz | GPIO3_C0 | bc571422 |
| origin panel4 type2 (ex-"V22") | NV3051D | 640×480 | 58 MHz 300/200/300, 17/5/13, 78.21 Hz | GPIO3_C0 | 8875cacc |
| k36, r36pro | ST7703 | 640×480 | 58.50 Hz | GPIO3_B7 | 94162d76 |
| r36max type1/2/3, r36ultra | ST7703 | **720×720** | 50 MHz 140/80/140, 20/4/20, 60.60 Hz | B7 or D3 | 0cdb2c3a |
| r36splus | ST7703 (sleep-out first) | 720×720 | 36.5 MHz, 59.87 Hz | GPIO3_C0 | a983969a |
| r36max2 | unknown (`EE 01 / EA 07`) | **1024×768** | 62 MHz, 60.83 Hz | GPIO3_D3 | f680da66 |
| rg351mp, rg351v panel1 | NV3051D | 640×480 | 26.4 / 25.5 MHz, 60.00 Hz | GPIO3_C0 | 912ba9e7 |

Stuart's stock DTB (`rf3536k3ka.dtb`, model "Rockchip rk3326 evb lpddr3 v12 board for linux"):
- NV3051D seq ca87b989, 30 MHz 80.05 Hz;
- reset GPIO3_B7, LCD power through a `power-supply` regulator on GPIO0_B5;
- joystick `play_joystick`, rocker0 (mux enable) 12, so Type 1.

arkos4clone's edited DTB moves the power pin into `enable-gpios`. The pin is the same.

**Three pin sets cover every 640×480 R36S-family entry:**

| Pin set | Reset | Power / enable | Boards |
|---|---|---|---|
| A | GPIO3_B7 | GPIO0_B5 | Type 1, K36, R36 Pro |
| B | GPIO3_D3 | GPIO3_A3 | Type 2, Type 1 Panel 5 |
| C | GPIO3_C0 | — | genuine R36S, R33S, RG351 |

A panel tried with the wrong pin set stays in reset: black screen, no harm (unverified, but the pins are plain GPIO
outputs).

### 3.3 What ROCKNIX (and stolen's overlay release) already carries (decoded: init sequences compared, `11`/`29` ignored)

| Overlay / DT description | Matches arkos4clone |
|---|---|
| ROCKNIX `rk3326-gameconsole-eeclone.dts` default (`&internal_display`, 720×720 ST7703) | r36max type1/2/3, r36ultra |
| ROCKNIX in-tree `gameconsole,r36s-panel` (ST7703 patch, 640×480 @ 50 MHz) | most likely origin panel0 (not compared byte for byte) |
| stolen `mipi-panel.dtbo.r36s-panel1` | origin panel1 |
| stolen `…r36s-panel2` | origin panel2 |
| stolen `…r36s-panel3` | origin panel3 |
| stolen `…r36s-panel4` (has hand-tested 50/60/75 Hz modes) | origin panel4 type1, r36h |
| stolen `…r36s_clone` (2025-04) | clone type1/type2 panel1, k36, r36pro |
| stolen `…rg351v-v2` | origin panel0, r33s, xu10, rg351v panel2 |
| stolen `…rg351mp-v2` | rg351mp, rg351v panel1 |
| ROCKNIX's generated overlay for Stuart | clone type1 panel2 / type1 panel5 / type2 panel2 |
| stolen `x35s`, `cb408` | none (other devices, the RK3566 DSI address) |

**Not covered by any ROCKNIX-side description today:** clone type1 panel3, panel4 and panel6; origin panel4 type2;
r36splus; r36max2. For these the stock DTB → overlay route is the only one. Our panel set could carry overlays
generated from the arkos4clone DTBs (licence, §5).

The stolen overlays are panel-only (one `target-path` fragment, no pins). They rely on the base DTB's pins:
ROCKNIX r36s for the origin panels, eeclone for the clones.

### 3.4 handhelds.wiki R36S clones page, and the V20 boards
- **handhelds.wiki returned HTTP 403** to the fetch tool. I did not get round that, so its content is **unconfirmed**.
  The search-engine extract says:
  - clones are grouped into roughly three types;
  - the EmuELEC clone list names **G80C-MB / G80CA-MB** boards with "Panel 8 / Panel 9";
  - some clones have 512 MB RAM;
  - **"on ArkOS4Clone, on certain V20 clones, the speaker may overvolt, permanently damaging it."** This is a reason
    to be careful with amplifier enable pins. Never enable an amplifier GPIO the stock DTB does not name.
- The "R36S-V20 2025-05-18: two versions with different panel, battery and button mappings" entry: **not confirmed**.
- A real V20 2025-05-18 report (github.com/AeolusUX/ArkOS-R3XS issue #293): the original card died. Every arkos4clone
  clone type gave a splash then a black screen, and the DTB identify tool found no match. This is the case only a
  panel finder plus a panel ID read could help.

---

## 4. Identifying the panel over DSI

### 4.1 Do these controllers answer ID reads?
- `MIPI_DCS_GET_DISPLAY_ID` (0x04) and `READ_DDB_START` (0xA1) are standard DCS (`include/video/mipi_display.h`).
  0xDA/0xDB/0xDC (read ID1/2/3) are near-universal manufacturer commands.
  - Sitronix's ST77903 sheet lists RDDID 04h and RDID1–3 DAh–DCh (source: the Espressif-hosted PDF).
  - **An ST7703 or NV3051D datasheet confirming the same was not found.** It is very likely but **unverified**, and so
    are the ID values.
- **Mainline panels that read IDs:**
  - `panel-novatek-nt35510.c` (`nt35510_read_id`: DCS reads of DA/DB/DC);
  - `panel-dsi-cm.c` (DA/DB/DC);
  - nt35560, s6e63m0-dsi, s6e8aa0 and sharp-lq101r1sx01 also use DCS/generic reads.
  - **Neither `panel-sitronix-st7703.c` nor `panel-newvision-nv3051d.c` reads anything** (read).
- **The host can read:** `dw-mipi-dsi.c` `dw_mipi_dsi_host_transfer` → `dw_mipi_dsi_read` when `rx_len` is set. The
  RK3326 uses the PX30 glue (`dw-mipi-dsi-rockchip.c`, `px30_chip_data`).
  - That bus turnaround works through the PX30's Innosilicon D-PHY is **unverified**. Nothing in ROCKNIX's RK3326
    trees exercises it.
- **ROCKNIX's generic driver already has the hook:** `I seq=da read=1` → `mipi_dsi_generic_read(…, 1 param, 1 byte)`,
  logged to dmesg.
  - Caveat: it sends a **generic** read (packet 0x14), not a **DCS** read (0x06). Many controllers answer both, and
    the ST7703 driver itself issues several commands as generic writes. But a silent answer would not prove the
    panel cannot answer.

### 4.2 Is "read ID, then choose" feasible at boot?
- **Video timing does not matter.** ID reads are LP-mode command transfers. The descriptions set `LPM` (0x800 in
  0xe03), so commands go in escape mode, independent of the HS lane rate the mode sets. Reads in `prepare` happen
  where the init writes already work: after reset, before video.
- **The pins matter.** With the wrong reset/power pin set, the panel never leaves reset and nothing answers. That is
  itself a useful result: no answer = wrong pin set, try the next (§3.2: three sets).
- **What a read can tell us:**
  - The controller family: ST7703 vs NV3051D vs unknown. This picks between about 6 and about 7 candidates.
  - Probably **not the glass/init variant.** Type 1 Panel 2 and Type 1 Panel 3 are both NV3051D with different
    gamma/power tables. Unless the module makers programmed distinct ID1–3 OTP values (unknown), only the eye can
    tell those apart.
  - So the read **narrows**, it does not **select**. Its value is largest for the black-screen case: it puts the
    likely panels first in the finder's order.
- **No kernel change is needed for the first experiment.** On Stuart's unit, prepend these to the `I` lines of a test
  overlay: `I seq=04 read=3`, `I seq=da read=1`, `I seq=db read=1`, `I seq=dc read=1`, and a `0xA1 read=8` attempt.
  Then `dmesg | grep read\[`.
  - If generic reads stay silent, a DCS-read variant needs our own kernel build of the driver (a GPL change to a GPL
    file, published with our sources).
  - Ask every tester for these bytes alongside their stock DTB; the ID table grows with the panel set.

---

## 5. Licensing

| Item | Licence | Can we ship it? |
|---|---|---|
| ROCKNIX RK3326 DTS (`rk3326-*.dts`, `rk3326-gameconsole-r3xs.dtsi`, eeclone) | **`(GPL-2.0+ OR MIT)`** SPDX on every file except `rk3326-device-group-r36.dtsi`, `retrogame_joypad_s2_f1.dtsi` and `rk3326-panel-elida-kd35t133.dtsi`, which have no header and so fall under ROCKNIX's GPL-2.0 (LICENSE.md: "Original software and scripts developed by the ROCKNIX team are licensed under … GPL Version 2") | **Yes**, as compiled DTBs with the DTS source named in our SOURCES list (ROCKNIX commit). We already do this for the kernel. Choosing MIT for the dual-licensed files is also open. |
| `panel-generic-dsi.c` (built into the kernel) | GPL-2.0 | Yes, as part of the kernel binary with its source (we already do). |
| `rocknix_dtbo.py`, `overlay_server.py` (stolen/overlay_server) | **MIT** | We could legally ship it. Stuart's rule still says **write our own** extractor: read it, do not copy it. Credit it as the reference. |
| `importpanel.py` | GPL-2.0 (ROCKNIX repo, no header) | Read for inspiration only, as planned. |
| arkos4clone DTBs | repo MIT. The DTBs are vendor BSP DTBs edited by the community; Rockchip's BSP DTS for these boards are normally `(GPL-2.0+ OR MIT)` (unverified for each file). | **Ship the facts, not the files.** Generate our own `panel_description` overlays from them, credit arkos4clone, and keep the source DTB name and commit in a manifest. A DTB itself is data, but it came through several hands, so extracting the panel facts is the cleaner record. |
| stolen `panel_overlays` release | no licence stated | Do not ship the files. Regenerate the same descriptions from the matching DTBs (§3.3), or ask stolen. |
| Users' stock DTBs → overlays | The user's own unit's data | Fine. Generate on their computer or their device, with our code. |
| ROCKNIX branding | CC BY-NC-SA 4.0 | Never use ROCKNIX's logos or name in our UI (we don't). |

The panel init sequences and timings are functional hardware parameters. They are the same bytes in every distro that
supports these panels. This is not legal advice; it records the licences as found.

---

## 6. What this means for §9 step 2 (device side) and the Card Maker

**Device side:**
1. **Base DTB by board ID.** Keep eeclone for 490–540 (Stuart's 508). Add ROCKNIX's `b_boot` table if we want
   rgb10x / xu-mini-m / gkd-pixel2, and treat the original R36S (`rk3326-gameconsole-r36s`, pin set C) as a separate
   choice: it has no ADC range in ROCKNIX's B script. Within EE clones, the ADC tells us nothing more.
2. **Split the overlay into two.** Stuart's audio broke with a panel-only overlay because the EE clone needs the
   *board* fragments (adc-keys, joypad, hp-det, amplifier, panel pins) as well. So make:
   - `board-<id>.dtbo`: pins + audio + joypad, from the stock DTB;
   - `panel-<id>.dtbo`: `panel_description` only.
   
   Then `FDTOVERLAYS /overlays/board.dtbo /overlays/panel.dtbo /overlays/vibe-r36s.dtbo`. The panel finder swaps only
   the panel file. Without a stock DTB, it also cycles the three pin sets.
3. **Panel finder candidates for the 640×480 R36S family:** about 12 init variants (§3.2) × the plausible pin sets.
   Order: the stock DTB's panel → the read-ID family → the most common (ST7703 94162d76, NV3051D ca87b989) → the rest.
   - **Alternative to overlay swapping:** `panel_generic_dsi.descfile=` on the `APPEND` line, with the descriptions in
     `vibe-initrd:/lib/firmware/`. One overlay, the panel chosen by a command-line word. Test early that the firmware
     loads at probe.
4. **Regeneration rule:** like ROCKNIX, but deterministic. Regenerate only when `stock/<the one DTB>` changes (store
   its hash next to the overlay), never by "file missing". Never let a multi-DTB `stock/` pick alphabetically.
5. **Do not copy ROCKNIX's "reinstall U-Boot to UART5" step.** Our image already ships the right loader. It only
   matters for a serial console.
6. **Amplifier safety:** enable an amplifier GPIO only when the stock DTB names one (the V20 speaker over-voltage
   report).

**Card Maker:**
- The extraction is the panel and board half of `rocknix_dtbo.py`, rewritten as our own library (§2.2 lists exactly
  what to read). Output the two overlays on the computer, so the first boot needs no generate-and-reboot.
- Identify the stock DTB by its **init-sequence hash** against our table (§3.2), not by `compatible` or file name.
  Show the community name ("Clone Type 1 Panel 2") as a label only.
- Keep the whole boot partition in `VIBEBOOT/stock/` as planned. Name the booted DTB explicitly (from `boot.ini`),
  because ROCKNIX-style tools would pick the alphabetically first.
- A wizard or Pi Imager-style tool can't run `rocknix_dtbo.py` (Python + pyfdt). Ours should be native: libfdt
  (BSD-2/GPL dual) or our own small FDT reader/writer.

## 7. What I could not confirm
- That `rocknix_dtbo.py` rebuilds Stuart's `mipi-panel.rocknix.dtbo` byte for byte. Running external code was blocked
  here; the structure matches.
- ST7703 / NV3051D answering 0x04 / 0xDA–0xDC, the ID values, and whether a *generic* read is accepted. Whether the
  PX30 D-PHY bus turnaround works. Test on Stuart's unit first (§4.2).
- The handhelds.wiki R36S clones page content (HTTP 403), including the "V20 2025-05-18 two versions" entry.
- The controllers behind origin panel1 (`E0 AB BA`), clone type1 panel4 (`B9 F1 12 87`) and r36max2 (`EE 01`).
- How ROCKNIX tells original-R36S owners to pick `extlinux.conf.r36s`. What the original R36S's ADC reads.
- Whether `descfile=` firmware loads from the initramfs at the built-in driver's probe time.
