# R36S — the drop-triggered charger trace, the INPUT FLOOR test switch, and STRESS driving the SDR (h80)

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

## RESUME HERE
- The repro (HOW TO TEST h80): start at 100 % (still topping up), power on without the cable, plug it in during the
  loading screen, tune a strong FM station. With the cord in at hwd start, "the cord was already in at start-up — the
  PMIC as found" is the state BEFORE our 0xDD write, followed by a START-UP burst.
- Read Stuart's next log: `chgtrace#N DROP` bursts. In the 2 s before a drop, look at the port volts (USB_VOL), CLAMP,
  and the battery current turning negative. Compare with the INPUT FLOOR setting and the cable he names. If the port
  never sags and the clamp never shows, hypothesis 1 is out; look at VBUS (hypothesis 2: boost vs pass-through, which
  RK817 register 0xB4/BOOST_EN and the vcc_host switch decide).
- Confirm the "ADC check" line: registers = sysfs within the 8 s staleness. If they disagree, the scaling is wrong; say so.
- If a floor fixes it, decide with Stuart whether the floor becomes the default or the row goes (it is a TEST row).
