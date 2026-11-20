#!/usr/bin/env python3
# chgtrace-test.py — vibe-hwd's FastTrace (h80, 2026-11-19: the RTL-SDR drops off USB on the charger near full; the old
# trace's sysfs figures were 8 s old). A fake PMIC (register map) and a fake USB list on a fake clock — no device.
#   python3 image/r36s/test/chgtrace-test.py        (on the Mac)
#   python3 image/r36s/test/chgtrace-test.py bench  (also: the Python cost of one sample, fake bus)
import importlib.machinery, importlib.util, os, sys, tempfile, time
T = tempfile.mkdtemp()
os.environ["VIBE_SYSFS"] = T
for d in ("battery", "rk817-charger"): os.makedirs(T + "/sys/class/power_supply/" + d)
HWD = os.path.join(os.path.dirname(__file__), "../rootfs/usr/lib/vibesdr-r36s/vibe-hwd")
loader = importlib.machinery.SourceFileLoader("vibehwd", HWD)
spec = importlib.util.spec_from_loader("vibehwd", loader); m = importlib.util.module_from_spec(spec); loader.exec_module(m)
LOG = []
m.log = lambda *a: LOG.append(" ".join(str(x) for x in a))
P = F = 0
def ok(c, what):
    global P, F
    P, F = (P + 1, F) if c else (P, F + 1); print("   %s %s" % ("ok  " if c else "FAIL", what))

# ── the scaling, worked by hand from rk817_charger.c (vcalib0 2048, vcalib1 3584) ──
#    k = 1725000 / 1536 = 1123 (C truncation), b = 4025 − 1123 × 3584 / 1000 = 4025 − 4024 = 1
#    usb raw 3200 (the sag) → (3593 + 1) × 60 / 46 = 4687 mV
#    bat raw 3720 → 1123 × 3720 + 1000 × 1 = 4 178 560 µV · usb raw 3330 → (3739 + 1) × 60 / 46 = 4878 mV · cur 2000 → 344 mA
kb = m.adc_coeffs(2048, 3584)
ok(kb == (1123, 1), "k, b as rk817_bat_calib_vol (%s)" % (kb,))
ok(abs(m.adc_bat_v(3720, kb) - 4.17856) < 1e-9, "battery volts: k × raw + 1000 × b µV")
ok(abs(m.adc_usb_v(3330, kb) - 4.878) < 1e-9, "port volts: (k × raw / 1000 + b) × 60 / 46 mV")
ok(m.adc_usb_v(1, kb) == 0.0, "port raw ≤ 1 reads 0 V, as the driver")
ok(abs(m.adc_bat_a(2000, 1) - 0.344) < 1e-9 and abs(m.adc_bat_a(-2000, 2) + 0.172) < 1e-9, "current: raw × 172 / res_div µA, signed")
ok(m.adc_bat_a(2000, None) is None, "★ sense resistor unknown: the current is left out, never guessed")
ok(m.adc_coeffs(3584, 2048) is None, "calibration words out of order: no volts at all")
ok(m.sts_text(0x92 | 0x04) == "off/CLAMP/usb" or m.sts_text(0xB6).startswith("cccv"), "CHRG_STS decoded")
ok(m.sts_text(0xB7) == "cccv/CLAMP/usb/eff", "CHRG_STS 0xB7 = cccv + clamp + usb + eff (%s)" % m.sts_text(0xB7))
ok(m.chrg_in_for(4300) == 0xBD and m.chrg_in_for(4000) == 0x8D, "INPUT FLOOR bytes")

REG = {0x93: 0x08, 0x94: 0x00, 0x95: 0x0E, 0x96: 0x00, 0xE4: 0xA0, 0xE5: 0xDD, 0xEB: 0xB3}
def set16(r, v, signed=False):
    b = int(v).to_bytes(2, "big", signed=signed); REG[r], REG[r + 1] = b[0], b[1]
set16(0x78, 3720); set16(0x7A, 2000, True); set16(0x7E, 3330)
READS = [0]
class FakePmic:
    def byte(self, r): READS[0] += 1; return REG.get(r, 0)
    def block(self, r, n): READS[0] += 1; return bytes(REG.get(r + i, 0) for i in range(n))
