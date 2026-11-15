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
ok(any("input limit 2 A at plug-in" in l and "0xdb -> 0xdd" in l for l in LOG), "the write is logged with old -> new")
ok(lines("plug-in seen"), "the edge is logged")
run(6)
tr = [l for l in LOG if l.startswith("charger: plug-in +")]
ok(48 <= len(tr) <= 52, "★ a trace line every 100 ms for 5 s after plug-in (%d lines)" % len(tr))
ok("port 4.670V" in tr[0] and "bat 4.050V" in tr[0] and "+0.300A" in tr[0] and "Charging" in tr[0] and "IN 0xdd OUT 0xa0" in tr[0],
   "a trace line has port V, battery V / A / status, CHRG_IN, CHRG_OUT: %s" % tr[0])
ok(len(WRITES) == 1, "0xDD held: no rewrite while it stands (%d writes)" % len(WRITES))
n0 = len(LOG); run(3)
ok(not [l for l in LOG[n0:] if "+" in l and "plug-in" in l], "the trace stops after 5 s")
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
tr = [l for l in LOG if l.startswith("charger: plug-out +")]
ok(48 <= len(tr) <= 52 and len(WRITES) == nw, "★ plug-out traced for 5 s, nothing written (%d lines)" % len(tr))
# ── a PMIC error at the edge: 5 s back-off, not 30 ──
FAIL[0] = True; put(True, "Charging"); run(0.3)
FAIL[0] = False; REGS[m.CHRG_IN] = 0xDB; run(6)
ok(REGS[m.CHRG_IN] == 0xDD, "after a bus error at plug-in the limit still goes on within ~5 s")
ok(len(lines("PMIC")) >= 1, "the bus error is logged (rate-limited)")
print("   %d passed, %d failed" % (P, F)); sys.exit(1 if F else 0)
