#!/usr/bin/env python3
# battery-cal-test.py — vibe-hwd's battery calibration and live figures (Stuart 2026-11-15: the gauge assumes 2808 mAh,
# this cell reads ~93 % at a true full), against a fake sysfs, a fake clock, no load processes and no power-off.
#   python3 image/r36s/test/battery-cal-test.py        (on the Mac; no device)
import asyncio, importlib.machinery, importlib.util, json, os, sys, tempfile
T = tempfile.mkdtemp()
os.environ["VIBE_SYSFS"] = T
os.environ["VIBE_STATE_DIR"] = T + "/state"
B, C = T + "/sys/class/power_supply/battery/", T + "/sys/class/power_supply/rk817-charger/"
POL = T + "/sys/devices/system/cpu/cpufreq/policy0/"
for d in (B, C, POL, T + "/sys/class/backlight/backlight", T + "/sys/class/thermal/thermal_zone0"): os.makedirs(d)
def put(online, status, v=4.10, a=-0.80, q=2000000, port=4.84, full=2808000, temp=50.0):
    for n, x in (("status", status), ("voltage_avg", int(v * 1e6)), ("current_avg", int(a * 1e6)), ("charge_now", q),
                 ("charge_full", full), ("capacity", 50)):
        open(B + n, "w").write(str(x))
    open(C + "online", "w").write("1" if online else "0"); open(C + "voltage_avg", "w").write(str(int(port * 1e6)))
    open(T + "/sys/class/thermal/thermal_zone0/temp", "w").write(str(int(temp * 1000)))
for n, x in (("scaling_governor", "schedutil"), ("scaling_available_frequencies", "408000 600000 816000 1008000 1200000 1296000 1416000 1512000"),
             ("scaling_max_freq", "1008000")):
    open(POL + n, "w").write(x)
open(T + "/sys/class/backlight/backlight/max_brightness", "w").write("255")
HWD = os.path.join(os.path.dirname(__file__), "../rootfs/usr/lib/vibesdr-r36s/vibe-hwd")
loader = importlib.machinery.SourceFileLoader("vibehwd", HWD)
spec = importlib.util.spec_from_loader("vibehwd", loader); m = importlib.util.module_from_spec(spec); loader.exec_module(m)
LOG = []
m.log = lambda *a: LOG.append(" ".join(str(x) for x in a))
MIX = []
m.amixer = lambda *a: MIX.append(a) or True
P = F = 0
def ok(c, what):
    global P, F
    P, F = (P + 1, F) if c else (P, F + 1); print("   %s %s" % ("ok  " if c else "FAIL", what))

# ── the pure parts ──
r = {"mah": 2620, "qEmpty": 188000}
ok(m.cal_pct(188000 + 2620000, r) == 100 and m.cal_pct(188000, r) == 0 and m.cal_pct(188000 + 1310000, r) == 50,
   "cal_pct: the counter between this cell's own empty and full")
ok(m.cal_pct(100000, r) == 0 and m.cal_pct(5000000, r) == 100, "cal_pct clamps to 0 … 100")
ok(m.cal_pct(1000, None) is None and m.cal_pct(None, r) is None, "cal_pct: nothing to say without a calibration or a counter")
ok(m.cal_plausible(2620, 2808) and not m.cal_plausible(500, 2808) and not m.cal_plausible(5000, 2808), "plausible: 30 … 150 % of the gauge's figure")
e = m.EmptyDetector()
ok(not e.feed(0, 3.45) and not e.feed(5, 3.48) and not e.feed(6, 3.52), "★ empty: a dip under 3.50 V that recovers is not empty")
ok(not e.feed(7, 3.49) and not e.feed(14, 3.47) and e.feed(15, 3.46), "★ empty: 3.50 V or under held 8 s is")
ok(not m.EmptyDetector().feed(0, None), "a missing reading is not empty")
u = m.UseSmoother()
u.feed(0, 3.0); u.feed(1, 3.0)
ok(abs(u.w - 3.0) < 1e-9, "use: a steady 3 W reads 3 W")
for t in range(2, 62): u.feed(t, 6.0)
ok(4.7 < u.w < 5.1, "use: a step to 6 W is ~63 %% there after a minute (%.2f)" % u.w)
ok(u.feed(70, None) == u.w and u.feed(71, -1) == u.w, "use: no reading (or none on the charger) keeps the last figure")
lp = m.live_power(False, "Discharging", 4.10, -0.80, 0.0, 3.28, 2000000, 2808000)
ok(lp["batA"] == 0.8 and lp["batW"] == 3.28 and lp["dir"] == "discharge" and lp["portV"] is None and lp["chargeMah"] == 2000,
   "live: on battery — 4.10 V 0.80 A 3.28 W, no port figure (%s)" % lp)

# ── the calibration, through Hwd on the fake sysfs ──
EMIT, DOWN = [], []
def make():
    h = m.Hwd()
    h.emit = EMIT.append
    async def shutdown(reason, how): DOWN.append((reason, how))
    h.shutdown = shutdown
    h.stress._spawn = lambda: FakeProc()
    return h
class FakeProc:
    n = 0
    def __init__(self): FakeProc.n += 1; self.pid = 999999; self.killed = False
    def kill(self): self.killed = True
    def wait(self, timeout=None): return 0
m.os_kill = None
_kill = m.os.kill
m.os.kill = lambda pid, sig: None if pid == 999999 else _kill(pid, sig)
loop = asyncio.new_event_loop(); asyncio.set_event_loop(loop)
def tick(h, t):
    async def go(): h.power_tick(t); await asyncio.sleep(0); await asyncio.sleep(0)   # (as power_loop runs it)
    loop.run_until_complete(go())

