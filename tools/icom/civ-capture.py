#!/usr/bin/env python3
"""civ-capture.py — Phase 0 of Icom support (docs/ICOM-SUPPORT-PLAN.md §7): record what an Icom sends.

READ-ONLY plus "scope on". It sends exactly these CI-V frames and nothing else:
    19 00   (who are you)        03   (frequency)       04   (mode)       15 02   (S-meter)
    27 10 01  (scope on)         27 11 01  (scope data to CI-V on)      and on exit  27 11 00
and, ONLY with --menu-scan, menu READS 1A 05 <item> (two BCD bytes, no value — a read, never a write).

Every frame passes the same whole-frame allow-list as VibeServer (civ_codec.h, plan §3.2) before it is
written; anything else is refused and the program stops. There is no code here that can build a transmit
command.

DTR and RTS are set LOW before the port is opened and again straight after (plan §3.3). On Windows that
means they are never raised. On Linux and macOS the operating system raises them for an instant as the port
opens — so CHECK THE RADIO'S MENU FIRST (see README.md) and run this with the radio into a DUMMY LOAD.

Output (in --out, default ./icom-capture-<time>/):
    capture.civhex   one frame per line: "<ms> <dir> <hex>", '<' radio->us, '>' us->radio
                     (this is the format of VibeServer's test fixtures: test/fixtures/icom/)
    capture.bin      every byte the radio sent, raw
    summary.txt      model, sweeps per second, frames, anything odd

usage:  python3 civ-capture.py --port /dev/ttyUSB0 [--baud 115200] [--civ 94] [--seconds 300] [--menu-scan]
needs:  pip install pyserial
"""
import argparse
import os
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("needs pyserial:  pip install pyserial")

CONTROLLER = 0xE0


def bcd_ok(b):
    return (b >> 4) <= 9 and (b & 0x0F) <= 9


def may_transmit(frame, radio, menu_scan):
    """The allow-list, as plan §3.2 — whole frame, exact data length. Only the shapes this tool uses."""
    if len(frame) < 6 or frame[0] != 0xFE or frame[1] != 0xFE or frame[-1] != 0xFD:
        return False
    if any(b in (0xFE, 0xFD) for b in frame[2:-1]):
        return False
    if frame[2] != radio or frame[3] != CONTROLLER:
        return False
    body = bytes(frame[4:-1])
    allowed = {bytes([0x19, 0x00]), bytes([0x03]), bytes([0x04]), bytes([0x15, 0x02]),
               bytes([0x27, 0x10, 0x01]), bytes([0x27, 0x11, 0x01]), bytes([0x27, 0x11, 0x00])}
    if body in allowed:
        return True
    # 1A 05 <item hi> <item lo> — a menu READ: exactly two BCD item bytes and NOTHING after them.
    if menu_scan and len(body) == 4 and body[0] == 0x1A and body[1] == 0x05 and bcd_ok(body[2]) and bcd_ok(body[3]):
        return True
    return False


def frame(radio, *body):
    return bytes([0xFE, 0xFE, radio, CONTROLLER, *body, 0xFD])


