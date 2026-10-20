#!/usr/bin/env python3
# ★★★ THE POCKET WI-FI STATE MACHINE, AGAINST A FAKE NETWORKMANAGER (2026-10-10).
#
# The real thing can only be proven on the Pi 3 A+ (docs/POCKET-VIBESERVER-PI3A.md has that
# checklist). What CAN be proven here is the LOGIC — the rules Stuart set and the ones a phone in a
# pocket depends on:
#   · networks 1/2/3 are STRICT ORDER: both in range ⇒ 1, whatever the signal;
#   · on network 2 when network 1 returns ⇒ move up; a network that refuses us rests 15 min;
#   · none in range ⇒ the SECURED hotspot, never the open one once setup has saved a secured one;
#   · a hotspot with a phone on it is never pulled from under the phone;
#   · a phone hotspot that is not advertising ("hidden") is still tried, by name;
#   · root reads the daemon's request only if it is a plain file from the daemon's user;
#   · no password ever reaches the log.
# ★ The fake models what matters: visibility, passwords, AP vs station (NM refuses a scan in AP mode),
#   a virtual clock. It does not model NM's own autoconnect — the service must not depend on it.
import importlib.machinery
import importlib.util
import io
import json
import os
import sys
import tempfile
import time
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
loader = importlib.machinery.SourceFileLoader("vp", os.path.join(HERE, "vibeserver-pocket"))
spec = importlib.util.spec_from_loader("vp", loader)
vp = importlib.util.module_from_spec(spec)
loader.exec_module(vp)

fails = 0


def ok(cond, what):
    global fails
    print("  %s %s" % ("\033[32mok\033[0m  " if cond else "\033[31mFAIL\033[0m", what), file=sys.__stdout__)
    if not cond:
        fails += 1


def esc(s):
    return s.replace("\\", "\\\\").replace(":", "\\:")


class FakeNM:
    def __init__(self):
        self.t = 0.0
        self.conns = {}            # name -> dict(type, ssid, psk, prio, hidden, mode)
        self.air = {}              # ssid -> (signal, real_psk)   what is in range
        self.active = ""
        self.stations = 0
        self.ap_force = False
        self.calls = []
        self.nft = []
        self.modified = []
        self.deleted = []
        self.link_sig, self.tx_rate = -60, 65.0     # the joined link as `iw … link` reports it
        self.assoc = ""                             # ★ the network the radio is REALLY on (set when a join succeeds)
        self.txpk, self.txrt = 1000, 10             # station dump counters (packets, retries)
        self.door = True                            # the server's front door accepts connections
        self.ap_up_seen = []                        # (redirect loaded, state mode, door open) at each hotspot up
        self.state_path = None
        self.wifi_dev = "wlan0"                     # None = no Wi-Fi chip (a Pi 2)
        self.eth = None                             # None = no Ethernet port; else NM's state, e.g. "connected"
        self.radio = True                           # nmcli radio wifi on/off
        self.last_stderr = ""                       # ★ as Shell: the last failed command's stderr line
        self.scan_fails = 0                         # this many station scans fail first ("not allowed" — NM's rate limit)
        # ★★★ AN IPHONE'S RULE (Stuart's road test, 2026-10-14): its Personal Hotspot is not on the air while the
        #     phone's own Wi-Fi is joined to another network. phone_ssid = the phone's hotspot; phone_on_ap = that phone
        #     is a station on the BOX's hotspot. While both hold, the phone's hotspot is missing from every scan.
        self.phone_ssid, self.phone_on_ap = None, False
        # ★ Wi-Fi power save (BRIEF-portable-wifi-realtime Phase 0): the radio's own state as `iw … get power_save` reports
        #   it, and whether a join switches it back on (brcmfmac after a reassociation / firmware recovery).
        self.ps = "off"
        self.ps_on_join = False
        self.ps_stuck = False                       # `iw … set power_save off` refused (a driver that will not)

    # Shell interface
    def port_open(self, port):
        return self.door

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s

    def run_input(self, args, data, timeout=20):
        self.nft.append(data)
        return 0

    def on_air(self):
        if self.phone_ssid and self.phone_on_ap and self.is_ap():
            return {k: v for k, v in self.air.items() if k != self.phone_ssid}
        return self.air

    def run(self, args, timeout=40, secret=False):
        self.calls.append(list(args))
        self.last_stderr = ""
        a = args
        if a[:2] == ["nmcli", "-t"] and a[4:] == ["device"]:
            out = ""
            if self.eth is not None:
                out += "eth0:ethernet:%s\n" % self.eth
            if self.wifi_dev:
                out += "%s:wifi:%s\n" % (self.wifi_dev, ("connected" if self.active else "disconnected") if self.radio else "unavailable")
            return 0, out + "lo:loopback:unmanaged\n"
        if a[:3] == ["nmcli", "radio", "wifi"]:
            self.radio = a[3] == "on"
            if not self.radio:
                self.active = ""
            return 0, ""
        if a[:2] == ["nmcli", "-g"] and a[2] == "IP4.ADDRESS" and a[-1] == "eth0":
            return 0, "192.168.1.77/24\n"
        if a[:2] == ["nmcli", "-t"] and a[4:6] == ["connection", "show"]:
            return 0, "".join("%s:%s\n" % (esc(n), c["type"]) for n, c in self.conns.items())
        if a[:2] == ["nmcli", "-g"] and "connection" in a:
            name = a[-1]
            c = self.conns.get(name)
            if not c:
                return 10, ""
            f = a[2]
            v = {"802-11-wireless.ssid": c["ssid"], "802-11-wireless.hidden": "yes" if c["hidden"] else "no",
                 "802-11-wireless.mode": c["mode"], "802-11-wireless-security.psk": c["psk"] if "-s" in a else "",
                 "802-11-wireless-security.key-mgmt": "wpa-psk" if c["psk"] else "",
                 "802-11-wireless.powersave": c.get("ps", "0 (default)")}.get(f, "")
            return 0, esc(v) + "\n"
        if a[:2] == ["nmcli", "-t"] and a[4:6] == ["device", "show"]:
            st = 100 if self.active else 30
            return 0, "GENERAL.STATE:%d (x)\nGENERAL.CONNECTION:%s\n" % (st, esc(self.active) if self.active else "--")
        if a[:2] == ["nmcli", "-g"] and a[2] == "IP4.ADDRESS":
            return 0, ("10.42.0.1/24\n" if self.is_ap() else "192.168.1.50/24\n") if self.active else "\n"
        if a[:3] == ["nmcli", "-t", "-f"] and "wifi" in a and "list" in a:
            if self.is_ap():
                return 1, ""       # ★ NM: "Scanning not allowed while in AP mode"
            if self.scan_fails > 0:
                self.scan_fails -= 1
                self.last_stderr = "Error: Scanning not allowed immediately following previous scan."
                return 1, ""
            return 0, "".join("%s:%d:WPA2\n" % (esc(s), sig) for s, (sig, _) in self.on_air().items())
        if a[:2] == ["iw", "dev"] and a[3:] == ["scan", "ap-force"]:
            if not self.ap_force:
                return 1, ""
            return 0, "".join("BSS x\n\tSSID: %s\n" % s for s in self.on_air())
        if a[:2] == ["iw", "dev"] and a[3:] == ["station", "dump"]:
            if self.active and not self.is_ap():     # as a client: the one station is the router, with counters
                return 0, "Station e4:5e:1b:00:00:01 (on wlan0)\n\ttx packets:\t%d\n\ttx retries:\t%d\n" % (self.txpk, self.txrt)
            return 0, "".join("Station aa:bb:cc:00:00:%02d (on wlan0)\n" % i for i in range(self.stations))
        if a[:2] == ["iw", "dev"] and a[3:] == ["get", "power_save"]:
            return 0, "Power save: %s\n" % self.ps
        if a[:2] == ["iw", "dev"] and a[3:] == ["set", "power_save", "off"]:
            if not self.ps_stuck:
                self.ps = "off"
            return 0, ""
        if a[:2] == ["iw", "dev"] and a[3:] == ["link"]:
            if not self.active or self.is_ap():
                return 0, "Not connected.\n"
            return 0, ("Connected to e4:5e:1b:00:00:01 (on wlan0)\n\tSSID: %s\n\tsignal: %d dBm\n"
                       "\trx bitrate: 72.2 MBit/s MCS 7 short GI\n\ttx bitrate: %.1f MBit/s MCS 5\n") % (self.assoc, self.link_sig, self.tx_rate)
        if a[:3] == ["nmcli", "--wait", a[2]] and a[3:5] == ["connection", "up"]:
            name = a[5]
            c = self.conns.get(name)
            self.t += 5
            if not c or not self.radio:
                return 10, ""
            if c["mode"] == "ap":
                # ★ What a phone joining at THIS instant would find: the redirect loaded, the state saying hotspot.
                st = {}
                try:
                    with open(self.state_path) as f:
                        st = json.load(f)
                except (OSError, ValueError, AttributeError, TypeError):
                    pass
                self.ap_up_seen.append((bool(self.nft), st.get("mode"), self.door))
                self.active = name
                return 0, ""
            # ★★ NetworkManager matches the saved name to the air BYTE FOR BYTE — "Stuart's" is not "Stuart’s".
            if self.is_ap():
                self.phone_on_ap = False            # leaving our hotspot for a network drops its stations
            real = self.on_air().get(c["ssid"])
            if real and real[1] == c["psk"]:
                self.active = name
                self.assoc = c["ssid"]
                if self.ps_on_join:
                    self.ps = "on"
                return 0, ""
            self.active = ""
            self.t += int(a[2])
            self.last_stderr = ("Error: Connection activation failed: (7) Secrets were required, but not provided."
                                if real else "Error: Connection activation failed: The Wi-Fi network could not be found.")
            return 4, ""
        if a[:3] == ["nmcli", "connection", "down"]:
            if self.active == a[3]:
                if self.is_ap():
                    self.phone_on_ap = False        # the hotspot went: every station on it is off it
                self.active = ""
            return 0, ""
        if a[:3] == ["nmcli", "connection", "delete"]:
            self.deleted.append(a[3])
            self.conns.pop(a[3], None)
            if self.active == a[3]:
                self.active = ""
            return 0, ""
        if a[:3] == ["nmcli", "connection", "add"]:
            kv = dict(zip(a[3::2], a[4::2]))
            name = kv["con-name"]
            self.conns[name] = {"type": "802-11-wireless", "ssid": kv["ssid"],
                                "psk": kv.get("wifi-sec.psk", ""),
                                "prio": int(kv.get("connection.autoconnect-priority", "0")),
                                "hidden": kv.get("802-11-wireless.hidden") == "yes",
                                "mode": kv.get("802-11-wireless.mode", "infrastructure"),
                                "ps": {"2": "2 (disable)"}.get(kv.get("802-11-wireless.powersave"), "0 (default)")}
            return 0, ""
        if a[:3] == ["nmcli", "connection", "modify"]:
            c = self.conns[a[3]]
            kv = dict(zip(a[4::2], a[5::2]))
            self.modified.append(a[3])
            if "802-11-wireless.ssid" in kv: c["ssid"] = kv["802-11-wireless.ssid"]
            if "wifi-sec.psk" in kv: c["psk"] = kv["wifi-sec.psk"]
            if "connection.autoconnect-priority" in kv: c["prio"] = int(kv["connection.autoconnect-priority"])
            if "802-11-wireless.hidden" in kv: c["hidden"] = kv["802-11-wireless.hidden"] == "yes"
            if "802-11-wireless.powersave" in kv: c["ps"] = {"2": "2 (disable)"}.get(kv["802-11-wireless.powersave"], kv["802-11-wireless.powersave"])
            return 0, ""
        return 0, ""

    def is_ap(self):
        return bool(self.active) and self.conns.get(self.active, {}).get("mode") == "ap"