h = make()
put(True, "Charging", v=4.15, a=0.30, q=2500000)
try: h.cal_start("normal"); ok(False, "refused while not Full")
except ValueError as ex: ok(str(ex) == "charge to Full first", "★ not Full: \"%s\"" % ex)
put(True, "Full", v=4.20, a=0.0, q=2611000)
h.cal_start("normal")
ok(h.cal.phase == "armed" and h.cal.q0 == 2611000, "on Full: armed, the counter at Full kept")
tick(h, 1)
put(True, "Full", v=4.20, a=0.0, q=2612000); tick(h, 2)
ok(h.cal.q0 == 2612000, "armed on the cord: the counter at Full follows to the unplug")
put(False, "Discharging", v=4.15, a=-0.80, q=2611000); tick(h, 3)
ok(h.cal.phase == "running" and any(e.get("phase") == "running" for e in EMIT), "unplugged: running, the UI told")
saved = json.load(open(m.CAL_JSON))
ok(saved["phase"] == "running" and saved["q0"] == 2612000, "★ the run is on the state partition (survives a power-off)")
# a power-off mid-way: a new Hwd resumes it (as normal)
h2 = make()
ok(h2.cal.phase == "running" and h2.cal.q0 == 2612000 and h2.cal.mode == "normal", "★ next boot: carries on, in normal mode")
ok(h2.power == {} and h2.cal.wire(None)["countedMah"] is None, "(nothing counted before the first look)")
put(False, "Discharging", v=3.80, a=-0.80, q=1200000); tick(h2, 10)
ok(h2.cal.wire(h2.q_now)["countedMah"] == 1412, "counted so far: 1412 mAh (%s)" % h2.cal.wire(h2.q_now)["countedMah"])
# a dip, then a sustained empty
put(False, "Discharging", v=3.45, a=-1.20, q=100000); tick(h2, 20)
put(False, "Discharging", v=3.60, a=-0.80, q=99000); tick(h2, 22)
ok(h2.cal.phase == "running" and not DOWN, "a load dip under 3.50 V is not empty")
for t in range(30, 40):
    put(False, "Discharging", v=3.48, a=-0.80, q=-8000); tick(h2, t)
ok(h2.cal.phase == "idle" and h2.cal.result and h2.cal.result["mah"] == 2620 and h2.cal.result["design"] == 2808,
   "★ 3.48 V held 8 s: measured 2620 mAh, the gauge assumes 2808 (%s)" % h2.cal.result)
ok(DOWN == [("battery-cal", "poweroff")], "★ then a CLEAN power-off (%s)" % DOWN)
ok(any(e.get("phase") == "done" and e.get("mah") == 2620 for e in EMIT), "the UI told: done, 2620 mAh")
# the % now on the measured capacity
put(False, "Discharging", v=3.90, q=-8000 + 1310000)
ok(h2.read_battery()["raw"] == 50, "★ calibrated: half the measured capacity left reads 50 %% (%s)" % h2.read_battery()["raw"])
put(True, "Full", v=4.20, q=-8000 + 2620000)
ok(h2.read_battery()["raw"] == 100, "★ calibrated: a true full reads 100 %, not 93 %")
ok(m.Hwd().cal.result["mah"] == 2620, "the result survives a restart")

# ── charging before empty cancels; a stress run ──
DOWN.clear(); EMIT.clear()
h3 = make()
put(True, "Full", v=4.20, a=0.0, q=2612000)
h3.cal_start("stress")
put(False, "Discharging", v=4.10, a=-1.50, q=2600000); tick(h3, 1)
ok(h3.stress.on and FakeProc.n == 4, "★ stress: four busy loops on unplug")
ok(open(POL + "scaling_governor").read() == "performance" and open(POL + "scaling_max_freq").read() == "1296000",
   "★ stress: performance at 1296 MHz — never the 1416 / 1512 overclock steps")
ok(open(T + "/sys/class/backlight/backlight/brightness").read() == "255", "stress: the backlight at full")
ok(("cset", "name=Master Playback Volume", "0,0") in MIX, "stress: the speaker muted")
put(False, "Discharging", v=4.00, a=-1.50, q=2500000, temp=76.0); tick(h3, 2)
ok(h3.stress.running == 2, "★ 76 °C: half the load paused (%d)" % h3.stress.running)
put(False, "Discharging", v=4.00, a=-1.50, q=2500000, temp=81.0); tick(h3, 3)
ok(h3.stress.running == 0, "81 °C: all paused")
put(False, "Discharging", v=4.00, a=-1.50, q=2500000, temp=65.0); tick(h3, 4)
ok(h3.stress.running == 4, "cooled: all running again")
put(True, "Charging", v=4.10, a=0.40, q=2400000); tick(h3, 5)
ok(h3.cal.phase == "idle" and h3.cal.note == "calibration cancelled — it was charged before empty", "★ charged before empty: cancelled, and said so")
ok(not h3.stress.on and open(POL + "scaling_governor").read() == "schedutil", "the load stops and the governor is put back")
ok(any(e.get("phase") == "cancelled" for e in EMIT) and not DOWN, "the UI told; no power-off")
ok(h3.cal.result and h3.cal.result["mah"] == 2620, "a cancelled run keeps the last good result")
# stress never resumes after a restart
put(True, "Full", v=4.20, q=2612000); h3.cal_start("stress"); put(False, "Discharging", v=4.1, a=-1.5, q=2600000); tick(h3, 10)
h3.stress.stop()
h4 = make()
ok(h4.cal.phase == "running" and h4.cal.mode == "normal" and not h4.stress.on, "★ a stress run ended by POWER resumes as an ordinary radio")
ok("calib" in h4.state() and "power" in h4.state(), "{\"get\":\"state\"} carries power + calib")
print("   %d passed, %d failed" % (P, F)); sys.exit(1 if F else 0)
