# R36S — the drop-triggered charger trace, the INPUT FLOOR test switch, STRESS driving the SDR (h80), and the fast USB-drop capture (h81)

Branch `v12-h80` (off v12-experimental), 2026-11-19. Image `12.0.0-beta86+handheld80-20261119`.

## The problem (Stuart's h79 log, debuglog-f0d2614a)
With the charger in and the battery at/near full (4.17–4.19 V, still taking 0.33–0.40 A) the RTL-SDR V4 drops off USB
("usb 1-1: USB disconnect") 0.6–6 s after it starts, re-enumerates, restarts, drops again (kernel 21.6, 25.6, 238.1,
242.1 s). On battery it never drops. CHRG_IN was 0xDD (2 A, 4.5 V floor), the port's voltage_avg 4.84–4.89 V. The old
plug trace printed sysfs figures, which the rk817 driver refreshes every 8 s (`queue_delayed_work … 8000`): 50 identical
lines. Also odd: after one plug-in the port read 0 V, then 1.84 V for ~95 s, with the console on battery (−0.75 A).
Since then (Stuart): a different cable gives a LOWER port voltage but an input current that matches what his power bank
says it delivers — the cable is in play.

Later (Stuart, 2026-11-19 evening): the long Anker cable is the best (4.76 V at plug-in, 4.91 V at full). A session
with three cables had no drops, so it is not the cable. Both sessions used the same sequence: this unit will NOT power
on with the cable in (open item, not investigated), so the cable goes in during the loading screen. The difference:
the failing session started at 100 % (4.185 V, still taking ~0.40 A, the last top-up); the good one started at 97 %
(4.17 V, 0.63 A) and charged on through Full at ~650 s with no drops. So the repro starts at 100 %. Also in the
h79 log: the 1 s check found CHRG_IN = 0xC8 (4.4 V floor, 450 mA) at 43.5 s, just before a plug-out at 43.6 s. It
was put back to 0xDD; probably the PMIC resetting as the cord came out, but the fast trace will show it if not.

Hypotheses: (1) the dongle's start-up surge pulls the input under the 4.5 V floor; the charger throttles and near full
the PMIC does not supplement from the battery fast enough. (2) With the charger online, the OTG port's VBUS comes from
the charger input, not the battery boost (~4.8 V less drops — marginal for the V4).

