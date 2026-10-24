#!/usr/bin/env python3
# ★★★ A PORTABLE UPDATES FULLY WITHOUT A NEW CARD (Stuart, 2026-10-13: "after initial setup we need to make sure that
#     VibeServer Portables can be fully updated without needing a full ISO again").
#
# vibeserver-pocket-converge against a FAKE ROOT (a temp dir shaped like Raspberry Pi OS Lite: etc/, boot/firmware/,
# lib/systemd/system, var/lib/dpkg/status) and a fake systemctl that makes the same symlinks the real one does. Proves:
#   · NOTHING happens without the pocket marker (an ordinary VibeServer is untouched — same files, same bytes);
#   · a box as Lite ships it is brought to the image's state: our drop-ins, avahi on every interface, units on/off/
#     masked, config.txt's pocket block ONCE, /boot read-only in fstab;
#   · IDEMPOTENT: the second run changes nothing and says "already converged";
#   · a box flashed by the 11.0.0 image (block already there) gets only what a NEW release adds, once;
#   · an owner's own change to a ONCE step (getty@tty1 unmasked, gpu_mem raised) survives every later run;
#   · pocket-packages.txt drives the missing-package list (what Update installs);
#   · it refuses to "apply" onto the live read-only overlay (the change would vanish), and --check never writes;
#   · vibeserver-maintenance, with apt and overlayroot-chroot stubbed: an Update on a Portable installs the missing
#     manifest packages and runs the converge; on an ordinary install it does neither.
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
LINUX = os.path.dirname(HERE)
CONVERGE = os.path.join(HERE, "vibeserver-pocket-converge")
MAINT = os.path.join(LINUX, "vibeserver-maintenance")

fails = 0
count = 0


def ok(cond, what):
    global fails, count
    count += 1
    print("  %s %s" % ("\033[32mok\033[0m  " if cond else "\033[31mFAIL\033[0m", what))
    if not cond:
        fails += 1


FAKE_SYSTEMCTL = r'''#!/usr/bin/env python3
# A fake systemctl: --root=R enable|disable|mask UNIT — the symlinks the real one makes, and a call log.
import os, sys
args = sys.argv[1:]
root = "/"
if args and args[0].startswith("--root="):
    root = args.pop(0)[7:]
verb, units = args[0], args[1:]
etc = os.path.join(root, "etc/systemd/system")
with open(os.path.join(root, ".systemctl.log"), "a") as f:
    f.write(" ".join([verb] + units) + "\n")
for u in units:
    if verb == "enable":
        tmpl = u.split("@")[0] + "@" + u.rsplit(".", 1)[1] if "@" in u else u
        wants = os.path.join(etc, "getty.target.wants" if u.startswith("getty@") else "multi-user.target.wants")
        os.makedirs(wants, exist_ok=True)
        l = os.path.join(wants, u)
        if not os.path.lexists(l):
            os.symlink("/lib/systemd/system/" + tmpl, l)
    elif verb == "disable":
        for d in os.listdir(etc):
            l = os.path.join(etc, d, u)
            if d.endswith((".wants", ".requires")) and os.path.lexists(l):
                os.unlink(l)
    elif verb == "mask":
        l = os.path.join(etc, u)
        if os.path.lexists(l):
            os.unlink(l)
        os.symlink("/dev/null", l)
    else:
        sys.exit(1)
'''

LITE_CONFIG_TXT = """# For more options and information see
# http://rptl.io/configtxt
dtparam=audio=on
camera_auto_detect=1
display_auto_detect=1
auto_initramfs=1
dtoverlay=vc4-kms-v3d
max_framebuffers=2
arm_boost=1

[cm4]
otg_mode=1

[all]
"""

LITE_FSTAB = """proc            /proc           proc    defaults          0       0
PARTUUID=abcd-01  /boot/firmware  vfat    defaults          0       2
PARTUUID=abcd-02  /               ext4    defaults,noatime  0       1
"""

AVAHI = """[server]
use-ipv4=yes
allow-interfaces=wlan0
#deny-interfaces=eth1
[wide-area]
enable-wide-area=yes
"""

