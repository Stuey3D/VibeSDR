#!/usr/bin/env python3
# ★★★ vibeserver-radios' UNIT-NAME CHECK, AND THE SERIAL AN ICOM IS GIVEN TO PASS IT (2026-11-03).
#     A real IC-7300 on a Pi 4 never started: its CP2102's USB serial is "IC-7300 03001234" — a SPACE, exactly as Icom
#     programs it — and valid_serial (the root script's guard on every `systemctl … vibeserver@<serial>`) refuses a
#     space, rightly. The fix is NOT to loosen valid_serial; it is that VibeServer stores the radio as
#     "IC-7300_03001234" (vibe::configSerial, android/app/src/main/cpp/vibe_serial.h). This proves, in real bash:
#       · valid_serial is unchanged — space, slash, '@', '\', leading '-'/'.', empty, over 64 are all still refused;
#       · the cleaned Icom serials pass, and two 7300s stay two different unit names;
#       · (bash 4+, i.e. Linux) the whole script run against stubbed vibeserver + systemctl starts
#         vibeserver@IC-7300_03001234 and ignores — never hands to systemctl — a raw serial with a space.
#     The C++ side (test-config-radios) asserts configSerial produces these same strings and runs this same
#     extracted function on them. Silent, no root, nothing touches the real systemd.
#  ★ The serials are made up (0300xxxx), not a real radio's.
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "vibeserver-radios")
BASH = shutil.which("bash")

fails = 0
count = 0


def ok(cond, what):
    global fails, count
    count += 1
    print("  %s %s" % ("\033[32mok\033[0m  " if cond else "\033[31mFAIL\033[0m", what))
    if not cond:
        fails += 1


def valid_serial_fn():
    s = open(SCRIPT).read()
    m = re.search(r"^valid_serial\(\) \{\n.*?^\}\n", s, re.S | re.M)
    assert m, "valid_serial() not found in vibeserver-radios"
    return m.group(0)


def valid(serial):
    r = subprocess.run([BASH, "-c", valid_serial_fn() + '\nvalid_serial "$1"', "x", serial])
    return r.returncode == 0


def function_tests():
    print("valid_serial — the unit-name guard, unchanged")
    for s in ["00000001", "240513CA60", "3b52ab5dac9d3f4b", "4", "a1:b2_c3.d-4", "_x", ":x", "usb:1-1.2"]:
        ok(valid(s), "accepts %r" % s)
    for s in ["IC-7300 03001234", "", "-x", ".x", "a/b", "a@b", "a\\b", "a b", "a\tb", "a\nb", "x" * 65,
              "$(id)", "a;b"]:
        ok(not valid(s), "★ refuses %r" % s)
    print("…and the serial VibeServer now gives an Icom passes it")
    ok(valid("IC-7300_03001234"), "★★★ IC-7300_03001234 (from \"IC-7300 03001234\") is a usable unit name")
    ok(valid("IC-7300_03005678"), "a second 7300's serial too")
    ok("IC-7300_03001234" != "IC-7300_03005678", "★ two 7300s are two unit names (the number is kept)")
    ok(valid("x" * 64), "64 characters is the limit configSerial cuts to")


def bash_major():
    r = subprocess.run([BASH, "-c", "echo ${BASH_VERSINFO[0]}"], stdout=subprocess.PIPE, universal_newlines=True)
    try:
        return int(r.stdout.strip())
    except ValueError:
        return 0


def script_test():
    print("the whole reconcile, vibeserver + systemctl stubbed")
    if bash_major() < 4:
        print("  (skipped: needs bash 4 for the script's associative arrays — this bash is %d; runs on Linux)"
              % bash_major())
        return
    with tempfile.TemporaryDirectory() as tmp:
        b = os.path.join(tmp, "bin")
        os.makedirs(b)
        log = os.path.join(tmp, "systemctl.log")
        stub_vs = os.path.join(b, "vibeserver")
        # ★ What a config holding one cleaned Icom, one RTL and a (hand-edited, never migrated) raw serial would list.
        open(stub_vs, "w").write("#!/bin/sh\nprintf '%s\\n' 'IC-7300_03001234' '00000003' 'IC-7300 03005678'\n")
        os.chmod(stub_vs, 0o755)
        # ★ As root (a build container) the script lists through `runuser -u vibeserver --`; no such user here.
        ru = os.path.join(b, "runuser")
        open(ru, "w").write('#!/bin/sh\nwhile [ "$#" -gt 0 ] && [ "$1" != "--" ]; do shift; done\nshift\nexec "$@"\n')
        os.chmod(ru, 0o755)
        sc = os.path.join(b, "systemctl")
        open(sc, "w").write("#!/bin/sh\necho \"$*\" >> '%s'\n" % log)
        os.chmod(sc, 0o755)
        conf = os.path.join(tmp, "config.json")
        open(conf, "w").write("{}")
        s = open(SCRIPT).read()
        s = s.replace("/usr/bin/vibeserver", stub_vs).replace("/var/lib/vibeserver", tmp)
        script = os.path.join(tmp, "vibeserver-radios")
        open(script, "w").write(s)
        r = subprocess.run([BASH, script], env=dict(os.environ, PATH=b + os.pathsep + os.environ["PATH"],
                                                     VIBESERVER_CONFIG=conf),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        calls = open(log).read() if os.path.exists(log) else ""
        ok(r.returncode == 0, "the script exits 0")
        ok("restart vibeserver@IC-7300_03001234.service" in calls, "★★★ the cleaned Icom is started")
        ok("restart vibeserver@00000003.service" in calls, "an RTL beside it is started as before")
        ok("03005678" not in calls, "★★ a raw serial with a space never reaches systemctl")
        ok("ignoring a radio with an unusable serial" in r.stdout, "…and is logged as ignored")


function_tests()
script_test()
print("%d checks, %d failures" % (count, fails))
sys.exit(1 if fails else 0)
