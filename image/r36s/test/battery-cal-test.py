#!/usr/bin/env python3
# battery-cal-test.py — vibe-hwd's battery calibration, the FULL JUDGE (2026-11-16: started on the charger, full from
# the charger's physics — a gauge 100 % with 0.96 A going in is not full) and the live figures, against a fake sysfs, a fake clock, no load processes and no power-off.
#   python3 image/r36s/test/battery-cal-test.py        (on the Mac; no device)
import asyncio, importlib.machinery, importlib.util, json, math, os, sys, tempfile
T = tempfile.mkdtemp()
os.environ["VIBE_SYSFS"] = T
os.environ["VIBE_STATE_DIR"] = T + "/state"
B, C = T + "/sys/class/power_supply/battery/", T + "/sys/class/power_supply/rk817-charger/"
POL = T + "/sys/devices/system/cpu/cpufreq/policy0/"
for d in (B, C, POL, T + "/sys/class/backlight/backlight", T + "/sys/class/thermal/thermal_zone0"): os.makedirs(d)
def put(online, status, v=4.10, a=-0.80, q=2000000, port=4.84, full=2808000, temp=50.0):
    for n, x in (("status", status), ("voltage_avg", int(v * 1e6)), ("current_avg", int(a * 1e6)), ("charge_now", q),
                 ("charge_full", full), ("capacity", 100), ("constant_charge_current_max", 1000000)):
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

# ── the unit's use: only a genuine discharge (2026-11-16) ──
u2 = m.UseSmoother()
u2.feed_battery(0, 4.15, +0.96)
ok(u2.w is None, "★ use: a CHARGE current (+0.96 A) is never taken for the unit's use")
u2.feed_battery(1, 4.0, -0.5)
ok(abs(u2.w - 2.0) < 1e-9 and u2.age(61) == 60, "use: a discharge is (2.0 W), and its age is kept (60 s)")

# ── what the rk817 is set to (the registers, the device tree, the fallbacks) ──
ok(m.cv_from_chrg_out(0xA0) == 4.20 and m.cv_from_chrg_out(0xC0) == 4.30 and m.cv_from_chrg_out(0xD1) == 4.35,
   "CV from CHRG_OUT: 0xA0 = 4.20 V (this image), 0xD1 = 4.35 V (a HV cell)")
ok([m.term_from_chrg_term(r) for r in (0, 1, 2, 3, 0x0B)] == [0.15, 0.2, 0.3, 0.4, 0.4] and m.term_from_chrg_term(0x04) is None,
   "termination from CHRG_TERM: 150 / 200 / 300 / 400 mA; digital mode = unknown")
ok(m.term_from_dt(300000) == 0.3 and m.term_from_dt(320000) == 0.3 and m.term_from_dt(100000) == 0.2 and m.term_from_dt(None) is None,
   "termination from the device tree, as the driver programs it (320 mA → 300; out of range → 200)")
ok(m.vlim_from_chrg_in(0xDD) == 4.5 and m.vlim_from_chrg_in(0xDB) == 4.5 and m.vlim_from_chrg_in(0x5D) is None,
   "input floor from CHRG_IN: 0xDD / 0xDB = 4.5 V; disabled = none")
REGS = {}
cfg = m.read_charger_cfg(lambda r: REGS.get(r))
ok(cfg["cvV"] == 4.20 and cfg["cvSrc"] == "assumed" and cfg["termA"] == 0.15 and cfg["termSrc"] == "assumed" and cfg["vlimV"] is None,
   "★ nothing readable: CV 4.20 V and 150 mA (the strictest step), both marked assumed (%s)" % cfg)
REGS.update({0xE4: 0xD0, 0xE6: 0x01, 0xE5: 0xDD})
cfg = m.read_charger_cfg(lambda r: REGS.get(r))
ok(cfg["cvV"] == 4.35 and cfg["termA"] == 0.2 and cfg["vlimV"] == 4.5 and cfg["cvSrc"] == "register",
   "★ read from the PMIC: CV 4.35 V, 200 mA, 4.5 V floor — never assumed when readable (%s)" % cfg)
