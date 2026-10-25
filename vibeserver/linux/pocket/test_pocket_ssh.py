#!/usr/bin/env python3
"""The owner's SSH switch on a Portable (vibeserver-pocket SshAccess, 11.0.11) on a temporary root, chpasswd/usermod/
systemctl faked. Proves:
  · off by default: no state, nothing started;
  · on: the password goes to chpasswd on STDIN only, never in an argument list, never in the log, never in the kept file
    (only the system's hash is kept, 0600), vibe unlocked, host keys kept, sshd STARTED (never enabled);
  · a reboot of a sealed box (shadow and /etc/ssh back to the image's) puts it all back: chpasswd -e with the hash, the
    SAME host keys, sshd started;
  · off: sshd stopped, vibe locked, the hash forgotten — and the next boot starts nothing;
  · the request is validated: on must be a boolean, the password 8–128 printable ASCII;
  · the admin page's state never carries the hash.
Run: python3 vibeserver/linux/pocket/test_pocket_ssh.py
"""
import importlib.machinery
import importlib.util
import io
import json
import os
import stat
import sys
import tempfile
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
loader = importlib.machinery.SourceFileLoader("vp_ssh", os.path.join(HERE, "vibeserver-pocket"))
spec = importlib.util.spec_from_loader("vp_ssh", loader)
vp = importlib.util.module_from_spec(spec)
loader.exec_module(vp)
fails = 0