DEVS = [frozenset({"1-1"})]
tr = m.FastTrace(FakePmic(), usb=lambda: DEVS[0], res_div=1)
t = [100.0]
def run(secs):
    for _ in range(int(round(secs / tr.SAMPLE_S))):
        tr.step(t[0]); t[0] += tr.SAMPLE_S

# ── on battery: nothing read ──
tr.set_online(False); run(1)
ok(READS[0] == 0 and not LOG, "★ on battery: the PMIC is not read at all")
# ── on the cord with a radio: sampling, no output until something happens ──
tr.set_online(True); run(3)
ok(READS[0] > 100, "on the cord with a radio: sampled every 20 ms (%d reads)" % READS[0])
ok(any("ADC check" in l and "bat 4.179V +0.344A port 4.878V" in l for l in LOG), "★ the ADC check line compares registers with sysfs")
ok(len(tr.ring) <= tr.ring.maxlen and tr.ring.maxlen == 101, "the ring is bounded at 2 s (%d)" % tr.ring.maxlen)
n0 = len(LOG)
# ── the drop: the port sags, the clamp comes on, then USB 1-1 vanishes and comes back ──
set16(0x7E, 3200); REG[0xEB] = 0xB7; run(0.1)
DEVS[0] = frozenset(); run(0.3)
DEVS[0] = frozenset({"1-1"}); set16(0x7E, 3330); REG[0xEB] = 0xB3; run(1.5)
out = LOG[n0:]
hd = [l for l in out if "chgtrace#1 DROP" in l]
ok(len(hd) == 1 and "USB 1-1 disappeared" in hd[0] and "a sample every 20 ms" in hd[0] and "input floor 4.5 V;" in hd[0], "★ a DROP burst header: %s" % hd[:1])
body = [l for l in out if l.startswith("chgtrace#1 ")]
ok(any("★ USB 1-1 GONE" in l for l in body) and any("★ USB 1-1 BACK after 0.3 s" in l for l in body), "the GONE and BACK marks are in the burst")
ok(any("cccv/CLAMP" in l and "port 4.687V" in l for l in body), "★ the sag and the input clamp before the drop are in it")
pre = [l for l in body if " -" in l.split("chgtrace#1 ", 1)[1][:2]]
ok(any(l.startswith("chgtrace#1 -1.9") or l.startswith("chgtrace#1 -2.0") for l in body), "it starts ~2 s before the drop")
ok(any("… same ×" in l for l in body), "identical samples are collapsed")
ok(len(body) < 30, "compact: %d lines for 3 s of 20 ms samples" % len(body))
end = [l for l in body if " end — " in l]
ok(len(end) == 1 and "port 4.687V/" in end[0] and "/4.878V" in end[0] and "(min/mean/max)" in end[0] and "input clamp" in end[0],
   "★ the burst's end line: port and battery min/mean/max (compare cables): %s" % end[:1])
ok(any("CHRG_STS 0xb3 -> 0xb7" in l for l in out), "a CHRG_STS change is a line of its own")
# ── a plug-out: the ring before it and 3 s after, though the cord is gone ──
n0 = len(LOG); run(2.5)
tr.edge("plug-out", t[0]); tr.set_online(False); r0 = READS[0]; run(3.5)
po = [l for l in LOG[n0:] if "PLUG-OUT" in l]
ok(len(po) == 1 and READS[0] > r0 + 100, "★ a plug-out burst, sampled on after the cord is out")
r1 = READS[0]; run(1)
ok(READS[0] == r1, "then on battery: reads stop")
# ── plug-in with no radio: still a burst ──
DEVS[0] = frozenset(); n0 = len(LOG)
tr.set_online(True); tr.edge("plug-in", t[0]); run(3.5)
ok(any("PLUG-IN" in l and "the cord went in" in l for l in LOG[n0:]), "a plug-in burst even with no radio plugged")
r2 = READS[0]; run(1)
ok(READS[0] == r2, "on the cord with no radio: no sampling")
# ── the burst cap ──
DEVS[0] = frozenset({"1-1"}); run(0.2)
for i in range(14):
    DEVS[0] = frozenset(); run(0.1); DEVS[0] = frozenset({"1-1"}); run(1.2)