## What h80 adds
1. **FastTrace** (vibe-hwd). PMIC registers read directly, each read one i2c transfer on the trace's own fd and thread,
   every 20 ms, while the charger is in AND a USB device is plugged. A 2 s ring. A USB device vanishing
   (`/sys/bus/usb/devices/*-*` listed per sample) logs the ring plus 1 s after (`chgtrace#N DROP`). Every plug-in / plug-out
   logs the ring plus 3 s after (`PLUG-IN` / `PLUG-OUT`), with or without a radio. Identical samples are collapsed; at most
   10 bursts per 10 min. Each burst ends with port / battery min/mean/max and the input-clamp count. Also logged: every
   `CHRG_STS` change while sampling (capped at 20 a minute), a 30 s summary on the cord (min/mean/max for comparing
   cables, plus the sampler's own CPU), and a one-time "ADC check" line comparing the registers with sysfs.
   Registers, with the mainline driver's scaling (drivers/power/supply/rk817_charger.c rk817_read_props;
   include/linux/mfd/rk808.h):
   - CHRG_OUT 0xE4, CHRG_IN 0xE5 (one block), CHRG_STS 0xEB (state 6:4, BAT_EXS 7, BAT_OVP 3, CLAMP 2, USB_EXS 1,
     USB_EFF 0 — the BSP charger's reg_fields)
   - BAT_VOL 0x78/79: µV = k·raw + 1000·b. BAT_CUR 0x7A/7B (signed): µA = raw·172/res_div. USB_VOL 0x7E/7F:
     mV = (k·raw/1000 + b)·60/46. Here k, b come from VCALIB0 0x93/94 and VCALIB1 0x95/96, and res_div from the DT's
     `rockchip,resistor-sense-micro-ohms` (10 mΩ on this board → 1; if unknown, the current is left out).
   - **Not certain:** how often the PMIC refreshes the ADC registers. Neither driver documents it, so runs of identical
     samples show the ADC's own cadence. The "registers" are the same ones sysfs `*_avg` is built from; whether the
     PMIC averages them internally is not documented. SYS_VOL 0x80/81 is left out because neither driver scales it.
2. **INPUT FLOOR (TEST)** in R36S HARDWARE: 4.5 V (normal) / 4.3 / 4.0. One fixed CHRG_IN byte per setting, written in
   one go, never read-modify-write: bit 7 VLIM_EN, bits 6:4 = (mV − 4000)/100 (BSP `INPUT_VOL_4000MV + …`), bit 3 ILIM_EN,
   bits 2:0 = 5 (2 A) → 0xDD / 0xBD / 0x8D. The setting is persisted (hwd.json `vlimMv`) and re-asserted at start-up, at
   plug-in and in the 1 s check, as 0xDD always was. Every change is logged.
3. **STRESS drives the SDR** (handheld-ui battery_cal.h StressSdr). While RUNNING, the radio is set to the top rate in
   hwinfo `rates` (under lockedRate), keeps decoding and streaming, is muted (vibe-hwd), and shows no waterfall. Its own
   rate comes back on done / cancelled / shutdown, and the per-radio memory keeps its own rate throughout. The screen
   shows "N of 4 busy loops — 76 °C" and "SDR 2.4 MS/s · speaker muted · no waterfall" (an RTL's top rate is 2.4 MS/s). If the rate is not the UI's to
   change, it says why: DAB, HF+, a server lock, or a mode that keeps its rate.

## Tests
- `python3 image/r36s/test/chgtrace-test.py [bench]`: 29/29. The scaling is worked by hand from the driver; it also
  covers the drop burst (GONE/BACK marks, the 2 s before, collapse, the min/mean/max end line), plug-out sampling past the
  cord, plug-in with no radio, no reads on battery or on the cord without a radio, the burst cap, and the 30 s summary.
- `charger-watch-test.py` 19/19 (the INPUT FLOOR bytes, a changed setting applied on the next tick). `battery-cal-test.py`
  77/77. `battery-smooth-test.py` 11/11.
- handheld-ui offscreen tests all pass (battery_screen_test 72/72 with the new stress line + INPUT FLOOR row),
  battery_math_test 52/52.
- CPU: 9.1 µs per sample of the real path (4 SMBus ioctls on i2c-stub + the USB listdir + Python) in colima on the M5.
  Estimate for the A35 at 1008 MHz: ~0.1–0.15 ms → ~0.5–0.8 % of one core while sampling. The real figure is in
  every 30 s summary ("sampler x.x % of a core").

## h81 (2026-11-20, branch `v12-h81`): the FAST USB-drop capture — `12.0.0-beta86+handheld81-20261120`

**Why.** The h80 card caught three drops of USB 1-1 (0.5 s each, at 179.9 / 182.9 / 186.9 s) with the cord in and the
battery in the CV tail (~0.24 A; termination is 0.20 A): "dwc2 ff300000.usb: Not connected", usb_submit_urb −19, "USB
disconnect", re-enumerated ~0.3 s later. The 20 ms trace saw NOTHING change (port 4.844 V, bat 4.170 V +0.235 A, cccv,
CHRG_IN 0xdd, no clamp); no PMIC write of ours near them; no drops at 0.4–0.7 A; never on battery. So the event is
shorter than 20 ms and comes nearly full. **Diagnosis only — charging is not changed** (Stuart has parked changing the
termination / CV).

**What h81 adds.**
1. **`vibe-usbcap`** (`image/r36s/native/vibe-usbcap.c`, C, built in the image's trixie arm64 container, installed in
   `/usr/lib/vibesdr-r36s/`). Every ~1 ms (`clock_nanosleep` absolute ticks on CLOCK_MONOTONIC; a late tick is not caught
   up) it reads:
   - one I2C block read **0xE4..0xF0**: CHRG_OUT 0xE4, CHRG_IN 0xE5, **CHRG_STS 0xEB**, **SYS_STS 0xF0** (bit 6
     `RK817_PLUG_IN_STS`). Sources: mainline `include/linux/mfd/rk808.h` (addresses, `RK817_BAT_EXS` bit 7,
     `RK817_CHG_STS` bits 6:4, `RK817_PLUG_IN_STS`) and `drivers/power/supply/rk817_charger.c` (reads CHRG_STS for the
     charge state + battery present, SYS_STS for "plugged_in"). CHRG_STS bits 3:0 (BAT_OVP / CLAMP / USB_EXS / USB_EFF)
     are the Rockchip BSP driver's, **not in mainline**.
   - one I2C block read **0x78..0x7F**: BAT_VOL, BAT_CUR (signed), USB_VOL — the same registers as the 20 ms trace.
   - **dwc2 HPRT0** (base + 0x440, one 32-bit load through `/dev/mem`): connect status, enable, overcurrent, port power,
     speed, the D+/D− line levels (mainline `drivers/usb/dwc2/hw.h`). Only when vibe-hwd found the dwc2 driver bound to
     the controller the radio hangs off (then its clocks are on: `dwc2_set_rk_params` sets power_down NONE and
     no_clock_gating) AND GSNPSID (base + 0x40) reads 0x4F54xxxx. If `/dev/mem` is refused, the capture runs without it.
   - the presence of each watched USB device (`access()` on `/sys/bus/usb/devices/<name>`).
   - Once at start and per dump: RK817_POWER_EN_REG(3) **0xB4** (mainline rk808-regulator.c: bit 1 BOOST, bit 2
     OTG_SWITCH) and RK817_BOOST_OTG_CFG **0xDE**; VCALIB 0x93..0x96 once at start.

   A ring of 8192 samples. **The trigger:** HPRT0's connect bit falling (the controller's own "Not connected") or a
   watched device leaving sysfs, whichever comes first; a second event inside the window joins it and extends it (never
   past 3 s). The dump = **1.0 s before and 1.0 s after**, raw registers, a line whenever anything changes and a run line
   for identical samples (the D+/D− levels are not a "change": they follow the high-speed traffic). At most 10 dumps in
   10 min. It never writes the PMIC.
