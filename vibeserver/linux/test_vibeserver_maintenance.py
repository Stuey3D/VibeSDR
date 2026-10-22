#!/usr/bin/env python3
# ★★★ RECEIVE BETA UPDATES (2026-10-22): vibeserver-maintenance's beta-on / beta-off / update-stable / update-check, run
#     for real in bash with apt, dpkg and systemd STUBBED on PATH and every fixed path rewritten into a temp directory.
#     Proves:
#       · offer_for — the choice of what "Update VibeServer" installs — for every combination of beta on/off, a
#         stable or beta version installed, and what each suite lists, INCLUDING the downgrade back to stable;
#       · the beta source line copies the stable line's options (one Signed-By for the host, or apt refuses to update),
#         falls back to the installer's form, and is refused when there is neither;
#       · beta-off removes it; the tick is the file and nothing else;
#       · update-stable installs the newest STABLE version with --allow-downgrades only when beta is off, and never
#         when it is on; `update` (what the schedule sends) never downgrades;
#       · the request is removed before acting, and the offer file the admin page reads says the right thing.
#     Silent, no network, no root: nothing here touches the real apt.
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MAINT = os.path.join(HERE, "vibeserver-maintenance")
BASH = shutil.which("bash")

fails = 0
count = 0


def ok(cond, what):
    global fails, count
    count += 1
    print("  %s %s" % ("\033[32mok\033[0m  " if cond else "\033[31mFAIL\033[0m", what))
    if not cond:
        fails += 1


# ★ dpkg --compare-versions, done properly (epoch, upstream, revision; ~ sorts before everything, even the end) so
#   the shell's choices are tested against Debian's real ordering, not a sort that happens to agree today.
DPKG = r'''#!/usr/bin/env python3
import sys
def order(c):
    if c == "~": return -1
    if c.isdigit(): return 0
    if c.isalpha(): return ord(c)
    return ord(c) + 256
def cmp_part(a, b):
    a, b = list(a), list(b)
    while a or b:
        fd = 0
        while (a and not a[0].isdigit()) or (b and not b[0].isdigit()):
            ac = order(a[0]) if a and not a[0].isdigit() else 0
            bc = order(b[0]) if b and not b[0].isdigit() else 0
            if ac != bc: return -1 if ac < bc else 1
            if a and not a[0].isdigit(): a.pop(0)
            if b and not b[0].isdigit(): b.pop(0)
        an = ""; bn = ""
        while a and a[0].isdigit(): an += a.pop(0)
        while b and b[0].isdigit(): bn += b.pop(0)
        ai, bi = int(an or 0), int(bn or 0)
        if ai != bi: return -1 if ai < bi else 1
    return 0
def split(v):
    e = 0
    if ":" in v: e, v = v.split(":", 1); e = int(e)
    r = "0"
    if "-" in v: v, r = v.rsplit("-", 1)
    return e, v, r
def cmp(x, y):
    ex, ux, rx = split(x); ey, uy, ry = split(y)
    if ex != ey: return -1 if ex < ey else 1
    return cmp_part(ux, uy) or cmp_part(rx, ry)
a = sys.argv[1:]
if a and a[0] == "--print-architecture": print("arm64"); sys.exit(0)
if a and a[0] == "--compare-versions":
    c = cmp(a[1], a[3]); op = a[2]
    r = {"gt": c > 0, ">>": c > 0, "lt": c < 0, "<<": c < 0, "ge": c >= 0, "le": c <= 0, "eq": c == 0}[op]
    sys.exit(0 if r else 1)
sys.exit(2)
'''


def make_bin(tmp):
    b = os.path.join(tmp, "bin")
    os.makedirs(b)
    p = os.path.join(b, "dpkg")
    open(p, "w").write("#!" + sys.executable + DPKG[DPKG.index("\n"):])   # ★ this interpreter, not whatever env finds
    os.chmod(p, 0o755)
    return b


def offer_lib():
    s = open(MAINT).read()
    a = s.index("# >>> offer_for")
    z = s.index("# <<< offer_for")
    return s[a:z]


