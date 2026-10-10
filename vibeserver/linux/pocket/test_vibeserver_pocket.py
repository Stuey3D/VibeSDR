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

    # Shell interface
    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s

    def run_input(self, args, data, timeout=20):
        self.nft.append(data)
        return 0

    def run(self, args, timeout=40, secret=False):
        self.calls.append(list(args))
        a = args
        if a[:2] == ["nmcli", "-t"] and a[4:] == ["device"]:
            return 0, "wlan0:wifi\nlo:loopback\n"
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
                 "802-11-wireless-security.key-mgmt": "wpa-psk" if c["psk"] else ""}.get(f, "")
            return 0, esc(v) + "\n"
        if a[:2] == ["nmcli", "-t"] and a[4:6] == ["device", "show"]:
            st = 100 if self.active else 30
            return 0, "GENERAL.STATE:%d (x)\nGENERAL.CONNECTION:%s\n" % (st, esc(self.active) if self.active else "--")
        if a[:2] == ["nmcli", "-g"] and a[2] == "IP4.ADDRESS":
            return 0, ("10.42.0.1/24\n" if self.is_ap() else "192.168.1.50/24\n") if self.active else "\n"
        if a[:3] == ["nmcli", "-t", "-f"] and "wifi" in a and "list" in a:
            if self.is_ap():
                return 1, ""       # ★ NM: "Scanning not allowed while in AP mode"
            return 0, "".join("%s:%d:WPA2\n" % (esc(s), sig) for s, (sig, _) in self.air.items())
        if a[:2] == ["iw", "dev"] and a[3:] == ["scan", "ap-force"]:
            if not self.ap_force:
                return 1, ""
            return 0, "".join("BSS x\n\tSSID: %s\n" % s for s in self.air)
        if a[:2] == ["iw", "dev"] and a[3:] == ["station", "dump"]:
            if self.active and not self.is_ap():     # as a client: the one station is the router, with counters
                return 0, "Station e4:5e:1b:00:00:01 (on wlan0)\n\ttx packets:\t%d\n\ttx retries:\t%d\n" % (self.txpk, self.txrt)
            return 0, "".join("Station aa:bb:cc:00:00:%02d (on wlan0)\n" % i for i in range(self.stations))
        if a[:2] == ["iw", "dev"] and a[3:] == ["link"]:
            if not self.active or self.is_ap():
                return 0, "Not connected.\n"
            return 0, ("Connected to e4:5e:1b:00:00:01 (on wlan0)\n\tSSID: %s\n\tsignal: %d dBm\n"
                       "\trx bitrate: 72.2 MBit/s MCS 7 short GI\n\ttx bitrate: %.1f MBit/s MCS 5\n") % (self.assoc, self.link_sig, self.tx_rate)
        if a[:3] == ["nmcli", "--wait", a[2]] and a[3:5] == ["connection", "up"]:
            name = a[5]
            c = self.conns.get(name)
            self.t += 5
            if not c:
                return 10, ""
            if c["mode"] == "ap":
                self.active = name
                return 0, ""
            real = self.air.get(c["ssid"])
            if real and real[1] == c["psk"]:
                self.active = name
                self.assoc = c["ssid"]
                return 0, ""
            self.active = ""
            self.t += int(a[2])
            return 4, ""
        if a[:3] == ["nmcli", "connection", "down"]:
            if self.active == a[3]:
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
                                "mode": kv.get("802-11-wireless.mode", "infrastructure")}
            return 0, ""
        if a[:3] == ["nmcli", "connection", "modify"]:
            c = self.conns[a[3]]
            kv = dict(zip(a[4::2], a[5::2]))
            self.modified.append(a[3])
            if "802-11-wireless.ssid" in kv: c["ssid"] = kv["802-11-wireless.ssid"]
            if "wifi-sec.psk" in kv: c["psk"] = kv["wifi-sec.psk"]
            if "connection.autoconnect-priority" in kv: c["prio"] = int(kv["connection.autoconnect-priority"])
            if "802-11-wireless.hidden" in kv: c["hidden"] = kv["802-11-wireless.hidden"] == "yes"
            return 0, ""
        return 0, ""

    def is_ap(self):
        return bool(self.active) and self.conns.get(self.active, {}).get("mode") == "ap"


def make(tmp):
    nm = FakeNM()
    box = vp.Box(nm, dict(vp.DEFAULTS), state_path=os.path.join(tmp, "state.json"))
    box.dev = "wlan0"
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
            if args[:2] == ["hostnamectl", "set-hostname"]:
                with open(vp.HOSTNAME_FILE, "w") as f: f.write(args[2] + "\n")
                return 0
            if args[:2] == ["systemctl", "try-restart"]:
                return 0
            return real_run(args, timeout, secret)
        box.sh.run = spy
        box.sync_hostname(False)
        ok(["hostnamectl", "set-hostname", "vibeserversetup"] in calls, "unconfigured → hostname vibeserversetup")
        ok(["systemctl", "try-restart", "avahi-daemon.service"] in calls, "…and avahi republishes it at once")
        ok("127.0.1.1\tvibeserversetup" in open(vp.HOSTS_FILE).read(), "…/etc/hosts follows (no 'unable to resolve host')")
        calls.clear(); box.sync_hostname(False)
        ok(not calls, "nothing to do → nothing run (it is called every few seconds)")
        box.sync_hostname(True)
        ok(["hostnamectl", "set-hostname", "vibepocket"] in calls, "configured → the setup name is retired (vibepocket)")

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