2. **`FastCapture` in vibe-hwd** starts vibe-usbcap when the cord is in, a USB device is plugged and the charge current
   has been **below 0.45 A for 2 s** (hysteresis 0.02 A; a negative current on the cord counts — the console taking more
   than the charger gives), and stops it 2 s after any of that ends (so a drop at the plug-out still gets its "after";
   a stop mid-window prints the dump marked "cut short"). ★ **0.45, not the brief's ~0.30 A:** the h79 drops came at
   0.33–0.40 A (the driver's 8 s average). The charge current is the 20 ms trace's register figure while fresh, else
   the driver's sysfs `current_avg`. A crash is logged and retried after 30 s, at most 5 starts in 10 min.
3. **The tail lines**: `chgtrace: charge current with the cord in BELOW 0.30 A — …` and `… back ABOVE 0.30 A after N s
   below — …` (held 2 s, back above at ≥ 0.32 A; at most 20 lines in 10 min), and `the cord came out while the charge
   current was below 0.30 A`. Each says which figures it used (registers or the driver's 8 s sysfs).

**How the bus is shared safely.** Every read in vibe-usbcap is ONE `I2C_SMBUS` ioctl (`I2C_SMBUS_I2C_BLOCK_DATA` /
`BYTE_DATA`), i.e. the register-pointer write and the read go to the adapter as one `i2c_transfer` with a repeated start,
under the adapter's bus lock — the same method as vibe-hwd's `_smbus` / `PmicFd`. So vibe-hwd's writes (CHRG_IN), its 20 ms
reads and the kernel's own rk808 traffic cannot land between our pointer write and our read (the handheld62 fault). vibe-usbcap
never writes: vibe-hwd stays the only writer of PMIC registers.

**Bus time.** i2c@ff180000 is 400 kHz (stock DTB `rf3536k3ka.dtb`; the eeclone DTB is assumed the same). The two block reads
are ~27 byte-times ≈ **0.6 ms of bus per sample** (estimate) — so the bus is ~60 % busy while the capture runs, each
transfer ≤ ~0.4 ms; a kernel PMIC access (DVFS, the gauge) waits at most one of ours. The real read time is in every STAT
and DUMP line ("PMIC reads x.xx ms a sample"); if it is longer than the period, the interval stretches and the lines say so.

### Reading it
```
chgtrace: charge current with the cord in BELOW 0.30 A — +0.288A at mono 151.2 (registers: cccv/usb/eff IN dd OUT a0 …)
chgtrace-fast: started at mono 140.105 — the cord in, the charge current below 0.45 A for 2 s, USB 1-1
chgtrace-fast: running — a sample every 1000 µs, …; dwc2 HPRT0 ok(GSNPSID 4f54330a); POWER_EN3 0x46 (BOOST on, OTG_SWITCH on), …
chgtrace-fast: last 30 s — 29850 samples, one every 1.005 ms (mean), longest gap 4.2 ms, …; PMIC reads 0.61 ms a sample; 3.0 % of a core
chgtrace-fast#1 DROP: the OTG port lost its device (dwc2 HPRT0 connect 1→0); USB 1-1 left sysfs at +0.0200 s at mono 179.9123 s
                — 1.000 s before, 1.020 s after; a sample every 1.001 ms (median; longest gap 3.1 ms), PMIC reads 0.61 ms …
chgtrace-fast#1 -1.0000 cccv/usb/eff IN dd OUT a0 bat 4.170V +0.235A port 4.844V · sys 40 plug · otg conn/en/pwr hs D+D- 10 · USB 1-1
chgtrace-fast#1   … same ×996 to -0.0040
chgtrace-fast#1 -0.0030 cccv/CLAMP/usb/eff … port 4.687V …          ← what we are looking for: anything moving just before +0
chgtrace-fast#1 +0.0000 … · otg pwr D+D- 00 · USB 1-1 ★ OTG PORT: NO DEVICE
chgtrace-fast#1 +0.0200 … · USB none ★ USB 1-1 GONE
chgtrace-fast#1 +0.3200 … ★ OTG PORT: DEVICE BACK ★ USB 1-1 BACK
chgtrace-fast#1 end — port …/…/… · bat … (min/mean/max) · input clamp 3 of 2021 samples · 2021 samples
```
- `mono` ≈ the kernel log's `[time]`: line the DROP up with "dwc2 … Not connected".
- **Where to look:** the lines just before `+0.0000`. A port volts dip, CLAMP, USB_EXS/USB_EFF dropping (`usb`/`eff`
  missing), `NO-PLUG` (SYS_STS bit 6), a charge-state change, `OVERCURRENT` or `pwr` going off in HPRT0 — any of those
  in the last few ms says power path. **Nothing moving at all at 1 ms** before the port loses the device points away
  from the PMIC (the dongle itself, the cable/connector, or the PHY), or to something faster than the ADC.
- Runs of identical ADC values show the ADC's own refresh cadence (undocumented); the status bits are likely live.
- `otg ?` = HPRT0 not read (see the `running` line for why). `cut short` = the capture stopped inside the window.

### Tests (h81)
- `python3 image/r36s/test/chgtrace-test.py`: **78/78** (the h80 31 + 47 new: the record parser on synthetic records
  incl. the exact scaling, marks, signed current, read errors, the line cap; Threshold debounce + hysteresis;
  FastCapture start / stop / grace / crash back-off / start cap / no-radio / negative current; dwc2_base_for; HPRT0 text).
  `charger-watch-test.py` 20/20, `battery-cal-test.py` 77/77, `battery-smooth-test.py` 11/11.
- `cc -O2 -Wall -Wextra image/r36s/test/usbcap-test.c && ./a.out`: **29/29** (includes the program with USBCAP_TEST:
  ring bound, trigger on HPRT0 / sysfs, the 1 s windows, run collapse, LNSTS flicker, a second drop, no HPRT0, SIGTERM
  mid-window, the 10-per-10-min cap, read errors). Also run by build-image.sh before the binary goes on the card.
- `image/r36s/test/usbcap-stub-test.sh`: **5/5** — the real binary against the kernel's i2c-stub in colima (the real
  ioctls, the clock loop, access(), a real sysfs drop), read by vibe-hwd's FastDumpReader. In the VM (an M5 core):
  **1.000 ms median interval, mean 0.999 ms, max gap 1.7 ms, 1.1 % of a core** — the stub answers in 4 µs, so the R36S's
  real figures (0.6 ms of 400 kHz bus per sample, the A35) come from its STAT lines.

### Only the device can tell
The real interval and CPU on the A35 with the real bus (estimate: ~1 ms kept, a few % of a core); whether `/dev/mem`
is allowed by the ROCKNIX kernel (if not: `HPRT0 off(/dev/mem: …)` and sysfs presence only); the dwc2 address and
GSNPSID; whether the RK817 ADC refreshes fast enough to show a sub-20 ms sag at all.

## RESUME HERE (h81)
- Stuart's h81 log: grep `chgtrace-fast#` and `charge current with the cord in`. First check the `running` line (HPRT0 ok?)
  and a STAT line (real interval, % of a core). Then each DROP: what moved in the last 20 ms before `+0.0000`.
- If nothing moves at 1 ms: the PMIC side is (nearly) ruled out — next is the dongle's own supply (a powered hub /
  Y-cable test) or the PHY. If CHRG_STS / SYS_STS / port volts move: power path, and the fix discussion with Stuart
  (charging changes are PARKED until he says).
- Decide afterwards whether vibe-usbcap stays in the image (diagnosis tool) or goes.

## RESUME HERE (h80)
- The repro (HOW TO TEST h80): start at 100 % (still topping up), power on without the cable, plug it in during the
  loading screen, tune a strong FM station. With the cord in at hwd start, "the cord was already in at start-up — the
  PMIC as found" is the state BEFORE our 0xDD write, followed by a START-UP burst.
- Read Stuart's next log: `chgtrace#N DROP` bursts. In the 2 s before a drop, look at the port volts (USB_VOL), CLAMP,
  and the battery current turning negative. Compare with the INPUT FLOOR setting and the cable he names. If the port
  never sags and the clamp never shows, hypothesis 1 is out; look at VBUS (hypothesis 2: boost vs pass-through, which
  RK817 register 0xB4/BOOST_EN and the vcc_host switch decide).
- Confirm the "ADC check" line: registers = sysfs within the 8 s staleness. If they disagree, the scaling is wrong; say so.
- If a floor fixes it, decide with Stuart whether the floor becomes the default or the row goes (it is a TEST row).