ok(tr.burst_n <= tr.BURST_MAX and any("not traced" in l for l in LOG), "★ bursts capped at %d per 10 min (%d)" % (tr.BURST_MAX, tr.burst_n))
# ── the 30 s summary ──
n0 = len(LOG); run(31)
sm = [l for l in LOG[n0:] if "on the cord —" in l]
ok(len(sm) >= 1 and "sampler" in sm[0] and "% of a core" in sm[0] and "USB 1-1" in sm[0], "★ the 30 s summary with the sampler's CPU: %s" % sm[:1])
ok(tr.SAMPLE_S == 0.02, "20 ms samples")

# ── start-up with the cord in: the snapshot line, then a burst named for it ──
tr3 = m.FastTrace(FakePmic(), usb=lambda: frozenset({"1-1"}), res_div=1); n0 = len(LOG)
tr3.snapshot("the cord was already in at start-up — the PMIC as found, before the input limit", 13.4)
tr3.set_online(True); tr3.edge("start-up", 13.45); tt = 13.45
for _ in range(200): tr3.step(tt); tt += 0.02
ok(any("already in at start-up" in l and "IN dd OUT a0" in l and "USB 1-1" in l for l in LOG[n0:]), "★ the start-up snapshot: registers + USB")
ok(any("START-UP: the cord was in at start-up" in l for l in LOG[n0:]), "a START-UP burst")
# ══ h81: the fast USB-drop capture — vibe-usbcap's records → log lines (FastDumpReader), the gate + tail lines ══
FL = []
rdr = m.FastDumpReader(1, logf=lambda *a: FL.append(" ".join(str(x) for x in a)))
rdr.line("READY period_us=1000 pre_us=1000000 post_us=1000000 vcalib=0800,0e00 watch=1-1 pwr_en3=46 boost_cfg=09 hprt=ok(GSNPSID 4f54330a)\n")
ok(rdr.kb == (1123, 1) and any("running" in l and "HPRT0 ok(GSNPSID 4f54330a)" in l and "BOOST on, OTG_SWITCH on" in l for l in FL),
   "★ READY: the ADC calibration from VCALIB (k, b as the 20 ms trace), HPRT0 state, the boost/OTG enables")
# raw: BAT_VOL 3720 (0e88), BAT_CUR 1366 (0556 → 0.235 A at ÷1), USB_VOL 3330 (0d02) → 4.179 V / +0.235 A / 4.878 V
DUMP = """DUMP n=1 t0=179912345 why=otg-port-disconnect,usb-1-1-gone@+0.0200 pre_us=1000000 post_us=1020000 samples=2021 med_us=1001 max_gap_us=3100 read_us=410 cut=0 pwr_en3=46 boost_cfg=09
S -1000000 a0 dd b3 40 0e88 0556 0d02 00001405 1 0
R 996 -4000
S -3000 a0 dd b7 40 0e88 0556 0c80 00001405 1 0
R 2 -1000
S +0 a0 dd b3 40 0e88 0556 0d02 00001000 1 0
R 19 +19000
S +20000 a0 dd b3 40 0e88 0556 0d02 00001000 0 0
R 299 +319000
S +320000 a0 dd b3 40 0e88 0556 0d02 00001405 1 0
R 700 +1020000
END n=1 lines=11"""
n0 = len(FL)
for ln in DUMP.split("\n"): rdr.line(ln + "\n")
out = FL[n0:]
hd = [l for l in out if "chgtrace-fast#1 DROP" in l]
ok(len(hd) == 1 and "the OTG port lost its device (dwc2 HPRT0 connect 1→0); USB 1-1 left sysfs at +0.0200 s" in hd[0]
   and "at mono 179.9123 s" in hd[0], "★ the DROP header: both events, the drop's monotonic time")
ok(len(hd) == 1 and "a sample every 1.001 ms (median; longest gap 3.1 ms)" in hd[0] and "PMIC reads 0.41 ms" in hd[0]
   and "1.000 s before, 1.020 s after" in hd[0], "★ the REAL interval and the read time are in the header")