def offer(b, on, inst, st, be):
    lib = offer_lib()
    r = subprocess.run([BASH, "-c", lib + '\noffer_for "$@"', "x", on, inst, st, be],
                       env=dict(os.environ, PATH=b + os.pathsep + os.environ["PATH"]),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    return r.stdout.strip()


def pure_tests():
    print("offer_for — what Update VibeServer installs")
    with tempfile.TemporaryDirectory() as tmp:
        b = make_bin(tmp)
        S, S_OLD, B1, B2 = "11.0.10-4", "11.0.10-3", "12.0.0~beta1-1", "12.0.0~beta2-1"
        cases = [
            # beta, installed, newest stable, newest beta  ->  offer
            ("0", S_OLD, S, "", "stable " + S, "beta off, stable behind: the new stable"),
            ("0", S, S, "", "none", "beta off, on the newest stable: nothing"),
            ("0", B1, S, "", "back-to-stable " + S, "★ beta off, a BETA installed: back to the newest stable (a downgrade)"),
            ("0", B1, S, B2, "back-to-stable " + S, "beta off: a beta the index still lists is IGNORED (off means off)"),
            ("1", S, S, B1, "beta " + B1, "beta on, on stable: the beta (12.0.0~beta1 sorts above 11.x)"),
            ("1", S_OLD, S, B1, "beta " + B1, "beta on, stable behind too: still the beta — the newest of both"),
            ("1", B1, S, B2, "beta " + B2, "beta on, an older beta installed: the newer beta"),
            ("1", B2, S, B2, "none", "beta on, on the newest beta: nothing"),
            ("1", B2, S, B1, "none", "beta on, a newer beta than any listed (pruned): nothing, never a downgrade"),
            ("1", B2, S, "", "none", "beta on, beta suite empty: nothing — beta ON never offers a downgrade"),
            ("1", B2, "12.0.0-1", B2, "stable 12.0.0-1", "beta on, V12 released as stable: 12.0.0-1 beats 12.0.0~beta2"),
            ("1", S_OLD, S, S, "stable " + S, "the same version in both suites counts as stable"),
            ("1", S_OLD, "", B1, "beta " + B1, "beta on, nothing in stable (offline cache?): the beta"),
            ("0", S, "", "", "unknown", "nothing listed at all: unknown, never 'up to date'"),
            ("0", "", S, "", "stable " + S, "nothing installed: the stable version"),
            ("0", "11.0.10-10", "11.0.10-9", "", "back-to-stable 11.0.10-9", "revisions compare as numbers (-10 > -9)"),
        ]
        for on, inst, st, be, want, what in cases:
            got = offer(b, on, inst, st, be)
            ok(got == want, "%s  [%s]" % (what, got))

        print("newest_in — reading apt-cache madison")
        mad = (" vibeserver | 12.0.0~beta2-1 | https://apt.vibesdr.net beta/main arm64 Packages\n"
               " vibeserver | 12.0.0~beta1-1 | https://apt.vibesdr.net beta/main arm64 Packages\n"
               " vibeserver | 11.0.10-10 | https://apt.vibesdr.net stable/main arm64 Packages\n"
               " vibeserver | 11.0.10-9 | https://apt.vibesdr.net stable/main arm64 Packages\n"
               " vibeserver | 99.0.0-1 | https://example.org stable/main arm64 Packages\n")
        lib = offer_lib()
        for suite, want in (("stable", "11.0.10-10"), ("beta", "12.0.0~beta2-1")):
            r = subprocess.run([BASH, "-c", lib + '\nnewest_in "$1"', "x", suite], input=mad,
                               env=dict(os.environ, PATH=b + os.pathsep + os.environ["PATH"]),
                               stdout=subprocess.PIPE, universal_newlines=True)
            ok(r.stdout.strip() == want, "newest %s = %s (another host's package ignored) [%s]" % (suite, want, r.stdout.strip()))


STABLE_INSTALLER = "deb [arch=arm64 signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net stable main\n"
STABLE_POCKET = "deb [arch=arm64,amd64,armhf signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net stable main\n"


def run_action(action, stable_list=STABLE_INSTALLER, keyring=True, beta_on=False, installed="11.0.10-3",
               stable_vers=("11.0.10-4", "11.0.10-3"), beta_vers=("12.0.0~beta1-1",)):
    """One maintenance action in a fake root. Returns (stdout, calls, files) — files = the beta list and the offer."""
    tmp = tempfile.mkdtemp()
    try:
        b = make_bin(tmp)
        etc = os.path.join(tmp, "etc")
        os.makedirs(os.path.join(etc, "sources.list.d"))
        stable = os.path.join(etc, "sources.list.d", "vibesdr.list")
        beta = os.path.join(etc, "sources.list.d", "vibesdr-beta.list")
        kr = os.path.join(tmp, "vibesdr.gpg")
        if stable_list is not None:
            open(stable, "w").write("# the installer wrote this\n" + stable_list)
        if keyring:
            open(kr, "w").write("key")
        if beta_on:
            open(beta, "w").write("deb [arch=arm64 signed-by=%s] https://apt.vibesdr.net beta main\n" % kr)
        log = os.path.join(tmp, "calls.log")
        stub = "#!/bin/sh\necho \"$(basename \"$0\") $*\" >> %s\n" % log
        for tool in ("apt-get", "systemctl", "chgrp"):
            p = os.path.join(b, tool)
            open(p, "w").write(stub)
            os.chmod(p, 0o755)
        # ★ apt-cache lists the beta suite only while the beta source exists — as apt would after an update.
        p = os.path.join(b, "apt-cache")
        sl = "".join(" vibeserver | %s | https://apt.vibesdr.net stable/main arm64 Packages\n" % v for v in stable_vers)
        bl = "".join(" vibeserver | %s | https://apt.vibesdr.net beta/main arm64 Packages\n" % v for v in beta_vers)
        open(p, "w").write(stub + "if [ -f '%s' ]; then printf '%%s' '%s'; fi\nprintf '%%s' '%s'\n" % (beta, bl, sl))
        os.chmod(p, 0o755)
        p = os.path.join(b, "dpkg-query")
        open(p, "w").write("#!/bin/sh\nprintf '%s'\n" % installed)
        os.chmod(p, 0o755)
        maint = os.path.join(tmp, "maint")
        os.makedirs(maint)
        req = os.path.join(tmp, "maintenance.request")
        open(req, "w").write(action)
        cmdline = os.path.join(tmp, "cmdline")
        open(cmdline, "w").write("console=tty1 root=PARTUUID=x\n")
        s = open(MAINT).read()
        s = (s.replace("/var/lib/vibeserver/maintenance.request", req)
              .replace("/var/lib/vibeserver-maint", maint)
              .replace("/var/lib/vibeserver/maintenance.log", os.path.join(tmp, "old.log"))
              .replace("/etc/apt/sources.list.d/vibesdr-beta.list", beta)
              .replace("/etc/apt/sources.list.d/vibesdr.list", stable)
              .replace("/usr/share/keyrings/vibesdr.gpg", kr)
              .replace("/proc/cmdline", cmdline)
              .replace("/etc/vibeserver-pocket/enabled", os.path.join(tmp, "no-pocket"))
              .replace('dd if="$REQ" bs=32 count=1 iflag=nofollow,nonblock', 'head -c 32 "$REQ"'))
        ms = os.path.join(tmp, "maint.sh")
        open(ms, "w").write(s)
        env = dict(os.environ, PATH=b + os.pathsep + os.environ["PATH"])
        r = subprocess.run([BASH, ms], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        calls = open(log).read() if os.path.exists(log) else ""
        files = {
            "beta": open(beta).read() if os.path.exists(beta) else None,
            "offer": open(os.path.join(maint, "update-offer")).read() if os.path.exists(os.path.join(maint, "update-offer")) else None,
            "request_gone": not os.path.exists(req),
            "kr": kr,
        }
        if os.environ.get("MAINT_DEBUG"):
            print(r.stdout, "----", calls, "----", files)
        return r.stdout, calls, files
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def offer_field(files, k):
    m = re.search(r"^%s=(.*)$" % k, files["offer"] or "", re.M)
    return m.group(1) if m else None


def action_tests():
    print("beta-on")
    out, calls, f = run_action("beta-on")
    ok("__VIBESERVER_DONE__ 0" in out, "finishes rc 0")
    ok(f["request_gone"], "the request is removed")
    line = [l for l in (f["beta"] or "").splitlines() if l.startswith("deb ")]
    ok(len(line) == 1 and line[0] == "deb [arch=arm64 signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net beta main",
       "writes ONE beta line, suite beta, the stable line's arch= and signed-by= [%s]" % (line[0] if line else None))
    ok("apt-get update" in calls, "then apt-get update")
    ok("install" not in calls, "and installs NOTHING — ticking only changes what is offered")
    ok(offer_field(f, "beta") == "1" and offer_field(f, "offer") == "beta" and offer_field(f, "version") == "12.0.0~beta1-1",
       "the offer: 12.0.0~beta1-1 (beta) [%s]" % (f["offer"] or "").replace("\n", " "))
    ok("A NEW VIBESERVER IS AVAILABLE: 12.0.0~beta1-1 (beta)." in out, "and says so in the log")

    out, calls, f = run_action("beta-on", stable_list=STABLE_POCKET)
    line = [l for l in (f["beta"] or "").splitlines() if l.startswith("deb ")]
    ok(line and line[0].startswith("deb [arch=arm64,amd64,armhf signed-by="),
       "a Portable's three-arch stable line: the beta line carries the same options [%s]" % (line[0] if line else None))

    out, calls, f = run_action("beta-on", stable_list=None)
    line = [l for l in (f["beta"] or "").splitlines() if l.startswith("deb ")]
    ok(line and line[0] == "deb [arch=arm64 signed-by=%s] https://apt.vibesdr.net beta main" % f["kr"],
       "no stable list of ours: the installer's form, from our keyring [%s]" % (line[0] if line else None))

    out, calls, f = run_action("beta-on", stable_list=None, keyring=False)
    ok("__VIBESERVER_DONE__ 1" in out and f["beta"] is None and "apt-get" not in calls,
       "neither a stable list nor our keyring: refused, nothing written, apt untouched")
    ok("run the one-line installer" in out, "and the log says what to do")

    out, calls, f = run_action("beta-on", stable_list="deb [signed-by=/x.gpg] https://apt.vibesdr.net stable main\n" +
                               "deb http://deb.debian.org/debian bookworm main\n")
    line = [l for l in (f["beta"] or "").splitlines() if l.startswith("deb ")]
    ok(line and line[0] == "deb [signed-by=/x.gpg] https://apt.vibesdr.net beta main",
       "only OUR line's options are copied, another repository's line is not [%s]" % (line[0] if line else None))

    print("beta-off")
    out, calls, f = run_action("beta-off", beta_on=True, installed="12.0.0~beta1-1")
    ok("__VIBESERVER_DONE__ 0" in out, "finishes rc 0")
    ok(f["beta"] is None, "the beta list is gone — the tick is the file")
    ok("apt-get update" in calls and "install" not in calls, "apt-get update, and NOTHING installed (no unasked downgrade)")
    ok(offer_field(f, "beta") == "0" and offer_field(f, "offer") == "back-to-stable" and offer_field(f, "version") == "11.0.10-4",
       "the offer: 11.0.10-4, back to stable [%s]" % (f["offer"] or "").replace("\n", " "))
    ok("press BACK TO STABLE" in out, "and the log says how")

    out, calls, f = run_action("beta-off", beta_on=True, installed="11.0.10-4")
    ok(offer_field(f, "offer") == "none", "beta-off on a box still on stable: nothing to do")

    print("update-stable — the only downgrade, by a person's press")
    out, calls, f = run_action("update-stable", installed="12.0.0~beta1-1")
    ok("__VIBESERVER_DONE__ 0" in out, "finishes rc 0")
    ok("apt-get install -y --allow-downgrades -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold vibeserver=11.0.10-4" in calls,
       "installs vibeserver=11.0.10-4 (the newest STABLE) with --allow-downgrades, keeping config files")
    ok(calls.index("apt-get update") < calls.index("--allow-downgrades"), "after a fresh apt-get update")

    out, calls, f = run_action("update-stable", beta_on=True, installed="12.0.0~beta1-1")
    ok("__VIBESERVER_DONE__ 1" in out and "--allow-downgrades" not in calls, "REFUSED while beta is still ticked")

    out, calls, f = run_action("update-stable", installed="11.0.10-4")
    ok("__VIBESERVER_DONE__ 0" in out and "install" not in calls, "already on the newest stable: nothing installed")
    out, calls, f = run_action("update-stable", installed="11.0.10-3")
    ok("install" not in calls, "on an OLDER stable: not this action's job (that is an ordinary update) — nothing installed")

    print("update — what the schedule sends — never goes backwards")
    out, calls, f = run_action("update", installed="12.0.0~beta1-1")
    ok("apt-get install -y --only-upgrade vibeserver" in calls and "--allow-downgrades" not in calls,
       "beta off with a beta installed: --only-upgrade only (apt cannot downgrade that way)")
    ok(offer_field(f, "offer") == "back-to-stable", "and the page is told what the button would do instead")
    out, calls, f = run_action("update", beta_on=True, installed="11.0.10-4")
    ok("apt-get install -y --only-upgrade vibeserver" in calls, "beta on: the same command (apt picks the newest of both suites)")

    print("update-check")
    out, calls, f = run_action("update-check", beta_on=True, installed="11.0.10-4")
    ok("A NEW VIBESERVER IS AVAILABLE: 12.0.0~beta1-1 (beta)." in out, "beta on: names the beta")
    out, calls, f = run_action("update-check", installed="11.0.10-3")
    ok("A NEW VIBESERVER IS AVAILABLE: 11.0.10-4." in out, "beta off: names the stable version")
    out, calls, f = run_action("update-check", installed="12.0.0~beta1-1")
    ok("Beta updates are off. 11.0.10-4 is the newest stable VibeServer" in out, "beta off on a beta: offers the way back")
    out, calls, f = run_action("update-check", installed="11.0.10-4")
    ok("VibeServer itself is up to date (11.0.10-4)." in out, "up to date: says so, with the version")
    ok(offer_field(f, "installed") == "11.0.10-4" and (offer_field(f, "checked") or "").isdigit(),
       "the offer file carries the installed version and when it was checked")

    print("unknown actions are still ignored")
    out, calls, f = run_action("beta-sideways")
    ok("ignoring unknown action 'beta-sideways'" in out and "apt" not in calls and "systemctl" not in calls, "an unknown name does nothing")


def main():
    print("vibeserver-maintenance — receive beta updates")
    pure_tests()
    action_tests()
    print()
    if fails:
        print("%d of %d FAILED" % (fails, count))
        return 1
    print("all %d passed" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