def make(tmp):
    nm = FakeNM()
    box = vp.Box(nm, dict(vp.DEFAULTS), state_path=os.path.join(tmp, "state.json"))
    box.dev = "wlan0"
    nm.state_path = box.state_path
    return nm, box


def run_for(nm, box, seconds):
    end = nm.t + seconds
    while nm.t < end:
        box.tick()
        box.write_state()
        nm.t += 5


def req(nets, ap=("Pocket-SDR", "pocketpass"), switch=False, ap_keep=False):
    r, err = vp.validate_request({
        "networks": [dict(ssid=s, psk=p, **kw) for s, p, kw in nets],
        "ap": {"ssid": ap[0], "psk": ap[1], "keep": ap_keep}, "country": "GB", "switch": switch})
    assert r, err
    return r



def road_test_2026_10_14(tmp):
    """★★★ STUART'S FIRST ROAD TEST (2026-10-14): the box never joined his iPhone's hotspot at boot, and choosing the
    phone in the app's menu from the box's own Wi-Fi never switched it. Each case below FAILS on the code before the fix
    (bar the two marked as guards)."""
    CURLY, ASCII = "Stuart’s iPhone", "Stuart's iPhone"

    def box_with(ssid, hidden=False, air=None):
        nm, box = make(tmp)
        nm.conns = {"vibe-net-1": {"type": "802-11-wireless", "ssid": ssid, "psk": "phonepass", "prio": 30,
                                   "hidden": hidden, "mode": "infrastructure"},
                    vp.AP_NAME: {"type": "802-11-wireless", "ssid": "Pocket-SDR", "psk": "pocketpass", "prio": 0,
                                 "hidden": False, "mode": "ap"}}
        nm.air = dict(air or {})
        return nm, box

    # ── the name typed on a Mac (') vs the name on the air (’) ──
    for hidden in (False, True):
        nm, box = box_with(ASCII, hidden=hidden, air={CURLY: (70, "phonepass")})
        run_for(nm, box, 120)
        ok(nm.active == "vibe-net-1" and nm.conns["vibe-net-1"]["ssid"] == CURLY,
           "typed \"Stuart's iPhone\", the iPhone says \"Stuart’s iPhone\"%s: JOINED, and the saved name corrected to "
           "the one on the air" % (" (ticked not visible)" if hidden else ""))
    nm, box = box_with("stuart’s iphone ", air={CURLY: (70, "phonepass")})
    run_for(nm, box, 120)
    ok(nm.active == "vibe-net-1", "…capitals and a trailing space typed wrong: still found and joined")
    nm, box = box_with(ASCII, air={CURLY: (70, "phonepass"), "Stuart‘s iPhone": (60, "other")})
    run_for(nm, box, 200)
    ok(nm.active == vp.AP_NAME and nm.conns["vibe-net-1"]["ssid"] == ASCII,
       "(guard) TWO networks on the air could be it: neither is guessed — a password is never offered to the wrong one")
    nm, box = box_with("Home", air={"Home": (50, "phonepass"), "HOME": (50, "y" * 8)})
    run_for(nm, box, 120)
    ok(nm.active == "vibe-net-1" and nm.conns["vibe-net-1"]["ssid"] == "Home",
       "(guard) an exact name always wins over a near one")

    # ── a refused scan is not an empty sky ──
    nm, box = box_with(CURLY, air={CURLY: (70, "phonepass")})
    nm.scan_fails = 1
    run_for(nm, box, 75)
    ok(nm.active == "vibe-net-1", "the first scan refused (NM's rate limit): looks again and joins the phone")
    nm, box = box_with(CURLY, air={CURLY: (70, "phonepass")})
    nm.scan_fails = 10 ** 6
    run_for(nm, box, 75)
    ok(nm.active == "vibe-net-1", "scans refused every time: the saved networks are tried by name, not given up on")

    # ── the WHY of a failed join is NetworkManager's, not a guess ──
    nm, box = box_with(CURLY, air={CURLY: (70, "WRONGpass")})
    run_for(nm, box, 120)
    ok(nm.active == vp.AP_NAME and "password" in box.last_error, "wrong password: the owner is told it refused the password")
    nm, box = box_with(CURLY, hidden=True, air={})
    run_for(nm, box, 120)
    ok("not found" in box.last_error and "password" not in box.last_error,
       "a hidden phone that is not on: 'not found', not 'check its password' (got: %s)" % box.last_error)

    # ── ★★★ the menu, from the box's own Wi-Fi, with the iPhone ON that Wi-Fi ──
    for ap_force in (False, True):
        nm, box = box_with(CURLY, air={})
        nm.ap_force = ap_force
        run_for(nm, box, 200)
        ok(nm.active == vp.AP_NAME, "(out and about: on its own Wi-Fi%s)" % (", ap-force scans work" if ap_force else ""))
        nm.stations, nm.phone_ssid, nm.phone_on_ap = 1, CURLY, True
        nm.air[CURLY] = (70, "phonepass")          # Personal Hotspot switched on — but the phone is on the box's Wi-Fi
        box.choose_net(1)
        run_for(nm, box, 60)
        ok(nm.active == "vibe-net-1",
           "picked the iPhone FROM the box's own Wi-Fi%s: the hotspot is left so the phone can host — joined"
           % (" (ap-force)" if ap_force else ""))
    nm, box = box_with(ASCII, air={})
    run_for(nm, box, 200)
    nm.stations, nm.phone_ssid, nm.phone_on_ap = 1, CURLY, True
    nm.air[CURLY] = (70, "phonepass")
    box.choose_net(1)
    run_for(nm, box, 60)
    ok(nm.active == "vibe-net-1" and nm.conns["vibe-net-1"]["ssid"] == CURLY,
       "…and with the name typed with a straight apostrophe: found, corrected, joined")
    nm, box = box_with(CURLY, air={})
    nm.ap_force = True
    run_for(nm, box, 200)
    box.choose_net(1)
    run_for(nm, box, int(vp.DEFAULTS["TRY_SEC"]) + 60)
    ok(nm.active == vp.AP_NAME and not box.want and "not found" in box.last_error,
       "(guard) picked, never switched on: its own Wi-Fi comes back and says why")


