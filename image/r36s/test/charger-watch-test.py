#!/usr/bin/env python3
# charger-watch-test.py — vibe-hwd's ChargerWatch (Stuart 2026-11-15: a plug-in took up to 58 s to be noticed, the SDR
# dropped off USB at plug-in, the charge LED flickers on the cord), against a fake sysfs and a fake PMIC, on a fake clock.
#   python3 image/r36s/test/charger-watch-test.py        (on the Mac; no device)
import importlib.machinery, importlib.util, os, sys, tempfile
T = tempfile.mkdtemp()
os.environ["VIBE_SYSFS"] = T
for d in ("battery", "rk817-charger"): os.makedirs(T + "/sys/class/power_supply/" + d)
def put(online, status="Discharging", port="4800000", uV="4050000", uA="-800000"):
    b = T + "/sys/class/power_supply/battery/"
    for n, v in (("status", status), ("voltage_avg", uV), ("current_avg", uA)): open(b + n, "w").write(str(v))
    c = T + "/sys/class/power_supply/rk817-charger/"
    open(c + "online", "w").write("1" if online else "0"); open(c + "voltage_avg", "w").write(str(port))
HWD = os.path.join(os.path.dirname(__file__), "../rootfs/usr/lib/vibesdr-r36s/vibe-hwd")
loader = importlib.machinery.SourceFileLoader("vibehwd", HWD)
spec = importlib.util.spec_from_loader("vibehwd", loader); m = importlib.util.module_from_spec(spec); loader.exec_module(m)
LOG = []
m.log = lambda *a: LOG.append(" ".join(str(x) for x in a))
REGS = {m.CHRG_IN: 0xDB, m.CHRG_OUT: 0xA0}
WRITES = []
FAIL = [False]
def rd_(dev, reg):
    if FAIL[0]: raise OSError("i2c timeout")
    return REGS[reg]
def wr_(dev, reg, val):
    if FAIL[0]: raise OSError("i2c timeout")
    WRITES.append((reg, val)); REGS[reg] = val
m.pmic_read, m.pmic_write = rd_, wr_
P = F = 0
def ok(c, what):
    global P, F
    P, F = (P + 1, F) if c else (P, F + 1); print("   %s %s" % ("ok  " if c else "FAIL", what))
w = m.ChargerWatch("/dev/i2c-0")
t = [1000.0]
def run(secs):
    n = int(round(secs / w.POLL_S))
    for _ in range(n):
        w.tick(t[0]); t[0] += w.POLL_S
lines = lambda k: [l for l in LOG if k in l]

put(False); run(2)
ok(not WRITES and not lines("plug"), "on battery: no writes, no trace")
# ── plug in: the limit goes on AT the edge ──
put(True, "Charging", port="4670000", uA="+300000")
w.tick(t[0]); edge = t[0]; t[0] += w.POLL_S
ok(WRITES == [(m.CHRG_IN, 0xDD)], "★ plug-in: CHRG_IN written to 0xDD on the very tick the cord was seen (%s)" % WRITES)
ok(any("input limit 2 A, floor 4.5 V at plug-in" in l and "0xdb -> 0xdd" in l for l in LOG), "the write is logged with old -> new")
ok(lines("plug-in seen"), "the edge is logged")
class FakeTrace:
    reader = object()
    def __init__(self): self.edges, self.online = [], None
    def set_online(self, o): self.online = o
    def edge(self, k, t): self.edges.append(k)
    def snapshot(self, why, t): self.edges.append("snapshot:" + ("before" if "before the input limit" in why else "?"))