open(B + "constant_charge_voltage_max", "w").write("4200000")
ok(m.read_charger_cfg(lambda r: REGS.get(r))["cvV"] == 4.20, "sysfs constant_charge_voltage_max wins (the driver's own read)")
os.remove(B + "constant_charge_voltage_max")

# ── the full judge ──
import random
CFG = {"cvV": 4.20, "ccA": 1.0, "termA": 0.20, "vlimV": 4.5}
j = m.FullJudge()
for t in range(0, 1800):   # ★★ Stuart's unit, 2026-11-16: gauge 100 %, 4.15 V, 0.96 A in, port 4.63 V — for 30 min
    j.feed(t, True, "Charging", 4.15 + t * 0.00001, 0.96, 4.63, CFG)
w = j.wire(1800, CFG)
ok(j.state == "cc" and w["nearVlim"] and not w["limited"],
   "★★ 4.15 V at 0.96 A for 30 min (a gauge at 100 %%): still CC, not full; port 4.63 V near the 4.5 V floor (%s)" % w)
j = m.FullJudge(); random.seed(7); full_at = hold_seen = None
for t in range(0, 7200):   # the CV taper: 0.6 A falling with a 20-min time constant, ±60 mA and ±10 mV noise, V 4.17–4.19
    a = 0.6 * math.exp(-t / 1200.0) + random.uniform(-0.06, 0.06)
    v = 4.17 + 0.02 * (1 - math.exp(-t / 900.0)) + random.uniform(-0.01, 0.01)
    s = j.feed(t, True, "Charging", v, a, 4.95, CFG)
    if s == "hold" and hold_seen is None: hold_seen = t
    if s == "full" and full_at is None: full_at = t
# smoothed current reaches 0.20 A at ~1320 s (+ the 30 s smoothing lag); full 300 s later
ok(full_at is not None and 1600 <= full_at <= 1800 and j.by == "taper",
   "★ taper with noise: full by the taper rule ~5 min after 0.20 A (at %s s, hold from %s s)" % (full_at, hold_seen))
ok(j.feed(7300, True, "Charging", 4.19, 0.9, 4.95, CFG) == "full", "full is latched while the cord stays in")
ok(j.feed(7301, False, "Discharging", 4.10, -0.8, 0, CFG) == "off" and j.feed(7302, True, "Charging", 4.19, 0.1, 4.95, CFG) != "full",
   "unplug clears it; plugged back, it has to be earned again")
j = m.FullJudge(); t = 0
for t in range(0, 200): j.feed(t, True, "Charging", 4.19, 0.15, 4.95, CFG)
ok(j.state == "hold" and 95 <= j.wire(t, CFG)["holdLeftS"] <= 135, "hold: under 0.20 A near CV — counting down (%s s left)" % j.wire(t, CFG)["holdLeftS"])
for t in range(200, 300): j.feed(t, True, "Charging", 4.19, 0.45, 4.95, CFG)
ok(j.state == "cv", "the current back up over 0.20 A (smoothed): the hold starts again — %s" % j.state)
j = m.FullJudge()
for t in range(0, 900): j.feed(t, True, "Charging", 4.10, 0.05, 4.95, CFG)
ok(j.state == "cc", "★ a low current far under CV (4.10 V) is NOT full — the charger is not in CV")
j = m.FullJudge()
for t in range(0, 900): j.feed(t, True, "Charging", 4.19, 0.10, 4.53, CFG)
ok(j.state == "cv" and j.limited, "★ a low current with the port at its 4.5 V floor is NOT full — a starved charger")
j = m.FullJudge()
for t in range(0, 900): j.feed(t, True, "Charging", 4.19, 0.10, 4.95, CFG, 0x34)   # CC/CV state 3 + CHRG_IN_CLAMP (bit 2)
ok(j.state != "full" and j.limited, "the PMIC's input clamp: never full")
j = m.FullJudge()
ok(j.feed(0, True, "Full", 4.19, 0.0, 4.95, CFG) == "full" and j.by == "charger", "★ the charger's own CHARGE_FINISH: full at once")
ok(m.FullJudge().feed(0, True, "Charging", 4.19, 0.0, 4.95, CFG, 0x40) == "full", "CHRG_STS state 4 (CHARGE_FINISH) read straight from the PMIC")
j = m.FullJudge()
for t in range(0, 900): j.feed(t, True, "Not charging", 4.12, -0.10, 4.95, CFG)
ok(j.state == "paused", "on the cord but the battery giving: paused, never full")
j = m.FullJudge(); seen_paused = False
for t in range(0, 400):   # tapered, with the status flapping to Not charging for 5 s in every 40 (the LED flicker)
    s = j.feed(t, True, "Not charging" if t % 40 < 5 else "Charging", 4.19, -0.05 if t % 40 < 5 else 0.12, 4.95, CFG)
    seen_paused = seen_paused or s == "paused"