def powersave_phase0(tmp):
    """★★★ BRIEF-portable-wifi-realtime.md, Phase 0 (2026-10-20): Wi-Fi power save is OFF on every profile we write, on
    every profile an older build wrote, and on the radio itself after every join — a dozing radio is tens of ms of jitter on
    each audio packet over a busy 2.4 GHz phone hotspot."""
    def ps_pair(args):
        return any(args[i:i + 2] == ["802-11-wireless.powersave", "2"] for i in range(len(args) - 1))

    # ── 0.3a: the args of every saved network and of both hotspots carry powersave 2 ──
    nm, box = make(tmp)
    nm.air = {"Home": (60, "homepass1")}
    box.ensure_setup_ap()
    box.apply(req([("Home", "homepass1", {}), ("Phone", "phonepass", {"hidden": True}), ("Cafe", "", {})]))
    writes = [c for c in nm.calls if c[:3] in (["nmcli", "connection", "add"], ["nmcli", "connection", "modify"])]
    names = {}
    for c in writes:
        n = c[c.index("con-name") + 1] if "con-name" in c else c[3]
        names.setdefault(n, []).append(c)
    want = ["vibe-net-1", "vibe-net-2", "vibe-net-3", vp.AP_NAME, vp.SETUP_NAME]
    ok(all(n in names and ps_pair(names[n][0]) for n in want),
       "0.3: every saved network (1, 2, 3) and BOTH hotspots are written with 802-11-wireless.powersave 2")
    ok("802-11-wireless.powersave" in vp.Box(nm, dict(vp.DEFAULTS)).ap_args()
       and ps_pair(vp.Box(nm, dict(vp.DEFAULTS)).ap_args()), "…ap_args() itself carries it (setup + fallback hotspot)")
    # a re-save of an existing slot (modify in place) carries it too
    nm.calls.clear()
    box.apply(req([("Home", "", {"keep": True}), ("Phone", "phonepass", {"hidden": True})]))
    mods = [c for c in nm.calls if c[:4] == ["nmcli", "connection", "modify", "vibe-net-1"]]
    ok(mods and ps_pair(mods[0]), "…and so does re-saving a slot in place (modify, not add)")

    # ── 0.1 / 0.3b: the migration ──
    nm, box = make(tmp)
    mk = lambda ssid, mode, ps: {"type": "802-11-wireless", "ssid": ssid, "psk": "x" * 8, "prio": 0,
                                 "hidden": False, "mode": mode, "ps": ps}
    nm.conns = {"vibe-net-1": mk("Home", "infrastructure", "0 (default)"),     # an older build: never written
                "vibe-net-2": mk("Phone", "infrastructure", "2 (disable)"),    # already right
                "vibe-net-3": mk("Cafe", "infrastructure", "disable"),         # right, in nmcli's other spelling
                vp.AP_NAME: mk("Pocket-SDR", "ap", "3 (enable)"),              # explicitly on
                vp.SETUP_NAME: mk("VibeServer", "ap", ""),                     # no field at all
                "preconfigured": mk("Imager", "infrastructure", "0 (default)")}  # NOT ours (Raspberry Pi Imager)
    nm.active = "vibe-net-1"
    nm.calls.clear()
    with redirect_stdout(io.StringIO()) as out:
        fixed = box.migrate_powersave()
    ok(sorted(fixed) == sorted(["vibe-net-1", vp.AP_NAME, vp.SETUP_NAME]),
       "0.1: the migration modifies the stale profiles (default, enable, missing) — got %s" % fixed)
    ok(all(nm.conns[n]["ps"].startswith("2") for n in ("vibe-net-1", vp.AP_NAME, vp.SETUP_NAME)),
       "…which now say disable")
    ok("vibe-net-2" not in nm.modified and "vibe-net-3" not in nm.modified,
       "…and leaves a correct one alone (\"2 (disable)\" and \"disable\" both read as correct)")
    ok("preconfigured" not in nm.modified, "…and never touches a profile that is not ours")
    ok(not any(c[:3] in (["nmcli", "connection", "down"], ["nmcli", "connection", "up"]) or c[3:5] == ["connection", "up"]
               for c in nm.calls) and nm.active == "vibe-net-1",
       "★★ …without bouncing the live link: no down, no up — the active profile is modified in place")
    ok("power save switched off in 3" in out.getvalue(), "…and says what it changed")
    nm.calls.clear()
    with redirect_stdout(io.StringIO()):
        again = box.migrate_powersave()
    ok(again == [] and not any(c[:3] == ["nmcli", "connection", "modify"] for c in nm.calls),
       "…idempotent: run again, nothing is written")
    ok(vp.powersave_disabled("2") and vp.powersave_disabled("2 (disable)") and vp.powersave_disabled("disable")
       and not vp.powersave_disabled("0 (default)") and not vp.powersave_disabled("") and not vp.powersave_disabled("20"),
       "nmcli's spellings of the setting are read right")
    # ★ The boot runs it, by name, before any radio exists (a wired box; a USB dongle plugged in later).
    nm, box = make(tmp)
    box.dev = None
    nm.wifi_dev = None
    nm.conns = {"vibe-net-1": mk("Home", "infrastructure", "0 (default)")}
    saved = (vp.AVAHI_CONF, vp.RESET_FILES, vp.DNSMASQ_CONF)
    vp.AVAHI_CONF, vp.RESET_FILES, vp.DNSMASQ_CONF = os.path.join(tmp, "nope.conf"), (), os.path.join(tmp, "dnsmasq.conf")
    try:
        with redirect_stdout(io.StringIO()):
            vp.setup_once(box)
    finally:
        vp.AVAHI_CONF, vp.RESET_FILES, vp.DNSMASQ_CONF = saved
    ok(nm.conns["vibe-net-1"]["ps"].startswith("2"), "the migration runs at service start, with or without a radio")

    # ── 0.2: the radio itself, after every join ──
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": mk("Home", "infrastructure", "2 (disable)")}
    nm.conns["vibe-net-1"]["psk"] = "homepass1"
    nm.air = {"Home": (60, "homepass1")}
    nm.ps_on_join = True                      # ★ brcmfmac: the join switches it back on, whatever the profile says
    with redirect_stdout(io.StringIO()) as out:
        run_for(nm, box, 90)
    st = json.load(open(box.state_path))
    ok(nm.active == "vibe-net-1" and nm.ps == "off" and ["iw", "dev", "wlan0", "set", "power_save", "off"] in nm.calls,
       "0.2: joined with the radio's power save ON — `iw … set power_save off` is applied")
    ok(st["link"] and st["link"].get("powerSave") == "off", "…and the state's link health says powerSave: off (the setup page)")
    ok(out.getvalue().count("power save was ON") == 1, "…logged once for that join")
    # a reassociation / firmware recovery switches it on again with no NM change: the minute's re-check catches it
    nm.ps = "on"
    with redirect_stdout(io.StringIO()) as out:
        run_for(nm, box, vp.POWERSAVE_RECHECK_SEC + 10)
    ok(nm.ps == "off", "★ TRAP: switched on again mid-join (no NM change) — the re-check switches it off again")
    gets = [c for c in nm.calls if c[3:] == ["get", "power_save"]]
    ok(len(gets) <= 2 + 2 * (90 + vp.POWERSAVE_RECHECK_SEC + 10) // vp.POWERSAVE_RECHECK_SEC + 2,
       "…and the check is cheap: about once a minute, not every tick (%d reads)" % len(gets))
    # a driver that will not switch it off: said, and shown as on
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": dict(mk("Home", "infrastructure", "2 (disable)"), psk="homepass1")}
    nm.air = {"Home": (60, "homepass1")}
    nm.ps_on_join, nm.ps_stuck = True, True
    with redirect_stdout(io.StringIO()) as out:
        run_for(nm, box, 60)
    st = json.load(open(box.state_path))
    ok(st["link"].get("powerSave") == "on" and "could NOT be switched off" in out.getvalue(),
       "a radio that refuses: the state says on and the log says so — never reported as off")
    # a hotspot is not a client: never asked
    nm, box = make(tmp)
    nm.air = {}
    run_for(nm, box, 120)
    ok(not any(c[3:] == ["get", "power_save"] for c in nm.calls), "on its own hotspot: not checked (AP mode does not doze)")


