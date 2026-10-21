#!/usr/bin/env python3
# ★★★ DOWNLOAD DIAGNOSTICS (vibeserver-diagnostics, 2026-10-20): the file an owner sends us must never carry a secret.
#     Planted everywhere a secret could leak from — a Wi-Fi password in NetworkManager and in a careless log line, the
#     admin password and PINs in the config AND in a logged URL, a radio's PIN in its status file, the directory key —
#     and none may survive. Also: the sections are there, the journal is capped (newest kept), a Portable gets its Wi-Fi
#     story and an ordinary server does not. Fake commands only: never the real journal, never NetworkManager.
import importlib.machinery
import importlib.util
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
loader = importlib.machinery.SourceFileLoader("vd", os.path.join(HERE, "vibeserver-diagnostics"))
spec = importlib.util.spec_from_loader("vd", loader)
vd = importlib.util.module_from_spec(spec)
loader.exec_module(vd)

fails = 0


def ok(cond, what):
    global fails
    print("  %s %s" % ("\033[32mok\033[0m  " if cond else "\033[31mFAIL\033[0m", what))
    if not cond:
        fails += 1


def main():
    print("vibeserver-diagnostics — the file an owner sends us")
    tmp = tempfile.mkdtemp()
    PSK, AP_PSK, ADMIN, PIN, RPIN, DKEY = "Ph0nePass!xyz", "hotspotPW99", "Adm1n-Secret-77", "482913", "771200", "dk_live_ABCDEF123456"
    conf = os.path.join(tmp, "config.json")
    json.dump({"name": "Pocket", "adminPass": ADMIN, "pin": PIN, "radios": [{"serial": "00000003", "pin": RPIN, "label": "V4"}]},
              open(conf, "w"))
    dstate = os.path.join(tmp, "directory.json")
    json.dump({"id": "abc", "key": DKEY, "slug": "pocket"}, open(dstate, "w"))
    status = os.path.join(tmp, "radio-status-00000003.json")
    open(status, "w").write('{"serial":"00000003","agc":"on","pin":"%s"}' % RPIN)
    hist = os.path.join(tmp, "join-history.json")
    json.dump([{"t": 1792500000, "ssid": "Stuey3D’s iPhone", "band": "5 GHz", "freq": 5180, "seen": "WPA2 WPA3",
                "visible": "probe", "used": "WPA2-PSK, PMF off", "result": "joined", "reason": "", "n": 1}], open(hist, "w"))
    pstate = os.path.join(tmp, "state.json")
    open(pstate, "w").write('{"mode":"client","ssid":"Stuey3D’s iPhone"}')
    marker = os.path.join(tmp, "enabled")
    wlog = os.path.join(tmp, "wifi-log.txt")
    open(wlog + ".1", "w").write("2026-10-19T09:00:00.000000+0100 pocket NetworkManager[400]: <info> OLDER-ROTATED-LINE\n")
    open(wlog, "w").write("2026-10-20T09:00:00.000000+0100 pocket kernel: brcmfmac: brcmf_run_escan: error (-110) BEFORE-REBOOT\n"
                          "2026-10-20T09:00:01.000000+0100 pocket wpa_supplicant[500]: wlan0: psk=%s KEPT-LINE\n" % PSK)
    open(marker, "w").close()
    journal = "\n".join(["2026-10-20T10:00:%02d vibeserver[1]: line %d" % (i % 60, i) for i in range(200)] + [
        "2026-10-20T10:05:00 vibeserver[1]: VibeAGC: gain step 28.0 dB",
        "2026-10-20T10:05:01 NetworkManager[2]: <info> wifi-sec.psk=%s (a careless debug line)" % PSK,
        "2026-10-20T10:05:02 vibeserver[1]: GET /vibeserver/config?vs_admin_nonce=n1&vs_admin_auth=abcdef123 pass %s" % ADMIN,
        "2026-10-20T10:05:03 vibeserver[1]: config {\"pin\": \"%s\", \"radioPin\": \"%s\"}" % (PIN, RPIN)])
    calls = []

    def run(args, timeout=30):
        calls.append(args)
        if args[0] == "journalctl":
            return 0, journal
        if args[:5] == ["nmcli", "-t", "-f", "NAME,TYPE", "connection"]:
            return 0, "vibe-net-1:802-11-wireless\nvibe-ap:802-11-wireless\nWired:802-3-ethernet\n"
        if args[:2] == ["nmcli", "-s"]:
            return 0, (PSK if args[-1] == "vibe-net-1" else AP_PSK) + "\n"
        if args[:2] == ["nmcli", "-g"]:
            f = args[2]
            return 0, {"802-11-wireless.ssid": "Stuey3D’s iPhone" if args[-1] == "vibe-net-1" else "Pocket-SDR",
                       "802-11-wireless-security.pmf": "1 (disable)"}.get(f, "") + "\n"
        if args[:2] == ["iw", "reg"]:
            return 0, "global\ncountry GB: DFS-ETSI\n"
        return 0, ""

    kw = dict(run=run, conf_paths=(conf, dstate), pocket_marker=marker, pocket_state=pstate, join_history=hist,
              status_glob=os.path.join(tmp, "radio-status-*.json"), server_conf=conf, wifi_log=wlog)
    text = vd.bundle(**kw)
    for name, v in (("the Wi-Fi password", PSK), ("the hotspot password", AP_PSK), ("the admin password", ADMIN),
                    ("the listener PIN", PIN), ("a radio's PIN", RPIN), ("the directory key", DKEY)):
        ok(v not in text, "★★★ %s is NOT in the file (planted in NM / a log line / the config / a status file)" % name)
    ok("vs_admin_auth=abcdef123" not in text, "…nor an admin proof from a logged URL")
    ok(not any(a[:2] == ["nmcli", "-s"] and "-g" in a and a[a.index("-g") + 1] != "802-11-wireless-security.psk" for a in calls),
       "secrets are only ever asked for in order to scrub them (the psk field, nothing else)")
    for sect in ("versions", "machine", "services", "usb", "server config (secrets removed)", "radio status",
                 "wi-fi firmware", "iw dev wlan0 link", "iw dev wlan0 get power_save", "iw reg get",
                 "networkmanager wi-fi profiles (no secrets)", "wi-fi join history", "wi-fi service state",
                 "wi-fi log kept across reboots",
                 "highlights", "journal, last 24 h"):
        ok("==== " + sect in text, "a Portable's file has its '%s' section" % sect)
    ok("SINCE THE LAST BOOT" in text and "No passwords" in text and "addresses" in text,
       "…and says what it is: since the last boot (a Portable's journal is in RAM), no passwords, addresses + network names")
    ok("Stuey3D’s iPhone" in text and "pmf=1 (disable)" in text and "WPA2-PSK, PMF off" in text,
       "…the profiles with their PMF, and the join history")
    ok("VibeAGC: gain step" in text.split("==== highlights")[1].split("==== journal")[0],
       "VibeAGC lines are lifted into the highlights (DIAG_HIGHLIGHTS takes more patterns)")
    ok(any(a[0] == "journalctl" and "vibeserver*" in a and "NetworkManager*" in a and "sdrplay*" in a for a in calls),
       "the journal asked for: our units, NetworkManager, avahi, the SDRplay service")
    kl = text.split("==== wi-fi log kept across reboots")[1].split("\n==== ")[0]
    ok("BEFORE-REBOOT" in kl and "KEPT-LINE" in kl and kl.index("OLDER-ROTATED-LINE") < kl.index("BEFORE-REBOOT"),
       "★★★ the Wi-Fi log kept on the settings partition is in the file — the rotated half first (oldest first)")
    ok(PSK not in kl, "…and scrubbed like everything else (a password planted in a kept line is gone)")
    # an ordinary (not Portable) server: no Wi-Fi sections, never NetworkManager's secrets
    calls.clear()
    kw2 = dict(kw, pocket_marker=os.path.join(tmp, "absent"))
    t2 = vd.bundle(**kw2)
    ok("==== wi-fi join history" not in t2 and "==== iw reg get" not in t2 and "(Portable)" not in t2,
       "an ordinary Linux server: no Portable Wi-Fi sections")
    ok(PSK not in t2 and ADMIN not in t2 and "last 24 h at most" in t2, "…still scrubbed, and says the journal covers 24 h at most")
    # the journal is capped, the newest kept
    big = "\n".join("2026-10-20T10:00:00 vibeserver[1]: %06d %s" % (i, "x" * 100) for i in range(60000))
    t3 = vd.bundle(**dict(kw2, run=lambda a, timeout=30: (0, big) if a[0] == "journalctl" else run(a)))
    ok(len(t3.encode()) < vd.JOURNAL_MAX + 400 * 1024 and "059999" in t3 and "the newest 3 MB only" in t3,
       "a 6 MB journal is cut to the NEWEST 3 MB, and the file says so (%d KB)" % (len(t3.encode()) // 1024))
    # the redaction rules on their own
    ok(vd.redact('{"adminPass": "zzz", "wifiPin":"1"} psk=abc password: xyz vs_auth=q1&x=2', set()) ==
       '{"adminPass": "[redacted]", "wifiPin":"[redacted]"} psk=[redacted] password: [redacted] vs_auth=[redacted]&x=2',
       "anything that LOOKS like a secret is scrubbed too (JSON keys, psk=, password:, a proof in a URL)")
    ok(vd.secret_values((conf, dstate), run=lambda a, timeout=30: (0, "")) >= {ADMIN, PIN, RPIN, DKEY},
       "the secrets are collected from the config (any depth) and the directory key")
    print()
    if fails:
        print("\033[31m%d failed\033[0m" % fails)
        return 1
    print("\033[32mall passed\033[0m")
    return 0


if __name__ == "__main__":
    sys.exit(main())