def ok(cond, what):
    global fails
    print("  %s %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        fails += 1


IMAGE_SHADOW = "root:*:20000:0:99999:7:::\nvibe:!$y$j9T$imagelocked$abc:20000:0:99999:7:::\n"


class FakeShell:
    def __init__(self, root):
        self.root, self.calls, self.inputs = root, [], []
        self.started = 0

    def _shadow(self, fn):
        p = os.path.join(self.root, "etc/shadow")
        lines = open(p).read().splitlines()
        out = []
        for l in lines:
            f = l.split(":")
            if f[0] == "vibe":
                f[1] = fn(f[1])
            out.append(":".join(f))
        open(p, "w").write("\n".join(out) + "\n")

    def run(self, args, timeout=40, secret=False):
        self.calls.append(list(args))
        if args[:2] == ["usermod", "-L"]:
            self._shadow(lambda h: h if h.startswith("!") else "!" + h)
        elif args[:2] == ["usermod", "-U"]:
            self._shadow(lambda h: h[1:] if h.startswith("!") else h)
        elif args[:2] == ["ssh-keygen", "-A"]:
            for k in ("rsa", "ed25519"):
                open(os.path.join(self.root, "etc/ssh/ssh_host_%s_key" % k), "w").write("PRIVATE-%s-%d" % (k, len(self.calls)))
                open(os.path.join(self.root, "etc/ssh/ssh_host_%s_key.pub" % k), "w").write("PUBLIC-%s" % k)
        elif args[:2] == ["systemctl", "start"]:
            self.started += 1
        return 0, ""

    def run_input(self, args, data, timeout=20):
        self.calls.append(list(args))
        self.inputs.append((list(args), data))
        user, val = data.rstrip("\n").split(":", 1)
        if args == ["chpasswd"]:
            self._shadow(lambda h: "$y$j9T$salt$hashof" + str(len(val)))
        elif args == ["chpasswd", "-e"]:
            self._shadow(lambda h: val)
        return 0


def make_root(t):
    for d in ("etc/ssh", "usr/sbin", "var/lib/vibeserver-pocket"):
        os.makedirs(os.path.join(t, d), exist_ok=True)
    open(os.path.join(t, "etc/shadow"), "w").write(IMAGE_SHADOW)
    open(os.path.join(t, "etc/passwd"), "w").write("root:x:0:0::/root:/bin/bash\nvibe:x:1000:1000::/home/vibe:/bin/bash\n")
    open(os.path.join(t, "usr/sbin/sshd"), "w").write("")


def access(t, sh):
    a = vp.SshAccess(sh)
    a.state = os.path.join(t, "var/lib/vibeserver-pocket/ssh.json")
    a.keep = os.path.join(t, "var/lib/vibeserver-pocket/ssh-host-keys")
    a.etc = os.path.join(t, "etc/ssh")
    a.shadow = os.path.join(t, "etc/shadow")
    a.sshd = os.path.join(t, "usr/sbin/sshd")
    # ★ pwd.getpwnam would read the TEST machine's users; the fake root's passwd is the one that counts.
    a.available = lambda: os.path.exists(a.sshd) and a._user_in(os.path.join(t, "etc/passwd"))
    return a


PASSWORD = "Owner typed: s3cret!"

with tempfile.TemporaryDirectory() as t:
    make_root(t)
    sh = FakeShell(t)
    a = access(t, sh)
    ok(a.doc() == {"available": True, "on": False, "user": "vibe", "lastError": ""}, "off by default, and offered (sshd + vibe present)")
    ok(a.restore() is False and sh.started == 0, "a first boot with no switch starts nothing")

    buf = io.StringIO()
    with redirect_stdout(buf):
        took = a.apply({"on": True, "pass": PASSWORD})
    log = buf.getvalue()
    ok(took, "on: applied")
    ok(all(PASSWORD not in " ".join(c) for c in sh.calls), "the password is in NO argument list")
    ok(sh.inputs[0] == (["chpasswd"], "vibe:" + PASSWORD + "\n"), "the password went to chpasswd on stdin, for vibe")
    ok(PASSWORD not in log, "the password is not in the log")
    kept = open(a.state).read()
    ok(PASSWORD not in kept and json.loads(kept)["hash"].startswith("$y$"), "only the system's hash is kept")
    ok(stat.S_IMODE(os.stat(a.state).st_mode) == 0o600, "the kept file is 0600")
    ok(not open(a.shadow).read().split("vibe:")[1].startswith("!"), "vibe unlocked")
    ok(["systemctl", "start", "ssh.service"] in sh.calls and not any(c[:2] == ["systemctl", "enable"] for c in sh.calls),
       "sshd STARTED, never enabled (the image's off stays the default)")
    keys = sorted(os.listdir(a.keep))
    ok("ssh_host_ed25519_key" in keys, "host keys made and kept on the settings partition")
    first_key = open(os.path.join(a.keep, "ssh_host_ed25519_key")).read()
    d = a.doc()
    ok(d["on"] is True and "hash" not in json.dumps(d), "the page is told on — and never the hash")

    # ── a sealed box reboots: the overlay forgot shadow and the host keys ──
    open(a.shadow, "w").write(IMAGE_SHADOW)
    for n in os.listdir(a.etc):
        os.unlink(os.path.join(a.etc, n))
    sh2 = FakeShell(t)
    a2 = access(t, sh2)
    ok(a2.restore() is True, "after a reboot: restored")
    ok(sh2.inputs and sh2.inputs[0][0] == ["chpasswd", "-e"] and sh2.inputs[0][1].startswith("vibe:$y$"),
       "the hash put back with chpasswd -e (the password itself was never kept)")
    ok(open(os.path.join(a.etc, "ssh_host_ed25519_key")).read() == first_key, "the SAME host key is back (no new-key warning)")
    ok(not any(c[:2] == ["ssh-keygen", "-A"] for c in sh2.calls), "no new host keys made")
    ok(sh2.started == 1, "sshd started")

    # ── off ──
    a2.apply({"on": False})
    ok(["systemctl", "stop", "ssh.service"] in sh2.calls, "off: sshd stopped")
    ok(open(a.shadow).read().split("vibe:")[1].startswith("!"), "off: vibe locked")
    ok("hash" not in json.loads(open(a.state).read()), "off: the hash forgotten")
    sh3 = FakeShell(t)
    ok(access(t, sh3).restore() is False and sh3.started == 0, "off survives a reboot: nothing started")

    # ── no sshd on the image ──
    os.unlink(a.sshd)
    a4 = access(t, FakeShell(t))
    ok(a4.doc()["available"] is False and a4.apply({"on": True, "pass": PASSWORD}) is False, "no sshd: not offered, refused")

# ── validation ──
v = vp.validate_ssh_request
ok(v({"on": True, "pass": "12345678"})[0] is not None, "8 characters accepted")
ok(v({"on": True, "pass": "1234567"})[0] is None, "7 refused")
ok(v({"on": True, "pass": "x" * 129})[0] is None, "129 refused")
ok(v({"on": True, "pass": "abcdefgh\nroot:x"})[0] is None, "a newline refused (it would end chpasswd's line)")
ok(v({"on": True, "pass": "pässwörd1"})[0] is None, "non-ASCII refused")
ok(v({"on": "yes"})[0] is None and v([])[0] is None, "on must be a boolean")
ok(v({"on": False, "pass": "ignored!"})[0] == {"on": False}, "off needs no password and carries none")

print("test_pocket_ssh: %s" % ("PASS" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