ok(seen_paused and j.state == "full", "★ brief Charging ↔ Not charging flaps keep the hold: full after it (%s)" % j.state)
j = m.FullJudge()
for t in range(0, 200): j.feed(t, True, "Charging", 4.19, 0.12, 4.95, CFG)
for t in range(200, 290): j.feed(t, True, "Not charging", 4.12, -0.10, 4.95, CFG)
for t in range(290, 400): j.feed(t, True, "Charging", 4.19, 0.12, 4.95, CFG)
ok(j.state == "hold", "a pause over a minute starts the hold again (%s)" % j.state)
j = m.FullJudge()
for t in range(0, 900): j.feed(t, True, "Charging", None if t % 3 else 4.19, None if t % 3 else 0.12, 4.95, CFG)
ok(j.state == "full", "missing readings change nothing (one in three present, still full after the hold)")

# ── the calibration, through Hwd on the fake sysfs ──
EMIT, DOWN = [], []
PM = {0xE4: 0xA0, 0xE6: 0x01, 0xE5: 0xDD}
def make():
    h = m.Hwd()
    h.emit = EMIT.append
    async def shutdown(reason, how): DOWN.append((reason, how))
    h.shutdown = shutdown
    h.stress._spawn = lambda: FakeProc()
    h.pmic = lambda r: PM.get(r)
    return h
class FakeProc:
    n = 0
    def __init__(self): FakeProc.n += 1; self.pid = 999999; self.killed = False
    def kill(self): self.killed = True
    def wait(self, timeout=None): return 0
_kill = m.os.kill
m.os.kill = lambda pid, sig: None if pid == 999999 else _kill(pid, sig)
loop = asyncio.new_event_loop(); asyncio.set_event_loop(loop)
def tick(h, t):
    async def go(): h.power_tick(t); await asyncio.sleep(0); await asyncio.sleep(0)   # (as power_loop runs it)
    loop.run_until_complete(go())
def taper(h, t0, secs, q=2600000):
    """CV taper to under 0.20 A, held: the judge's full (~8 min)."""
    for t in range(t0, t0 + secs):
        put(True, "Charging", v=4.19, a=0.12, q=q); tick(h, t)

h = make()
put(False, "Discharging", v=4.0, a=-0.9, q=2000000); tick(h, 0)
try: h.cal_start("normal"); ok(False, "refused off the charger")
except ValueError as ex: ok(str(ex) == "plug in the charger first — it starts from a full charge", "★ START off the charger: \"%s\"" % ex)
# ★★ Stuart's case: the gauge at 100 %, 0.96 A still going in — START is accepted and WAITS
put(True, "Charging", v=4.15, a=0.96, q=2500000); tick(h, 1)
h.cal_start("normal")
ok(h.cal.phase == "charging" and EMIT[-1] == {"event": "battery_cal", "phase": "charging", "mode": "normal", "start": True},
   "★★ START on the charger at a gauge 100 %% with 0.96 A going in: accepted, waiting for a genuine full (%s)" % h.cal.phase)
pw = h.state()["power"]
ok(pw["charge"]["state"] == "cc" and pw["charge"]["cvV"] == 4.20 and pw["charge"]["termA"] == 0.2 and pw["charge"]["nearVlim"] is False,
   "the wire: charge state cc, CV 4.20 V and 200 mA read from the PMIC (%s)" % pw["charge"])
