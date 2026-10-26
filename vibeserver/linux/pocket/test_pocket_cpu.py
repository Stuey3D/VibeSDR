#!/usr/bin/env python3
"""vibeserver-pocket-cpu on a fake root (2026-10-25): a Portable card puts the ARMv6 build in place on a processor
without NEON — written on a writable root, bind-mounted on the read-only overlay — and does nothing anywhere else.
Run: python3 vibeserver/linux/pocket/test_pocket_cpu.py
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "vibeserver-pocket-cpu")
fails = 0
PI_ZERO = "processor\t: 0\nmodel name\t: ARMv6-compatible processor rev 7 (v6l)\nFeatures\t: half thumb fastmult vfp edsp java tls\n"
PI_2 = "processor\t: 0\nmodel name\t: ARMv7 Processor rev 5 (v7l)\nFeatures\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
PI_3_64K = "processor\t: 0\nFeatures\t: fp asimd evtstrm crc32 cpuid\n"   # a 64-bit kernel, 32-bit userland


def check(name, cond, detail=""):
    global fails
    print(("  ok  " if cond else "  FAIL ") + name + ("" if cond else "  — " + detail))
    if not cond:
        fails += 1


def box(t, name, marker=True, v6=True, rsp=True):
    r = os.path.join(t, name)
    for d in ("etc/vibeserver-pocket", "usr/bin", "usr/lib/vibeserver/armv6", "usr/lib/vibeserver/openrsp"):
        os.makedirs(os.path.join(r, d), exist_ok=True)
    if marker:
        open(os.path.join(r, "etc/vibeserver-pocket/enabled"), "w").write("1\n")
    open(os.path.join(r, "usr/bin/vibeserver"), "w").write("NEON BUILD")
    if v6:
        open(os.path.join(r, "usr/lib/vibeserver/armv6/vibeserver"), "w").write("ARMV6 BUILD")
        os.chmod(os.path.join(r, "usr/lib/vibeserver/armv6/vibeserver"), 0o755)
    if rsp:
        open(os.path.join(r, "usr/lib/vibeserver/openrsp/sdrplay_apiService"), "w").write("x")
    return r


def run(r, cpuinfo, rootfs="ext4"):
    ci = os.path.join(r, "cpuinfo")
    open(ci, "w").write(cpuinfo)
    p = subprocess.run(["bash", TOOL], env=dict(os.environ, CPU_ROOT=r, CPUINFO=ci, CPU_ROOTFS=rootfs),
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def F(r, p):
    try:
        return open(os.path.join(r, p)).read()
    except OSError:
        return None


with tempfile.TemporaryDirectory() as t:
    r = box(t, "zero-first-boot")
    rc, out = run(r, PI_ZERO)
    check("Pi Zero, writable root: the ARMv6 build installed", F(r, "usr/bin/vibeserver") == "ARMV6 BUILD", out)
    check("Pi Zero, writable root: the ARMv7 SDRplay service removed", F(r, "usr/lib/vibeserver/openrsp/sdrplay_apiService") is None)
    check("Pi Zero: says so", "ARMv6" in out and rc == 0, out)
    rc, out = run(r, PI_ZERO)
    check("Pi Zero: a second boot finds it in place", "already in place" in out, out)

    r = box(t, "zero-sealed")
    rc, out = run(r, PI_ZERO, rootfs="overlay")
    check("Pi Zero, sealed (overlay) root: bind-mounted, the file itself untouched",
          F(r, "bind-mounted") is not None and F(r, "usr/bin/vibeserver") == "NEON BUILD", out)

    for name, ci in (("Pi 2 (neon)", PI_2), ("Pi 3, 64-bit kernel (asimd)", PI_3_64K)):
        r = box(t, name.split()[1])
        run(r, ci)
        check(name + ": nothing changed", F(r, "usr/bin/vibeserver") == "NEON BUILD"
              and F(r, "usr/lib/vibeserver/openrsp/sdrplay_apiService") == "x")

    r = box(t, "ordinary", marker=False)
    rc, out = run(r, PI_ZERO)
    check("no Portable marker (an ordinary install — postinst already chose): nothing, silently",
          F(r, "usr/bin/vibeserver") == "NEON BUILD" and out == "", out)
    r = box(t, "arm64pkg", v6=False)
    rc, out = run(r, PI_ZERO)
    check("no ARMv6 build in the package: nothing", F(r, "usr/bin/vibeserver") == "NEON BUILD" and rc == 0, out)

print("test_pocket_cpu: %s" % ("PASS" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
