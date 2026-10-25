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
        # ★★★ THE PI'S OWN CHIP (brcmfmac, FullMAC) reports "tx failed" and NO "tx retries" — which is why the retry % was
        #     null on every Portable (2026-10-20). True = the dump looks like a Pi's; txrt then counts failed frames.
        self.brcmfmac = False
        self.door = True                            # the server's front door accepts connections
        self.ap_up_seen = []                        # (redirect loaded, state mode, door open) at each hotspot up
        self.state_path = None
        self.wifi_dev = "wlan0"                     # None = no Wi-Fi chip (a Pi 2)
        self.more_wifi = []                         # ★ further radios (USB dongles), listed after wifi_dev
        self.unmanaged = set()                      # radios told `nmcli device set … managed no`
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
        # ★★★ WHAT EACH NETWORK ADVERTISES AND WHAT IT REALLY TAKES (2026-10-21, measured on the Portable). kind[ssid]:
        #     None/"wpa2"   plain WPA2-PSK, no MFP: takes wpa-psk with PMF off or optional (not required, not SAE)
        #     "wpa2-mfpc"   WPA2, MFP capable: takes any wpa-psk
        #     "pmf-required" WPA2, MFP required: refuses PMF off
        #     "transition"  WPA2/WPA3 (PSK+SAE, MFPC) — STUART'S IPHONE: refuses PMF off (instantly, 2026-10-21)
        #     "wpa3"        SAE only, MFP required: refuses PMF off
        #     "sae-broken"  ADVERTISES transition but fails whenever SAE is offered (the safety net's case)
        #     "liar-mfpc"   ADVERTISES MFP capable but refuses PMF optional (the safety net's other case)
        #     "open"        no security
        #   bss[ssid] = [(kind, signal), …] — several access points under one name (old router + new mesh): the join works
        #   if ANY of them takes the profile, as wpa_supplicant picks among the matching ones.
        #   quiet = answers only a DIRECTED probe (an idle iPhone hotspot): missing from wildcard scans, listed after
        #   `rescan ssid <it>`, joinable only by a profile that probes by name.
        self.kind, self.quiet, self.freq, self.bss = {}, set(), {}, {}
        self.iw_dump_ok = True                      # `iw dev … scan dump` answers
        self.probed = []                            # every ssid a directed probe asked for
        self.attempts = []                          # (ssid, key-mgmt, pmf) of every station join tried
        self._probe_hits = set()

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
            for d in self.more_wifi:
                out += "%s:wifi:%s\n" % (d, "unmanaged" if d in self.unmanaged else "disconnected")
            return 0, out + "lo:loopback:unmanaged\n"
        if a[:3] == ["nmcli", "device", "set"] and a[4] == "managed":
            (self.unmanaged.add if a[5] == "no" else self.unmanaged.discard)(a[3])
            return 0, ""
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
                 "802-11-wireless-security.key-mgmt": (c.get("km") or "wpa-psk") if c["psk"] else "",
                 "802-11-wireless.powersave": c.get("ps", "0 (default)"),
                 "802-11-wireless-security.pmf": c.get("pmf", "0 (default)")}.get(f, "")
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
            listed = {k: v for k, v in self.on_air().items() if k not in self.quiet or ("no" in a and k in self._probe_hits)}
            if "yes" in a:
                self._probe_hits = set()            # a wildcard scan replaces the list
            def sec(k):
                return {"transition": "WPA2 WPA3", "sae-broken": "WPA2 WPA3", "wpa3": "WPA3", "open": ""}.get(
                    self.kinds(k)[0][0], "WPA2")
            return 0, "".join("%s:%d:%s:%d MHz\n" % (esc(k), sig, sec(k), self.freq.get(k, 2437))
                              for k, (sig, _) in listed.items())
        if a[:4] == ["nmcli", "device", "wifi", "rescan"]:
            if self.is_ap():
                return 1, ""
            asked = [a[i + 1] for i in range(len(a) - 1) if a[i] == "ssid"]
            self.probed += asked
            self._probe_hits |= {x for x in asked if x in self.on_air()}
            return 0, ""
        if a[:2] == ["iw", "dev"] and a[3:] == ["scan", "dump"]:
            if not self.iw_dump_ok:
                return 1, ""
            if self.is_ap():
                return 0, ""
            listed = [k for k in self.on_air() if k not in self.quiet or k in self._probe_hits]
            out, n = "", 0
            for k in listed:
                for kind, sig in self.kinds(k):
                    n += 1
                    out += iw_bss("02:00:00:00:%02x:%02x" % (n // 256, n % 256), k, kind, sig, self.freq.get(k, 2437))
            return 0, out
        if a[:2] == ["iw", "dev"] and a[3:] == ["scan", "ap-force"]:
            if not self.ap_force:
                return 1, ""
            return 0, "".join("BSS x\n\tSSID: %s\n" % s for s in self.on_air())
        if a[:2] == ["iw", "dev"] and a[3:] == ["station", "dump"]:
            if self.active and not self.is_ap():     # as a client: the one station is the router, with counters
                if self.brcmfmac:     # as `iw` prints it on a Pi 3A+/Zero 2 W/4/5: tx failed, no tx retries line
                    return 0, ("Station e4:5e:1b:00:00:01 (on wlan0)\n\tinactive time:\t30 ms\n\trx bytes:\t912345\n"
                               "\trx packets:\t4521\n\ttx bytes:\t8812345\n\ttx packets:\t%d\n\ttx failed:\t%d\n"
                               "\tsignal:  \t-42 dBm\n\ttx bitrate:\t433.3 MBit/s\n" % (self.txpk, self.txrt))
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
            pmf = (c.get("pmf") or "0")[0]
            km = c.get("km") or "wpa-psk"
            if real and c["ssid"] in self.quiet and not c["hidden"]:
                real = None                         # ★ a wildcard look never finds it; only a by-name profile does
            self.attempts.append((c["ssid"], km, pmf))
            if real and c["psk"] and not any(takes(kind, km, pmf) for kind, _ in self.kinds(c["ssid"])):
                self.active = ""
                self.t += int(a[2])
                self.last_stderr = "Error: Connection activation failed: (7) Secrets were required, but not provided."
                return 4, ""
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
                                "ps": {"2": "2 (disable)"}.get(kv.get("802-11-wireless.powersave"), "0 (default)"),
                                "pmf": {"1": "1 (disable)", "2": "2 (optional)", "3": "3 (required)"}.get(kv.get("wifi-sec.pmf"), "0 (default)"),
                                "km": kv.get("wifi-sec.key-mgmt", "")}
            return 0, ""
        if a[:3] == ["nmcli", "connection", "modify"]:
            c = self.conns[a[3]]
            kv = dict(zip(a[4::2], a[5::2]))
            self.modified.append(a[3])
            if "802-11-wireless.ssid" in kv: c["ssid"] = kv["802-11-wireless.ssid"]
            if "wifi-sec.psk" in kv: c["psk"] = kv["wifi-sec.psk"]
            if "connection.autoconnect-priority" in kv: c["prio"] = int(kv["connection.autoconnect-priority"])
            if "802-11-wireless.hidden" in kv: c["hidden"] = kv["802-11-wireless.hidden"] == "yes"
            for kk in ("wifi-sec.key-mgmt", "802-11-wireless-security.key-mgmt"):
                if kk in kv: c["km"] = kv[kk]
            for pk in ("wifi-sec.pmf", "802-11-wireless-security.pmf"):
                if pk in kv: c["pmf"] = {"0": "0 (default)", "1": "1 (disable)", "2": "2 (optional)", "3": "3 (required)"}[kv[pk]]
            if "802-11-wireless.powersave" in kv: c["ps"] = {"2": "2 (disable)"}.get(kv["802-11-wireless.powersave"], kv["802-11-wireless.powersave"])
            return 0, ""
        return 0, ""

    def kinds(self, ssid):
        if ssid in self.bss:
            return sorted(self.bss[ssid], key=lambda x: -x[1])
        return [(self.kind.get(ssid) or "wpa2", self.air.get(ssid, (60, ""))[0])]

    def is_ap(self):
        return bool(self.active) and self.conns.get(self.active, {}).get("mode") == "ap"


def takes(kind, km, pmf):
    """Does an access point of `kind` (FakeNM.kind) accept a profile asking for key-mgmt `km`, PMF `pmf`?"""
    if kind == "open":
        return False                                # (a password profile never matches an open network)
    if kind in ("wpa2", None):
        return km == "wpa-psk" and pmf != "3"
    if kind == "wpa2-mfpc":
        return km == "wpa-psk"
    if kind in ("pmf-required", "transition", "wpa3"):
        return pmf != "1"
    if kind == "sae-broken":
        return km == "wpa-psk" and pmf == "1"
    if kind == "liar-mfpc":
        return km == "wpa-psk" and pmf == "1"
    return False


def iw_bss(bssid, ssid, kind, sig, freq):
    """One access point as `iw dev wlan0 scan dump` prints it (iw 6.x on Raspberry Pi OS bookworm)."""
    rsn = {"wpa2": ("PSK", "16-PTKSA-RC 1-GTKSA-RC (0x000c)"),
           "wpa2-mfpc": ("PSK", "16-PTKSA-RC 1-GTKSA-RC MFP-capable (0x008c)"),
           "liar-mfpc": ("PSK", "16-PTKSA-RC 1-GTKSA-RC MFP-capable (0x008c)"),
           "pmf-required": ("PSK/SHA-256", "16-PTKSA-RC 1-GTKSA-RC MFP-required MFP-capable (0x00cc)"),
           "transition": ("PSK SAE", "16-PTKSA-RC 1-GTKSA-RC MFP-capable (0x008c)"),
           "sae-broken": ("PSK SAE", "16-PTKSA-RC 1-GTKSA-RC MFP-capable (0x008c)"),
           "wpa3": ("SAE", "16-PTKSA-RC 1-GTKSA-RC MFP-required MFP-capable (0x00cc)")}.get(kind or "wpa2")
    name = "".join(c if (c.isprintable() and c.isascii() and c not in " \\") or (c == " " and 0 < i < len(ssid) - 1)
                   else "".join("\\x%02x" % b for b in c.encode("utf-8")) for i, c in enumerate(ssid))
    out = ("BSS %s(on wlan0)\n\tlast seen: 412.580s [boottime]\n\tTSF: 0 usec (0d, 00:00:00)\n\tfreq: %d.0\n"
           "\tbeacon interval: 100 TUs\n\tcapability: ESS %sShortSlotTime (0x%s)\n\tsignal: %.2f dBm\n"
           "\tlast seen: 240 ms ago\n\tInformation elements from Probe Response frame:\n\tSSID: %s\n"
           "\tSupported rates: 6.0* 9.0 12.0* 18.0 24.0* 36.0 48.0 54.0 \n"
           % (bssid, freq, "" if rsn is None else "Privacy ", "0401" if rsn is None else "0411", -100 + sig / 2.0, name))
    if rsn:
        out += ("\tRSN:\t * Version: 1\n\t\t * Group cipher: CCMP\n\t\t * Pairwise ciphers: CCMP\n"
                "\t\t * Authentication suites: %s\n\t\t * Capabilities: %s\n" % rsn)
    out += "\tExtended capabilities:\n\t\t * Extended Channel Switching\n\t\t * Operating Mode Notification\n"
    return out


def make(tmp):
    nm = FakeNM()
    box = vp.Box(nm, dict(vp.DEFAULTS), state_path=os.path.join(tmp, "state.json"))
    box.history_path = os.path.join(tmp, "join-history-%d.json" % id(box))
    box.history = []
    box.sec_path = os.path.join(tmp, "wifi-security-%d.json" % id(box))
    box.sec_memory = {}
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


def scan_lists_every_security(tmp):
    """★★ PHASE 3 (BRIEF-portable-wifi-realtime, 2026-10-20): Stuart's iPhone hotspot without Maximise Compatibility is
    5 GHz ch 36, WPA2/WPA3 Personal — and never appeared on the box. Our own list is NOT the filter: every network nmcli
    reports is listed whatever its security column says (transition mode, WPA3 only, SAE, OWE, enterprise); only an
    empty column is marked open. If it is missing, the radio never reported it."""
    nm, box = make(tmp)
    rows = {"iPhone": "WPA2 WPA3", "WPA3 only": "WPA3", "Sae": "SAE", "Owe": "OWE", "Work": "WPA2 802.1X", "Cafe": "",
            "Old": "WPA1 WPA2"}
    real = nm.run
    def run(args, timeout=40, secret=False):
        if args[:3] == ["nmcli", "-t", "-f"] and "list" in args:
            return 0, "".join("%s:%d:%s\n" % (esc(k), 50, v) for k, v in rows.items())
        return real(args, timeout, secret)
    nm.run = run
    seen = box.scan()
    ok(seen == set(rows), "the scan keeps every network whatever its security (WPA2/WPA3 transition, WPA3, SAE, OWE, 802.1X)")
    sec = {x["ssid"]: x["secure"] for x in box.scan_cache}
    ok(sec["iPhone"] and sec["WPA3 only"] and sec["Sae"] and not sec["Cafe"],
       "…and marks only the one with no security as open (the page's \"(open)\" label)")
    # the request a WPA2/WPA3 network is saved with: a PSK profile — NM's wpa-psk joins a transition AP by its PSK AKM
    box.apply(req([("iPhone", "phonepass", {})]))
    add = [c for c in nm.calls if c[:3] == ["nmcli", "connection", "add"] and "iPhone" in c]
    ok(add and add[0][add[0].index("wifi-sec.key-mgmt") + 1] == "wpa-psk" and "802-11-wireless.band" not in add[0],
       "a WPA2/WPA3 network is saved as wpa-psk, and the client profile pins NO band (5 GHz is never excluded by us)")


def kept_log(tmp):
    """★★★ THE WI-FI LOG THAT SURVIVES A REBOOT (2026-10-21: the RAM journal lost a whole afternoon's evidence). Copied in
    batches, Wi-Fi lines only, overlap cut, capped with one rotation, 0600, never raises."""
    class J:
        def __init__(self):
            self.t, self.units, self.kern, self.calls = 0.0, [], [], []
        def now(self):
            return self.t
        def run(self, args, timeout=40, secret=False):
            self.calls.append(list(args))
            if args[0] != "journalctl":
                return 0, ""
            src = self.kern if "-k" in args else self.units
            return 0, "".join(l + "\n" for l in src)
    j = J()
    path = os.path.join(tmp, "kept", "wifi-log.txt")
    wall = [1792560000.0]
    k = vp.KeptLog(j, path=path, cap=4096, wall=lambda: wall[0])
    ok(not k.due(), "not copied in the first minute (the boot's lines are copied once NetworkManager has settled)")
    j.t = 61
    j.units = ["2026-10-21T14:00:01.000001+0100 pocket NetworkManager[412]: <info> device (wlan0): Activation: starting",
               "2026-10-21T14:00:03.000001+0100 pocket vibeserver-pocket[380]: vibeserver-pocket: network \"Meow!\" advertises WPA2/WPA3"]
    j.kern = ["2026-10-21T14:00:02.000001+0100 pocket kernel: brcmfmac: brcmf_cfg80211_connect: Connecting",
              "2026-10-21T14:00:02.500000+0100 pocket kernel: usb 1-1: new high-speed USB device number 3"]
    ok(k.due() and k.flush() == 3, "the first copy: this boot's NetworkManager / supplicant / our lines + the kernel's Wi-Fi lines (3)")
    text = open(path, encoding="utf-8").read()
    ok("usb 1-1" not in text and text.index("Activation: starting") < text.index("brcmf_cfg80211_connect") < text.index("advertises"),
       "…kernel lines other than Wi-Fi are left out, and the two reads merge in time order")
    ok(oct(os.stat(path).st_mode & 0o777) == "0o600", "…the file is root-only (SSIDs are private)")
    ok(["-b"] == [a for a in j.calls[0] if a == "-b"] and all("-u" in c or "-k" in c for c in j.calls)
       and any("NetworkManager.service" in c for c in j.calls) and any("wpa_supplicant.service" in c for c in j.calls),
       "…asked of journalctl: this boot, NetworkManager + wpa_supplicant + vibeserver-pocket, and the kernel")
    # the next copy: the overlap with the previous read is cut, only new lines are appended
    wall[0] += 900
    j.t += 900
    j.units = j.units + ["2026-10-21T14:15:00.000001+0100 pocket NetworkManager[412]: <info> device (wlan0): state change: activated -> failed"]
    ok(k.due() and k.flush() == 1, "fifteen minutes on: only the one new line is copied (the overlap is cut)")
    ok(any("--since" in c for c in j.calls[2:]) and open(path, encoding="utf-8").read().count("Activation: starting") == 1,
       "…read with --since the last copy, nothing repeated")
    # after a join: soon, but not more than once every two minutes
    k.soon()
    ok(k.next_at == j.t + vp.WIFI_LOG_MIN_GAP_SEC - 0 and not k.due(), "a join straight after a copy: the next copy waits for the minimum gap")
    k.flush()
    j.t += 600
    k.soon()
    ok(k.next_at == j.t + vp.WIFI_LOG_AFTER_JOIN_SEC, "a join later on: copied ~20 s after it (NetworkManager's lines about it included)")
    # nothing new: nothing written (no SD write for nothing)
    size = os.path.getsize(path)
    j.t += 30
    ok(k.flush() == 0 and os.path.getsize(path) == size, "nothing new: nothing written")
    # a stop writes the marker line even with nothing new
    k.flush("vibeserver-pocket stopped (a reboot, an update or a restart)")
    ok("stopped (a reboot" in open(path, encoding="utf-8").read(), "a stop leaves a line saying so")
    # the cap: one rotation, never more than two files of the cap
    for i in range(40):
        j.units = j.units + ["2026-10-21T15:%02d:00.000001+0100 pocket NetworkManager[412]: <info> %s" % (i % 60, "x" * 200)]
        j.t += 1000
        k.flush()
    files = sorted(os.listdir(os.path.dirname(path)))
    ok(files == ["wifi-log.txt", "wifi-log.txt.1"] and all(os.path.getsize(os.path.join(os.path.dirname(path), f)) <= 4096 for f in files),
       "capped: wifi-log.txt + one rotation (wifi-log.txt.1), each within the cap — bounded on the card")
    # a restarted service reads the whole boot again: the last kept line cuts the repeat
    k2 = vp.KeptLog(j, path=path, cap=4096, wall=lambda: wall[0])
    before = open(path, encoding="utf-8").read()
    j.t += 100
    n = k2.flush()
    after = open(path, encoding="utf-8").read()
    ok(n == 0 and after.count(j.units[-1]) == 1, "a restarted service (whole boot read again): nothing already kept is repeated")
    # a journal that will not answer, a path that cannot be written: a log line, never an exception
    open(os.path.join(tmp, "blocker"), "w").close()        # a FILE where the directory should be
    bad = vp.KeptLog(j, path=os.path.join(tmp, "blocker", "wifi-log.txt"), cap=4096)
    with redirect_stdout(io.StringIO()) as out:
        r = bad.flush()
    ok(r == 0 and "could not keep the Wi-Fi log" in out.getvalue(), "an unwritable settings partition: one log line, never a crash")


# ★★★ WHAT `iw dev wlan0 scan dump` PRINTS (iw 6.x, Raspberry Pi OS bookworm; layout as iw's scan.c writes it). One
#     fixture per case Stuart's Portable meets: the iPhone with Maximise Compatibility OFF (5 GHz WPA2/WPA3 transition)
#     and ON (2.4 GHz plain WPA2), a WPA3-only router, a WPA2 router that offers PMF, one that requires it (PSK/SHA-256),
#     an open café, a hidden network (empty SSID), an enterprise one, an old WPA1 one, and one access point printed twice
#     (beacon + probe response) with the RSN capabilities only as hex on the second.
IW_IPHONE_TRANSITION = """BSS 9e:2c:4a:1b:7d:01(on wlan0)
\tlast seen: 1871.204s [boottime]
\tTSF: 0 usec (0d, 00:00:00)
\tfreq: 5220.0
\tbeacon interval: 100 TUs
\tcapability: ESS Privacy SpectrumMgmt ShortSlotTime RadioMeasure (0x1511)
\tsignal: -41.00 dBm
\tlast seen: 120 ms ago
\tInformation elements from Probe Response frame:
\tSSID: Stuey3D\\xe2\\x80\\x99s iPhone
\tSupported rates: 6.0* 9.0 12.0* 18.0 24.0* 36.0 48.0 54.0
\tDS Parameter set: channel 44
\tCountry: GB\tEnvironment: Indoor/Outdoor
\t\tChannels [36 - 48] @ 23 dBm
\tPower constraint: 0 dB
\tRSN:\t * Version: 1
\t\t * Group cipher: CCMP
\t\t * Pairwise ciphers: CCMP
\t\t * Authentication suites: PSK SAE
\t\t * Capabilities: 1-PTKSA-RC 1-GTKSA-RC MFP-capable (0x0080)
\tHT capabilities:
\t\tCapabilities: 0x1ef
\t\t\tRX LDPC
\tExtended capabilities:
\t\t * Extended Channel Switching
\t\t * BSS Transition
\t\t * Operating Mode Notification
\tWMM:\t * Parameter version 1
\t\t * BE: CW 15-1023, AIFSN 3
"""
IW_IPHONE_COMPAT = """BSS 9e:2c:4a:1b:7d:02(on wlan0)
\tlast seen: 1871.300s [boottime]
\tfreq: 2437
\tbeacon interval: 100 TUs
\tcapability: ESS Privacy ShortSlotTime (0x0411)
\tsignal: -44.00 dBm
\tlast seen: 80 ms ago
\tSSID: Stuey3D\\xe2\\x80\\x99s iPhone
\tDS Parameter set: channel 6
\tRSN:\t * Version: 1
\t\t * Group cipher: CCMP
\t\t * Pairwise ciphers: CCMP
\t\t * Authentication suites: PSK
\t\t * Capabilities: 1-PTKSA-RC 1-GTKSA-RC (0x0000)
"""
IW_WPA3_ONLY = """BSS 3c:28:6d:aa:bb:01(on wlan0)
\tfreq: 5500
\tcapability: ESS Privacy SpectrumMgmt ShortSlotTime (0x0511)
\tsignal: -60.00 dBm
\tSSID: Lab WPA3
\tRSN:\t * Version: 1
\t\t * Group cipher: CCMP
\t\t * Pairwise ciphers: CCMP
\t\t * Authentication suites: SAE
\t\t * Capabilities: 16-PTKSA-RC 1-GTKSA-RC MFP-required MFP-capable (0x00cc)
"""
IW_WPA2_MFPC = """BSS 74:ac:b9:00:11:22(on wlan0) -- associated
\tfreq: 5180
\tcapability: ESS Privacy SpectrumMgmt ShortSlotTime (0x0511)
\tsignal: -52.00 dBm
\tSSID: Meow!
\tRSN:\t * Version: 1
\t\t * Group cipher: CCMP
\t\t * Pairwise ciphers: CCMP
\t\t * Authentication suites: PSK FT/PSK
\t\t * Capabilities: 16-PTKSA-RC 1-GTKSA-RC MFP-capable (0x008c)
"""
IW_WPA2_MFPR = """BSS 74:ac:b9:00:11:33(on wlan0)
\tfreq: 2412
\tcapability: ESS Privacy ShortSlotTime (0x0411)
\tsignal: -70.00 dBm
\tSSID: Office
\tRSN:\t * Version: 1
\t\t * Group cipher: CCMP
\t\t * Pairwise ciphers: CCMP
\t\t * Authentication suites: PSK/SHA-256
\t\t * Capabilities: 16-PTKSA-RC 1-GTKSA-RC MFP-required MFP-capable (0x00cc)
"""
IW_OPEN_AND_HIDDEN = """BSS 00:11:22:33:44:55(on wlan0)
\tfreq: 2462
\tcapability: ESS ShortSlotTime (0x0401)
\tsignal: -75.00 dBm
\tSSID: Cafe Free WiFi
BSS 00:11:22:33:44:66(on wlan0)
\tfreq: 2462
\tcapability: ESS Privacy ShortSlotTime (0x0411)
\tsignal: -50.00 dBm
\tSSID:
\tRSN:\t * Version: 1
\t\t * Authentication suites: PSK
\t\t * Capabilities: 1-PTKSA-RC 1-GTKSA-RC (0x0000)
BSS 00:11:22:33:44:77(on wlan0)
\tfreq: 5745
\tsignal: -66.00 dBm
\tSSID: \\x00\\x00\\x00\\x00
"""
IW_ENTERPRISE_WPA1 = """BSS 08:00:00:00:00:01(on wlan0)
\tfreq: 5200
\tcapability: ESS Privacy (0x0011)
\tsignal: -58.00 dBm
\tSSID: eduroam
\tRSN:\t * Version: 1
\t\t * Group cipher: CCMP
\t\t * Pairwise ciphers: CCMP
\t\t * Authentication suites: IEEE 802.1X FT/IEEE 802.1X
\t\t * Capabilities: 1-PTKSA-RC 1-GTKSA-RC MFP-capable (0x0080)
BSS 08:00:00:00:00:02(on wlan0)
\tfreq: 2422
\tcapability: ESS Privacy (0x0011)
\tsignal: -80.00 dBm
\tSSID: OldRouter
\tWPA:\t * Version: 1
\t\t * Group cipher: TKIP
\t\t * Pairwise ciphers: TKIP
\t\t * Authentication suites: PSK
"""
# One access point printed twice in one dump (iw on some kernels repeats a BSS whose beacon and probe response differ);
# the second copy has the capabilities as HEX ONLY (an older iw prints no words for them).
IW_TWICE_HEX_ONLY = """BSS 9e:2c:4a:1b:7d:01(on wlan0)
\tfreq: 5220
\tsignal: -45.00 dBm
\tSSID: Twice
\tRSN:\t * Version: 1
\t\t * Authentication suites: PSK
\t\t * Capabilities: (0x0000)
BSS 9e:2c:4a:1b:7d:01(on wlan0)
\tfreq: 5220
\tsignal: -45.00 dBm
\tSSID: Twice
\tRSN:\t * Version: 1
\t\t * Authentication suites: PSK SAE
\t\t * Capabilities: (0x0080)
"""


def rsn_detection():
    """★★★ DETECTION FIRST (2026-10-21): the RSN element read from iw → what the network is → what we ask for."""
    IPH = "Stuey3D’s iPhone"
    def one(text, ssid):
        return vp.advert_of(vp.parse_iw_scan(text)[ssid])

    a = one(IW_IPHONE_TRANSITION, IPH)
    b = vp.parse_iw_scan(IW_IPHONE_TRANSITION)[IPH][0]
    ok(b["akm"] == {"PSK", "SAE"} and b["mfpc"] and not b["mfpr"] and b["freq"] == 5220 and b["signal"] == -41.0
       and b["bssid"] == "9e:2c:4a:1b:7d:01",
       "iw fixture, iPhone Maximise Compatibility OFF: PSK+SAE, MFP capable not required, 5220 MHz, -41 dBm, curly ’ decoded")
    ok(a["kind"] == "transition" and vp.plan_for(a) == {"km": "wpa-psk", "pmf": "2"},
       "…→ WPA2/WPA3 transition → wpa-psk (SAE allowed), PMF OPTIONAL — what joined it on the device")
    ok("Country" not in str(b) and b["akm"] == {"PSK", "SAE"},
       "…the HT / Extended capabilities / WMM '*' lines after the RSN element are not read as RSN")
    a = one(IW_IPHONE_COMPAT, IPH)
    ok(a["kind"] == "wpa2" and vp.plan_for(a) == {"km": "wpa-psk", "pmf": "1"},
       "iw fixture, iPhone Maximise Compatibility ON: plain WPA2, no MFP → PMF off")
    a = one(IW_WPA3_ONLY, "Lab WPA3")
    ok(a["kind"] == "wpa3" and vp.plan_for(a) == {"km": "sae", "pmf": "3"}, "iw fixture, WPA3 only: SAE + MFP required → sae, PMF required")
    a = one(IW_WPA2_MFPC, "Meow!")
    ok(a["kind"] == "wpa2-mfpc" and vp.plan_for(a) == {"km": "wpa-psk", "pmf": "2"},
       "iw fixture, WPA2 mesh (PSK + FT/PSK) with MFP capable, associated: → PMF optional")
    a = one(IW_WPA2_MFPR, "Office")
    ok(a["kind"] == "wpa2-mfpr" and vp.plan_for(a) == {"km": "wpa-psk", "pmf": "3"},
       "iw fixture, WPA2 PSK/SHA-256 + MFP required: → PMF required")
    got = vp.parse_iw_scan(IW_OPEN_AND_HIDDEN)
    ok(set(got) == {"Cafe Free WiFi"} and vp.plan_for(vp.advert_of(got["Cafe Free WiFi"])) is None
       and vp.advert_of(got["Cafe Free WiFi"])["kind"] == "open",
       "iw fixture, open café: open, nothing to choose; hidden access points (empty / NUL SSID) skipped")
    got = vp.parse_iw_scan(IW_ENTERPRISE_WPA1)
    ok(vp.advert_of(got["eduroam"])["kind"] == "other" and vp.plan_for(vp.advert_of(got["eduroam"])) is None
       and got["eduroam"][0]["akm"] == {"IEEE 802.1X", "FT/IEEE 802.1X"},
       "iw fixture, enterprise (802.1X): not a password network — left alone ('IEEE 802.1X' kept as one suite)")
    ok(vp.advert_of(got["OldRouter"])["kind"] == "wpa1" and vp.plan_for(vp.advert_of(got["OldRouter"]))["pmf"] == "1",
       "iw fixture, old WPA1 (TKIP) router: wpa-psk, PMF off")
    got = vp.parse_iw_scan(IW_TWICE_HEX_ONLY)["Twice"]
    a = vp.advert_of(got)
    ok(len(got) == 2 and got[1]["mfpc"] and not got[1]["mfpr"] and a["kind"] == "transition" and a["mixed"],
       "one BSSID printed twice, capabilities as hex only: MFPC read from 0x0080; PSK vs PSK+SAE → the setting both take")
    ok(vp.parse_iw_scan("") == {} and vp.parse_iw_scan("garbage\nSSID: x\n") == {} and vp.advert_of([]) is None,
       "empty or junk iw output: nothing heard, never an exception")
    allfix = (IW_IPHONE_TRANSITION + IW_WPA3_ONLY + IW_WPA2_MFPC + IW_WPA2_MFPR + IW_OPEN_AND_HIDDEN + IW_ENTERPRISE_WPA1)
    ok(set(vp.parse_iw_scan(allfix)) == {IPH, "Lab WPA3", "Meow!", "Office", "Cafe Free WiFi", "eduroam", "OldRouter"},
       "every fixture in one dump: each network found under its own name")
    # fingerprints: the same security from another BSSID is the same; any change in AKM or MFP is not
    f1 = vp.advert_of(vp.parse_iw_scan(IW_WPA2_MFPC)["Meow!"])["fp"]
    f2 = vp.advert_of(vp.parse_iw_scan(IW_WPA2_MFPC.replace("74:ac:b9:00:11:22", "aa:bb:cc:dd:ee:ff"))["Meow!"])["fp"]
    f3 = vp.advert_of(vp.parse_iw_scan(IW_WPA2_MFPC.replace("MFP-capable (0x008c)", "(0x000c)"))["Meow!"])["fp"]
    ok(f1 == f2 == "FT/PSK+PSK/C" and f3 == "FT/PSK+PSK/0",
       "fingerprint: a new BSSID with the same security is the same network; MFP switched off is a change")
    # nmcli's SECURITY column, the second opinion
    nmk = {s: vp.advert_from_nm(s)["kind"] for s in ("WPA2 WPA3", "WPA3", "WPA2", "WPA1 WPA2", "", "--", "WPA2 802.1X", "OWE", "WPA1")}
    ok(nmk == {"WPA2 WPA3": "transition", "WPA3": "wpa3", "WPA2": "wpa2-mfpc", "WPA1 WPA2": "wpa2-mfpc", "": "open",
               "--": "open", "WPA2 802.1X": "other", "OWE": "other", "WPA1": "wpa1"},
       "nmcli SECURITY → kind (no MFP in nmcli, so plain WPA2 is asked for with PMF optional)")
    ok(vp.plan_for(None) == {"km": "wpa-psk", "pmf": "2"}, "not heard (hidden / not on yet) → wpa-psk, PMF optional")
    # the ladder
    L = lambda km, pmf: [(p["km"], p["pmf"]) for p in vp.ladder_of({"km": km, "pmf": pmf})]
    ok(L("sae", "3") == [("wpa-psk", "2"), ("wpa-psk", "1")] and L("wpa-psk", "3") == [("wpa-psk", "2"), ("wpa-psk", "1")]
       and L("wpa-psk", "2") == [("wpa-psk", "1")] and L("wpa-psk", "1") == [("wpa-psk", "2")] and L("wpa-psk", "0") == [("wpa-psk", "1")],
       "the ladder after a refusal: SAE/required → optional → off; optional → off; off → optional")
    ok(vp.plan_words({"km": "sae", "pmf": "3"}) == "WPA3 (SAE), PMF required" and vp.plan_words({"km": "", "pmf": "0"}) == "open",
       "the history's words for each setting")
    ok(vp.load_wifi_sec("/nonexistent/x.json") == {}, "no memory file: nothing remembered, never a failure")


def wpa3_transition_hotspots(tmp):
    """★★★ STUART'S PHONE HOTSPOTS (2026-10-20/21): iPhone and Galaxy A26, both 5 GHz WPA2/WPA3 transition. On the device
    (2026-10-21) the iPhone REFUSED a PMF-off profile at once and JOINED with SAE allowed + PMF optional. So: read what
    the network advertises (iw's RSN element) before every join, ask for exactly that, run the whole ladder on a refusal,
    remember what joined only for as long as the network advertises the same thing; a hotspot that answers only its own
    name is asked for by name; and every attempt lands in a join history the owner can read later."""
    def prof(ssid, psk, rank, pmf="0 (default)", hidden=False, km=""):
        return {"type": "802-11-wireless", "ssid": ssid, "psk": psk, "prio": vp.PRIORITY.get(rank, 0),
                "hidden": hidden, "mode": "infrastructure", "pmf": pmf, "km": km}
    AP = {"type": "802-11-wireless", "ssid": "Pocket-SDR", "psk": "pocketpass", "prio": 0, "hidden": False, "mode": "ap"}
    IPH, A26 = "Stuey3D’s iPhone", "Stuart's A26"

    def again(nm, box):
        """The link drops (the router is swapped, the phone hotspot restarts): the box looks again."""
        nm.active, nm.attempts = "", []
        box.read_active()

    # ── ★★★ THE DEVICE CASE: an 11.0.9 profile (PMF off) for the 5 GHz transition iPhone — detected, rewritten, joined ──
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof(IPH, "phonepass", 1, pmf="1 (disable)"),
                "vibe-net-2": prof("Meow!", "meowpass1", 2, pmf="1 (disable)"),
                "vibe-net-3": prof("Cafe", "", 3), vp.AP_NAME: dict(AP)}
    nm.air = {IPH: (60, "phonepass"), "Meow!": (80, "meowpass1")}
    nm.kind, nm.freq = {IPH: "transition"}, {IPH: 5220, "Meow!": 5180}
    with redirect_stdout(io.StringIO()) as out:
        run_for(nm, box, 60)
    h = box.history
    ok(nm.active == "vibe-net-1" and nm.attempts[:1] == [(IPH, "wpa-psk", "2")],
       "★★★ the 5 GHz WPA2/WPA3 iPhone (saved PMF off by 11.0.9): FIRST attempt already SAE allowed + PMF optional, joined")
    ok(nm.conns["vibe-net-1"]["pmf"].startswith("2") and nm.conns["vibe-net-2"]["pmf"].startswith("1"),
       "…its profile now says PMF optional; Meow! (not joined, plain WPA2) is untouched")
    ok(h and h[-1]["result"] == "joined" and h[-1]["band"] == "5 GHz" and h[-1]["freq"] == 5220
       and h[-1]["seen"] == "WPA2/WPA3" and h[-1]["used"] == "WPA2/WPA3 (SAE allowed), PMF optional",
       "…the history says: 5 GHz (5220), advertised WPA2/WPA3, joined with SAE allowed + PMF optional")
    ok("advertises WPA2/WPA3 — joining with WPA2/WPA3 (SAE allowed), PMF optional" in out.getvalue(),
       "…and the log says why, in plain words")
    ok(any(e["ssid"] == "Cafe" and e["result"] == "not seen" for e in h), "…and the absent network 3, asked for by name, is 'not seen'")
    ok(box.sec_memory.get(IPH, {}).get("pmf") == "2" and vp.load_wifi_sec(box.sec_path) == box.sec_memory
       and oct(os.stat(box.sec_path).st_mode & 0o777) == "0o600",
       "…what joined it is remembered (settings partition, root-only)")

    # ── each advertisement → the setting asked for, first time, no retry ──
    cases = [("wpa3", ("sae", "3")), ("transition", ("wpa-psk", "2")), ("wpa2-mfpc", ("wpa-psk", "2")),
             ("pmf-required", ("wpa-psk", "3")), ("wpa2", ("wpa-psk", "1"))]
    for kind, want in cases:
        nm, box = make(tmp)
        nm.conns = {"vibe-net-1": prof("Net", "netpass12", 1, pmf="1 (disable)"), vp.AP_NAME: dict(AP)}
        nm.air, nm.kind = {"Net": (70, "netpass12")}, {"Net": kind}
        with redirect_stdout(io.StringIO()):
            run_for(nm, box, 40)
        ok(nm.active == "vibe-net-1" and nm.attempts == [("Net", want[0], want[1])],
           "advertises %-12s → joined first time with key-mgmt %s, PMF %s" % (kind, want[0], want[1]))

    # ── not heard (iw has nothing, nmcli has nothing: a hidden network) → PMF optional ──
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof("Hidden", "hidpass12", 1, pmf="1 (disable)", hidden=True), vp.AP_NAME: dict(AP)}
    nm.air, nm.kind, nm.quiet = {"Hidden": (60, "hidpass12")}, {"Hidden": "pmf-required"}, {"Hidden"}
    real = nm.run
    def no_probe(args, timeout=40, secret=False):
        if args[:4] == ["nmcli", "device", "wifi", "rescan"]:
            return 1, ""                             # a directed probe that hears nothing back
        return real(args, timeout, secret)
    nm.run = no_probe
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 60)
    ok(nm.active == "vibe-net-1" and nm.attempts[:1] == [("Hidden", "wpa-psk", "2")],
       "a network not heard at all (hidden, nothing from iw or nmcli): joined blind with PMF optional")

    # ── iw has nothing, nmcli's SECURITY column is the second opinion ──
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof("W3", "w3pass123", 1), vp.AP_NAME: dict(AP)}
    nm.air, nm.kind, nm.iw_dump_ok = {"W3": (60, "w3pass123")}, {"W3": "wpa3"}, False
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 40)
    ok(nm.active == "vibe-net-1" and nm.attempts[:1] == [("W3", "sae", "3")],
       "iw cannot say: nmcli's \"WPA3\" is enough — SAE, PMF required")

    # ── THE SAFETY NET: a router that advertises what it does not honour → the ladder, kept, remembered ──
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof("Liar", "liarpass1", 1), vp.AP_NAME: dict(AP)}
    nm.air, nm.kind = {"Liar": (60, "liarpass1")}, {"Liar": "sae-broken"}
    with redirect_stdout(io.StringIO()) as out:
        run_for(nm, box, 90)
    ok(nm.active == "vibe-net-1" and [x[2] for x in nm.attempts] == ["2", "1"] and nm.conns["vibe-net-1"]["pmf"].startswith("1"),
       "advertises WPA2/WPA3 but SAE fails: optional refused → plain WPA2 (PMF off) joined, kept on the profile")
    ok([e["result"] for e in box.history] == ["auth failed", "joined"] and box.sec_memory["Liar"]["pmf"] == "1",
       "…the history shows both, and PMF off is remembered for this advertisement")
    # next boot: the same advertisement → straight to what worked, no refusal first
    nm.active, nm.attempts = "", []
    box2 = vp.Box(nm, dict(vp.DEFAULTS), state_path=box.state_path)
    box2.dev, box2.history_path, box2.history, box2.sec_path = "wlan0", box.history_path + "2", [], box.sec_path
    box2.sec_memory = vp.load_wifi_sec(box.sec_path)
    nm.conns["vibe-net-1"]["pmf"] = "2 (optional)"            # (as if NM had it otherwise — the memory decides)
    with redirect_stdout(io.StringIO()) as out:
        run_for(nm, box2, 60)
    ok(nm.active == "vibe-net-1" and nm.attempts == [("Liar", "wpa-psk", "1")] and "as when it last joined" in out.getvalue(),
       "after a restart, the same advertisement: what joined last time, first time (no refusal first)")

    # WPA3 advertised, SAE fails, PSK-only works: SAE → PSK optional → PSK off
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof("Liar3", "liarpass1", 1), vp.AP_NAME: dict(AP)}
    nm.air, nm.kind = {"Liar3": (60, "liarpass1")}, {"Liar3": "sae-broken"}
    real2 = nm.run
    def wpa3_says(args, timeout=40, secret=False):              # it ADVERTISES SAE only
        rc, o = real2(args, timeout, secret)
        if args[3:] == ["scan", "dump"]:
            o = o.replace("Authentication suites: PSK SAE", "Authentication suites: SAE").replace("(0x008c)", "(0x00cc)")
        return rc, o
    nm.run = wpa3_says
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 120)
    ok(nm.active == "vibe-net-1" and nm.attempts == [("Liar3", "sae", "3"), ("Liar3", "wpa-psk", "2"), ("Liar3", "wpa-psk", "1")],
       "advertises WPA3 only but will not do it: the WHOLE ladder — SAE → WPA2/WPA3 optional → plain WPA2 — joined on the last")

    # a router that says MFP capable and then refuses PMF: optional → off
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof("Mfp", "mfppass12", 1), vp.AP_NAME: dict(AP)}
    nm.air, nm.kind = {"Mfp": (60, "mfppass12")}, {"Mfp": "liar-mfpc"}
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 90)
    ok(nm.active == "vibe-net-1" and [x[2] for x in nm.attempts] == ["2", "1"],
       "advertises PMF capable but refuses it: optional → off, joined")

    # ── ★★★ THE SAME NAME, DIFFERENT ROUTERS (Stuart: "I have had Meow! over multiple different routers") ──
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof("Meow!", "meowpass1", 1), vp.AP_NAME: dict(AP)}
    nm.air, nm.kind = {"Meow!": (80, "meowpass1")}, {"Meow!": "wpa2"}
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 40)
    ok(nm.active == "vibe-net-1" and nm.attempts == [("Meow!", "wpa-psk", "1")], "(Meow! on the old router: plain WPA2, PMF off)")
    # the router is replaced by a WPA2/WPA3 one: same name, same password — and PMF off would now be refused
    nm.kind["Meow!"] = "transition"
    again(nm, box)
    with redirect_stdout(io.StringIO()) as out:
        run_for(nm, box, 60)
    ok(nm.active == "vibe-net-1" and nm.attempts == [("Meow!", "wpa-psk", "2")] and "changed since it last joined" in out.getvalue(),
       "★★ Meow! moves to a WPA2/WPA3 router: the change is SEEN, the profile rebuilt before the join, joined first time")
    ok(box.sec_memory["Meow!"]["pmf"] == "2" and box.sec_memory["Meow!"]["adv"] == "transition", "…and the memory follows it")
    # …then a WPA3-only one, then back to plain WPA2 (a DOWNGRADE: joined, logged in plain words)
    nm.kind["Meow!"] = "wpa3"
    again(nm, box)
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 60)
    ok(nm.active == "vibe-net-1" and nm.attempts == [("Meow!", "sae", "3")], "Meow! as WPA3 only: SAE, PMF required, first time")
    nm.kind["Meow!"] = "wpa2"
    again(nm, box)
    with redirect_stdout(io.StringIO()) as out:
        run_for(nm, box, 60)
    ok(nm.active == "vibe-net-1" and nm.attempts == [("Meow!", "wpa-psk", "1")],
       "★★ WPA3 only → plain WPA2 (a downgrade): NOT blocked — rebuilt and joined first time")
    ok("WEAKER than the WPA3 only (PMF required) it advertised when it last joined" in out.getvalue()
       and "another network using the same name" in out.getvalue(),
       "…and the downgrade is logged in plain words (a replaced router — or another network using the name)")
    # a remembered setting that is refused (same advertisement, the router now behaves differently): the ladder again
    nm.kind["Meow!"] = "wpa2-mfpc"
    again(nm, box)
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 40)
    nm.kind["Meow!"] = "liar-mfpc"                    # (prints the same RSN — the fingerprint does not change)
    again(nm, box)
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 90)
    ok(nm.active == "vibe-net-1" and [x[2] for x in nm.attempts] == ["2", "1"],
       "a REMEMBERED setting refused: never 'known, give up' — the ladder runs again and the new answer is kept")
    ok(box.sec_memory["Meow!"]["pmf"] == "1", "…and remembered")

    # ── two access points, one name, different security (old router + new mesh, both live) ──
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof("Meow!", "meowpass1", 1, pmf="1 (disable)"), vp.AP_NAME: dict(AP)}
    nm.air = {"Meow!": (80, "meowpass1")}
    nm.bss = {"Meow!": [("wpa2", 80), ("wpa3", 50)]}
    with redirect_stdout(io.StringIO()) as out:
        run_for(nm, box, 40)
    ok(nm.active == "vibe-net-1" and nm.attempts == [("Meow!", "wpa-psk", "2")] and "access points disagree" in out.getvalue(),
       "two access points named Meow! (plain WPA2 + WPA3 only): the one setting both take — SAE allowed, PMF optional")
    adv = vp.advert_of(vp.parse_iw_scan(nm.run(["iw", "dev", "wlan0", "scan", "dump"])[1])["Meow!"])
    ok(adv["kind"] == "transition" and adv["n"] == 2 and adv["mixed"] and adv["fp"] == "PSK/0,SAE/R",
       "…its fingerprint is the set of (AKM, MFP) heard — never the BSSIDs")

    # ── a wrong password: the ladder once, the profile back to the detected setting, no flip-flopping later ──
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof(IPH, "WRONGpass", 1, pmf="1 (disable)"), vp.AP_NAME: dict(AP)}
    nm.air, nm.kind = {IPH: (60, "phonepass")}, {IPH: "transition"}
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 200)
    ups = sum(1 for c in nm.calls if c[3:6] == ["connection", "up", "vibe-net-1"])
    ok(nm.active == vp.AP_NAME and nm.conns["vibe-net-1"]["pmf"].startswith("2") and 2 <= ups <= 3 and IPH not in box.sec_memory,
       "a wrong password: the ladder once (%d attempts), profile back to what it advertises, nothing remembered" % ups)
    ok("password" in box.last_error, "…and the owner is still told it refused the password")

    # ── saving: each network as advertised in the last scan; a re-save keeps a slot's setting ──
    nm, box = make(tmp)
    nm.air, nm.kind = {"Office": (60, "officepass"), "W3": (60, "w3pass123")}, {"Office": "pmf-required", "W3": "wpa3"}
    with redirect_stdout(io.StringIO()):
        box.scan()
        box.apply(req([("Office", "officepass", {}), ("W3", "w3pass123", {}), ("Unheard", "unheard12", {})]))
    ok(nm.conns["vibe-net-1"]["pmf"].startswith("3") and nm.conns["vibe-net-2"]["km"] == "sae"
       and nm.conns["vibe-net-3"]["pmf"].startswith("2"),
       "saving networks from the scan: WPA2 PMF-required → required, WPA3 → SAE, one not heard → PMF optional")
    with redirect_stdout(io.StringIO()):
        box.apply(req([("Office", "", {"keep": True})]))
    ok(nm.conns["vibe-net-1"]["pmf"].startswith("3"), "…re-saving Wi-Fi keeps a slot's setting (same slot, same name)")

    # ── ★★★ a hotspot that answers only when asked by name (an idle iPhone): directed probe, saved "not visible" ──
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof(IPH, "phonepass", 1, pmf="1 (disable)"),
                "vibe-net-2": prof("Meow!", "meowpass1", 2, pmf="1 (disable)"), vp.AP_NAME: dict(AP)}
    nm.air = {IPH: (60, "phonepass"), "Meow!": (80, "meowpass1")}
    nm.kind, nm.quiet, nm.freq = {IPH: "transition"}, {IPH}, {IPH: 5180, "Meow!": 5180}
    with redirect_stdout(io.StringIO()) as out:
        run_for(nm, box, 90)
    ok(nm.active == "vibe-net-1" and IPH in nm.probed and nm.conns["vibe-net-1"]["hidden"],
       "★★★ an iPhone hotspot missing from every scan: asked for BY NAME, found, saved as not visible, joined")
    ok("Meow!" not in nm.probed, "★★ privacy: a network the scan already saw is never asked for by name")
    ok(box.history[-1]["visible"] == "probe" and box.history[-1]["result"] == "joined",
       "…the history says it was found by asking (visible: probe)")
    # on a working link with somebody listening: no probing at all
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof(IPH, "phonepass", 1, pmf="1 (disable)"),
                "vibe-net-2": prof("Meow!", "meowpass1", 2, pmf="1 (disable)"), vp.AP_NAME: dict(AP)}
    nm.air = {"Meow!": (80, "meowpass1")}
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 60)
    ok(nm.active == "vibe-net-2", "(at home on network 2)")
    saved_af = vp.ACTIVE_FILE
    vp.ACTIVE_FILE = os.path.join(tmp, "active-wpa3")
    try:
        n0 = len(nm.probed)
        for _ in range(60):                         # 5 min of someone listening
            open(vp.ACTIVE_FILE, "w").close()
            box.tick(); nm.t += 5
        ok(len(nm.probed) == n0, "while somebody listens on a working link: never a probe (not even a scan)")
        os.remove(vp.ACTIVE_FILE)
        # ★ nobody listening: the idle upgrade check asks for the absent rank-1 phone by name — and the phone comes on
        nm.air[IPH], nm.quiet, nm.kind = (60, "phonepass"), {IPH}, {IPH: "transition"}
        with redirect_stdout(io.StringIO()):
            run_for(nm, box, 400)
        ok(nm.active == "vibe-net-1", "★★ nobody listening at home: the rank-1 iPhone hotspot comes on and the box MOVES UP to it by itself")
    finally:
        vp.ACTIVE_FILE = saved_af

    # ── an absent phone asked for every two minutes: ONE history line with a count, the list stays useful ──
    nm, box = make(tmp)
    nm.conns = {"vibe-net-1": prof(IPH, "phonepass", 1, pmf="1 (disable)"), vp.AP_NAME: dict(AP)}
    nm.air = {}
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 3600)
    nots = [e for e in box.history if e["result"] == "not seen"]
    ok(len(nots) == 1 and nots[0]["n"] >= 2, "an absent hotspot asked for all hour: one 'not seen' line, counted (×%d)" % (nots[0]["n"] if nots else 0))
    for i in range(30):
        box.record("Net %d" % i, "auth failed", "x")
    ok(len(box.history) == vp.JOIN_KEEP, "the history keeps the last %d attempts" % vp.JOIN_KEEP)
    # persisted, private, and in the (admin-only) state
    st = os.stat(box.history_path)
    ok(oct(st.st_mode & 0o777) == "0o600", "…saved on the settings partition, root-only (SSIDs are private)")
    b2 = vp.Box(nm, dict(vp.DEFAULTS), state_path=os.path.join(tmp, "s2.json"))
    ok(vp.load_history(box.history_path) == box.history, "…and read back after a restart")
    box.write_state()
    ok(json.load(open(box.state_path))["joins"] == box.history, "…and carried in the state the setup page reads (admin-gated)")
    ok(vp.load_history(os.path.join(tmp, "nope.json")) == [], "a missing history is an empty one, never a failure")
    del b2

    # ── the menu choice of a quiet rank-3 hotspot, from home ──
    nm, box = make(tmp)
    nm.conns = {"vibe-net-2": prof("Meow!", "meowpass1", 2, pmf="1 (disable)"),
                "vibe-net-3": prof(A26, "a26pass12", 3, pmf="1 (disable)"), vp.AP_NAME: dict(AP)}
    nm.air = {"Meow!": (80, "meowpass1")}
    with redirect_stdout(io.StringIO()):
        run_for(nm, box, 60)
        nm.air[A26] = (55, "a26pass12")
        nm.quiet, nm.kind, nm.freq = {A26}, {A26: "transition"}, {A26: 5745}
        box.choose_net(3)
        run_for(nm, box, 60)
    ok(nm.active == "vibe-net-3" and box.history[-1]["result"] == "joined" and box.history[-1]["band"] == "5 GHz",
       "the A26 (rank 3, quiet, 5 GHz WPA2/WPA3) chosen from home: asked by name, joined, recorded")

    ok(vp.join_result("Secrets were required, but not provided.") == "auth failed"
       and vp.join_result("The Wi-Fi network could not be found") == "not seen"
       and vp.join_result("IP configuration could not be reserved (no available address, timeout, etc.)") == "DHCP failed"
       and vp.join_result("Activation timed out") == "assoc timeout",
       "NetworkManager's reasons read as: auth failed / not seen / DHCP failed / assoc timeout")
    ok(vp.pmf_value("1 (disable)") == "1" and vp.pmf_value("optional") == "2" and vp.pmf_value("") == "",
       "nmcli's PMF spellings are read right")


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
        # ★ ≤ 5: the back-off's 4, plus the ONE PMF retry a refusal gets per run (join(), BRIEF-portable-wifi-realtime).
        ok(tries(nm6) <= 5 and nm6.active == "vibe-net-2",
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
        scan_lists_every_security(tempfile.mkdtemp())
        wpa3_transition_hotspots(tempfile.mkdtemp())
        rsn_detection()
        kept_log(tempfile.mkdtemp())

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
        ok(box.link.get("retryKind") == "retries", "…labelled as retries (a driver that reports them)")
        run_for(nm, box, 5)
        ok(box.link["retry"] == 0, "…and 0 % — not null — when nothing has been sent since the last reading")
        # ★★★ THE PI'S CHIP: no "tx retries" line at all, only "tx failed" — the null of 2026-10-20.
        nm.brcmfmac = True
        nm.txpk, nm.txrt = 50000, 40
        run_for(nm, box, 5)
        ok(box.link["retry"] is None, "brcmfmac: the first reading after the switch has nothing to compare with (null, once)")
        nm.txpk, nm.txrt = 50990, 50               # 990 sent, 10 more failed
        run_for(nm, box, 5)
        ok(box.link["retry"] == 1 and box.link.get("retryKind") == "failed",
           "brcmfmac (tx failed, no tx retries): the share of failed sends is READ — 10 of 1000 = 1 %, kind \"failed\"")
        nm.txpk, nm.txrt = 100, 0                  # counters back to zero: a re-join reset the driver's station entry
        run_for(nm, box, 5)
        ok(box.link["retry"] is None, "a counter that went BACKWARDS (a re-join) is a new baseline, not a negative %")
        nm.txpk, nm.txrt = 400, 100
        run_for(nm, box, 5)
        ok(box.link["retry"] == 25, "…and the next reading is measured from it (100 of 400 = 25 %)")
        nm.brcmfmac = False
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

    # ── ★★★ USB WI-FI DONGLES ADDED LATER (Stuart, 2026-10-25: "dont force a resetup if a wifi dongle is detected but when
    #    a user goes into setup show an option for new wifi hardware detected, configure it now?") ──
    with tempfile.TemporaryDirectory() as tmp, redirect_stdout(io.StringIO()) as hwlog:
        vp.HOSTNAME_FILE = os.path.join(tmp, "hostname"); vp.HOSTS_FILE = os.path.join(tmp, "hosts")
        with open(vp.HOSTNAME_FILE, "w") as f: f.write("vibepocket\n")
        with open(vp.HOSTS_FILE, "w") as f: f.write("127.0.1.1\tvibepocket\n")
        IDS = {"wlan0": "dev:brcmfmac:mmc1:0001:1", "wlx00c0ca123456": "usb:0bda:8179:00e04c000001",
               "wlan1": "usb:148f:7601:port-1-1.2"}

        def hwbox(name, wifi="wlan0", more=(), eth="connected", record=None, configured=True):
            nm = FakeNM()
            nm.wifi_dev, nm.more_wifi, nm.eth = wifi, list(more), eth
            box = vp.Box(nm, dict(vp.DEFAULTS), state_path=os.path.join(tmp, name + "-state.json"))
            nm.state_path = box.state_path
            box.hw_path = os.path.join(tmp, name + "-hw.json")
            box.reset_path = os.path.join(tmp, name + "-reset")
            box.history_path = os.path.join(tmp, name + "-hist.json"); box.history = []
            box.sec_path = os.path.join(tmp, name + "-sec.json"); box.sec_memory = {}
            box.radio_info = lambda d: {"iface": d, "id": IDS[d], "driver": "rtl8xxxu" if d != "wlan0" else "brcmfmac",
                                        "usb": IDS[d].split(":")[1] + ":" + IDS[d].split(":")[2] if IDS[d].startswith("usb") else ""}
            if record is not None:
                with open(box.hw_path, "w") as f: json.dump(record, f)
            box.sync_hostname(configured)
            return nm, box

        def hotspots(nm):
            return [c for c in nm.calls if c[3:5] == ["connection", "up"] and c[5] in (vp.SETUP_NAME, vp.AP_NAME)]

        # 1 ── a set-up WIRED box with no Wi-Fi (Nick's Orange Pi One) — a dongle appears at boot ──
        nm, box = hwbox("wired", wifi=None, more=["wlx00c0ca123456"], record={"v": 1, "known": {}, "ignored": {}})
        vp.boot(box, configured=True)
        run_for(nm, box, 300)
        st = state_of(box)
        ok(not hotspots(nm) and vp.SETUP_NAME not in nm.conns and nm.active == "",
           "★★★ a NEW dongle on a set-up wired box: NO hotspot, no join, no re-setup")
        ok(st["newWifi"] == [{"iface": "wlx00c0ca123456", "driver": "rtl8xxxu", "usb": "0bda:8179"}] and st["hasWifi"] is False,
           "…state.json says newWifi (iface, driver, USB id); hasWifi stays false (nothing is managed)")
        ok("wlx00c0ca123456" in nm.unmanaged, "…and NetworkManager is told not to manage it (it joins nothing on its own)")
        # 2 ── Configure ──
        box.adopt_new_radio()
        run_for(nm, box, 300)
        st = state_of(box)
        ok(box.dev == "wlx00c0ca123456" and st["hasWifi"] is True and st["newWifi"] == [] and "wlx00c0ca123456" not in nm.unmanaged,
           "Configure: the dongle becomes this box's Wi-Fi (managed by NM again), no longer 'new'")
        ok(not hotspots(nm) and st["mode"] == "wifi-unset" and st["wifiUnset"] is True,
           "★ …and with nothing saved yet it WAITS for the owner's settings — still no open hotspot on a live server")
        ok("usb:0bda:8179:00e04c000001" in json.load(open(box.hw_path))["known"], "…recorded as known on the settings partition")
        nm.air = {"Home": (60, "homepass1")}
        box.apply(req([("Home", "homepass1", dict(hidden=False, keep=False))]))
        run_for(nm, box, 120)
        ok(nm.active == "vibe-net-1" and box.mode == "client" and not box.adopting,
           "…the owner saves networks + its own hotspot: managed as normal (joins Home)")
        # 3 ── the next boot: the same dongle is KNOWN ──
        nm2, box2 = hwbox("wired", wifi=None, more=["wlx00c0ca123456"])
        nm2.conns, nm2.air = nm.conns, nm.air
        vp.boot(box2, configured=True)
        run_for(nm2, box2, 120)
        ok(box2.dev == "wlx00c0ca123456" and state_of(box2)["newWifi"] == [] and "wlx00c0ca123456" not in nm2.unmanaged
           and nm2.active == "vibe-net-1", "a KNOWN dongle at boot: managed at once, not asked about again")
        # 4 ── unplug it ──
        nm2.more_wifi = []
        run_for(nm2, box2, 60)
        st = state_of(box2)
        ok(box2.dev is None and st["mode"] == "wired" and st["hasWifi"] is False and st["newWifi"] == [],
           "unplugged: wired only, nothing new, nothing broken")
        ok("usb:0bda:8179:00e04c000001" in json.load(open(box2.hw_path))["known"], "…and it is still known (plug it back: managed)")
        nm2.more_wifi = ["wlx00c0ca123456"]
        run_for(nm2, box2, 60)
        ok(box2.dev == "wlx00c0ca123456" and nm2.active == "vibe-net-1", "plugged back in: managed again, joins its network")
        # 5 ── Not now / Ignore ──
        nm, box = hwbox("ign", wifi=None, more=["wlan1"], record={"v": 1, "known": {}, "ignored": {}})
        vp.boot(box, configured=True)
        run_for(nm, box, 30)
        box.ignore_new_radio()
        run_for(nm, box, 30)
        st = state_of(box)
        ok(st["newWifi"] == [] and [i["iface"] for i in st["ignoredWifi"]] == ["wlan1"] and box.dev is None and not hotspots(nm),
           "Not now: recorded known-but-off — not new, not managed, no hotspot")
        nm3, box3 = hwbox("ign", wifi=None, more=["wlan1"])
        vp.boot(box3, configured=True)
        run_for(nm3, box3, 60)
        ok(state_of(box3)["newWifi"] == [] and "wlan1" in nm3.unmanaged and box3.dev is None,
           "…the next boot does not ask again, and NM still leaves it alone")
        ok(box3.adopt_new_radio() and box3.dev == "wlan1", "…and Configure can still use it later (the page offers it)")
        # 6 ── a box WITH onboard Wi-Fi on a network gains a dongle ──
        nm, box = hwbox("onboard", wifi="wlan0", record={"v": 1, "known": {IDS["wlan0"]: {}}, "ignored": {}})
        nm.air = {"Home": (60, "homepass1")}
        box.apply(req([("Home", "homepass1", dict(hidden=False, keep=False))]))
        run_for(nm, box, 120)
        nm.more_wifi = ["wlan1"]
        run_for(nm, box, 120)
        ok(box.dev == "wlan0" and nm.active == "vibe-net-1" and [i["iface"] for i in state_of(box)["newWifi"]] == ["wlan1"]
           and "wlan1" in nm.unmanaged, "onboard on Home + a new dongle: Home untouched, the dongle new and left alone")
        box.adopt_new_radio()
        ok(box.dev == "wlan1" and "wlan0" in nm.unmanaged and box.prefer_dev == "wlan1"
           and ["nmcli", "connection", "modify", "vibe-net-1", "connection.interface-name", "wlan1"] in nm.calls,
           "Configure it: the dongle takes over, the box's profiles move to it, the onboard radio is left alone")
        # 7 ── migration: a box set up before this release (no record) keeps its onboard Wi-Fi ──
        nm, box = hwbox("old", wifi="wlan0")
        nm.air = {"Home": (60, "homepass1")}
        box.apply(req([("Home", "homepass1", dict(hidden=False, keep=False))]))
        run_for(nm, box, 120)
        ok(box.dev == "wlan0" and nm.active == "vibe-net-1" and state_of(box)["newWifi"] == []
           and IDS["wlan0"] in json.load(open(box.hw_path))["known"],
           "★ an update on a box set up before the record existed: its radio is adopted, Wi-Fi carries on")
        # 8 ── first boot (never set up) behaves exactly as before ──
        nm, box = hwbox("first", wifi=None, more=["wlx00c0ca123456"], configured=False)
        vp.boot(box, configured=False)
        run_for(nm, box, int(vp.DEFAULTS["BOOT_GRACE_SEC"]) + 90)
        ok(box.dev == "wlx00c0ca123456" and nm.active == vp.SETUP_NAME and box.mode == "setup-ap"
           and "wlx00c0ca123456" not in nm.unmanaged, "a box NEVER set up: a dongle gets the open setup hotspot, as ever")
        ok("usb:0bda:8179:00e04c000001" in json.load(open(box.hw_path))["known"], "…and is recorded as known (it is the one being set up)")
        # 9 ── the owner's reset file still opens the door on a set-up box ──
        nm, box = hwbox("reset", wifi="wlan0", record={"v": 1, "known": {IDS["wlan0"]: {}}, "ignored": {}})
        box.reset_wifi()
        run_for(nm, box, 60)
        ok(nm.active == vp.SETUP_NAME, "a set-up box whose owner RESET the Wi-Fi: the open setup hotspot comes up (the way back in)")
        nm.air = {"Home": (60, "homepass1")}
        box.apply(req([("Home", "homepass1", dict(hidden=False, keep=False))]))
        ok(not os.path.exists(box.reset_path), "…and the reset is over once Wi-Fi is saved")
    ok("NEW Wi-Fi hardware" in hwlog.getvalue(), "the journal says why a radio is left alone")
    # ── what a radio IS, from a sysfs laid out as Linux lays it out (USB dongle; SDIO chip; none) ──
    with tempfile.TemporaryDirectory() as tmp:
        def mk(path, text=None):
            os.makedirs(os.path.dirname(path) if text is not None else path, exist_ok=True)
            if text is not None:
                open(path, "w").write(text)
        usb = os.path.join(tmp, "devices/platform/soc/1c1b000.usb/usb3/3-1")
        mk(os.path.join(usb, "3-1:1.0")); mk(os.path.join(usb, "idVendor"), "0bda\n"); mk(os.path.join(usb, "idProduct"), "8179\n")
        mk(os.path.join(usb, "serial"), "00e04c000001\n")
        mk(os.path.join(tmp, "bus/usb/drivers/rtl8xxxu"))
        os.symlink(os.path.join(tmp, "bus/usb/drivers/rtl8xxxu"), os.path.join(usb, "3-1:1.0", "driver"))
        sdio = os.path.join(tmp, "devices/platform/soc/1c10000.mmc/mmc_host/mmc1/mmc1:0001/mmc1:0001:1")
        mk(sdio); mk(os.path.join(tmp, "bus/sdio/drivers/brcmfmac"))
        os.symlink(os.path.join(tmp, "bus/sdio/drivers/brcmfmac"), os.path.join(sdio, "driver"))
        net = os.path.join(tmp, "class/net")
        for dev, target in (("wlx00e04c000001", os.path.join(usb, "3-1:1.0")), ("wlan0", sdio)):
            mk(os.path.join(net, dev)); os.symlink(target, os.path.join(net, dev, "device"))
        keep = vp.SYS_NET; vp.SYS_NET = net
        try:
            nm, box = make(tmp)
            u, d, n = box.radio_info("wlx00e04c000001"), box.radio_info("wlan0"), box.radio_info("wlan9")
        finally:
            vp.SYS_NET = keep
        ok(u == {"iface": "wlx00e04c000001", "driver": "rtl8xxxu", "usb": "0bda:8179", "id": "usb:0bda:8179:00e04c000001"},
           "sysfs: a USB dongle is its vendor:product + serial, with its driver (%r)" % (u,))
        ok(d["id"] == "dev:brcmfmac:mmc1:0001:1" and d["usb"] == "" and d["driver"] == "brcmfmac",
           "sysfs: an SDIO chip (a Pi's brcmfmac) is its driver + bus address, not USB (%r)" % (d,))
        ok(n["id"] == "if:wlan9", "no sysfs entry: the interface name is all there is")

    # ── ★★★ http://vibeserversetup.local/ ON ETHERNET, ONLY WHILE SETTING UP (Nick, G4IRX, a real Orange Pi One on a cable,
    #    2026-10-25: "entering http://vibeserversetup.local/ in the browser results in 'site not found'") ──
    with tempfile.TemporaryDirectory() as tmp, redirect_stdout(io.StringIO()):
        vp.HOSTNAME_FILE = os.path.join(tmp, "hostname"); vp.HOSTS_FILE = os.path.join(tmp, "hosts")
        with open(vp.HOSTNAME_FILE, "w") as f: f.write("vibeserversetup\n")
        with open(vp.HOSTS_FILE, "w") as f: f.write("127.0.1.1\tvibeserversetup\n")
        nm, box = make(tmp)
        nm.wifi_dev, nm.eth, box.dev = None, "connected", None     # a board card with no Wi-Fi, on Ethernet
        box.sync_hostname(False)
        door = [d for d in nm.nft if vp.SETUP_TABLE in d]
        ok(door and "tcp dport 80 redirect to :48000" in door[-1] and "iifname" not in door[-1],
           "unconfigured, wired, no Wi-Fi: :80 → the front door on EVERY link (no interface named)")
        ok(door and "table ip " in door[-1] and "table ip6 " in door[-1], "…for IPv4 AND IPv6 (the name may resolve to IPv6 first)")
        n = len(nm.nft); box.sync_hostname(False); box.tick()
        ok(len(nm.nft) == n, "…loaded once, not every few seconds")
        box.sync_hostname(True)
        ok(len(nm.nft) == n, "setup saved JUST NOW: the door stays (the page that pressed Save is still talking to :80)")
        nm.t += vp.RETIRE_SEC + 5
        box.sync_hostname(True)
        ok(len(nm.nft) == n + 1 and "delete table ip " + vp.SETUP_TABLE in nm.nft[-1] and "redirect" not in nm.nft[-1]
           and "delete table ip6 " + vp.SETUP_TABLE in nm.nft[-1],
           "★ setup finished: the door is REMOVED, both families — a set-up server does not take over port 80")
        box.sync_hostname(True)
        ok(len(nm.nft) == n + 1, "…and stays removed (nothing re-run)")
        # a box that BOOTS set up never opens it (only clears any left over)
        nm2, box2 = make(tmp)
        nm2.wifi_dev, nm2.eth, box2.dev = None, "connected", None
        with open(vp.HOSTNAME_FILE, "w") as f: f.write("vibepocket\n")
        box2.sync_hostname(True)
        ok(not any("redirect" in d for d in nm2.nft if vp.SETUP_TABLE in d),
           "a box that boots already set up never redirects :80 on the wired network")
        # ★ a kernel without IPv6 NAT: the whole script is refused → IPv4 alone
        nm3, box3 = make(tmp)
        nm3.wifi_dev, nm3.eth, box3.dev = None, "connected", None
        tried = []
        def no_ip6(args, data, timeout=20):
            tried.append(data)
            return 1 if "table ip6" in data and "redirect" in data else 0
        box3.sh.run_input = no_ip6
        box3.setup_door(True)
        ok(len(tried) == 2 and "table ip6" not in tried[1] and "redirect to :48000" in tried[1] and box3.setup_door_port == 48000,
           "no IPv6 NAT in the kernel: falls back to the IPv4 redirect, as before")
        # the Wi-Fi hotspot's own redirect is IPv6-capable now too, and still names the radio
        nm4, box4 = make(tmp)
        box4.captive_on()
        ok(nm4.nft and 'iifname "wlan0"' in nm4.nft[-1] and "table ip6 " + vp.NFT_TABLE in nm4.nft[-1],
           "the Wi-Fi :80 redirect (unchanged rule: the radio's interface) now covers IPv6 too")

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
