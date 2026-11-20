#!/usr/bin/env bash
# usbcap-stub-test.sh — vibe-usbcap's REAL path (the I2C_SMBUS ioctls, the clock loop, the access() presence, SIGTERM)
# against the kernel's i2c-stub in the colima VM, with its output read by vibe-hwd's own FastDumpReader. No device.
#   image/r36s/test/usbcap-stub-test.sh
# ★ It loads i2c-stub (chip 0x20) in the colima VM for the run and unloads it after. The registers are set to the h80
#   card's figures (4.170 V, +0.235 A, 4.844 V, cccv, CHRG_IN 0xdd); a fake sysfs holds USB 1-1, which is removed for
#   0.3 s mid-run — the drop. Also a bench: the real interval and CPU in the VM (an M5 core, NOT the R36S's A35).
# ★ No HPRT0 here (no dwc2 in the VM): the sysfs presence is the trigger, as on a card where /dev/mem is refused.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# ★ grep -c, not -q, under pipefail. Already loaded with chip 0x20 (an earlier run of this or of the h80 bench): used as
#   it is and left loaded; loaded by this run: unloaded after.
if [ "$(colima ssh -- lsmod 2>/dev/null | grep -c '^i2c_stub' || true)" -gt 0 ]; then
  CA=$(colima ssh -- cat /sys/module/i2c_stub/parameters/chip_addr | cut -d, -f1)
  [ "$CA" = 32 ] || { echo "!! i2c-stub is loaded with chip $CA, not 0x20 — not touching it"; exit 1; }
  echo "==> i2c-stub already loaded with chip 0x20 — using it, leaving it loaded"
else
  colima ssh -- sudo modprobe i2c-stub chip_addr=0x20
  trap 'colima ssh -- sudo rmmod i2c-stub >/dev/null 2>&1 || true' EXIT
fi
BUS=$(colima ssh -- sh -c 'for d in /sys/bus/i2c/devices/i2c-*; do grep -l "SMBus stub" $d/name >/dev/null 2>&1 && basename $d; done' | head -1)
[ -n "$BUS" ] || { echo "!! no SMBus stub bus"; exit 1; }
echo "==> i2c-stub on /dev/$BUS"
nice -n 10 docker run --rm --cpus 2 --privileged -v /dev:/dev -v "$HERE":/src:ro debian:trixie-slim bash -euo pipefail -c '
  apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev linux-libc-dev python3 >/dev/null
  gcc -O2 -Wall -Wextra -Werror -o /tmp/vibe-usbcap /src/native/vibe-usbcap.c
  python3 - /dev/'"$BUS"' <<"PY"
import ctypes, fcntl, os, sys
class D(ctypes.Union): _fields_ = [("byte", ctypes.c_uint8), ("word", ctypes.c_uint16), ("block", ctypes.c_uint8 * 34)]
class A(ctypes.Structure): _fields_ = [("rw", ctypes.c_uint8), ("cmd", ctypes.c_uint8), ("size", ctypes.c_uint32), ("data", ctypes.POINTER(D))]
fd = os.open(sys.argv[1], os.O_RDWR); fcntl.ioctl(fd, 0x0706, 0x20)
def wb(r, v): d = D(); d.byte = v; fcntl.ioctl(fd, 0x0720, A(0, r, 2, ctypes.pointer(d)))
# VCALIB0 2048, VCALIB1 3584 (k 1123, b 1); BAT_VOL 3712 → 4.170 V; BAT_CUR 1366 → +0.235 A; USB_VOL 3307 → 4.844 V
for r, v in {0x93: 0x08, 0x94: 0x00, 0x95: 0x0E, 0x96: 0x00, 0x78: 0x0E, 0x79: 0x80, 0x7A: 0x05, 0x7B: 0x56,
             0x7E: 0x0C, 0x7F: 0xEB, 0xE4: 0xA0, 0xE5: 0xDD, 0xEB: 0xB3, 0xF0: 0x40, 0xB4: 0x46, 0xDE: 0x09}.items():
    wb(r, v)
PY
  mkdir -p /tmp/fs/bus/usb/devices/1-1
  echo "--- bench: 5000 samples"
  /tmp/vibe-usbcap --i2c /dev/'"$BUS"' --watch 1-1 --sysfs /tmp/fs --samples 5000 > /tmp/bench.txt
  cat /tmp/bench.txt
  echo "--- the drop: USB 1-1 gone for 0.3 s at ~1.5 s, SIGTERM at 3.5 s"
  ( /tmp/vibe-usbcap --i2c /dev/'"$BUS"' --watch 1-1 --sysfs /tmp/fs > /tmp/run.txt ) & P=$!
  sleep 1.5; rmdir /tmp/fs/bus/usb/devices/1-1; sleep 0.3; mkdir /tmp/fs/bus/usb/devices/1-1
  sleep 1.7; kill -TERM $P; wait $P || true
  echo "records: $(wc -l < /tmp/run.txt) — $(cut -d" " -f1 /tmp/run.txt | sort | uniq -c | tr "\n" " ")"
  python3 - /tmp/run.txt /src/rootfs/usr/lib/vibesdr-r36s/vibe-hwd <<"PY"
import importlib.machinery, importlib.util, os, sys, tempfile
os.environ["VIBE_SYSFS"] = tempfile.mkdtemp()
l = importlib.machinery.SourceFileLoader("h", sys.argv[2]); sp = importlib.util.spec_from_loader("h", l)
m = importlib.util.module_from_spec(sp); l.exec_module(m)
L = []; r = m.FastDumpReader(1, logf=lambda *a: L.append(" ".join(map(str, a))))
for ln in open(sys.argv[1]): r.line(ln)
for x in L[:6] + ["…"] + L[-4:]: print("   " + x)
P = F = 0
def ok(c, w):
    global P, F
    P, F = (P + 1, F) if c else (P, F + 1); print("   %s %s" % ("ok  " if c else "FAIL", w))
hd = [x for x in L if "DROP:" in x]
ok(len(hd) == 1 and "USB 1-1 left sysfs" in hd[0], "one DROP from the real binary, on the sysfs presence")
ok(any("bat 4.170V +0.235A port 4.844V" in x for x in L), "the stub registers scale to the h80 card figures")
ok(any("★ USB 1-1 GONE" in x for x in L) and any("★ USB 1-1 BACK" in x for x in L), "GONE and BACK marks")
ok(any(" end — " in x for x in L) and not any("bad record" in x or "unknown record" in x for x in L), "an end line; every record parsed")
ok(any("BOOST on, OTG_SWITCH on" in x for x in L), "POWER_EN3 read (0x46)")
print("   %d passed, %d failed" % (P, F)); sys.exit(1 if F else 0)
PY'