LITE_UNITS = ["NetworkManager.service", "avahi-daemon.service", "bluetooth.service", "hciuart.service",
              "ModemManager.service", "triggerhappy.service", "triggerhappy.socket", "apt-daily.timer",
              "apt-daily-upgrade.timer", "man-db.timer", "e2scrub_all.timer", "keyboard-setup.service",
              "getty@.service", "serial-getty@.service", "udisks2.service", "nftables.service",
              "dphys-swapfile.service", "fake-hwclock.service", "rpi-display-backlight.service",
              "vibeserver.service", "vibeserver-radios.service", "vibeserver-maintenance.path",
              "vibeserver-pocket.service", "vibeserver-pocket-data.service", "vibeserver-pocket-seal.service",
              "vibeserver-pocket-cpu.service"]
LITE_ENABLED = ["NetworkManager.service", "avahi-daemon.service", "bluetooth.service", "hciuart.service",
                "ModemManager.service", "triggerhappy.service", "apt-daily.timer", "man-db.timer",
                "keyboard-setup.service", "dphys-swapfile.service"]


def dpkg_status(pkgs):
    return "".join("Package: %s\nStatus: install ok installed\nArchitecture: arm64\n\n" % p for p in pkgs)


ALL_PKGS = ["network-manager", "dnsmasq-base", "nftables", "iw", "rfkill", "python3", "ca-certificates", "curl",
            "gnupg", "avahi-daemon", "ffmpeg", "overlayroot", "initramfs-tools", "vibeserver"]


def make_root(tmp, marker=True, pkgs=ALL_PKGS):
    r = os.path.join(tmp, "root")
    for d in ("etc/systemd/system/multi-user.target.wants", "etc/avahi", "etc/apt/sources.list.d",
              "boot/firmware", "lib/systemd/system", "var/lib/dpkg", "usr/share/keyrings", "usr/lib/vibeserver"):
        os.makedirs(os.path.join(r, d), exist_ok=True)
    for u in LITE_UNITS:
        open(os.path.join(r, "lib/systemd/system", u), "w").write("[Unit]\n")
    for u in LITE_ENABLED:
        os.symlink("/lib/systemd/system/" + u, os.path.join(r, "etc/systemd/system/multi-user.target.wants", u))
    os.makedirs(os.path.join(r, "etc/systemd/system/getty.target.wants"))
    os.symlink("/lib/systemd/system/getty@.service", os.path.join(r, "etc/systemd/system/getty.target.wants/getty@tty1.service"))
    open(os.path.join(r, "boot/firmware/config.txt"), "w").write(LITE_CONFIG_TXT)
    open(os.path.join(r, "etc/fstab"), "w").write(LITE_FSTAB)
    open(os.path.join(r, "etc/avahi/avahi-daemon.conf"), "w").write(AVAHI)
    open(os.path.join(r, "var/lib/dpkg/status"), "w").write(dpkg_status(pkgs))
    open(os.path.join(r, "usr/share/keyrings/vibesdr.gpg"), "wb").write(b"key")
    shutil.copy(os.path.join(HERE, "pocket-packages.txt"), os.path.join(r, "usr/lib/vibeserver/pocket-packages.txt"))
    if marker:
        os.makedirs(os.path.join(r, "etc/vibeserver-pocket"))
        open(os.path.join(r, "etc/vibeserver-pocket/enabled"), "w").write("1\n")
    sc = os.path.join(tmp, "systemctl")
    open(sc, "w").write(FAKE_SYSTEMCTL)
    os.chmod(sc, 0o755)
    mounts = os.path.join(tmp, "mounts")
    open(mounts, "w").write("/dev/mmcblk0p2 / ext4 rw 0 0\n")
    return r, sc, mounts


def converge(root, sc, mounts, *args, extra_env=None):
    env = dict(os.environ, VIBE_POCKET_ROOT=root, SYSTEMCTL=sc, VIBE_POCKET_PROC_MOUNTS=mounts)
    env.update(extra_env or {})
    p = subprocess.run([sys.executable, CONVERGE] + list(args), env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True)
    return p.returncode, p.stdout