def main():
    print("vibeserver-pocket — the pocket box's Wi-Fi state machine")
    tmp = tempfile.mkdtemp()
    log = io.StringIO()
    with redirect_stdout(log):
        # ── first boot ──
        nm, box = make(tmp)
        nm.air = {"Home": (60, "homepass1"), "Neighbour": (80, "zzzzzzzz")}
        box.tick()
        ok(nm.active == vp.SETUP_NAME and box.mode == "setup-ap", "first boot: the OPEN setup hotspot comes up")
        ok(nm.conns[vp.SETUP_NAME]["ssid"] == "VibeServer" and nm.conns[vp.SETUP_NAME]["psk"] == "",
           "it is called VibeServer and has no password")
        ok({x["ssid"] for x in box.scan_cache} == {"Home", "Neighbour"}, "networks were scanned BEFORE the hotspot (setup page's list)")
        ok(nm.nft and "redirect to :48000" in nm.nft[-1], "port 80 is redirected to the front door")
        ok(nm.ap_up_seen and nm.ap_up_seen[0] == (True, "setup-ap", True),
           "the redirect is loaded and the state says setup-ap BEFORE the hotspot exists (a phone's one check is answered)")

        # ── ★★★ the hotspot waits for the front door (v7 card, 2026-10-10: joined, no captive sheet) ──
        tmpw = tempfile.mkdtemp()
        nmw, boxw = make(tmpw)
        nmw.door = False
        boxw.tick()
        ok(nmw.active == "" and not nmw.ap_up_seen, "server not listening yet: NO hotspot on the air")
        run_for(nmw, boxw, 30)
        ok(nmw.active == "", "…still none 30 s on, while the server is not answering")
        nmw.door = True
        run_for(nmw, boxw, 10)
        ok(nmw.active == vp.SETUP_NAME and nmw.ap_up_seen[-1] == (True, "setup-ap", True),
           "server answers: the setup hotspot comes up, with the redirect and state already in place")
        tmpd = tempfile.mkdtemp()
        nmd, boxd = make(tmpd)
        nmd.door = False
        run_for(nmd, boxd, int(vp.DEFAULTS["DOOR_WAIT_SEC"]) + 20)
        ok(nmd.active == vp.SETUP_NAME, "a server that never answers: the hotspot opens anyway after DOOR_WAIT_SEC (box stays reachable)")

        # ── setup saves 3 networks + the hotspot, without switching ──
        nm.stations = 1
        box.apply(req([("Home", "homepass1", {}), ("Stuart iPhone", "phonepass", {"hidden": True}),
                       ("Work", "workpass1", {})]))
        ok([nm.conns["vibe-net-%d" % i]["prio"] for i in (1, 2, 3)] == [30, 20, 10],
           "autoconnect-priority 30/20/10 — network 1 highest")
        ok(nm.conns["vibe-net-2"]["hidden"], "the phone hotspot is saved as not-advertising (hidden)")
        ok(nm.conns[vp.AP_NAME]["psk"] == "pocketpass" and nm.conns[vp.AP_NAME]["mode"] == "ap",
           "the fallback hotspot is saved, WPA2")
        ok(nm.active == vp.SETUP_NAME, "saving without 'switch' keeps the phone on the setup hotspot")
        run_for(nm, box, 300)
        ok(nm.active == vp.SETUP_NAME, "…and a phone still on it is never pulled off, however long")

        # ── the phone leaves; the box moves on, network 1 first ──
        nm.stations = 0
        nm.air["Work"] = (95, "workpass1")          # ★ stronger, but ranked 3rd
        run_for(nm, box, 200)
        ok(nm.active == "vibe-net-1" and box.mode == "client",
           "idle setup hotspot closes; network 1 (Home) wins over a STRONGER network 3")
        ok(box.captive_port is not None, ":80 redirect KEPT as a client — http://vibepocket.local/ works on the home network")

        # ── strict order on a later boot ──
        nm2, box2 = make(tmp)
        nm2.conns = json.loads(json.dumps(nm.conns))
        nm2.air = {"Home": (20, "homepass1"), "Work": (99, "workpass1")}
        run_for(nm2, box2, 120)
        ok(nm2.active == "vibe-net-1", "boot with 1 and 3 in range: joins 1, whatever the signal")

        # ── only 2 in range, then 1 returns ──
        nm3, box3 = make(tmp)
        nm3.conns = json.loads(json.dumps(nm.conns))
        nm3.air = {"Stuart iPhone": (70, "phonepass")}
        run_for(nm3, box3, 120)
        ok(nm3.active == "vibe-net-2", "away from home: joins network 2 (the phone)")
        nm3.air["Home"] = (40, "homepass1")
        run_for(nm3, box3, 150)
        ok(nm3.active == "vibe-net-1", "home again: MOVES UP from 2 to 1 by itself")

        # ── 1 visible but its password is wrong ──
        nm4, box4 = make(tmp)
        nm4.conns = json.loads(json.dumps(nm.conns))
        nm4.air = {"Stuart iPhone": (70, "phonepass"), "Home": (40, "CHANGED-by-router")}
        run_for(nm4, box4, 200)
        ok(nm4.active == "vibe-net-2", "network 1 refuses us: on network 2, not stuck")
        upgrades = sum(1 for c in nm4.calls if c[3:6] == ["connection", "up", "vibe-net-1"])
        run_for(nm4, box4, 600)
        again = sum(1 for c in nm4.calls if c[3:6] == ["connection", "up", "vibe-net-1"])
        ok(again == upgrades and nm4.active == "vibe-net-2", "…and it rests 15 min instead of an outage every 2 min")

        # ── …but not a 25 s outage every 16½ min FOR EVER (the portable box, 2026-10-11: every listener dropped
        #    on exactly that beat all night). The rest doubles, and a listener is never dropped for a retry. ──
        def tries(n):
            return sum(1 for c in n.calls if c[3:6] == ["connection", "up", "vibe-net-1"])

        def scans(n):
            return sum(1 for c in n.calls if "--rescan" in c)
        refusing = {"Stuart iPhone": (70, "phonepass"), "Home": (40, "CHANGED-by-router")}
        nm6, box6 = make(tmp)
        nm6.conns = json.loads(json.dumps(nm.conns))
        nm6.air = dict(refusing)
        run_for(nm6, box6, 3 * 3600)
        ok(tries(nm6) <= 4 and nm6.active == "vibe-net-2",
           "nobody listening, 3 h with network 1 refusing: %d tries (a flat 15-min rest made 12) — 15 min, 30, 1 h …"
           % tries(nm6))

        saved_active = vp.ACTIVE_FILE
        vp.ACTIVE_FILE = os.path.join(tmp, "pocket-active-upgrade")

        def listening(on):
            if on:
                open(vp.ACTIVE_FILE, "w").close()
                os.utime(vp.ACTIVE_FILE, None)
            elif os.path.exists(vp.ACTIVE_FILE):
                os.utime(vp.ACTIVE_FILE, (time.time() - 99999, time.time() - 99999))

        def run_listening(n, b, seconds):
            end = n.t + seconds
            while n.t < end:
                listening(True)                       # the server touches it every 30 s; fresh is enough here
                b.tick(); b.write_state(); n.t += 5

        # ── ★★★ IN USE = STAY PUT (Stuart, 2026-10-11): no move up, no retry, NOT EVEN A SCAN, until the radio is quiet ──
        nm7, box7 = make(tmp)
        nm7.conns = json.loads(json.dumps(nm.conns))
        nm7.air = {"Stuart iPhone": (70, "phonepass")}
        run_for(nm7, box7, 120)
        ok(nm7.active == "vibe-net-2", "away from home: on network 2")
        nm7.air["Home"] = (40, "homepass1")
        s0 = scans(nm7)
        run_listening(nm7, box7, 3 * 3600)
        ok(nm7.active == "vibe-net-2" and scans(nm7) == s0 and tries(nm7) == 0,
           "radio IN USE for 3 h with network 1 back: stays on 2, %d scans, %d tries (the link is never touched)"
           % (scans(nm7) - s0, tries(nm7)))
        listening(False)
        run_for(nm7, box7, 200)
        ok(nm7.active == "vibe-net-1", "…the radio goes quiet: moves up to network 1 within a few minutes")

        # ── ★★★ THE 16½-MINUTE DROP (portable box, all night 2026-10-11): network 1 is the owner's phone, saved "not
        #    visible", and the phone is not hosting. The box sat on Home (2) and probed the phone blind — 30 s off the
        #    air — every time the 15-minute rest ran out. From a working link a hidden network is only tried if SEEN. ──
        nm9, box9 = make(tmp)
        nm9.conns = json.loads(json.dumps(nm.conns))
        nm9.conns["vibe-net-1"], nm9.conns["vibe-net-2"] = nm9.conns["vibe-net-2"], nm9.conns["vibe-net-1"]
        nm9.conns["vibe-net-1"]["prio"], nm9.conns["vibe-net-2"]["prio"] = 30, 20
        ok(nm9.conns["vibe-net-1"]["hidden"] and nm9.conns["vibe-net-1"]["ssid"] == "Stuart iPhone",
           "(the portable box's order: 1 = the phone, not visible; 2 = home)")
        nm9.air = {"Home": (50, "homepass1")}
        listening(False)
        run_for(nm9, box9, 120)
        ok(nm9.active == "vibe-net-2", "boot with the phone's hotspot off: the phone is tried blind once, then home")
        downs = lambda n: sum(1 for c in n.calls if c[3:6] == ["connection", "up", "vibe-net-1"])
        d0 = downs(nm9)
        run_for(nm9, box9, 8 * 3600)
        ok(downs(nm9) == d0 and nm9.active == "vibe-net-2",
           "…8 h on home, nobody listening: the hidden phone is never probed from the working link (was every 16½ min)")
        nm9.air["Stuart iPhone"] = (70, "phonepass")          # the phone starts hosting (advertising)
        run_for(nm9, box9, 200)
        ok(nm9.active == "vibe-net-1", "…the phone starts hosting where the box can see it: idle, so it moves up to it")

        # ── ★★★ THE MENU CHOICE (Stuart, 2026-10-11: "Stuey3D's iPhone | Meow! | Own WiFi") ──
        nmc, boxc = make(tmp)
        nmc.conns = json.loads(json.dumps(nm9.conns))
        nmc.conns["vibe-net-1"]["hidden"] = False
        nmc.air = {"Home": (50, "homepass1")}
        run_for(nmc, boxc, 120)
        ok(nmc.active == "vibe-net-2", "(on home, the phone's hotspot off)")
        boxc.choose_net(1)
        run_listening(nmc, boxc, 30)
        ok(nmc.active == "vibe-net-2" and boxc.want and json.load(open(boxc.state_path))["trying"] == "Stuart iPhone",
           "picked the phone: home is KEPT while the box looks, and the state says what it is looking for")
        nmc.air["Stuart iPhone"] = (70, "phonepass")          # the owner switches Personal Hotspot on
        run_listening(nmc, boxc, 20)
        ok(nmc.active == "vibe-net-1" and not boxc.want, "the hotspot comes on: joined within a tick or two")
        nmc.air["Home"] = (50, "homepass1")
        run_listening(nmc, boxc, 3600)
        ok(nmc.active == "vibe-net-1", "…held while the radio is in use")
        boxc.choose_net(2)
        run_listening(nmc, boxc, 30)
        ok(nmc.active == "vibe-net-2" and boxc.hold_net == "vibe-net-2", "picked home (ranked BELOW the phone): joined…")
        run_listening(nmc, boxc, 3600)
        ok(nmc.active == "vibe-net-2", "…and NOT moved back up to the phone while in use")
        listening(False)
        run_for(nmc, boxc, 300)
        ok(nmc.active == "vibe-net-2", "…nobody listening for 5 min: still held")
        boxc.hold_at = time.time() - int(vp.DEFAULTS["RELEASE_SEC"]) - 1   # the hold is on the wall clock (as Own Wi-Fi's)
        run_for(nmc, boxc, 300)
        ok(nmc.active == "vibe-net-1" and not boxc.hold_net, "…30 quiet minutes later: the usual order again")

        nmn, boxn = make(tmp)
        nmn.conns = json.loads(json.dumps(nmc.conns))
        nmn.air = {"Home": (50, "homepass1")}
        run_for(nmn, boxn, 120)
        boxn.choose_net(1)
        run_listening(nmn, boxn, int(vp.DEFAULTS["TRY_SEC"]) + 30)
        ok(nmn.active == "vibe-net-2" and not boxn.want and "not found" in boxn.last_error,
           "picked the phone and it never came on: still on home after %s s, never dropped, and says why"
           % vp.DEFAULTS["TRY_SEC"])
        ok(tries(nmn) == 0, "…the phone was never tried blind (it was not hidden, and never seen)")

        nmh, boxh = make(tmp)
        nmh.conns = json.loads(json.dumps(nm9.conns))           # the phone saved "not visible"
        nmh.air = {"Home": (50, "homepass1")}
        run_for(nmh, boxh, 120)
        boxh.choose_net(1)
        run_listening(nmh, boxh, int(vp.DEFAULTS["TRY_SEC"]) + 60)
        ok(nmh.active == "vibe-net-2" and not boxh.want,
           "a NOT-VISIBLE phone chosen and never switched on: tried by name, then back on home")

        nmo, boxo = make(tmp)
        nmo.conns = json.loads(json.dumps(nmc.conns))
        nmo.air = {}
        run_for(nmo, boxo, 200)
        ok(nmo.active == vp.AP_NAME, "(out and about: on its own Wi-Fi)")
        boxo.choose_net(1)
        run_listening(nmo, boxo, 10)
        nmo.air["Stuart iPhone"] = (70, "phonepass")
        run_listening(nmo, boxo, 60)
        ok(nmo.active == "vibe-net-1", "on its own Wi-Fi, picked the phone, switched it on: joined")
        nmo2, boxo2 = make(tmp)
        nmo2.conns = json.loads(json.dumps(nmc.conns))
        nmo2.air = {}
        run_for(nmo2, boxo2, 200)
        boxo2.choose_net(1)
        run_listening(nmo2, boxo2, int(vp.DEFAULTS["TRY_SEC"]) + 60)
        ok(nmo2.active == vp.AP_NAME, "on its own Wi-Fi, picked the phone, never switched on: its own Wi-Fi comes back")
        vp.ACTIVE_FILE = saved_active

        # ── the phone hotspot not advertising ──
        nm5, box5 = make(tmp)
        nm5.conns = json.loads(json.dumps(nm.conns))
        nm5.air = {}
        hidden_phone = {"Stuart iPhone": (0, "phonepass")}
        # The fake's scan shows everything in `air`; model "not advertising" by scanning nothing but
        # still accepting the join.
        real_run = nm5.run

        def run_hidden(args, timeout=40, secret=False):
            if args[:3] == ["nmcli", "--wait", args[2]] and args[5:6] == ["vibe-net-2"]:
                nm5.air.update(hidden_phone)
                rc = real_run(args, timeout, secret)
                return rc
            return real_run(args, timeout, secret)
        nm5.run = run_hidden
        run_for(nm5, box5, 200)
        ok(nm5.active == "vibe-net-2", "a phone hotspot that is NOT in the scan is still tried by name")

        # ── nothing in range ⇒ the SECURED hotspot ──
        nm6, box6 = make(tmp)
        nm6.conns = json.loads(json.dumps(nm.conns))
        nm6.air = {"Neighbour": (80, "zzzzzzzz")}
        run_for(nm6, box6, 200)
        ok(nm6.active == vp.AP_NAME and box6.mode == "fallback-ap",
           "no saved network in range: the SECURED fallback hotspot, not the open one")
        # Home returns while a phone is listening through the hotspot.
        nm6.stations = 1
        nm6.air["Home"] = (50, "homepass1")
        run_for(nm6, box6, 600)
        ok(nm6.active == vp.AP_NAME, "home returns while a phone is on the hotspot: the phone keeps it")
        nm6.stations = 0
        run_for(nm6, box6, 400)
        ok(nm6.active == "vibe-net-1", "phone gone, hotspot idle: rescans (disruptively — no ap-force) and rejoins home")

        # ── ap-force: no needless drop when nothing of ours is out there ──
        nm7, box7 = make(tmp)
        nm7.conns = json.loads(json.dumps(nm.conns))
        nm7.ap_force = True
        nm7.conns["vibe-net-2"]["hidden"] = False
        nm7.air = {}
        run_for(nm7, box7, 200)
        downs = sum(1 for c in nm7.calls if c[:3] == ["nmcli", "connection", "down"])
        run_for(nm7, box7, 900)
        downs2 = sum(1 for c in nm7.calls if c[:3] == ["nmcli", "connection", "down"])
        ok(nm7.active == vp.AP_NAME and downs2 == downs,
           "with ap-force scans and nothing of ours in range, the hotspot is never dropped")

        # ── reorder without retyping passwords ──
        box.apply(req([("Stuart iPhone", "", {"keep": True}), ("Home", "", {"keep": True})], ap_keep=True,
                      ap=("Pocket-SDR", "")))
        ok(nm.conns["vibe-net-1"]["ssid"] == "Stuart iPhone" and nm.conns["vibe-net-1"]["psk"] == "phonepass",
           "reorder with keep: the phone is now 1, its password carried over")
        ok(nm.conns["vibe-net-2"]["psk"] == "homepass1" and "vibe-net-3" not in nm.conns, "home is 2, Work removed")
        ok(nm.conns[vp.AP_NAME]["psk"] == "pocketpass", "hotspot password kept")
        ok("vibe-net-1" in nm.modified and "vibe-net-2" in nm.modified and "vibe-net-1" not in nm.deleted
           and "vibe-net-2" not in nm.deleted and "vibe-net-3" in nm.deleted,
           "a re-save MODIFIES networks in place (a cut never leaves none); only the dropped rank 3 is deleted")
        i_del = max(i for i, c in enumerate(nm.calls) if c[:4] == ["nmcli", "connection", "delete", "vibe-net-3"])
        i_mod = max(i for i, c in enumerate(nm.calls) if c[:3] == ["nmcli", "connection", "modify"] and c[3].startswith("vibe-net-"))
        ok(i_del > i_mod, "…and that deletion comes after every network has been rewritten")

        # ── reset ──
        box.reset_wifi()
        nm.active = ""
        box.tick()
        ok(nm.active == vp.SETUP_NAME, "after a Wi-Fi reset the OPEN setup hotspot is back")

        road_test_2026_10_14(tempfile.mkdtemp())
        powersave_phase0(tempfile.mkdtemp())

    out = log.getvalue()
    for secret in ("homepass1", "phonepass", "pocketpass", "workpass1"):
        ok(secret not in out, "the log never contains the password %s…" % secret[:3])
    state = json.load(open(os.path.join(tmp, "state.json")))
    ok("psk" not in json.dumps(state) and "homepass1" not in json.dumps(state), "the state file has no passwords")

    # ── validation + the inbox file ──
    bad = [
        ({"networks": [], "ap": {"ssid": "P", "psk": ""}}, "open fallback hotspot"),
        ({"networks": [], "ap": {"ssid": "VibeServer", "psk": "12345678"}}, "hotspot named VibeServer"),
        ({"networks": [{"ssid": str(i), "psk": "12345678"} for i in range(4)], "ap": {"ssid": "P", "psk": "12345678"}}, "four networks"),
        ({"networks": [{"ssid": "x" * 33, "psk": "12345678"}], "ap": {"ssid": "P", "psk": "12345678"}}, "33-byte SSID"),
        ({"networks": [{"ssid": "a\nb", "psk": "12345678"}], "ap": {"ssid": "P", "psk": "12345678"}}, "control char in SSID"),
        ({"networks": [{"ssid": "A", "psk": "short"}], "ap": {"ssid": "P", "psk": "12345678"}}, "7-char password"),
        ({"networks": [], "ap": {"ssid": "P", "psk": "12345678"}, "country": "gb"}, "lower-case country"),
    ]
    for doc, what in bad:
        r, err = vp.validate_request(doc)
        ok(r is None, "root refuses: " + what)
    r, err = vp.validate_request({"networks": [{"ssid": "\"; reboot #", "psk": "12345678"}],
                                  "ap": {"ssid": "P", "psk": "12345678"}})
    ok(r is not None and r["networks"][0]["ssid"] == "\"; reboot #", "a shell-looking SSID is just a name (argv, no shell)")

    me = os.getuid()
    p = os.path.join(tmp, "req")
    with open(p, "w") as f:
        f.write("{}")
    ok(vp.read_request_file(p, me) == b"{}" and not os.path.exists(p), "a plain file from the right user is read, then deleted")
    with open(p, "w") as f:
        f.write("{}")
    ok(vp.read_request_file(p, me + 1) is None and not os.path.exists(p), "the wrong owner: refused (and removed)")
    target = os.path.join(tmp, "secret")
    with open(target, "w") as f:
        f.write("root-only")
    os.symlink(target, p)
    ok(vp.read_request_file(p, me) is None and os.path.exists(target) and not os.path.lexists(p),
       "a symlink is never followed: removed, its target untouched")
    with open(p, "w") as f:
        f.write("x" * 20000)
    ok(vp.read_request_file(p, me) is None, "an oversized request is refused")

    # ── the tunnel's gate: NetworkManager's connectivity, by event ──
    class FakeOut:
        def __init__(self, chunks): self.chunks = list(chunks)
        def read(self): return self.chunks.pop(0) if self.chunks else None
        def fileno(self): return 0

    class FakeProc:
        def __init__(self, chunks): self.stdout = FakeOut(chunks)
        def poll(self): return None
    nmw = FakeNM()
    with redirect_stdout(io.StringIO()):
        w = vp.NetWatch(nmw)
        w.proc = FakeProc([b"wlan0: connected\nConnectivity is now 'lim", b"ited'\nConnectivity is now 'full'\n"])
        first = w.poll()
        st = w.poll()
    ok(first == "unknown" and st == "full", "a 'Connectivity is now' line split across reads waits for the whole line, then → full")
    st_doc = json.load(open(os.path.join(tmp, "state.json")))
    ok(st_doc.get("internet") == "none", "on a hotspot the state says internet: none — the tunnel never starts there")

    # ── The setup name: VibeServerSetup.local until configured, then retired (Stuart, 2026-10-10) ──
    with tempfile.TemporaryDirectory() as tmp:
        cfgp = os.path.join(tmp, "config.json")
        ok(vp.server_configured(cfgp) is False, "no server config yet → not configured (the setup name)")
        with open(cfgp, "w") as f: f.write('{"configured": false}')
        ok(vp.server_configured(cfgp) is False, "configured: false → the setup name")
        with open(cfgp, "w") as f: f.write('{"configured": true, "name": "x"}')
        ok(vp.server_configured(cfgp) is True, "configured: true → the run name")
        with open(cfgp, "w") as f: f.write('{"configured": tr')
        ok(vp.server_configured(cfgp) is False, "a torn config reads as not configured — never a crash")
        vp.HOSTNAME_FILE = os.path.join(tmp, "hostname"); vp.HOSTS_FILE = os.path.join(tmp, "hosts")
        with open(vp.HOSTNAME_FILE, "w") as f: f.write("vibepocket\n")
        with open(vp.HOSTS_FILE, "w") as f: f.write("127.0.0.1\tlocalhost\n127.0.1.1\tvibepocket\n")
        nm, box = make(tmp)
        calls = []
        real_run = nm.run
        def spy(args, timeout=40, secret=False):
            calls.append(list(args))
            # ★★ (rc, out) — the REAL Shell.run contract. This fake returned a bare 0, which hid the v8 bug that
            #    compared the pair with 0 and skipped the avahi restart (vibeserversetup.local never retired).
            if args[:2] == ["hostnamectl", "set-hostname"]:
                with open(vp.HOSTNAME_FILE, "w") as f: f.write(args[2] + "\n")
                return 0, ""
            if args[:2] == ["systemctl", "try-restart"]:
                return 0, ""
            return real_run(args, timeout, secret)
        box.sh.run = spy
        box.sync_hostname(False)
        ok(["hostnamectl", "set-hostname", "vibeserversetup"] in calls, "unconfigured → hostname vibeserversetup")
        ok(["systemctl", "try-restart", "avahi-daemon.service"] in calls, "…and avahi republishes it at once")
        ok("127.0.1.1\tvibeserversetup" in open(vp.HOSTS_FILE).read(), "…/etc/hosts follows (no 'unable to resolve host')")
        calls.clear(); box.sync_hostname(False)
        ok(not calls, "nothing to do → nothing run (it is called every few seconds)")
        box.sync_hostname(True)
        ok(["hostnamectl", "set-hostname", "vibepocket"] not in calls,
           "set up JUST NOW: the setup name stays (the page that pressed Save is still talking to it)")
        nm.t += 200
        box.sync_hostname(True)
        ok(["hostnamectl", "set-hostname", "vibepocket"] in calls, "…retired 3 minutes later (vibepocket)")
        ok(calls[-1] == ["systemctl", "try-restart", "avahi-daemon.service"],
           "…and avahi is restarted so vibepocket.local answers and vibeserversetup.local goes")
        nm2, box2 = make(tmp)
        calls.clear(); box2.sh.run = spy
        with open(vp.HOSTNAME_FILE, "w") as f: f.write("vibeserversetup\n")
        box2.sync_hostname(True)
        ok(["hostnamectl", "set-hostname", "vibepocket"] in calls, "a box that BOOTS already set up switches at once")

    # ── REORDER WHILE CONNECTED (Stuart's Pi 3 A+, 2026-10-10): phone hotspot moved to 1 while on Home, phone in range ──
    with tempfile.TemporaryDirectory() as tmp:
        nm, box = make(tmp)
        nm.air = {"Home": (70, "homepass1"), "Stuart iPhone": (70, "phonepass")}
        box.apply(req([("Home", "homepass1", {}), ("Stuart iPhone", "phonepass", {"hidden": True})]))
        box.leave_hotspot_and_try("test: setup finished")
        run_for(nm, box, 20)
        ok(nm.active == "vibe-net-1" and nm.conns["vibe-net-1"]["ssid"] == "Home", "on Home, which is network 1")
        nm.calls.clear()
        box.apply(vp.validate_request({"networks": [{"ssid": "Stuart iPhone", "keep": True}, {"ssid": "Home", "keep": True}],
                                       "ap": {"ssid": "Pocket-SDR", "keep": True}, "country": "GB"})[0])
        run_for(nm, box, 20)
        ok([nm.conns["vibe-net-%d" % i]["ssid"] for i in (1, 2)] == ["Stuart iPhone", "Home"], "the new order is saved: phone 1, Home 2")
        rejoined = any(c[:4] == ["nmcli", "connection", "down", "vibe-net-1"] for c in nm.calls) and \
                   any(c[3:6] == ["connection", "up", "vibe-net-1"] for c in nm.calls)
        ok(rejoined, "…and the box REJOINS in the new order (it was left physically on Home under the phone's slot)")
        ok(nm.conns["vibe-net-1"]["psk"] == "phonepass" and nm.conns["vibe-net-2"]["psk"] == "homepass1",
           "…each network keeps its own password through the swap")
        st = json.load(open(os.path.join(tmp, "state.json")))
        ok([x["ssid"] for x in st["saved"]] == ["Stuart iPhone", "Home"], "the state reports the new order")

    # ── THE STATUS TELLS THE TRUTH, AND A MISMATCH IS CORRECTED (Stuart: "its a lie, that IP address is home network") ──
    with tempfile.TemporaryDirectory() as tmp:
        nm, box = make(tmp)
        nm.air = {"Home": (70, "homepass1"), "Stuart iPhone": (70, "phonepass")}
        box.apply(req([("Home", "homepass1", {}), ("Stuart iPhone", "phonepass", {"hidden": True})]))
        box.leave_hotspot_and_try("test: setup finished")
        run_for(nm, box, 10)
        # What the OLD image left behind: slot 1 rewritten to the phone, the radio still on Home.
        nm.conns["vibe-net-1"], nm.conns["vibe-net-2"] = nm.conns["vibe-net-2"], nm.conns["vibe-net-1"]
        box.invalidate()
        box.link_info()
        box.mode = "client"; box.write_state()
        ok(json.load(open(os.path.join(tmp, "state.json")))["ssid"] == "Home",
           "the status names the network the RADIO is on (Home), not the slot's new name")
        run_for(nm, box, 30)
        ok(nm.assoc == "Stuart iPhone" and nm.active == "vibe-net-1", "…and the mismatch is corrected: it rejoins in order — the phone")
        ok(json.load(open(os.path.join(tmp, "state.json")))["ssid"] == "Stuart iPhone", "…and then says so")

    # ── OWN WI-FI BY HAND (Stuart, 2026-10-10): held while the radio is in use, released after 30 quiet minutes ──
    with tempfile.TemporaryDirectory() as tmp:
        nm, box = make(tmp)
        vp.ACTIVE_FILE = os.path.join(tmp, "pocket-active")
        nm.air = {"Home": (70, "homepass1")}
        box.apply(req([("Home", "homepass1", {})]))
        box.leave_hotspot_and_try("test: setup finished")
        run_for(nm, box, 30)
        ok(nm.active == "vibe-net-1" and box.mode == "client", "on the home network")
        ok(box.link and box.link["signal"] == -60 and box.link["tx"] == 65.0, "the link's signal and tx rate are read (iw link)")
        nm.txpk, nm.txrt = 1100, 30            # 100 sent, 20 more retries
        run_for(nm, box, 5)
        ok(box.link["retry"] == 17, "…and the share of retried transmissions since the last reading (20 of 120 = 17 %)")
        st = json.load(open(os.path.join(tmp, "state.json")))
        ok(st.get("link", {}).get("signal") == -60 and st.get("hold") is False, "the state carries the link and hold: false")

        box.own_wifi()
        ok(nm.active == vp.AP_NAME and box.hold_own, "Own Wi-Fi: the box leaves Home for its own secured hotspot, held")
        nm.stations = 0
        open(vp.ACTIVE_FILE, "w").close()                     # the radio is in use
        run_for(nm, box, 900)
        ok(nm.active == vp.AP_NAME, "…and stays there with Home in range and nobody on it — it is held, not idle-closed")
        st = json.load(open(os.path.join(tmp, "state.json")))
        ok(st.get("hold") is True and st.get("link") is None, "the state says hold: true (no link while a hotspot)")

        box.hold_at = time.time() - 2000
        os.utime(vp.ACTIVE_FILE, (time.time() - 60, time.time() - 60))
        run_for(nm, box, 10)
        ok(nm.active == vp.AP_NAME, "used a minute ago: still held, however long ago it was chosen")
        os.utime(vp.ACTIVE_FILE, (time.time() - 1900, time.time() - 1900))
        nm.ap_force = True
        run_for(nm, box, 30)
        ok(nm.active == "vibe-net-1" and not box.hold_own, "30 min unused, Home in range: back to Home, hold released")

        box.own_wifi(); run_for(nm, box, 10)
        box.network_again("the owner chose Network")
        ok(nm.active == "vibe-net-1" and not box.hold_own, "Network: straight back to the saved network")

        box.own_wifi()
        nm2, box2 = make(tmp)                                  # ★ the power is pulled: a new service, nothing held
        nm2.conns = json.loads(json.dumps(nm.conns)); nm2.air = dict(nm.air)
        run_for(nm2, box2, 120)
        ok(nm2.active == "vibe-net-1" and not box2.hold_own, "after a power cycle it comes back on the main Wi-Fi")

        nm3, box3 = make(tmp)
        nm3.air = {"Home": (70, "homepass1")}
        nm3.conns = {k: v for k, v in json.loads(json.dumps(nm.conns)).items() if k != vp.AP_NAME}
        run_for(nm3, box3, 60)
        box3.own_wifi()
        ok(nm3.active == "vibe-net-1" and not box3.hold_own and "hotspot" in box3.last_error,
           "with no hotspot of its own set up, Own Wi-Fi is refused and says why")

    # ── ★★★ ETHERNET AS A SECOND WAY IN (Stuart, 2026-10-13): "if a user installs it on a wired only pi such as the Pi2
    #    the MDNS for vibeserversetup.local:48000 must be live the second it detects a network, and the user must have
    #    the ability to hide the wifi option when not in use" — and "we are keeping the setup as it stands now". ──
    with tempfile.TemporaryDirectory() as tmp, redirect_stdout(io.StringIO()):
        vp.HOSTNAME_FILE = os.path.join(tmp, "hostname"); vp.HOSTS_FILE = os.path.join(tmp, "hosts")
        vp.STATE_CONF = os.path.join(tmp, "pocket.conf")
        vp.DNSMASQ_CONF = os.path.join(tmp, "dnsmasq", "captive.conf")
        vp.AVAHI_CONF = os.path.join(tmp, "avahi-daemon.conf")
        vp.ACTIVE_FILE = os.path.join(tmp, "pocket-active")
        vp.RESET_FILES = ()

        def hostfiles(name):
            with open(vp.HOSTNAME_FILE, "w") as f: f.write(name + "\n")
            with open(vp.HOSTS_FILE, "w") as f: f.write("127.0.0.1\tlocalhost\n127.0.1.1\t%s\n" % name)

        def renamer(nm):
            real = nm.run
            def run(args, timeout=40, secret=False):
                if args[:2] == ["hostnamectl", "set-hostname"]:
                    nm.calls.append(list(args))
                    with open(vp.HOSTNAME_FILE, "w") as f: f.write(args[2] + "\n")
                    return 0, ""
                return real(args, timeout, secret)
            nm.run = run

        def state_of(box):
            box.write_state()
            return json.load(open(box.state_path))

        def fresh(wifi="wlan0", eth="connected", conf=None):
            nm = FakeNM()
            nm.wifi_dev, nm.eth = wifi, eth
            c = dict(vp.DEFAULTS); c.update(conf or {})
            box = vp.Box(nm, c, state_path=os.path.join(tmp, "state-%d.json" % id(nm)))
            nm.state_path = box.state_path
            renamer(nm)
            return nm, box

        def joins(nm):
            return sum(1 for c in nm.calls if c[3:5] == ["connection", "up"])

        # ── a Pi 2: no Wi-Fi chip at all ──
        hostfiles("vibepocket")                    # (a box whose name is not the setup name yet)
        nm, box = fresh(wifi=None, eth="connected")
        vp.boot(box, configured=False)
        ok(["iw", "reg", "set", "GB"] in nm.calls,
           "the Wi-Fi country is applied at every boot (setup's choice is on /data; /boot is read-only)")
        ok(["hostnamectl", "set-hostname", "vibeserversetup"] in nm.calls and box.dev is None,
           "Pi 2 (no Wi-Fi chip): the setup name is set at once — nothing waits for, or gives up on, a Wi-Fi device")
        i_name = nm.calls.index(["hostnamectl", "set-hostname", "vibeserversetup"])
        i_dev = next(i for i, c in enumerate(nm.calls) if c[:2] == ["nmcli", "-t"] and c[-1] == "device")
        ok(i_name < i_dev, "…and the name comes BEFORE the device list is even read")
        run_for(nm, box, 600)
        st = state_of(box)
        ok(box.mode == "wired" and st["mode"] == "wired" and st["hasWifi"] is False and st["wired"] is True
           and st["wiredIp"] == "192.168.1.77",
           "…keeps running as a WIRED box: mode wired, hasWifi false, wired true, its Ethernet address in the state")
        ok(joins(nm) == 0 and not any(c[:3] == ["nmcli", "connection", "add"] for c in nm.calls),
           "…and never tries to raise a hotspot or join anything")
        box.internet = "full"
        ok(state_of(box)["internet"] == "full", "…its internet (over Ethernet) reaches the server, so the tunnel can run")
        nm.eth = "unavailable"                    # the cable pulled
        run_for(nm, box, 10)
        ok(state_of(box)["wired"] is False and box.mode == "wired", "cable pulled: wired false, still running")
        nm.wifi_dev = "wlan1"                     # ★ a USB Wi-Fi dongle plugged in
        nm.eth = "connected"
        run_for(nm, box, int(vp.DEFAULTS["BOOT_GRACE_SEC"]) + 90)
        ok(box.dev == "wlan1" and nm.active == vp.SETUP_NAME and box.mode == "setup-ap",
           "a USB Wi-Fi dongle appears: managed from then on (nothing saved → the setup hotspot, as ever)")

        # ── a Pi 3 wired in, its Wi-Fi chip present, nothing saved: SETUP STAYS AS IT IS (Stuart, 2026-10-13) ──
        nm, box = fresh(eth="connected")
        box.dev = "wlan0"
        run_for(nm, box, 30)
        st = state_of(box)
        ok(nm.active == vp.SETUP_NAME and st["mode"] == "setup-ap" and st["wired"] is True and st["hasWifi"] is True,
           "Pi 3 on Ethernet with nothing saved: the open setup hotspot still comes up (unchanged) — and the state says wired")

        # ── Wi-Fi off, refused without a wired link ──
        nm, box = fresh(eth=None)
        box.dev = "wlan0"
        run_for(nm, box, 30)
        ok(box.wifi_switch(True) is False and "wired" in box.last_error and nm.radio and nm.active == vp.SETUP_NAME,
           "Wi-Fi off with NO wired link: refused, says why, the hotspot stays")
        nm.eth = "disconnected"
        ok(box.wifi_switch(True) is False and nm.radio, "…an Ethernet port with no link is not a wired link either")
        ok(not os.path.exists(vp.STATE_CONF) or "WIFI_OFF=1" not in open(vp.STATE_CONF).read(), "…and nothing is saved")

        # ── Wi-Fi off with a wired link ──
        nm.eth = "connected"
        ok(box.wifi_switch(True) is True and ["nmcli", "radio", "wifi", "off"] in nm.calls and not nm.radio,
           "Wi-Fi off on a wired box: the radio is switched off")
        ok(nm.active == "" and box.mode == "wifi-off", "…the setup hotspot is gone; mode wifi-off")
        ok("WIFI_OFF=1" in open(vp.STATE_CONF).read(), "…and the choice is saved (it survives a power cycle)")
        ups = joins(nm)
        run_for(nm, box, 3600)
        st = state_of(box)
        ok(joins(nm) == ups and nm.active == "" and not nm.radio,
           "…an hour on, nothing saved: NO hotspot raised, nothing joined, the radio still off")
        ok(st["mode"] == "wifi-off" and st["wifiOff"] is True and st["wired"] is True, "…and the state says so")
        ok(box.wifi_switch(False) is True and nm.radio and "WIFI_OFF=0" in open(vp.STATE_CONF).read(),
           "Wi-Fi on: the radio comes back and the setting is saved")
        run_for(nm, box, int(vp.DEFAULTS["BOOT_GRACE_SEC"]) + 90)
        ok(nm.active == vp.SETUP_NAME and box.mode == "setup-ap", "…and ordinary management resumes (nothing saved → setup hotspot)")

        # ── Wi-Fi off with a saved network: a power cycle keeps it off; switching on rejoins in order ──
        nm, box = fresh(eth="connected", conf={"WIFI_OFF": "1"})
        box.dev = "wlan0"
        nm.conns["vibe-net-1"] = {"type": "802-11-wireless", "ssid": "Home", "psk": "homepass1", "prio": 30,
                                  "hidden": False, "mode": "infrastructure"}
        nm.air = {"Home": (60, "homepass1")}
        vp.setup_once(box)
        ok(["nmcli", "radio", "wifi", "on"] not in nm.calls, "boot with Wi-Fi off: the start-up does not switch the radio on")
        run_for(nm, box, 600)
        ok(not nm.radio and nm.active == "" and box.mode == "wifi-off", "…10 min on, wired: the radio stays off, Home is not joined")
        box.wifi_switch(False)
        run_for(nm, box, int(vp.DEFAULTS["BOOT_GRACE_SEC"]) + 90)
        ok(nm.active == "vibe-net-1" and box.mode == "client", "switched on: joins Home (network 1)")

        # ── ★★★ OFF MEANS OFF (Stuart, 2026-10-13: "disable wifi fully with the warning that if ethernet ever disconnects
        #    they will loose access until it is reconnected") — no automatic switch-back ──
        ok("WIFI_OFF_SAFETY_SEC" not in vp.DEFAULTS, "there is no automatic switch-back timer any more")
        nm, box = fresh(eth="connected", conf={"WIFI_OFF": "1"})
        box.dev = "wlan0"
        run_for(nm, box, 60)
        nm.eth = "unavailable"                    # the cable is pulled
        run_for(nm, box, 3600)
        ok(not nm.radio and box.wifi_off and box.mode == "wifi-off" and nm.active == "" and joins(nm) == 0,
           "Wi-Fi off and the cable pulled: an hour on, Wi-Fi is STILL off — no radio, no hotspot, nothing joined")
        ok(box.conf.get("WIFI_OFF") == "1" and not any(c[:4] == ["nmcli", "radio", "wifi", "on"] for c in nm.calls),
           "…the owner's choice is untouched by a lost cable, and the radio is never switched on")
        st = state_of(box)
        ok(st["wifiOff"] is True and st["wired"] is False, "…and the state says so: wifiOff true, wired false")
        nm.eth = "connected"                      # the cable is back
        run_for(nm, box, 30)
        ok(not nm.radio and state_of(box)["wired"] is True, "cable back: reachable on it again, Wi-Fi still off")
        nm, box = fresh(eth="disconnected", conf={"WIFI_OFF": "1"})
        box.dev = "wlan0"
        run_for(nm, box, 3600)
        ok(not nm.radio and box.wifi_off, "booted with Wi-Fi off and no cable at all: Wi-Fi stays off (the owner was warned)")

        # ── ★★★ NO HOME NETWORK REQUIRED (Stuart, 2026-10-13: "make sure that a captive hotspot is setup whenever wifi is
        #    present, but dont force a connection to a home network") ──
        nm, box = fresh(eth=None)
        box.dev = "wlan0"
        nm.air = {"Neighbour": (80, "zzzzzzzz")}
        run_for(nm, box, 30)
        ok(nm.active == vp.SETUP_NAME, "(first boot: the open setup hotspot)")
        box.apply(req([], switch=True))
        box.leave_hotspot_and_try("the owner asked to switch now")
        run_for(nm, box, 30)
        ok(nm.active == vp.AP_NAME and box.mode == "fallback-ap" and nm.conns[vp.AP_NAME]["psk"] == "pocketpass",
           "set up with ZERO home networks and only its own hotspot: the box runs its SECURED hotspot")
        ok(box.last_error == "", "…and reports no problem (nothing was asked of it that it could not do)")
        downs0 = sum(1 for c in nm.calls if c[:3] == ["nmcli", "connection", "down"])
        run_for(nm, box, 3 * 3600)
        ok(nm.active == vp.AP_NAME and sum(1 for c in nm.calls if c[:3] == ["nmcli", "connection", "down"]) == downs0,
           "…3 h on, nobody connected: still its own hotspot, never dropped to look for a home network")
        nm2, box2 = fresh(eth=None)
        box2.dev = "wlan0"
        nm2.conns = json.loads(json.dumps(nm.conns))
        run_for(nm2, box2, 120)
        ok(nm2.active == vp.AP_NAME and "vibe-net-1" not in nm2.conns, "…and after a power cycle: its own secured hotspot again, never the open one")

        # ── an Ethernet port NetworkManager does not manage still counts, if it has an address ──
        nm, box = fresh(eth="unmanaged")
        real = nm.run
        nm.run = lambda a, timeout=40, secret=False: \
            (0, "2: eth0    inet 192.168.1.88/24 brd 192.168.1.255 scope global eth0\n") if a[:2] == ["ip", "-4"] \
            else real(a, timeout, secret)
        box.refresh_links()
        ok(box.wired and box.wired_ip == "192.168.1.88", "an unmanaged Ethernet port with an address is a wired link")

        # ── avahi must answer on EVERY interface ──
        with open(vp.AVAHI_CONF, "w") as f:
            f.write("[server]\nuse-ipv4=yes\n#allow-interfaces=eth0\n[publish]\n")
        ok(vp.avahi_limits() == [], "avahi's stock config (allow-interfaces commented out): no limit reported")
        with open(vp.AVAHI_CONF, "w") as f:
            f.write("[server]\nallow-interfaces=wlan0\n[reflector]\n")
        ok(vp.avahi_limits() == ["allow-interfaces=wlan0"], "an allow-interfaces=wlan0 line is reported (Ethernet would be deaf)")

    # ── THE CPU GOVERNOR FOLLOWS THE RADIO (battery box, Stuart 2026-10-10) ──
    with tempfile.TemporaryDirectory() as tmp:
        vp.CPUFREQ_DIR = os.path.join(tmp, "cpu")
        for c in range(4):
            d = os.path.join(vp.CPUFREQ_DIR, "cpu%d" % c, "cpufreq"); os.makedirs(d)
            open(os.path.join(d, "scaling_governor"), "w").write("performance")
            open(os.path.join(d, "scaling_available_governors"), "w").write("conservative ondemand userspace powersave performance schedutil\n")
        os.makedirs(os.path.join(vp.CPUFREQ_DIR, "cpufreq"))      # not a cpuN — ignored
        vp.ACTIVE_FILE = os.path.join(tmp, "pocket-active")
        vp.SERVER_CONF = os.path.join(tmp, "config.json")
        open(vp.SERVER_CONF, "w").write('{"configured": true}')
        gov_of = lambda: {open(os.path.join(vp.CPUFREQ_DIR, "cpu%d" % c, "cpufreq", "scaling_governor")).read() for c in range(4)}
        g = vp.GovernorFollower()
        open(vp.ACTIVE_FILE, "w").close()
        g.tick()
        ok(gov_of() == {"performance"}, "a listener just now: performance on every core")
        os.utime(vp.ACTIVE_FILE, (time.time() - 90, time.time() - 90))
        g.tick()
        ok(gov_of() == {"ondemand"}, "nobody for a minute and a half: ondemand (the CPU may idle down)")
        os.utime(vp.ACTIVE_FILE, None)
        g.tick()
        ok(gov_of() == {"performance"}, "a listener again: straight back to performance")
        os.remove(vp.ACTIVE_FILE)
        g.tick()
        ok(gov_of() == {"ondemand"}, "never used since boot: idles")
        open(vp.SERVER_CONF, "w").write('{"configured": true, "cpuGovernor": "schedutil"}')
        for c in range(4): open(os.path.join(vp.CPUFREQ_DIR, "cpu%d" % c, "cpufreq", "scaling_governor"), "w").write("schedutil")
        open(vp.ACTIVE_FILE, "w").close()
        g.tick()
        ok(gov_of() == {"schedutil"}, "a governor the OWNER chose in setup is never touched")

    # ── unused hardware off (power + EMI) ──
    nm = FakeNM()
    vp.quiet_hardware(nm, which=lambda _: "/usr/bin/x")
    ok(["vcgencmd", "display_power", "0"] in nm.calls, "the display output is powered down (HDMI / composite)")
    ok(["rfkill", "block", "bluetooth"] in nm.calls, "Bluetooth is blocked")
    nm = FakeNM()
    vp.quiet_hardware(nm, which=lambda _: None)
    ok(nm.calls == [], "…and nothing is run where the tools are missing (not a Pi)")

    # ── the start-time converge check: report only, never a crash ──
    class R:
        def __init__(self, rc, out):
            self.returncode, self.stdout = rc, out
    with tempfile.TemporaryDirectory() as tmp:
        fake = os.path.join(tmp, "converge")
        open(fake, "w").close()
        seen = []
        with redirect_stdout(io.StringIO()):
            got = vp.converge_report(run=lambda a, **k: (seen.append(a), R(1, b"vibeserver-pocket-converge: !! system packages missing (Update installs them): ffmpeg\n"))[1], path=fake)
        ok(seen == [[fake, "--check"]], "converge check at start: --check only (never apply on the live overlay)")
        ok(len(got) == 1 and "ffmpeg" in got[0], "…and what is not converged is logged")
        with redirect_stdout(io.StringIO()):
            got = vp.converge_report(run=lambda a, **k: R(0, b"vibeserver-pocket-converge: converged\n"), path=fake)
        ok(got == [], "a converged box logs nothing")
        def boom(a, **k):
            raise OSError("no")
        with redirect_stdout(io.StringIO()):
            got = vp.converge_report(run=boom, path=fake)
        ok(got == [], "a check that cannot run is a log line, not a crash")
        ok(vp.converge_report(path=os.path.join(tmp, "absent")) == [], "no converge script (an older package): nothing")

    # ★★★ THE REAL Shell, not the fake: nmcli must be run in a UTF-8 locale, or glib prints "Stuart?s iPhone".
    sh = vp.Shell()
    rc, out = sh.run(["sh", "-c", 'printf "%s|%s|" "$LC_ALL" "$LANG"; printf "\\342\\200\\231"'])
    ok(rc == 0 and out == "C.UTF-8|C.UTF-8|’",
       "tools run with LC_ALL=C.UTF-8 and their UTF-8 output is read as UTF-8 (got %r)" % out)
    with redirect_stdout(io.StringIO()):
        rc, _ = sh.run(["sh", "-c", "echo 'Error: Connection activation failed: (7) Secrets were required, but not provided.' >&2; exit 4"])
    ok(rc == 4 and vp.nm_reason(sh.last_stderr) == "Secrets were required, but not provided.",
       "a failed command's reason is kept (stderr's last line), and NM's '(7)' code stripped")
    ok(vp.ssid_key("Stuart’s  iPhone ") == vp.ssid_key("stuart's iphone") and
       vp.ssid_key("A–B") == vp.ssid_key("a-b") and vp.ssid_key("Café") == vp.ssid_key("Café"),
       "the name key: apostrophes, dashes, spaces, capitals and Unicode composition all fold")

    ok(vp.terse_split(r"My\:Net\\x:70:WPA2") == ["My:Net\\x", "70", "WPA2"], "nmcli terse escapes are undone")
    ok(vp.iw_unescape(r"Caf\xc3\xa9") == "Café", "iw's \\x escapes are decoded")

    print()
    if fails:
        print("\033[31m%d failed\033[0m" % fails)
        return 1
    print("\033[32mall passed\033[0m")
    return 0


if __name__ == "__main__":
    sys.exit(main())