ok(len(hd) == 1 and "input floor 4.5 V;" in hd[0] and "POWER_EN3 0x46 (BOOST on, OTG_SWITCH on), BOOST_OTG_CFG 0x09" in hd[0], "the floor and the regulator enables")
body = [l for l in out if l.startswith("chgtrace-fast#1 ")]
ok(any(l.startswith("chgtrace-fast#1 -1.0000 cccv/usb/eff IN dd OUT a0 bat 4.179V +0.235A port 4.878V · sys 40 plug · otg conn/en/pwr hs D+D- 10 · USB 1-1") for l in body),
   "★ a fast line: the same scaling as the 20 ms trace, SYS_STS plug, HPRT0 decoded, USB present")
ok(any(l.startswith("chgtrace-fast#1 -0.0030 cccv/CLAMP/usb/eff") and "port 4.687V" in l for l in body), "the 3 ms sag with the clamp, 3 ms before")
ok(any(l.startswith("chgtrace-fast#1 +0.0000") and "★ OTG PORT: NO DEVICE" in l and "otg pwr" in l for l in body), "★ +0: the port without its device")
ok(any(l.startswith("chgtrace-fast#1 +0.0200") and "★ USB 1-1 GONE" in l and "USB none" in l for l in body), "+20 ms: sysfs follows")
ok(any(l.startswith("chgtrace-fast#1 +0.3200") and "★ OTG PORT: DEVICE BACK" in l and "★ USB 1-1 BACK" in l for l in body), "+320 ms: back")
ok(any("… same ×996 to -0.0040" in l for l in body), "runs collapse as in the 20 ms trace")
end = [l for l in body if " end — " in l]
ok(len(end) == 1 and "port 4.687V/" in end[0] and "/4.878V" in end[0] and "input clamp 3 of 2021 samples" in end[0] and "2021 samples" in end[0],
   "★ the end line: min/mean/max over every sample (runs counted): %s" % end[:1])
ok(m.fmt_sample((0, 0xB3, 0xDD, 0xA0, 3720, 1366, 3330), (1123, 1), 1) == tr._fmt((0, 0xB3, 0xDD, 0xA0, 3720, 1366, 3330)) if tr.kb == (1123, 1) else True,
   "the fast lines and the 20 ms lines are one formatter")
# a negative current, a read error, no HPRT0
n0 = len(FL)
for ln in ["DUMP n=2 t0=5000000 why=usb-1-1-gone pre_us=500000 post_us=99000 samples=600 med_us=1000 max_gap_us=1000 read_us=400 cut=1 pwr_en3=? boost_cfg=?",
           "S -500000 a0 dd b3 40 0e88 fe00 0d02 - 1 0", "S -1000 a0 dd b3 40 0000 0000 0000 - 1 1", "S +0 a0 dd b3 00 0e88 fe00 0d02 - 0 0", "END n=2 lines=3"]:
    rdr.line(ln)
out = FL[n0:]
ok(any("-0.088A" in l for l in out), "a negative BAT_CUR (0xfe00 = −512 → −0.088 A) is signed")
ok(any("✗ PMIC read failed" in l and "STS ?" in l for l in out), "a failed read shows as ? with a mark")
ok(any("+0.0000" in l and "NO-PLUG" in l and "otg ?" in l and "★ USB 1-1 GONE" in l for l in out), "no HPRT0: 'otg ?'; SYS_STS without the plug bit")
ok(any("cut short" in l and "POWER_EN3 0x? (?)" in l for l in out), "a dump cut by the stop says so")
n0 = len(FL)
rdr.line("STAT wall_us=30000000 samples=29850 mean_us=1005 max_gap_us=4200 over2x=3 read_us=412 read_max_us=1300 cpu_us=900000")
ok(any("29850 samples, one every 1.005 ms (mean), longest gap 4.2 ms, 3 over twice the period; PMIC reads 0.41 ms a sample (max 1.30); 3.0 % of a core" in l
       for l in FL[n0:]), "★ STAT: the real interval and the sampler's CPU, every 30 s")
