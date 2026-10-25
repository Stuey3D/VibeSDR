#!/usr/bin/env python3
# ★★★ THE APT REPOSITORY'S TWO SUITES (2026-10-22, "Receive beta updates"): scripts/publish-apt.sh — the indexer both
#     publish routes run — exercised for real in its index-only mode (what publish-apt-remote.sh runs on the Mac, in Docker),
#     on a scratch repository, with the Debian tools STUBBED (dpkg, dpkg-deb, dpkg-scanpackages, apt-ftparchive: none
#     exist on the Mac). The stubs index what is on disk the way the real ones do; what is under test is the script's
#     own logic:
#       · a beta publish lands in pool/beta + dists/beta ONLY — the stable index, Release and pool are byte-identical
#         afterwards — and the reverse;
#       · each suite's Packages lists only its own packages; each Release names its own suite;
#       · the revision high-water mark spans BOTH suites (one version never names two files), the KEEP prune is per
#         suite (a run of betas never prunes a stable package);
#       · the landing page shows the stable version AND the beta line, whichever suite published last;
#       · VIBE_SUITE other than stable/beta is refused.
#     ★ NOT covered (needs Docker and the signing key): the real dpkg-scanpackages/apt-ftparchive output,
#       signing, and the remote wrapper's sync/verify steps. No network, no key, nothing outside a temp directory.
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASH = shutil.which("bash")

fails = 0
count = 0


def ok(cond, what):
    global fails, count
    count += 1
    print("  %s %s" % ("\033[32mok\033[0m  " if cond else "\033[31mFAIL\033[0m", what))
    if not cond:
        fails += 1


STUBS = {
    "dpkg": r'''#!/bin/sh
[ "$1" = --print-architecture ] && { echo arm64; exit 0; }
exit 2
''',
    # ★ dpkg-deb: the package's fields come from its NAME (vibeserver_<version>_<arch>.deb), as a real one's would.
    "dpkg-deb": r'''#!/bin/sh
if [ "$1" = --fsys-tarfile ]; then echo "First-time setup"; exit 0; fi
if [ "$1" = -f ]; then
  b=$(basename "$2" .deb); b=${b#.prebuilt-}; v=${b#vibeserver_}; a=${v##*_}; v=${v%_*}
  case "$3" in Version) echo "$v";; Architecture) echo "$a";; Depends) echo "libc6 (>= 2.31), adduser, curl";; esac
  exit 0
fi
exit 2
''',
    # ★ dpkg-scanpackages --arch A DIR /dev/null: one stanza per .deb of that arch UNDER DIR, Filename relative.
    "dpkg-scanpackages": r'''#!/bin/sh
arch=$2; dir=$3
find "$dir" -name "*_${arch}.deb" | sort | while read -r f; do
  b=$(basename "$f" .deb); v=${b#vibeserver_}; v=${v%_*}
  printf 'Package: vibeserver\nVersion: %s\nArchitecture: %s\nFilename: %s\nSHA256: %s\n\n' "$v" "$arch" "$f" "$(shasum -a 256 "$f" | cut -d' ' -f1)"
done
''',
    # ★ apt-ftparchive -c CONF release DIR: the Release header from CONF, and the indexes under DIR.
    "apt-ftparchive": r'''#!/bin/sh
conf=$2; dir=$4
for k in Origin Label Suite Codename Architectures Components Description; do
  v=$(sed -n "s/^APT::FTPArchive::Release::$k \"\(.*\)\";/\1/p" "$conf"); echo "$k: $v"
done
echo "SHA256:"
( cd "$dir" && find main -name 'Packages*' | sort | while read -r f; do echo " $(shasum -a 256 "$f" | cut -d' ' -f1) $(wc -c < "$f" | tr -d ' ') $f"; done )
''',
}