class Capture:
    def __init__(self, args):
        self.args = args
        self.radio = int(args.civ, 16)
        os.makedirs(args.out, exist_ok=True)
        self.hex = open(os.path.join(args.out, "capture.civhex"), "w")
        self.raw = open(os.path.join(args.out, "capture.bin"), "wb")
        self.t0 = time.monotonic()
        self.buf = bytearray()
        self.frames_in = 0
        self.sweeps = 0
        self.sweep_times = []
        self.replies = {}
        self.hex.write(f"# civ-capture.py  port={args.port} baud={args.baud} civ={args.civ}  {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        self.hex.write("# format: <ms> <dir> <hex>  — '<' radio->us, '>' us->radio\n")

        s = serial.Serial()
        s.port = args.port
        s.baudrate = args.baud
        s.bytesize = 8
        s.parity = "N"
        s.stopbits = 1
        s.timeout = 0.02
        s.rtscts = False      # ★ no hardware flow control: it DRIVES RTS
        s.dsrdtr = False
        s.dtr = False         # ★ set BEFORE open — pyserial applies them as the port opens
        s.rts = False
        s.open()
        try:                  # ★ and again, the instant it is open
            s.dtr = False
            s.rts = False
        except OSError as e:  # a pseudo-terminal (tools/icom/fake-icom) has no modem lines at all
            if e.errno not in (22, 25):
                raise
        self.s = s

    def ms(self):
        return (time.monotonic() - self.t0) * 1000.0

    def send(self, f):
        if not may_transmit(f, self.radio, self.args.menu_scan):
            # ★ This cannot happen with the frames above; if it ever does, stop — never "send anyway".
            self.close()
            sys.exit("REFUSED to send " + f.hex(" ") + " — not on the allow-list. Nothing was sent.")
        self.hex.write(f"{self.ms():.1f} > {f.hex(' ').upper()}\n")
        self.s.write(f)

    def pump(self, seconds):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            data = self.s.read(512)
            if not data:
                continue
            self.raw.write(data)
            self.buf += data
            self.split()

    def split(self):
        while True:
            start = self.buf.find(b"\xFE\xFE")
            if start < 0:
                self.buf.clear()
                return
            end = self.buf.find(b"\xFD", start + 2)
            if end < 0:
                if start:
                    del self.buf[:start]
                return
            f = bytes(self.buf[start:end + 1])
            del self.buf[:end + 1]
            # strip a run of FE (sync) at the front
            while len(f) > 2 and f[2] == 0xFE:
                f = f[1:]
            self.frames_in += 1
            self.hex.write(f"{self.ms():.1f} < {f.hex(' ').upper()}\n")
            if len(f) >= 6 and f[3] == self.radio:
                cmd = f[4]
                if cmd == 0x27 and len(f) > 8 and f[5] == 0x00:
                    seq, total = f[7], f[8]
                    if seq == total:
                        self.sweeps += 1
                        self.sweep_times.append(time.monotonic())
                else:
                    self.replies[cmd] = f

    def ask(self, *body, wait=0.4):
        self.send(frame(self.radio, *body))
        self.pump(wait)

    def close(self):
        try:
            self.hex.flush(); self.raw.flush()
            self.s.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(description="Record an Icom's CI-V (read-only + scope on). Radio into a DUMMY LOAD.")
    ap.add_argument("--port", required=True, help="/dev/ttyUSB0, /dev/cu.SLAB_USBtoUART, COM3 …")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--civ", default="94", help="the radio's CI-V address in hex (IC-7300: 94)")
    ap.add_argument("--seconds", type=int, default=300, help="how long to record the scope (default 5 minutes)")
    ap.add_argument("--menu-scan", action="store_true",
                    help="also READ menu items 0001-0110 (1A 05, reads only) so their numbers can be matched to the radio's menu")
    ap.add_argument("--out", default="icom-capture-" + time.strftime("%Y%m%d-%H%M%S"))
    args = ap.parse_args()

    print("Radio into a DUMMY LOAD? USB SEND and USB Keying (CW/RTTY) OFF, VOX OFF? (README.md)  Starting in 5 s — Ctrl-C to stop.")
    time.sleep(5)
    c = Capture(args)
    lines = []
    try:
        c.ask(0x19, 0x00)
        idf = c.replies.get(0x19)
        lines.append("ID reply: " + (idf.hex(" ").upper() if idf else "NONE — check baud / CI-V address / 'Unlink from [REMOTE]'"))
        c.ask(0x03)
        c.ask(0x04)
        c.ask(0x15, 0x02)
        if args.menu_scan:
            print("menu scan (reads only) …")
            for item in range(1, 111):
                hi, lo = item // 100, item % 100
                c.ask(0x1A, 0x05, (hi // 10) << 4 | (hi % 10), (lo // 10) << 4 | (lo % 10), wait=0.15)
        c.ask(0x27, 0x10, 0x01)
        c.ask(0x27, 0x11, 0x01)
        print(f"recording the scope for {args.seconds} s — tune the radio, change span, switch centre/fixed …")
        t_end = time.monotonic() + args.seconds
        next_meter = 0.0
        while time.monotonic() < t_end:
            if time.monotonic() >= next_meter:          # an S-meter read every 2 s, as a poll would
                c.send(frame(c.radio, 0x15, 0x02))
                next_meter = time.monotonic() + 2.0
            c.pump(0.2)
    except KeyboardInterrupt:
        print("stopped")
    finally:
        try:
            c.send(frame(c.radio, 0x27, 0x11, 0x00))  # ★ scope data output back off
            c.pump(0.3)
        finally:
            c.close()
    st = c.sweep_times
    rate = (len(st) - 1) / (st[-1] - st[0]) if len(st) > 2 else 0.0
    lines += [f"frames from the radio: {c.frames_in}", f"whole sweeps: {c.sweeps}", f"sweeps per second (measured): {rate:.2f}",
              f"baud: {args.baud}"]
    with open(os.path.join(args.out, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"\nWrote {args.out}/ — please send the whole folder, plus the answers in README.md.")


if __name__ == "__main__":
    main()