rdr.line("SKIP t=7000000 why=usb-1-1-gone dumps=10 skipped=1"); rdr.line("garbage here"); rdr.line("S 1 2")
ok(any("not dumped" in l for l in FL) and any("unknown record" in l for l in FL), "SKIP + unknown records logged, never raised")
# a dump bigger than LINE_MAX is cut with a note
rdr2 = m.FastDumpReader(1, logf=lambda *a: FL.append(" ".join(str(x) for x in a))); rdr2.LINE_MAX = 5
rdr2.line("READY vcalib=0800,0e00 watch=1-1"); n0 = len(FL)
rdr2.line("DUMP n=3 t0=1 why=usb-1-1-gone samples=20")
for i in range(20): rdr2.line("S %d a0 dd b3 40 0e88 %04x 0d02 - 1 0" % (i, i))
rdr2.line("END n=3")
ok(sum(1 for l in FL[n0:] if l.startswith("chgtrace-fast#3 +")) == 5 and any("15 more lines not logged" in l for l in FL[n0:]), "★ a dump is capped (LINE_MAX) with a note")

# ── Threshold: debounce + hysteresis ──
th = m.Threshold(0.30, 2.0, 0.02)
flips = [th.update(t_, c) for t_, c in [(0, 0.29), (1.0, 0.29), (1.9, 0.29)]]
ok(not any(flips) and not th.below, "below for < 2 s: no flip")
ok(th.update(2.0, 0.28) and th.below, "below for 2 s: flips to below")
ok(not th.update(3, 0.31) and not th.update(6, 0.31) and th.below, "★ hysteresis: 0.31 A is not 'back above' (needs ≥ 0.32)")
ok(not th.update(7, 0.33) and not th.update(8, None) and th.update(9.0, 0.33) and not th.below, "≥ 0.32 for 2 s: above again (None changes nothing)")
ok(not th.update(10, 0.20) and not th.update(11, 0.40) and not th.update(12.5, 0.20) and not th.below, "a brief dip resets the hold")

# ── FastCapture: when vibe-usbcap runs, the tail lines, the stop grace, the restart cap ──
class FakeProc:
    def __init__(self, argv): self.argv, self.returncode, self.stdout, self.terms = argv, None, None, 0
    def poll(self): return self.returncode
    def terminate(self): self.terms += 1; self.returncode = -15
SP = []
def spawn(argv): p = FakeProc(argv); SP.append(p); return p
UDEV = [frozenset({"1-1"})]
LOG.clear()
fc = m.FastCapture("/dev/i2c-0", 1, usb=lambda: UDEV[0], spawn=spawn, dwc2=lambda d: 0xff300000, threads=False)
tt = 1000.0
def fc_run(secs, online, cur, step=0.1):
    global tt
    for _ in range(int(round(secs / step))):
        fc.tick(tt, online, cur, " (x)"); tt += step
fc_run(5, False, None)
ok(not SP and not LOG, "on battery: nothing")
fc_run(10, True, 0.60)
ok(not SP and not LOG, "★ on the cord, charging at 0.60 A: no sampler, no line")
fc_run(1.5, True, 0.40)
ok(not SP, "0.40 A for 1.5 s: not yet (2 s hold)")
fc_run(1.0, True, 0.40)
ok(len(SP) == 1 and SP[0].argv[:5] == [m.USBCAP, "--i2c", "/dev/i2c-0", "--watch", "1-1"] and SP[0].argv[5:] == ["--dwc2", "0xff300000"],
   "★ below 0.45 A for 2 s with a radio: vibe-usbcap starts, watching 1-1 with HPRT0 (%s)" % (SP[0].argv if SP else None))