ok(pw["useW"] == 3.6 and pw["useAgeS"] == 1, "the unit's use is the discharge before the cord (0.9 A × 4.0 V), 1 s old")
for t in range(2, 600):
    put(True, "Charging", v=4.15, a=0.96, q=2500000 + t * 260); tick(h, t)
ok(h.cal.phase == "charging", "10 min at 0.96 A: still waiting")
# unplugged before full: nothing starts, it says so
put(False, "Discharging", v=4.05, a=-0.9, q=2650000); tick(h, 600)
ok(h.cal.phase == "charging" and not h.cal.plugged and EMIT[-1]["phase"] == "notfull" and not h.stress.on,
   "★ unplugged before full: not started — 'notfull' told, still waiting (%s)" % EMIT[-1])
ok(h.state()["calib"]["plugged"] is False, "the wire says so (plugged false)")
for t in range(601, 700):
    put(False, "Discharging", v=4.05, a=-0.9, q=2650000 - t * 100); tick(h, t)
ok(h.cal.phase == "charging" and h.cal.wire(h.q_now)["countedMah"] is None, "nothing counted while unplugged before full")
put(True, "Charging", v=4.16, a=0.9, q=2580000); tick(h, 700)
ok(h.cal.plugged and EMIT[-1]["phase"] == "charging", "plugged back in: carries on waiting")
taper(h, 701, 400)
ok(h.cal.phase == "armed" and EMIT[-1]["phase"] == "armed" and h.judge.by == "taper",
   "★ the taper held 5 min: genuinely full → armed, the UI told (%s)" % EMIT[-1])
put(True, "Charging", v=4.19, a=0.11, q=2612000); tick(h, 1101)
ok(h.cal.q0 == 2612000, "armed on the cord: the counter at Full follows to the unplug")
put(False, "Discharging", v=4.15, a=-0.80, q=2611000); tick(h, 1102)
ok(h.cal.phase == "running" and EMIT[-1]["phase"] == "running", "★ unplugged after full: the count begins")
saved = json.load(open(m.CAL_JSON))
ok(saved["phase"] == "running" and saved["q0"] == 2612000, "★ the run is on the state partition (survives a power-off)")
# a power-off mid-way: a new Hwd resumes it (as normal)
h2 = make()
ok(h2.cal.phase == "running" and h2.cal.q0 == 2612000 and h2.cal.mode == "normal", "★ next boot: carries on, in normal mode")
ok(h2.power == {} and h2.cal.wire(None)["countedMah"] is None, "(nothing counted before the first look)")
put(False, "Discharging", v=3.80, a=-0.80, q=1200000); tick(h2, 10)
ok(h2.cal.wire(h2.q_now)["countedMah"] == 1412, "counted so far: 1412 mAh (%s)" % h2.cal.wire(h2.q_now)["countedMah"])
put(False, "Discharging", v=3.45, a=-1.20, q=100000); tick(h2, 20)
put(False, "Discharging", v=3.60, a=-0.80, q=99000); tick(h2, 22)
ok(h2.cal.phase == "running" and not DOWN, "a load dip under 3.50 V is not empty")
for t in range(30, 40):
    put(False, "Discharging", v=3.48, a=-0.80, q=-8000); tick(h2, t)
ok(h2.cal.phase == "idle" and h2.cal.result and h2.cal.result["mah"] == 2620 and h2.cal.result["design"] == 2808,
   "★ 3.48 V held 8 s: measured 2620 mAh, the gauge assumes 2808 (%s)" % h2.cal.result)
ok(DOWN == [("battery-cal", "poweroff")], "★ then a CLEAN power-off (%s)" % DOWN)
ok(any(e.get("phase") == "done" and e.get("mah") == 2620 for e in EMIT), "the UI told: done, 2620 mAh")
put(False, "Discharging", v=3.90, q=-8000 + 1310000)
ok(h2.read_battery()["raw"] == 50, "★ calibrated: half the measured capacity left reads 50 %% (%s)" % h2.read_battery()["raw"])
put(True, "Full", v=4.20, q=-8000 + 2620000)
ok(h2.read_battery()["raw"] == 100, "★ calibrated: a true full reads 100 %, not 93 %")
ok(m.Hwd().cal.result["mah"] == 2620, "the result survives a restart")

