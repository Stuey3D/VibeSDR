#!/usr/bin/env python3
# battery-smooth-test.py — vibe-hwd's battery meter (Stuart 2026-11-14: "erratic when charging"), against a fake sysfs.
#   python3 image/r36s/test/battery-smooth-test.py        (on the Mac; no device)
import importlib.machinery, importlib.util, os, sys, tempfile
T = tempfile.mkdtemp()
os.environ["VIBE_SYSFS"] = T
for d in ("battery", "rk817-charger"): os.makedirs(T + "/sys/class/power_supply/" + d)
def put(cap, status, online, uV="4100000", uA="500000"):
    b = T + "/sys/class/power_supply/battery/"
    for n, v in (("capacity", cap), ("status", status), ("voltage_now", uV), ("current_now", uA)): open(b + n, "w").write(str(v))
    open(T + "/sys/class/power_supply/rk817-charger/online", "w").write("1" if online else "0")
HWD = os.path.join(os.path.dirname(__file__), "../rootfs/usr/lib/vibesdr-r36s/vibe-hwd")
loader = importlib.machinery.SourceFileLoader("vibehwd", HWD)
spec = importlib.util.spec_from_loader("vibehwd", loader); m = importlib.util.module_from_spec(spec); loader.exec_module(m)
m.log = lambda *a: None
h = m.Hwd.__new__(m.Hwd)
h.battery = {"pct": None, "raw": None, "charging": False, "online": False, "status": "Unknown"}; h.bat_against = 0
P = F = 0
def ok(c, what):
    global P, F
    P, F = (P + 1, F) if c else (P, F + 1); print("   %s %s" % ("ok  " if c else "FAIL", what))
def step(cap, status, online):
    put(cap, status, online); h.battery = h.smooth_battery(h.read_battery()); return h.battery
b = step(50, "Discharging", False); ok(b["pct"] == 50 and not b["charging"], "first reading shown as read (50 %)")
b = step(56, "Charging", True);    ok(b["pct"] == 50 and b["charging"], "★ plugged in: the gauge's jump to 56 is HELD at the plug change (50)")
b = step(56, "Charging", True);    ok(b["pct"] == 52, "then followed at 2 % a reading (52)")
b = step(49, "Not charging", True); ok(b["pct"] == 52 and b["charging"], "★ a sag on the charger does not move it down, and the bolt stays (status flicker ignored)")
b = step(54, "Charging", True);    ok(b["pct"] == 54, "climbs again with the gauge (54)")
b = step(48, "Discharging", False); ok(b["pct"] == 54 and not b["charging"], "★ unplugged: the gauge's drop to 48 is held at the plug change")
b = step(48, "Discharging", False); ok(b["pct"] == 52, "then followed down at 2 % a reading")
b = step(53, "Discharging", False); ok(b["pct"] == 52, "★ off the charger it never goes UP on a blip")
seq = [step(30, "Discharging", False)["pct"] for _ in range(14)]
ok(seq[-1] == 30, "a real fall is followed to the end (%s)" % seq)
b = step(2, "Discharging", False);  ok(b["raw"] == 2 and b["pct"] > 2, "★ the shutdown floor reads the RAW gauge (raw 2, meter %d)" % b["pct"])
for _ in range(4): step(31, "Charging", True)
seq = [step(29, "Charging", True)["pct"] for _ in range(4)]
ok(seq[0] == seq[1] and seq[-1] < seq[0], "★ a charger too weak for the load: the fall is shown once it has held 3 readings (%s)" % seq)
print("   %d passed, %d failed" % (P, F)); sys.exit(1 if F else 0)