def make_src(tmp, version, pre):
    """A source tree with only what publish-apt.sh reads: itself, install.sh, the page, CMakeLists' version."""
    src = os.path.join(tmp, "src-%s%s" % (version, pre))
    if os.path.isdir(src):
        return src
    os.makedirs(os.path.join(src, "scripts"))
    os.makedirs(os.path.join(src, "vibeserver"))
    for f in ("publish-apt.sh", "install.sh", "apt-index.html"):
        shutil.copy(os.path.join(HERE, f), os.path.join(src, "scripts", f))
    open(os.path.join(src, "vibeserver", "CMakeLists.txt"), "w").write(
        'project(vibeserver VERSION %s LANGUAGES CXX C)\nset(VIBESERVER_PRERELEASE "%s")\n' % (version, pre))
    return src


def run(tmp, apt, src, suite, *args, env_extra=None, deb=None):
    b = os.path.join(tmp, "bin")
    env = dict(os.environ, PATH=b + os.pathsep + os.environ["PATH"], APT_DIR=apt, VIBE_PUBLISH_ARCH="arm64")
    if suite is not None:
        env["VIBE_SUITE"] = suite
    if deb:
        env.update(VIBE_INDEX_ONLY="1", VIBE_PREBUILT_DEB=deb)
    if env_extra:
        env.update(env_extra)
    return subprocess.run([BASH, os.path.join(src, "scripts", "publish-apt.sh")] + list(args), env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)


def publish(tmp, apt, version, pre, suite):
    """--next-rev, "build" a package of that version, index it — the remote route's three steps for one arch."""
    src = make_src(tmp, version, pre)
    r = run(tmp, apt, src, suite, "--next-rev")
    ver = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    debdir = os.path.join(tmp, "debs")
    os.makedirs(debdir, exist_ok=True)
    deb = os.path.join(debdir, "vibeserver_%s_arm64.deb" % ver)
    open(deb, "w").write("a package: %s\n" % ver)
    r2 = run(tmp, apt, src, suite, deb=deb)
    if os.environ.get("APT_DEBUG"):
        print(r.stdout, r2.stdout)
    return ver, r2


def snap(apt, rel):
    out = {}
    base = os.path.join(apt, rel)
    for d, _, fs in os.walk(base):
        for f in fs:
            p = os.path.join(d, f)
            out[os.path.relpath(p, apt)] = open(p, "rb").read()
    return out


def versions(apt, suite):
    p = os.path.join(apt, "dists", suite, "main", "binary-arm64", "Packages")
    return re.findall(r"^Version: (\S+)", open(p).read(), re.M) if os.path.exists(p) else []


