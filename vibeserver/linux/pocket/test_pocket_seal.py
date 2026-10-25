#!/usr/bin/env python3
"""vibeserver-pocket-seal on a fake root (2026-10-25): the read-only switch lands in the RIGHT boot file for each boot
chain, exactly once, and nothing is touched where it must not be.

  · Raspberry Pi: overlayroot=tmpfs at the front of cmdline.txt (/boot/firmware), the rest of the line kept.
  · Armbian / DietPi-on-Armbian (image/sbc): overlayroot=tmpfs at the front of the FIRST extraargs= in
    /boot/armbianEnv.txt (or dietpiEnv.txt) — added as a line when there is none, the owner's own args kept.
  · Idempotent: a second run changes nothing.
  · No marker, the no-seal file, no initramfs/uInitrd, already sealed: nothing written.
Run: python3 vibeserver/linux/pocket/test_pocket_seal.py
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SEAL = os.path.join(HERE, "vibeserver-pocket-seal")
fails = 0


def check(name, cond, detail=""):
    global fails
    print(("  ok  " if cond else "  FAIL ") + name + ("" if cond else "  — " + detail))
    if not cond:
        fails += 1


def mk(root, files):
    for path, text in files.items():
        full = os.path.join(root, path.lstrip("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as f:
            f.write(text)


def run(root, cmdline="console=ttyS0 root=UUID=x"):
    cl = os.path.join(root, "proc-cmdline")
    with open(cl, "w") as f:
        f.write(cmdline + "\n")
    env = dict(os.environ, SEAL_ROOT=root, SEAL_NO_REBOOT="1", SEAL_CMDLINE=cl)
    p = subprocess.run(["bash", SEAL], env=env, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def read(root, path):
    with open(os.path.join(root, path.lstrip("/"))) as f:
        return f.read()


MARK = {"/etc/vibeserver-pocket/enabled": "1\n"}
ENV = ("verbosity=1\nbootlogo=false\nconsole=both\noverlay_prefix=sun8i-h3\n"
       "rootdev=UUID=2d377195-31ea-46ef-bb1d-127bdb7777ac\nrootfstype=ext4\n")

with tempfile.TemporaryDirectory() as t:
    # ── Raspberry Pi ──
    r = os.path.join(t, "pi")
    mk(r, dict(MARK, **{"/boot/firmware/cmdline.txt": "console=serial0,115200 root=PARTUUID=ab-02 rootwait\n",
                        "/boot/firmware/initramfs8": "x"}))
    rc, out = run(r)
    c = read(r, "/boot/firmware/cmdline.txt")
    check("Pi: overlayroot=tmpfs at the front of cmdline.txt", c == "overlayroot=tmpfs console=serial0,115200 root=PARTUUID=ab-02 rootwait\n", repr(c))
    check("Pi: says it will reboot", "rebooting" in out, out)
    run(r)
    check("Pi: second run changes nothing", read(r, "/boot/firmware/cmdline.txt") == c)

    # ── Armbian, no extraargs line ──
    r = os.path.join(t, "armbian")
    mk(r, dict(MARK, **{"/boot/armbianEnv.txt": ENV, "/boot/uInitrd": "x"}))
    rc, out = run(r)
    e = read(r, "/boot/armbianEnv.txt")
    check("Armbian: extraargs=overlayroot=tmpfs added as its own line", e == ENV + "extraargs=overlayroot=tmpfs\n", repr(e))
    check("Armbian: no cmdline.txt made", not os.path.exists(os.path.join(r, "boot/cmdline.txt")))
    check("Armbian: says it will reboot", "rebooting" in out, out)
    run(r)
    check("Armbian: second run changes nothing", read(r, "/boot/armbianEnv.txt") == e)

    # ── Armbian, the owner's own extraargs, no final newline ──
    r = os.path.join(t, "armbian-args")
    mk(r, dict(MARK, **{"/boot/armbianEnv.txt": "verbosity=1\nextraargs=net.ifnames=0 quiet\nusbstoragequirks=0x2537:0x1066:u",
                        "/boot/uInitrd": "x"}))
    run(r)
    e = read(r, "/boot/armbianEnv.txt")
    check("Armbian: the owner's extraargs kept, the switch in front",
          e == "verbosity=1\nextraargs=overlayroot=tmpfs net.ifnames=0 quiet\nusbstoragequirks=0x2537:0x1066:u\n", repr(e))
    check("Armbian: one extraargs line only", e.count("extraargs=") == 1, repr(e))

    # ── DietPi on an Armbian board ──
    r = os.path.join(t, "dietpi")
    mk(r, dict(MARK, **{"/boot/dietpiEnv.txt": "rootdev=UUID=x\nextraargs=\n", "/boot/uInitrd": "x"}))
    run(r)
    check("DietPi: dietpiEnv.txt gets the switch", read(r, "/boot/dietpiEnv.txt") == "rootdev=UUID=x\nextraargs=overlayroot=tmpfs\n",
          repr(read(r, "/boot/dietpiEnv.txt")))

    # ── nothing to do ──
    r = os.path.join(t, "nomark")
    mk(r, {"/boot/armbianEnv.txt": ENV, "/boot/uInitrd": "x"})
    rc, out = run(r)
    check("no marker (an ordinary install): untouched, silent", read(r, "/boot/armbianEnv.txt") == ENV and out == "", out)
    r = os.path.join(t, "noseal")
    mk(r, dict(MARK, **{"/boot/armbianEnv.txt": ENV, "/boot/uInitrd": "x", "/etc/vibeserver-pocket/no-seal": ""}))
    run(r)
    check("no-seal file: untouched", read(r, "/boot/armbianEnv.txt") == ENV)
    r = os.path.join(t, "nouinitrd")
    mk(r, dict(MARK, **{"/boot/armbianEnv.txt": ENV}))
    rc, out = run(r)
    check("no uInitrd: untouched, and says why", read(r, "/boot/armbianEnv.txt") == ENV and "uInitrd" in out, out)
    r = os.path.join(t, "sealed")
    mk(r, dict(MARK, **{"/boot/armbianEnv.txt": ENV, "/boot/uInitrd": "x"}))
    rc, out = run(r, cmdline="root=UUID=x overlayroot=tmpfs")
    check("already sealed (overlayroot=tmpfs on the running command line): untouched", read(r, "/boot/armbianEnv.txt") == ENV)
    r = os.path.join(t, "nothing")
    mk(r, MARK)
    rc, out = run(r)
    check("no boot file at all: says so, exits 0", rc == 0 and "not sealing" in out, out)

print("test_pocket_seal: %s" % ("PASS" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
