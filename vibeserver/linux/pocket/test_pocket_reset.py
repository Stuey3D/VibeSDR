#!/usr/bin/env python3
"""The forget-the-Wi-Fi file (vibeserver-pocket take_reset_files, 11.0.11): found in every place an owner can put it —
the Pi's FAT boot partition, /boot, the board card's FAT VIBESETUP partition, with or without Notepad's .txt — the Wi-Fi
reset ONCE, every copy deleted; a read-only mount is remounted rw for the delete (the Pi's boot partition is ro on a
Portable, so the file used to stay and the box forgot its Wi-Fi at every boot); a copy on a sealed root's lower layer
is deleted there too. Run: python3 vibeserver/linux/pocket/test_pocket_reset.py
"""
import errno
import importlib.machinery
import importlib.util
import io
import os
import sys
import tempfile
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
loader = importlib.machinery.SourceFileLoader("vp_reset", os.path.join(HERE, "vibeserver-pocket"))
spec = importlib.util.spec_from_loader("vp_reset", loader)
vp = importlib.util.module_from_spec(spec)
loader.exec_module(vp)
fails = 0


def ok(cond, what):
    global fails
    print("  %s %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        fails += 1


class Box:
    resets = 0

    def reset_wifi(self):
        self.resets += 1


class Sh:
    """mount -o remount,rw|ro <dir>: tracks which directories are 'read-only'."""
    def __init__(self, ro):
        self.ro, self.calls = set(ro), []

    def run(self, args, timeout=40, secret=False):
        self.calls.append(list(args))
        if args[:2] == ["mount", "-o"]:
            (self.ro.discard if args[2] == "remount,rw" else self.ro.add)(args[3])
        return 0, ""


ok(any(f.endswith("/run/vibeserver-setup/vibeserver-reset-wifi") for f in vp.RESET_FILES), "the VIBESETUP partition is one of the places")
ok(any(f.endswith("/boot/firmware/vibeserver-reset-wifi") for f in vp.RESET_FILES)
   and any(f.endswith("/boot/vibeserver-reset-wifi") for f in vp.RESET_FILES), "the old places still count")
ok(any(f.endswith("vibeserver-reset-wifi.txt") for f in vp.RESET_FILES), "Notepad's .txt counts")

with tempfile.TemporaryDirectory() as t:
    fat, boot, setup, lower = (os.path.join(t, d) for d in ("bootfw", "boot", "setup", "lower"))
    for d in (fat, boot, setup, lower + boot):
        os.makedirs(d)
    files = [os.path.join(d, "vibeserver-reset-wifi" + x) for d in (fat, boot, setup) for x in ("", ".txt")]
    sh = Sh(ro=[fat])
    real_unlink = os.unlink

    def unlink(p, *a, **k):
        if os.path.dirname(p) in sh.ro:
            raise OSError(errno.EROFS, "Read-only file system", p)
        return real_unlink(p, *a, **k)

    vp.os.unlink = unlink
    vp.LOWER_ROOT = lower
    box = Box()
    with redirect_stdout(io.StringIO()):
        n0 = vp.take_reset_files(box, sh, files)
    ok(n0 == 0 and box.resets == 0, "no file: nothing reset")
    open(os.path.join(fat, "vibeserver-reset-wifi"), "w").close()
    open(os.path.join(setup, "vibeserver-reset-wifi.txt"), "w").close()
    open(os.path.join(boot, "vibeserver-reset-wifi"), "w").close()
    open(lower + os.path.join(boot, "vibeserver-reset-wifi"), "w").close()   # the same file, on the sealed root's lower layer
    with redirect_stdout(io.StringIO()):
        n = vp.take_reset_files(box, sh, files)
    ok(n == 3 and box.resets == 1, "three files: the Wi-Fi reset ONCE")
    ok(not any(os.path.exists(f) for f in files), "every copy deleted")
    ok(["mount", "-o", "remount,rw", fat] in sh.calls and fat in sh.ro, "the read-only FAT boot partition: remounted rw for the delete, ro again after")
    ok(not os.path.exists(lower + os.path.join(boot, "vibeserver-reset-wifi")), "the copy on the sealed root's lower layer deleted too")
    with redirect_stdout(io.StringIO()):
        n2 = vp.take_reset_files(box, sh, files)
    ok(n2 == 0 and box.resets == 1, "the next boot: nothing to reset (acts once)")
    vp.os.unlink = real_unlink

print("test_pocket_reset: %s" % ("PASS" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