run(6)
ok(not [l for l in LOG if l.startswith("charger: plug-in +")], "★ h80: no more 5 s of 8-s-old sysfs lines after an edge (the fast trace replaces them)")
ok(len(WRITES) == 1, "0xDD held: no rewrite while it stands (%d writes)" % len(WRITES))
# ── the driver rewrites the limit: the 1 s check puts it back ──
REGS[m.CHRG_IN] = 0xDB; run(1.2)
ok(len(WRITES) == 2 and REGS[m.CHRG_IN] == 0xDD, "the once-a-second check puts 0xDD back")
# ── the LED symptom: status flips with the cord in ──
put(True, "Not charging", port="4480000"); run(0.2)
fl = lines("with the cord in")
ok(len(fl) == 1 and "Charging -> Not charging" in fl[0] and "port 4.480V" in fl[0], "★ a status flip on the cord is logged with the port voltage: %s" % (fl[:1],))
for i in range(60):
    put(True, "Charging" if i % 2 else "Not charging"); run(0.1)
fl = lines("with the cord in")
ok(len(fl) <= w.FLIP_MAX + 1, "flips are capped per minute (%d lines)" % len(fl))
# ── plug out: traced too, no write ──
nw = len(WRITES); put(False, "Discharging", port="0"); run(6)
ok(len(WRITES) == nw and lines("plug-out seen"), "★ plug-out seen, nothing written")
# ── a PMIC error at the edge: 5 s back-off, not 30 ──
FAIL[0] = True; put(True, "Charging"); run(0.3)
FAIL[0] = False; REGS[m.CHRG_IN] = 0xDB; run(6)
ok(REGS[m.CHRG_IN] == 0xDD, "after a bus error at plug-in the limit still goes on within ~5 s")
ok(len(lines("PMIC")) >= 1, "the bus error is logged (rate-limited)")
# ── h80: the INPUT FLOOR test setting — the watch holds whatever byte the setting names ──
TGT = [0xBD]; FT = FakeTrace()
w2 = m.ChargerWatch("/dev/i2c-0", lambda: TGT[0], FT)
WRITES.clear(); REGS[m.CHRG_IN] = 0xDB; put(False); w2.tick(t[0]); t[0] += 0.1
put(True, "Charging"); w2.tick(t[0]); t[0] += 0.1
ok(WRITES == [(m.CHRG_IN, 0xBD)], "★ 4.3 V setting: plug-in writes 0xBD (%s)" % WRITES)
ok(any("floor 4.3 V (TEST) at plug-in" in l for l in LOG), "the write says the floor and TEST")
ok(FT.edges == ["plug-in"] and FT.online is True, "the edge and the cord's state go to the fast trace")
TGT[0] = 0x8D; w2.next_check = 0; w2.tick(t[0]); t[0] += 0.1
ok(REGS[m.CHRG_IN] == 0x8D, "a changed setting is applied on the next tick (4.0 V = 0x8D)")
TGT[0] = 0xDD; w2.next_check = 0; w2.tick(t[0])
ok(REGS[m.CHRG_IN] == 0xDD, "back to 4.5 V = 0xDD")
ok(m.VLIM_CHOICES == {4500: 0xDD, 4300: 0xBD, 4000: 0x8D}, "the encoding: 0x80 | (mV-4000)/100 << 4 | 0x08 | 5")
ok(all(m.vlim_from_chrg_in(v) == mv / 1000 for mv, v in m.VLIM_CHOICES.items()), "FullJudge's decoder reads each one back")
# ── h80: the cord already in at start-up (plugged in during the loading screen): snapshot BEFORE the write, then a burst ──
FT3 = FakeTrace(); REGS[m.CHRG_IN] = 0xDB; WRITES.clear(); put(True, "Charging")
w3 = m.ChargerWatch("/dev/i2c-0", lambda: 0xDD, FT3); w3.tick(t[0])
ok(FT3.edges == ["snapshot:before", "start-up"] and WRITES == [(m.CHRG_IN, 0xDD)],
   "★ cord in at start-up: the PMIC snapshot first, then the limit, then a start-up burst (%s)" % FT3.edges)
print("   %d passed, %d failed" % (P, F)); sys.exit(1 if F else 0)
