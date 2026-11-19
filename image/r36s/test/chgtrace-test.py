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

if sys.argv[1:] == ["bench"]:
    N = 20000; tr2 = m.FastTrace(FakePmic(), usb=lambda: frozenset({"1-1"}), res_div=1); tr2.set_online(True)
    m.log = lambda *a: None
    c0 = time.process_time()
    for i in range(N): tr2.step(1000.0 + i * 0.02)
    us = (time.process_time() - c0) / N * 1e6
    print("   bench: %.1f µs of Python per sample here (fake bus, a real usb listdir would add) → %.2f %% of a core at 50/s" % (us, us * 50 / 1e4))
print("   %d passed, %d failed" % (P, F)); sys.exit(1 if F else 0)