ok(any("chgtrace-fast: started" in l for l in LOG) and not any("BELOW 0.30" in l for l in LOG), "a start line; no tail line at 0.40 A")
fc_run(3, True, 0.24)
ok(sum(1 for l in LOG if "BELOW 0.30 A — +0.240A" in l and "(x)" in l) == 1, "★ the tail line: below 0.30 A, once")
fc_run(30, True, 0.24)
ok(len(SP) == 1 and SP[0].terms == 0, "one sampler, still running")
fc_run(5, True, 0.50)   # 2 s hold + 2 s grace
ok(sum(1 for l in LOG if "back ABOVE 0.30 A after" in l) == 1, "the tail line back above")
ok(SP[0].terms == 1 and any("stopping" in l and "back above 0.45 A" in l for l in LOG), "★ above 0.45 A (+ the grace): the sampler is stopped")
fc_run(3, True, 0.24); fc_run(1, True, 0.24)
ok(len(SP) == 2, "low again: a new sampler")
fc_run(1.0, False, None)
ok(SP[1].terms == 0, "★ the cord out: it keeps running through the grace (a drop at the plug-out still gets its 'after')")
fc_run(2.0, False, None)
ok(SP[1].terms == 1 and any("cord came out while the charge current was below" in l for l in LOG), "then stopped; the tail line says the cord came out")
UDEV[0] = frozenset(); fc_run(5, True, 0.10)
ok(len(SP) == 2, "on the cord with no radio: no sampler (nothing to drop)")
UDEV[0] = frozenset({"1-1"}); fc_run(0.5, True, 0.10)
ok(len(SP) == 3, "a radio plugged in: it starts")
SP[-1].returncode = 1; fc_run(1, True, 0.10)
ok(len(SP) == 3 and any("ended (exit 1)" in l for l in LOG), "★ a crash: logged, and no instant restart")
fc_run(31, True, 0.10)
ok(len(SP) == 4, "restarted after 30 s")
for i in range(6):
    SP[-1].returncode = 0; fc_run(0.2, True, 0.10)
ok(len(SP) <= 4 + 2 and any("starts in 10 min" in l for l in LOG), "★ at most 5 starts in 10 min (%d)" % len(SP))
LOG.clear(); fc2 = m.FastCapture("/dev/i2c-0", 1, usb=lambda: frozenset({"1-1"}), spawn=spawn, dwc2=lambda d: None, threads=False)
n_sp = len(SP)
for i in range(30): fc2.tick(2000 + i * 0.1, True, None)
ok(len(SP) == n_sp, "no current reading at all: nothing starts")
for i in range(30): fc2.tick(2010 + i * 0.1, True, -0.30)
ok(len(SP) == n_sp + 1 and "--dwc2" not in SP[-1].argv and any("sysfs presence only" in l for l in LOG),
   "a NEGATIVE current on the cord counts as low; no dwc2 under it: sysfs only")

# ── dwc2_base_for: only a BOUND dwc2 the device hangs off ──
D = T + "/sys"
os.makedirs(D + "/devices/platform/ff300000.usb/usb1/1-1", exist_ok=True)
os.makedirs(D + "/bus/usb/devices", exist_ok=True); os.makedirs(D + "/bus/platform/drivers/dwc2", exist_ok=True)
if not os.path.lexists(D + "/bus/usb/devices/1-1"): os.symlink(D + "/devices/platform/ff300000.usb/usb1/1-1", D + "/bus/usb/devices/1-1")
ok(m.dwc2_base_for({"1-1"}) is None, "dwc2 driver not bound: no HPRT0")
if not os.path.lexists(D + "/bus/platform/drivers/dwc2/ff300000.usb"): os.symlink(D + "/devices/platform/ff300000.usb", D + "/bus/platform/drivers/dwc2/ff300000.usb")
ok(m.dwc2_base_for({"1-1"}) == 0xff300000, "★ bound + the device under it: 0xff300000")
ok(m.dwc2_base_for({"2-1"}) is None, "a device on another controller: none")
ok(m.hprt_text(0x00001405) == "otg conn/en/pwr hs D+D- 10" and m.hprt_text(0x00021015).startswith("otg conn/en/OVERCURRENT/pwr fs"), "HPRT0 decoded (hw.h bits)")

if sys.argv[1:] == ["bench"]:
    N = 20000; tr2 = m.FastTrace(FakePmic(), usb=lambda: frozenset({"1-1"}), res_div=1); tr2.set_online(True)
    m.log = lambda *a: None
    c0 = time.process_time()
    for i in range(N): tr2.step(1000.0 + i * 0.02)
    us = (time.process_time() - c0) / N * 1e6
    print("   bench: %.1f µs of Python per sample here (fake bus, a real usb listdir would add) → %.2f %% of a core at 50/s" % (us, us * 50 / 1e4))
print("   %d passed, %d failed" % (P, F)); sys.exit(1 if F else 0)