def snapshot(root):
    """Every path under the root with its content (or link target) — 'nothing changed' means THIS is equal."""
    out = {}
    for dp, dns, fns in os.walk(root):
        for n in dns + fns:
            full = os.path.join(dp, n)
            rel = os.path.relpath(full, root)
            if rel == ".systemctl.log":
                continue
            if os.path.islink(full):
                out[rel] = "-> " + os.readlink(full)
            elif os.path.isfile(full):
                out[rel] = open(full, "rb").read()
            else:
                out[rel] = "dir"
    return out


def F(root, rel):
    try:
        return open(os.path.join(root, rel)).read()
    except OSError:
        return None


def calls(root):
    return F(root, ".systemctl.log") or ""


def main():
    # ── no marker: an ordinary VibeServer is never touched ──
    with tempfile.TemporaryDirectory() as tmp:
        r, sc, m = make_root(tmp, marker=False)
        before = snapshot(r)
        rc, out = converge(r, sc, m)
        ok(rc == 0 and "not a VibeServer Portable" in out, "no marker: says so and exits 0")
        ok(snapshot(r) == before and calls(r) == "", "no marker: not one file, link or unit touched")
        rc, out = converge(r, sc, m, "--missing-packages")
        ok(rc == 0 and out.strip() == "", "no marker: --missing-packages lists nothing (maintenance installs nothing)")
        rc, out = converge(r, sc, m, "--check")
        ok(rc == 0, "no marker: --check is clean")

    # ── a box as Lite ships it, marker on: the image's state ──
    with tempfile.TemporaryDirectory() as tmp:
        r, sc, m = make_root(tmp)
        before = snapshot(r)
        rc, out = converge(r, sc, m, "--check")
        ok(rc == 1 and "pending:" in out, "--check on an unconverged box: pending items, exit 1")
        ok(snapshot(r) == before and calls(r) == "", "--check writes nothing and runs no systemctl")
        rc, out = converge(r, sc, m)
        ok(rc == 0 and "change(s)" in out, "apply: reports its changes")
        ok("Storage=volatile" in (F(r, "etc/systemd/journald.conf.d/60-vibeserver-pocket.conf") or ""), "journald in RAM")
        ok("Mechanism=zram" in (F(r, "etc/rpi/swap.conf.d/60-vibeserver-pocket.conf") or ""), "zram swap only")
        # ★ Whole lines, not substrings (CodeQL py/incomplete-url-substring-sanitization, 2026-10-14): the host must be
        #   the URL's host, not merely appear somewhere in the file.
        ok(re.search(r"^uri=http://nmcheck\.gnome\.org/check_network_status\.txt$",
                     F(r, "etc/NetworkManager/conf.d/50-vibeserver-connectivity.conf") or "", re.M) is not None,
           "NetworkManager connectivity check (the tunnel's gate)")
        ok(re.search(r"^deb \S*.* https://apt\.vibesdr\.net stable main$",
                     F(r, "etc/apt/sources.list.d/vibesdr.list") or "", re.M) is not None, "a missing apt source is restored")
        av = F(r, "etc/avahi/avahi-daemon.conf")
        ok("\n#allow-interfaces=wlan0  # off" in av and "\nallow-interfaces" not in av, "avahi: allow-interfaces switched off")
        ok("#deny-interfaces=eth1\n" in av, "avahi: an already-commented line is left as it was")
        wants = os.path.join(r, "etc/systemd/system/multi-user.target.wants")
        for u in ("vibeserver.service", "vibeserver-pocket.service", "vibeserver-maintenance.path",
                  "vibeserver-pocket-data.service", "vibeserver-pocket-seal.service", "vibeserver-radios.service",
                  "vibeserver-pocket-cpu.service"):
            ok(os.path.lexists(os.path.join(wants, u)), "enabled: " + u)
        for u in ("bluetooth.service", "ModemManager.service", "apt-daily.timer", "dphys-swapfile.service"):
            ok(not os.path.lexists(os.path.join(wants, u)), "disabled: " + u)
        for u in ("udisks2.service", "getty@tty1.service", "serial-getty@ttyAMA0.service"):
            ok(os.readlink(os.path.join(r, "etc/systemd/system", u)) == "/dev/null", "masked: " + u)
        ok(not os.path.lexists(os.path.join(r, "etc/systemd/system/getty.target.wants/getty@tty1.service")),
           "getty@tty1 disabled as well as masked")
        ct = F(r, "boot/firmware/config.txt")
        for line in ("gpu_mem=16", "dtoverlay=disable-bt", "dtparam=audio=off", "disable_splash=1"):
            ok(("\n%s\n" % line) in ct, "config.txt: " + line)
        ok(ct.count("auto_initramfs=1") == 1, "config.txt: auto_initramfs already set by Lite — not added twice")
        ok("#dtoverlay=vc4-kms-v3d  # off" in ct and "#dtparam=audio=on" in ct and "#camera_auto_detect=1" in ct,
           "config.txt: KMS, onboard audio and camera probe commented out (never deleted)")
        ok(ct.count("# VibeServer pocket") == 1, "config.txt: ONE pocket block")
        ok(ct.index("# VibeServer pocket") > ct.index("[cm4]") and "[all]\ngpu_mem=16" in ct,
           "config.txt: the block sits under [all], not inside the [cm4] filter")
        ok(F(r, "boot/firmware/config.txt.bak-pocket") == LITE_CONFIG_TXT, "config.txt: Lite's original kept as .bak-pocket")
        ok("/boot/firmware  vfat    defaults,ro " in F(r, "etc/fstab"), "fstab: the boot partition read-only")
        ok(os.path.isdir(os.path.join(r, "data")) and os.path.isdir(os.path.join(r, "var/lib/vibeserver-pocket")),
           "/data and /var/lib/vibeserver-pocket exist (the settings partition's mount points)")
        ok("disable:bluetooth.service" in F(r, "etc/vibeserver-pocket/converge.done"), "the ONCE steps are ledgered")

        # ★★★ IDEMPOTENT
        before = snapshot(r)
        n_calls = calls(r)
        rc, out = converge(r, sc, m)
        ok(rc == 0 and "already converged" in out, "second run: 'already converged'")
        ok(snapshot(r) == before, "second run: not one byte or link changed")
        ok(calls(r) == n_calls, "second run: no systemctl call at all")
        rc, out = converge(r, sc, m, "--check")
        ok(rc == 0 and "converged" in out, "--check after apply: converged, exit 0")
        for _ in range(3):
            converge(r, sc, m)
        ok(snapshot(r) == before, "five runs = one run")

        # ★★ THE OWNER'S OWN CHANGE TO A ONCE STEP STANDS
        os.unlink(os.path.join(r, "etc/systemd/system/getty@tty1.service"))               # unmask the HDMI console
        ct = F(r, "boot/firmware/config.txt").replace("gpu_mem=16", "gpu_mem=64")
        open(os.path.join(r, "boot/firmware/config.txt"), "w").write(ct)
        rc, out = converge(r, sc, m)
        ok(not os.path.lexists(os.path.join(r, "etc/systemd/system/getty@tty1.service")), "an owner's unmasked getty@tty1 stays unmasked")
        ok("gpu_mem=64" in F(r, "boot/firmware/config.txt") and "gpu_mem=16" not in F(r, "boot/firmware/config.txt"),
           "an owner's gpu_mem stays theirs")
        rc, out = converge(r, sc, m, "--check")
        ok(rc == 0, "…and --check does not call the owner's choice drift")
        # ★ …but an ENFORCED step comes back
        os.unlink(os.path.join(wants, "vibeserver-pocket.service"))
        open(os.path.join(r, "etc/systemd/journald.conf.d/60-vibeserver-pocket.conf"), "w").write("[Journal]\nStorage=persistent\n")
        rc, out = converge(r, sc, m)
        ok(os.path.lexists(os.path.join(wants, "vibeserver-pocket.service")), "our own unit is re-enabled (ENFORCED)")
        ok("Storage=volatile" in F(r, "etc/systemd/journald.conf.d/60-vibeserver-pocket.conf"), "our own drop-in is restored (ENFORCED)")

        # ★★★ THE LIVE READ-ONLY OVERLAY: refused, nothing written
        open(m, "w").write("/dev/mmcblk0p2 /media/root-ro ext4 ro 0 0\noverlayroot / overlay rw 0 0\n")
        # ★ ROOT=/ is the only case the guard applies to, and on this Mac / has no marker — so the guard is exercised
        #   through the module directly.
        import importlib.machinery, importlib.util
        loader = importlib.machinery.SourceFileLoader("pc", CONVERGE)
        spec = importlib.util.spec_from_loader("pc", loader)
        pc = importlib.util.module_from_spec(spec)
        loader.exec_module(pc)
        pc.ROOT, pc.PROC_MOUNTS = "/", m
        ok(pc.live_overlay_root() is True, "the live overlay root is recognised (/ is 'overlay')")
        open(m, "w").write("/dev/mmcblk0p2 / ext4 rw 0 0\n")
        ok(pc.live_overlay_root() is False, "overlayroot-chroot's view (/ is the real ext4) is not refused")
        pc.ROOT = r
        ok(pc.live_overlay_root() is False, "a fake/image root is never refused")

    # ── a box flashed from the 11.0.0 image: everything there; a NEW release adds one boot line + one package ──
    with tempfile.TemporaryDirectory() as tmp:
        pkgs = [p for p in ALL_PKGS if p not in ("ffmpeg", "iw")]
        r, sc, m = make_root(tmp, pkgs=pkgs)
        old_block = LITE_CONFIG_TXT + "\n# VibeServer pocket — headless radio box\ngpu_mem=16\ndtoverlay=disable-bt\ndtparam=audio=off\n"
        open(os.path.join(r, "boot/firmware/config.txt"), "w").write(old_block)
        rc, out = converge(r, sc, m, "--missing-packages")
        ok(out.strip() == "iw ffmpeg", "manifest-driven: the missing packages, in manifest order (%r)" % out.strip())
        rc, out = converge(r, sc, m, "--check")
        ok(rc == 1 and "system packages missing (Update installs them): iw ffmpeg" in out, "--check names the missing packages")
        rc, out = converge(r, sc, m)
        ct = F(r, "boot/firmware/config.txt")
        ok(ct.count("# VibeServer pocket") == 2 and "added by an update\n[all]\ndisable_splash=1\n" in ct,
           "an image-era block gets ONLY the line a later release added (disable_splash), under [all]")
        ok("dtparam=audio=on" in ct and "#dtparam=audio=on" not in ct,
           "…and lines outside our block are not re-edited on an already-pocket config.txt")
        before = F(r, "boot/firmware/config.txt")
        converge(r, sc, m)
        ok(F(r, "boot/firmware/config.txt") == before, "…once")
        # the manifest grows
        with open(os.path.join(r, "usr/lib/vibeserver/pocket-packages.txt"), "a") as f:
            f.write("rtl-sdr   # a future need\n")
        rc, out = converge(r, sc, m, "--missing-packages")
        ok(out.strip() == "iw ffmpeg rtl-sdr", "a line added to pocket-packages.txt is missing until installed")
        open(os.path.join(r, "var/lib/dpkg/status"), "a").write(
            dpkg_status(["iw", "ffmpeg"]) + "Package: rtl-sdr\nStatus: deinstall ok config-files\n\n")
        rc, out = converge(r, sc, m, "--missing-packages")
        ok(out.strip() == "rtl-sdr", "installed packages drop out; a removed one (config-files only) is still missing")

    # ── no config.txt visible (overlayroot-chroot without the boot partition) ──
    with tempfile.TemporaryDirectory() as tmp:
        r, sc, m = make_root(tmp)
        os.unlink(os.path.join(r, "boot/firmware/config.txt"))
        rc, out = converge(r, sc, m)
        ok(rc == 0 and "no config.txt visible" in out, "no boot partition in view: said, not failed")
        ok("boot:gpu_mem=16" not in (F(r, "etc/vibeserver-pocket/converge.done") or ""),
           "…and the boot lines are NOT ledgered, so the run that can see it still adds them")
        open(os.path.join(r, "boot/firmware/config.txt"), "w").write(LITE_CONFIG_TXT)
        converge(r, sc, m)
        ok("\ngpu_mem=16\n" in F(r, "boot/firmware/config.txt"), "…which it does")

    # ── Bookworm's "moved to /boot/firmware" stub must never be mistaken for config.txt ──
    with tempfile.TemporaryDirectory() as tmp:
        r, sc, m = make_root(tmp)
        os.unlink(os.path.join(r, "boot/firmware/config.txt"))         # the partition not mounted in this chroot
        stub = "DO NOT EDIT THIS FILE\n\nThe file you are looking for has moved to /boot/firmware/config.txt\n"
        open(os.path.join(r, "boot/config.txt"), "w").write(stub)
        rc, out = converge(r, sc, m)
        ok(F(r, "boot/config.txt") == stub and "no config.txt visible" in out,
           "the /boot/config.txt stub is left alone while /boot/firmware exists (lines not lost to it)")

    maintenance_tests()

    print()
    if fails:
        print("\033[31m%d of %d failed\033[0m" % (fails, count))
        return 1
    print("\033[32mall %d passed\033[0m" % count)
    return 0


