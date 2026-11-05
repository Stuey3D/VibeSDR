#!/usr/bin/env python3
# fake-keys.py — press the R36S's buttons without a person: virtual evdev devices (uinput) with the REAL devices'
# names, so vibe-hwd / vibe-bootkeys pick them up exactly as they pick up the hardware ones.
#   fake-keys.py volume up|down [N]        a "gpio-keys" device, N presses
#   fake-keys.py power short|long          an "rk805 pwrkey" device: 0.3 s or 2.6 s
#   fake-keys.py pad-hold SECONDS          an "r36s_Gamepad" with Start + Select held for SECONDS
#   fake-keys.py pad-press                 an "r36s_Gamepad", one A press
#   fake-keys.py jack in|out               inject SW_HEADPHONE_INSERT into the real "rk817_ext Headphones" device
import fcntl, glob, os, struct, sys, time

EV_SYN, EV_KEY, EV_SW = 0, 1, 5
def IOW(nr, size): return (1 << 30) | (size << 16) | (ord("U") << 8) | nr
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_SWBIT = IOW(100, 4), IOW(101, 4), IOW(109, 4)
UI_DEV_CREATE, UI_DEV_DESTROY = (ord("U") << 8) | 1, (ord("U") << 8) | 2
EV = struct.Struct("llHHi")

def emit(fd, typ, code, val):
    os.write(fd, EV.pack(0, 0, typ, code, val))
    os.write(fd, EV.pack(0, 0, EV_SYN, 0, 0))

def uinput(name, keys):
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    for k in keys:
        fcntl.ioctl(fd, UI_SET_KEYBIT, k)
    # struct uinput_user_dev: name[80], input_id (4 × u16), ff_effects_max, absmax/min/fuzz/flat [64] each
    dev = name.encode().ljust(80, b"\0") + struct.pack("4HI", 0x19, 1, 1, 1, 0) + b"\0" * (4 * 64 * 4)
    os.write(fd, dev)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(6)          # ★ vibe-hwd rescans input devices every 5 s
    return fd

a = sys.argv[1:]
if a[0] == "volume":
    k = 115 if a[1] == "up" else 114
    fd = uinput("gpio-keys", [114, 115])
    for _ in range(int(a[2]) if len(a) > 2 else 1):
        emit(fd, EV_KEY, k, 1); time.sleep(0.05); emit(fd, EV_KEY, k, 0); time.sleep(0.2)
elif a[0] == "power":
    fd = uinput("rk805 pwrkey", [116])
    emit(fd, EV_KEY, 116, 1); time.sleep(0.3 if a[1] == "short" else 2.6); emit(fd, EV_KEY, 116, 0)
elif a[0] == "pad-hold":
    fd = uinput("r36s_Gamepad", [0x130, 0x131, 0x13A, 0x13B])
    emit(fd, EV_KEY, 0x13A, 1); emit(fd, EV_KEY, 0x13B, 1)
    time.sleep(float(a[1])); emit(fd, EV_KEY, 0x13A, 0); emit(fd, EV_KEY, 0x13B, 0)
elif a[0] == "pad-press":
    fd = uinput("r36s_Gamepad", [0x130, 0x131, 0x13A, 0x13B])
    emit(fd, EV_KEY, 0x131, 1); time.sleep(0.1); emit(fd, EV_KEY, 0x131, 0)
elif a[0] == "jack":
    for p in glob.glob("/sys/class/input/event*"):
        if open(p + "/device/name").read().strip() == "rk817_ext Headphones":
            fd = os.open("/dev/input/" + os.path.basename(p), os.O_WRONLY)
            emit(fd, EV_SW, 2, 1 if a[1] == "in" else 0)
            break
    else:
        sys.exit("no rk817_ext Headphones device")
time.sleep(1)
