#!/usr/bin/env python3
# ★★★ CHECK THIS RADIO'S MEMORY (2026-11-03): vibeserver-maintenance's `memcheck <serial>`, run for real in bash with
#     systemctl, journalctl, runuser and friends STUBBED on PATH and every fixed path rewritten into a temp directory.
#     Proves:
#       · the serial is held to the serial alphabet — options, paths, unit names, shell, over-long all refused;
#       · it must be one the BINARY lists (--list-memcheck-radios), and the listed unit is the one used; a listing line
#         whose unit does not match its serial is ignored;
#       · nothing is stopped, written or started for a refused serial;
#       · a good run: stops the radio's unit, swaps ONLY ExecStart by a drop-in (the unit's own arguments, the ASan
#         binary, timeout 60, Restart=no), starts it, saves the report in root's directory, removes the drop-in, and
#         puts the radio back as it was — started again if it was running, LEFT STOPPED if it was paused;
#       · the unit's arguments are read back from systemd, and a word that is not plain (a glob, a %) falls back to
#         the package default rather than reaching the drop-in.
#     Silent, no root, nothing touches the real systemd.
import os
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


def lib():
    s = open(MAINT).read()
    return s[s.index("# >>> memcheck_lib"):s.index("# <<< memcheck_lib")]


def sh(script, *args, stdin=""):
    r = subprocess.run([BASH, "-c", lib() + "\n" + script, "x"] + list(args), input=stdin,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    return r.returncode, r.stdout.strip()


LISTING = "31A0B7C vibeserver@31A0B7C\n4 vibeserver\n../x vibeserver@../x\nABC vibeserver@DEF\n3b52ab5dac9d3f4b vibeserver@3b52ab5dac9d3f4b\n"


def pure_tests():
    print("mc_valid_serial — the serial alphabet")
    for s in ("31A0B7C", "0000000001", "3b52ab5dac9d3f4b", "4", "2405XY:01", "A_b-c.d"):
        ok(sh('mc_valid_serial "$1"', s)[0] == 0, "accepts %r" % s)
    for s in ("", "-rf", "--config", ".hidden", "../etc/passwd", "a/b", "a b", "x;reboot", "a$(id)", "`id`",
              "vibeserver@x", "a\\b", "*", "a\nb", "A" * 65):
        ok(sh('mc_valid_serial "$1"', s)[0] != 0, "refuses %r" % s)

    print("mc_unit_for — only a serial the binary lists, with ITS unit")
    rc, out = sh('mc_unit_for "$1"', "31A0B7C", stdin=LISTING)
    ok(rc == 0 and out == "31A0B7C vibeserver@31A0B7C", "a secondary radio → its vibeserver@ unit [%s]" % out)
    rc, out = sh('mc_unit_for "$1"', "4", stdin=LISTING)
    ok(rc == 0 and out == "4 vibeserver", "the Simple-mode primary → vibeserver.service [%s]" % out)
    for s, why in (("9999", "a serial not in the settings"), ("../x", "a listed line with a path for a serial"),
                   ("ABC", "a listed line whose unit names ANOTHER serial"), ("31a0b7c", "a different case"),
                   ("31A0B7", "a prefix of a real one"), ("", "nothing")):
        rc, out = sh('mc_unit_for "$1"', s, stdin=LISTING)
        ok(rc != 0 and out == "", "refuses %s (%r)" % (why, s))

    print("mc_args_for — the unit's own arguments, only the binary swapped")
    show = "{ path=/usr/bin/vibeserver ; argv[]=/usr/bin/vibeserver --config /etc/vibeserver/vibeserver.conf --radio-serial 31A0B7C ; ignore_errors=no ; start_time=[n/a] ; stop_time=[n/a] ; pid=0 ; code=(null) ; status=0/0 }"
    rc, out = sh('mc_args_for "$1" "$2"', "vibeserver@31A0B7C", "31A0B7C", stdin=show + "\n")
    ok(out == "--config /etc/vibeserver/vibeserver.conf --radio-serial 31A0B7C", "read back from systemd [%s]" % out)
    rc, out = sh('mc_args_for "$1" "$2"', "vibeserver@31A0B7C", "31A0B7C",
                 stdin=show.replace("--radio-serial 31A0B7C", "--bg-decode on --radio-serial 31A0B7C") + "\n")
    ok(out.endswith("--bg-decode on --radio-serial 31A0B7C"), "an owner's extra argument is carried [%s]" % out)
    for bad, why in (("--radio-serial 31A0B7C *", "a glob"), ("--radio-serial %i", "a % specifier"),
                     ("--radio-serial 31A0B7C ;reboot", "a ';'"), ("--x $(id)", "a $( )")):
        rc, out = sh('mc_args_for "$1" "$2"', "vibeserver@31A0B7C", "31A0B7C",
                     stdin=show.replace("--radio-serial 31A0B7C", bad) + "\n")
        ok(out == "--config /etc/vibeserver/vibeserver.conf --radio-serial 31A0B7C",
           "%s in the unit → the package default instead [%s]" % (why, out))
    rc, out = sh('mc_args_for "$1" "$2"', "vibeserver@31A0B7C", "31A0B7C",
                 stdin=show.replace("path=/usr/bin/vibeserver ; argv[]=/usr/bin/vibeserver", "path=/tmp/x ; argv[]=/tmp/x") + "\n")
    ok(out == "--config /etc/vibeserver/vibeserver.conf --radio-serial 31A0B7C", "another binary in the unit → the default [%s]" % out)
    rc, out = sh('mc_args_for "$1" "$2"', "vibeserver", "4", stdin="\n")
    ok(out == "--config /etc/vibeserver/vibeserver.conf", "nothing from systemd, the primary → its default [%s]" % out)

    print("mc_verdict")
    with tempfile.TemporaryDirectory() as t:
        p = os.path.join(t, "o")
        open(p, "w").write("==7==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x6020\n")
        ok("FOUND A MEMORY ERROR" in sh('mc_verdict "$1" 1', p)[1], "an ASan ERROR line → FOUND A MEMORY ERROR")
        open(p, "w").write("VibeServer: MEMORY-CHECK BUILD\nlistening\n")
        ok("no memory error in 60 s" in sh('mc_verdict "$1" 124', p)[1], "timed out (124) clean → no memory error in 60 s")
        ok("stopped after a short run (exit 1)" in sh('mc_verdict "$1" 1', p)[1], "an early exit with no ASan report says so")


def run_action(request, prior="active", have_asan=True, listing=LISTING):
    tmp = tempfile.mkdtemp()
    try:
        b = os.path.join(tmp, "bin")
        os.makedirs(b)
        calls = os.path.join(tmp, "calls.log")
        state = os.path.join(tmp, "unit-state")
        open(state, "w").write(prior)
        dropbase = os.path.join(tmp, "run-systemd")
        # ★ systemctl: records every call; is-active answers the unit's state; start copies the drop-in into the log
        #   (it is removed by the time the run ends) and the unit exits at once.
        open(os.path.join(b, "systemctl"), "w").write(r'''#!/bin/sh
echo "systemctl $*" >> "%(calls)s"
case "$1" in
  is-active) cat "%(state)s"; [ "$(cat "%(state)s")" = active ] ;;
  stop) echo inactive > "%(state)s" ;;
  start) for f in "%(drop)s"/*/zz-vibeserver-memcheck.conf; do [ -f "$f" ] && sed 's/^/DROPIN: /' "$f" >> "%(calls)s"; done
         echo inactive > "%(state)s" ;;
  show) case "$*" in
          *ExecStart*) echo "{ path=/usr/bin/vibeserver ; argv[]=/usr/bin/vibeserver --config /etc/vibeserver/vibeserver.conf --radio-serial 31A0B7C ; ignore_errors=no }" ;;
          *InvocationID*) echo "0123abcd" ;;
          *ExecMainStatus*) echo "1" ;;
        esac ;;
esac
exit 0
''' % {"calls": calls, "state": state, "drop": dropbase})
        open(os.path.join(b, "journalctl"), "w").write('''#!/bin/sh
echo "journalctl $*" >> "%s"
case "$*" in *--sync*) exit 0 ;; esac
echo "VibeServer: MEMORY-CHECK BUILD (AddressSanitizer)"
echo "==12==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000038"
echo "    #0 0x55 in UsrpSource::readLoop usrp_source.cpp:412"
echo "SUMMARY: AddressSanitizer: heap-buffer-overflow usrp_source.cpp:412 in UsrpSource::readLoop"
''' % calls)
        lister = os.path.join(b, "vibeserver-lister")
        open(lister, "w").write("#!/bin/sh\n[ \"$1\" = --list-memcheck-radios ] && printf '%s'\n" % listing.replace("\n", "\\n"))
        for tool in ("chgrp", "flock", "dpkg-query", "runuser"):
            open(os.path.join(b, tool), "w").write("#!/bin/sh\necho \"%s $*\" >> \"%s\"\nexit 0\n" % (tool, calls))
        for f in os.listdir(b):
            os.chmod(os.path.join(b, f), 0o755)
        asan = os.path.join(tmp, "vibeserver-asan")
        if have_asan:
            open(asan, "w").write("#!/bin/sh\n")
            os.chmod(asan, 0o755)
        maint = os.path.join(tmp, "maint")
        os.makedirs(maint)
        req = os.path.join(tmp, "maintenance.request")
        open(req, "w").write(request)
        cmdline = os.path.join(tmp, "cmdline")
        open(cmdline, "w").write("console=tty1\n")
        s = open(MAINT).read()
        s = (s.replace("/var/lib/vibeserver/maintenance.request", req)
              .replace("/var/lib/vibeserver-maint", maint)
              .replace("/var/lib/vibeserver/maintenance.log", os.path.join(tmp, "old.log"))
              .replace("/proc/cmdline", cmdline)
              .replace("/etc/vibeserver-pocket/enabled", os.path.join(tmp, "no-pocket"))
              .replace("/usr/lib/vibeserver/diag/vibeserver-asan", asan)
              .replace("/usr/bin/vibeserver --list-memcheck-radios", lister + " --list-memcheck-radios")
              .replace("/run/systemd/system", dropbase)
              .replace("/run/vibeserver-memcheck.lock", os.path.join(tmp, "mc.lock"))
              .replace('dd if="$REQ" bs=96 count=1 iflag=nofollow,nonblock', 'head -c 96 "$REQ"'))
        ms = os.path.join(tmp, "maint.sh")
        open(ms, "w").write(s)
        env = dict(os.environ, PATH=b + os.pathsep + os.environ["PATH"])
        r = subprocess.run([BASH, ms], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        c = open(calls).read() if os.path.exists(calls) else ""
        diag = os.path.join(maint, "diag")
        reports = sorted(os.listdir(diag)) if os.path.isdir(diag) else []
        rep = open(os.path.join(diag, reports[-1])).read() if reports else ""
        left = []
        for root, _, files in os.walk(dropbase):
            left += [os.path.join(root, f) for f in files]
        if os.environ.get("MAINT_DEBUG"):
            print(r.stdout, "----", c, "----", reports, rep)
        return r.stdout, c, reports, rep, left
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def action_tests():
    print("refusals — nothing is stopped, written or started")
    for req, why in (("memcheck ../../etc/passwd", "a path"), ("memcheck -rf", "an option"),
                     ("memcheck 9999", "a serial not in the settings"), ("memcheck", "no serial at all"),
                     ("memcheck vibeserver@31A0B7C", "a unit name"), ("memcheck 31A0B7C;reboot", "shell after it"),
                     ("memcheck ABC", "a listed serial whose unit names another")):
        out, calls, reports, rep, left = run_action(req)
        ok("__VIBESERVER_DONE__ 1" in out and "systemctl stop" not in calls and "systemctl start" not in calls
           and not reports and not left, "%s (%r): refused, rc 1, systemd untouched" % (why, req))
    out, calls, reports, rep, left = run_action("memcheck 31A0B7C", have_asan=False)
    ok("__VIBESERVER_DONE__ 1" in out and "systemctl" not in calls and "amd64 and arm64 only" in out,
       "no memory-check binary in this package: refused, and says why")

    print("a running radio")
    out, calls, reports, rep, left = run_action("memcheck 31A0B7C", prior="active")
    ok("__VIBESERVER_DONE__ 1" not in out and "__VIBESERVER_DONE__" in out, "finishes [%s]" % out.strip().splitlines()[-1])
    lines = calls.splitlines()
    i_stop = next((i for i, l in enumerate(lines) if l == "systemctl stop vibeserver@31A0B7C.service"), -1)
    starts = [i for i, l in enumerate(lines) if l == "systemctl start vibeserver@31A0B7C.service"]
    ok(i_stop >= 0 and len(starts) == 2 and i_stop < starts[0], "stops vibeserver@31A0B7C, runs it, starts it again (it was active)")
    drop = [l[8:] for l in lines if l.startswith("DROPIN: ")]
    ok(any(l == "ExecStart=" for l in drop) and
       any(l.startswith("ExecStart=/usr/bin/timeout --kill-after=10 60 ") and l.endswith("vibeserver-asan --config /etc/vibeserver/vibeserver.conf --radio-serial 31A0B7C") for l in drop),
       "the drop-in swaps ONLY ExecStart: the ASan binary, the unit's own arguments, under timeout 60")
    ok("Restart=no" in drop and "LogRateLimitIntervalSec=0" in drop, "…Restart=no and no journal throttling")
    ok(not left, "the drop-in is gone afterwards %s" % left)
    ok("journalctl --no-pager -o cat _SYSTEMD_INVOCATION_ID=0123abcd" in calls, "the output is that run's journal, by invocation id")
    ok(len(reports) == 1 and reports[0].startswith("memcheck-31A0B7C-") and reports[0].endswith(".txt"),
       "one report, memcheck-<serial>-<time>.txt, in root's diag directory %s" % reports)
    ok(rep.splitlines()[1].startswith("result:   FOUND A MEMORY ERROR") and "usrp_source.cpp:412" in rep,
       "the report leads with the verdict and carries ASan's output")
    ok("SUMMARY: AddressSanitizer: heap-buffer-overflow" in out, "the admin page's log shows ASan's SUMMARY too")

    print("a PAUSED radio — run anyway, left paused")
    out, calls, reports, rep, left = run_action("memcheck 31A0B7C", prior="inactive")
    starts = [l for l in calls.splitlines() if l == "systemctl start vibeserver@31A0B7C.service"]
    ok(len(starts) == 1 and "leaving vibeserver@31A0B7C.service stopped" in out, "started once (the check), then LEFT STOPPED")
    ok(len(reports) == 1 and not left, "…with its report, and no drop-in left behind")

    print("the Simple-mode primary")
    out, calls, reports, rep, left = run_action("memcheck 4", prior="active")
    ok("systemctl stop vibeserver.service" in calls and calls.count("systemctl start vibeserver.service") == 2,
       "serial 4 is served by vibeserver.service: that unit is the one stopped, run and restarted")

    print("the other actions are untouched by the argument")
    out, calls, reports, rep, left = run_action("restart 31A0B7C")
    ok("systemctl restart vibeserver" in calls and "31A0B7C" not in calls, "an argument on another action is ignored")


def main():
    print("vibeserver-maintenance — check this radio's memory")
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