def maintenance_tests():
    """vibeserver-maintenance's `update` with every system tool stubbed on PATH and its fixed paths rewritten into a
    temp dir. Proves the order (vibeserver first, then the manifest, then the converge), that it all goes through
    apt_do (overlayroot-chroot on a sealed box), and that an ordinary install does exactly what it did before."""
    bash = shutil.which("bash")
    for portable, sealed in ((True, True), (True, False), (False, False)):
        with tempfile.TemporaryDirectory() as tmp:
            r, sc, m = make_root(tmp, marker=portable, pkgs=[p for p in ALL_PKGS if p != "ffmpeg"])
            bin_ = os.path.join(tmp, "bin")
            os.makedirs(bin_)
            log = os.path.join(tmp, "calls.log")
            stub = "#!/bin/sh\necho \"$(basename \"$0\") $*\" >> %s\n" % log
            # ★ the real root as overlayroot-chroot sees it (its lowerdir), with the empty dirs the binds land on
            lower = os.path.join(tmp, "lower")
            for d in ("dev", "boot/firmware"):
                os.makedirs(os.path.join(lower, d))
            mounts = os.path.join(tmp, "mounts")
            open(mounts, "w").write("/dev/mmcblk0p2 %s ext4 ro 0 0\n/dev/mmcblk0p1 /boot/firmware vfat ro 0 0\n"
                                    "overlayroot / overlay rw,lowerdir=%s,upperdir=/media/root-rw/overlay 0 0\n" % (lower, lower))
            for tool in ("apt-get", "systemctl", "mount", "umount", "sync", "mountpoint", "findmnt", "chgrp"):
                p = os.path.join(bin_, tool)
                # ★ mountpoint: the live boot partition and the lowerdir are mounts; the binds' targets are not yet
                body = stub
                if tool == "mountpoint":
                    body += 'for a; do case "$a" in /boot/firmware|%s) exit 0;; -*) ;; *) exit 1;; esac; done\nexit 1\n' % lower
                open(p, "w").write(body)
                os.chmod(p, 0o755)
            # ★ overlayroot-chroot: log, then run the command (the "real root" here is the fake root)
            p = os.path.join(bin_, "overlayroot-chroot")
            open(p, "w").write(stub + "exec \"$@\"\n")
            os.chmod(p, 0o755)
            # ★ the converge as the box has it, pointed at the fake root
            conv = os.path.join(tmp, "vibeserver-pocket-converge")
            open(conv, "w").write("#!/bin/sh\necho \"converge $*\" >> %s\nVIBE_POCKET_ROOT=%s SYSTEMCTL=%s VIBE_POCKET_PROC_MOUNTS=%s exec %s %s \"$@\"\n"
                                  % (log, r, sc, m, sys.executable, CONVERGE))
            os.chmod(conv, 0o755)
            req = os.path.join(tmp, "maintenance.request")
            open(req, "w").write("update")
            cmdline = os.path.join(tmp, "cmdline")
            open(cmdline, "w").write("console=tty1 root=PARTUUID=x" + (" overlayroot=tmpfs" if sealed else "") + "\n")
            s = open(MAINT).read()
            s = (s.replace("/var/lib/vibeserver/maintenance.request", req)
                  .replace("/var/lib/vibeserver-maint", os.path.join(tmp, "maint"))
                  .replace("/var/lib/vibeserver/maintenance.log", os.path.join(tmp, "old.log"))
                  .replace("/proc/cmdline", cmdline)
                  .replace("/proc/mounts", mounts)
                  .replace("/etc/vibeserver-pocket/enabled", os.path.join(r, "etc/vibeserver-pocket/enabled"))
                  .replace("/usr/lib/vibeserver/vibeserver-pocket-converge", conv)
                  # ★ macOS dd has no iflag=nofollow,nonblock — the read itself is not what is under test here
                  .replace('dd if="$REQ" bs=32 count=1 iflag=nofollow,nonblock', 'head -c 32 "$REQ"'))
            ok('head -c 32 "$REQ"' in s, "(test harness: the request read was rewritten for this OS)")
            ms = os.path.join(tmp, "maint.sh")
            open(ms, "w").write(s)
            env = dict(os.environ, PATH=bin_ + os.pathsep + os.environ["PATH"])
            p = subprocess.run([bash, ms], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
            c = open(log).read() if os.path.exists(log) else ""
            tag = "%s%s" % ("Portable" if portable else "ordinary install", ", sealed" if sealed else "")
            if os.environ.get("CONVERGE_DEBUG"):
                print(p.stdout, "----", c)
            ok("__VIBESERVER_DONE__ 0" in p.stdout, "%s: update finishes rc 0" % tag)
            ok("apt-get install -y --only-upgrade vibeserver" in c, "%s: vibeserver upgraded" % tag)
            if portable:
                i_up = c.index("--only-upgrade vibeserver")
                i_pk = c.find("apt-get install -y --no-install-recommends ffmpeg")
                i_cv = c.rfind("converge \n") if "converge \n" in c else c.rfind("converge")
                ok(i_pk > i_up, "%s: the missing manifest package (ffmpeg) installed AFTER the vibeserver upgrade" % tag)
                ok("converge --missing-packages" in c and i_cv > i_pk, "%s: then the converge runs" % tag)
                ok(os.path.lexists(os.path.join(r, "etc/systemd/system/multi-user.target.wants/vibeserver-pocket.service")),
                   "%s: the converge really applied (vibeserver-pocket enabled on the root)" % tag)
                if sealed:
                    ok(c.count("overlayroot-chroot") >= 4, "%s: all of it through overlayroot-chroot (the REAL root)" % tag)
                    ok("systemctl reboot" in c and c.index("systemctl reboot") > i_cv, "%s: and THEN the reboot" % tag)
                    # ★★ Stuart's Portable, 2026-10-13: mounting the boot device INSIDE the chroot failed (no /dev there).
                    #    /dev and the boot partition are bound onto the lowerdir from OUTSIDE, before every chroot, and
                    #    unmounted after it.
                    bd, bb = "mount --bind /dev %s/dev" % lower, "mount --bind /boot/firmware %s/boot/firmware" % lower
                    i_ch = c.index("overlayroot-chroot")
                    ok(bd in c and c.index(bd) < i_ch, "%s: /dev bound into the real root before the chroot" % tag)
                    ok(bb in c and c.index(bb) < i_ch, "%s: the boot partition bound into the real root before the chroot" % tag)
                    ok(c.count(bd) == c.count("overlayroot-chroot") and c.count("umount %s/dev" % lower) == c.count(bd),
                       "%s: bound for EVERY chroot, and unmounted after each (%d)" % (tag, c.count(bd)))
                    ok(c.count("umount %s/boot/firmware" % lower) == c.count(bb), "%s: the boot bind undone each time" % tag)
                    ok("could not be" not in p.stdout, "%s: no 'could not be mounted' line" % tag)
                else:
                    ok("overlayroot-chroot" not in c and "systemctl reboot" not in c, "%s: no chroot, no reboot" % tag)
                    ok("--bind" not in c, "%s: nothing bound" % tag)
            else:
                ok("--no-install-recommends" not in c and "converge" not in c and "overlayroot-chroot" not in c,
                   "%s: no manifest, no converge, no chroot — exactly as before" % tag)
                ok(c.count("apt-get") == 2, "%s: apt-get update + the one upgrade, nothing else" % tag)


if __name__ == "__main__":
    sys.exit(main())