def main():
    print("publish-apt.sh — the stable and beta suites")
    tmp = tempfile.mkdtemp()
    try:
        b = os.path.join(tmp, "bin")
        os.makedirs(b)
        for name, body in STUBS.items():
            p = os.path.join(b, name)
            open(p, "w").write(body)
            os.chmod(p, 0o755)
        apt = os.path.join(tmp, "apt")
        os.makedirs(os.path.join(apt, "dists", "stable"))

        print("stable, as it has always been")
        for i in range(4):
            v, r = publish(tmp, apt, "11.0.10", "", None)   # ★ no VIBE_SUITE at all: the default is stable
            ok(r.returncode == 0, "stable publish %d: %s (rc %d)" % (i + 1, v, r.returncode))
        ok(v == "11.0.10-4", "revisions count up 1..4 [%s]" % v)
        pool_main = sorted(os.listdir(os.path.join(apt, "pool/main/v/vibeserver")))
        ok(pool_main == ["vibeserver_11.0.10-%d_arm64.deb" % i for i in (2, 3, 4)], "KEEP=3 in pool/main %s" % pool_main)
        ok(sorted(versions(apt, "stable")) == ["11.0.10-2", "11.0.10-3", "11.0.10-4"], "stable Packages lists the pool")
        ok(not os.path.exists(os.path.join(apt, "dists/beta")) and not os.path.exists(os.path.join(apt, "pool/beta")),
           "no beta suite or pool appears from a stable publish")
        rel = open(os.path.join(apt, "dists/stable/Release")).read()
        ok("Suite: stable" in rel and "Origin: VibeSDR" in rel and "Label: VibeSDR" in rel, "stable Release: suite stable, Origin/Label VibeSDR")
        ok(re.search(r"Filename: pool/main/v/vibeserver/", open(os.path.join(apt, "dists/stable/main/binary-arm64/Packages")).read()) is not None,
           "Filename: lines unchanged (pool/main/v/vibeserver/…)")

        print("a beta publish leaves stable byte-for-byte alone")
        before = snap(apt, "dists/stable")
        before.update(snap(apt, "pool/main"))
        conf_before = open(os.path.join(apt, "aptftp.conf")).read()
        for i in range(4):
            v, r = publish(tmp, apt, "12.0.0", "beta1", "beta")
            ok(r.returncode == 0, "beta publish %d: %s (rc %d)" % (i + 1, v, r.returncode))
        after = snap(apt, "dists/stable")
        after.update(snap(apt, "pool/main"))
        ok(before == after, "dists/stable and pool/main identical after four beta publishes")
        ok(open(os.path.join(apt, "aptftp.conf")).read() == conf_before, "and stable's aptftp.conf untouched")
        ok(v == "12.0.0~beta1-4", "beta revisions count up [%s]" % v)
        pool_beta = sorted(os.listdir(os.path.join(apt, "pool/beta/v/vibeserver")))
        ok(pool_beta == ["vibeserver_12.0.0~beta1-%d_arm64.deb" % i for i in (2, 3, 4)], "KEEP=3 in pool/beta, its own prune %s" % pool_beta)
        bv = versions(apt, "beta")
        ok(sorted(bv) == ["12.0.0~beta1-2", "12.0.0~beta1-3", "12.0.0~beta1-4"], "beta Packages lists ONLY the betas %s" % bv)
        ok(all(v.startswith("11.") for v in versions(apt, "stable")), "and stable Packages has no beta in it")
        brel = open(os.path.join(apt, "dists/beta/Release")).read()
        ok("Suite: beta" in brel and "Codename: beta" in brel and "Origin: VibeSDR" in brel and "Architectures: arm64" in brel,
           "beta Release: suite beta, same Origin, its own architectures")
        ok(os.path.exists(os.path.join(apt, "aptftp-beta.conf")), "beta has its own aptftp-beta.conf")
        page = open(os.path.join(apt, "index.html")).read()
        ok("Current package: arm64 11.0.10-4" in page, "the page still names STABLE as the current package")
        ok("arm64 12.0.0~beta1-4" in page and "__BETAVERSIONS__" not in page, "and lists the beta on its own line")
        ok("vibeserver_11.0.10-4_arm64.deb" in page and "12.0.0~beta1" not in page.split("<pre>")[-1][:300],
           "the .deb lines are stable's only")

        print("and a stable publish leaves beta alone")
        bbefore = snap(apt, "dists/beta")
        bbefore.update(snap(apt, "pool/beta"))
        v, r = publish(tmp, apt, "11.0.10", "", "stable")
        ok(r.returncode == 0 and v == "11.0.10-5", "stable publish 5 [%s]" % v)
        bafter = snap(apt, "dists/beta")
        bafter.update(snap(apt, "pool/beta"))
        ok(bbefore == bafter, "dists/beta and pool/beta identical after a stable publish")
        page = open(os.path.join(apt, "index.html")).read()
        ok("arm64 12.0.0~beta1-4" in page, "and the page written by a STABLE publish still lists the beta")

        print("one version never names two files")
        v, r = publish(tmp, apt, "11.0.10", "", "beta")
        ok(v == "11.0.10-6", "the same upstream published to beta takes the next revision ACROSS suites (6, not 1) [%s]" % v)
        v, r = publish(tmp, apt, "11.0.10", "", "stable")
        ok(v == "11.0.10-7", "and back on stable, 7 — never a revision the beta suite used [%s]" % v)

        print("refusals")
        r = run(tmp, apt, make_src(tmp, "11.0.10", ""), "testing", "--next-rev")
        ok(r.returncode != 0 and "VIBE_SUITE must be stable or beta" in r.stdout, "VIBE_SUITE=testing is refused")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print()
    if fails:
        print("%d of %d FAILED" % (fails, count))
        return 1
    print("all %d passed" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