# ── already full when START is pressed; CANCEL while waiting ──
DOWN.clear(); EMIT.clear()
h5 = make()
put(True, "Full", v=4.20, a=0.0, q=2612000); tick(h5, 0)
h5.cal_start("normal")
ok(h5.cal.phase == "armed" and EMIT[-1]["phase"] == "armed", "START with the charger already Full: straight to 'unplug to start'")
h5.cal_cancel()
ok(h5.cal.phase == "idle" and EMIT[-1]["phase"] == "cancelled", "CANCEL before the unplug: nothing measured, idle")
put(True, "Charging", v=4.1, a=0.9, q=2400000); tick(h5, 1); h5.cal_start("stress")
h5.cal_cancel()
ok(h5.cal.phase == "idle" and not h5.stress.on, "CANCEL while waiting on the charger")

# ── charging before empty cancels; a stress run ──
DOWN.clear(); EMIT.clear()
h3 = make()
put(True, "Charging", v=4.10, a=0.9, q=2500000); tick(h3, 0)
h3.cal_start("stress")
for t in range(1, 30):
    put(True, "Charging", v=4.12, a=0.9, q=2500000); tick(h3, t)
ok(h3.cal.phase == "charging" and not h3.stress.on and FakeProc.n == 0, "★ STRESS waiting on the charger: NO load (it would only slow the charge)")
taper(h3, 30, 400, q=2612000)
ok(h3.cal.phase == "armed" and not h3.stress.on, "full: armed, still no load")
put(False, "Discharging", v=4.10, a=-1.50, q=2600000); tick(h3, 500)
ok(h3.stress.on and FakeProc.n == 4, "★ stress: four busy loops on unplug")
ok(open(POL + "scaling_governor").read() == "performance" and open(POL + "scaling_max_freq").read() == "1296000",
   "★ stress: performance at 1296 MHz — never the 1416 / 1512 overclock steps")
ok(open(T + "/sys/class/backlight/backlight/brightness").read() == "255", "stress: the backlight at full")
ok(("cset", "name=Master Playback Volume", "0,0") in MIX, "stress: the speaker muted")
put(False, "Discharging", v=4.00, a=-1.50, q=2500000, temp=76.0); tick(h3, 502)
ok(h3.stress.running == 2, "★ 76 °C: half the load paused (%d)" % h3.stress.running)
put(False, "Discharging", v=4.00, a=-1.50, q=2500000, temp=81.0); tick(h3, 503)
ok(h3.stress.running == 0, "81 °C: all paused")
put(False, "Discharging", v=4.00, a=-1.50, q=2500000, temp=65.0); tick(h3, 504)
ok(h3.stress.running == 4, "cooled: all running again")
put(True, "Charging", v=4.10, a=0.40, q=2400000); tick(h3, 505)
ok(h3.cal.phase == "idle" and h3.cal.note == "calibration cancelled — it was charged before empty", "★ charged before empty: cancelled, and said so")
ok(not h3.stress.on and open(POL + "scaling_governor").read() == "schedutil", "the load stops and the governor is put back")
ok(any(e.get("phase") == "cancelled" for e in EMIT) and not DOWN, "the UI told; no power-off")
ok(h3.cal.result and h3.cal.result["mah"] == 2620, "a cancelled run keeps the last good result")
# a waiting STRESS run keeps its mode over a restart; a running one never resumes as stress
h3.cal_start("stress"); taper(h3, 506, 400, q=2612000)
ok(make().cal.mode == "stress" and make().cal.phase == "armed", "a stress run waiting on the charger keeps STRESS over a restart")
put(False, "Discharging", v=4.1, a=-1.5, q=2600000); tick(h3, 1000)
h3.stress.stop()
h4 = make()
ok(h4.cal.phase == "running" and h4.cal.mode == "normal" and not h4.stress.on, "★ a stress run ended by POWER resumes as an ordinary radio")
ok("calib" in h4.state() and "power" in h4.state(), "{\"get\":\"state\"} carries power + calib")
print("   %d passed, %d failed" % (P, F)); sys.exit(1 if F else 0)
