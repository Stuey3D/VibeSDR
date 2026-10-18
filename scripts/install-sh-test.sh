#!/bin/bash
# Container test for scripts/install.sh (2026-10-18). Runs INSIDE a throwaway container as root,
# with this file and install.sh mounted at /t, against the REAL https://apt.vibesdr.net:
#   cp scripts/install.sh scripts/install-sh-test.sh DIR; cd DIR; docker run --rm [--platform linux/386] -v "$PWD":/t debian:bookworm-slim bash /t/install-sh-test.sh main
# Modes: main (fresh, re-run, --auto-updates + unattended-upgrade --dry-run, upgrade, --no-auto-updates)
#        manual (stale/duplicate/deb822 hand-made entries)  uuadd (u-u already on)  tooold  nosse2
# Usage: install-sh-test.sh main|manual|uuadd|tooold|nosse2
# ★ tooold wants a system BELOW glibc 2.35 (debian:bullseye, ubuntu:20.04). Ubuntu 22.04 was the 'too old'
#   case only while packages linked Debian's libhackrf0; the self-contained build installs there (2026-10-18).
set -u
MODE="$1"
echo 'APT::Install-Recommends "0";' > /etc/apt/apt.conf.d/99test-norecommends   # keep the test small
pass=0; fail=0
ok()  { echo "  PASS: $*"; pass=$((pass+1)); }
bad() { echo "  FAIL: $*"; fail=$((fail+1)); }
chk() { local d="$1"; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }
active_lines() { cat /etc/apt/sources.list /etc/apt/sources.list.d/*.list 2>/dev/null | grep -cE '^[[:space:]]*deb .*apt\.vibesdr\.net' || true; }
ARCH=$(dpkg --print-architecture)
WANT="deb [arch=$ARCH signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net stable main"
. /etc/os-release; echo "### $PRETTY_NAME / $ARCH / $MODE"

case "$MODE" in
main)
  echo "--- 1 fresh install (piped, as the one-liner)"
  cat /t/install.sh | bash > /tmp/o1 2>&1; rc=$?; tail -14 /tmp/o1
  chk "fresh exit 0 (got $rc)" [ $rc = 0 ]
  V=$(dpkg-query -W -f='${Version}' vibeserver 2>/dev/null); chk "vibeserver installed ($V)" [ -n "$V" ]
  chk "list is exactly the wanted line" [ "$(cat /etc/apt/sources.list.d/vibesdr.list)" = "$WANT" ]
  chk "keyring present 0644" [ "$(stat -c %a /usr/share/keyrings/vibesdr.gpg)" = 644 ]
  chk "/usr/bin/vibeserver runs (--version or --help)" bash -c '/usr/bin/vibeserver --version >/dev/null 2>&1 || /usr/bin/vibeserver --help >/dev/null 2>&1'
  echo "--- 2 re-run (idempotent)"
  M1=$(md5sum /etc/apt/sources.list.d/vibesdr.list /usr/share/keyrings/vibesdr.gpg)
  bash /t/install.sh > /tmp/o2 2>&1; rc=$?; tail -4 /tmp/o2
  chk "re-run exit 0" [ $rc = 0 ]
  chk "re-run says already latest" grep -q 'already the latest' /tmp/o2
  chk "list+key unchanged" [ "$M1" = "$(md5sum /etc/apt/sources.list.d/vibesdr.list /usr/share/keyrings/vibesdr.gpg)" ]
  chk "one active repo line ($(active_lines))" [ "$(active_lines)" = 1 ]
  chk "no backups made" [ -z "$(ls /etc/apt/sources.list.d/*.vibesdr.bak 2>/dev/null)" ]
  echo "--- 3 --auto-updates through the pipe (env form)"
  cat /t/install.sh | VIBE_AUTO_UPDATES=1 bash > /tmp/o3 2>&1; rc=$?; grep -E 'automatic|on —' /tmp/o3
  chk "auto exit 0" [ $rc = 0 ]
  F=/etc/apt/apt.conf.d/52vibesdr-auto-updates
  chk "conf written" [ -f $F ]
  cat $F | grep -v '^//'
  apt-config dump | grep -E '^Unattended-Upgrade::(Origins-Pattern|Allowed-Origins)' > /tmp/origins
  echo "    effective:"; sed 's/^/      /' /tmp/origins
  chk "no other origin has a value" [ "$(grep -E '"[^"]+"' /tmp/origins | grep -vc 'origin=VibeSDR,label=VibeSDR,site=apt.vibesdr.net' || true)" = 0 ]
  chk "...and it is the VibeSDR pattern" grep -q 'origin=VibeSDR,label=VibeSDR,site=apt.vibesdr.net' /tmp/origins
  chk "periodic unattended on" [ "$(apt-config dump | grep -c 'APT::Periodic::Unattended-Upgrade "1"')" -ge 1 ]
  echo "--- 4 unattended-upgrade --dry-run with vibeserver faked one version older"
  sed -i '/^Package: vibeserver$/,/^$/ s/^Version: .*/Version: 11.0.4-1/' /var/lib/dpkg/status
  apt-get -qq update >/dev/null 2>&1
  unattended-upgrade --dry-run -d > /tmp/uu 2>&1; grep -E 'Allowed origins|Packages that will be upgraded|pkgs that look like' /tmp/uu | head -5
  chk "u-u would upgrade vibeserver" grep -qE 'Packages that will be upgraded:.*vibeserver' /tmp/uu
  chk "u-u allows only our origin" grep -qxE "Allowed origins are: origin=VibeSDR,label=VibeSDR,site=apt.vibesdr.net" /tmp/uu
  apt list --upgradable 2>/dev/null | grep -v '^Listing' | grep -v '^vibeserver/' > /tmp/osup || true
  NOS=$(grep -c . /tmp/osup || true); sed 's/^/    OS upgradable: /' /tmp/osup
  chk "u-u picks vibeserver ONLY ($NOS other OS package(s) upgradable and left alone)" grep -qx 'Packages that will be upgraded: vibeserver' /tmp/uu
  echo "--- 5 upgrade path (installed 11.0.4-1 -> latest)"
  bash /t/install.sh > /tmp/o5 2>&1; rc=$?; tail -3 /tmp/o5
  chk "upgrade exit 0" [ $rc = 0 ]
  chk "says upgraded from 11.0.4-1" grep -q 'upgraded from 11.0.4-1 to' /tmp/o5
  echo "--- 6 --no-auto-updates"
  bash /t/install.sh --no-auto-updates > /tmp/o6 2>&1; rc=$?
  chk "no-auto exit 0, conf gone" [ $rc = 0 -a ! -f $F ]
  ;;
manual)
  echo "--- pre-existing hand-made entries: stale vibesdr.list (wrong arch), a duplicate .list with another signed-by, a deb822 .sources"
  apt-get -qq update >/dev/null && apt-get -qq install -y curl gpg ca-certificates >/dev/null 2>&1
  curl -fsSL https://apt.vibesdr.net/KEY.gpg | gpg --dearmor -o /usr/share/keyrings/vibesdr.gpg
  mkdir -p /etc/apt/keyrings; cp /usr/share/keyrings/vibesdr.gpg /etc/apt/keyrings/vibe-old.gpg
  # ★ and take the tools away again, so the script must install them WITH the conflict in place
  apt-get -qq -y purge curl gpg ca-certificates >/dev/null 2>&1; apt-get -qq -y autoremove >/dev/null 2>&1
  echo "    curl/gpg removed: $(command -v curl gpg | wc -l) left"
  OTHER=amd64; [ "$ARCH" = amd64 ] && OTHER=arm64
  echo "deb [arch=$OTHER signed-by=/usr/share/keyrings/vibesdr.gpg] https://apt.vibesdr.net stable main" > /etc/apt/sources.list.d/vibesdr.list
  printf '# my radio repo\ndeb [signed-by=/etc/apt/keyrings/vibe-old.gpg] https://apt.vibesdr.net stable main\n' > /etc/apt/sources.list.d/vibeserver.list
  printf 'Types: deb\nURIs: https://apt.vibesdr.net\nSuites: stable\nComponents: main\nSigned-By: /etc/apt/keyrings/vibe-old.gpg\n' > /etc/apt/sources.list.d/vibe.sources
  DS=/etc/apt/sources.list.d/debian.sources; [ -f $DS ] || DS=/etc/apt/sources.list.d/ubuntu.sources
  HAVE_DS=0; if [ -f $DS ]; then HAVE_DS=1; NDS=$(grep -c '^URIs:' $DS)
    printf '\nTypes: deb\nURIs: https://apt.vibesdr.net\nSuites: stable\nComponents: main\nSigned-By: /etc/apt/keyrings/vibe-old.gpg\n' >> $DS; fi
  apt-get update > /tmp/pre 2>&1; echo "    (apt update before the script: exit $? — $(grep -m1 -oE 'Conflicting values[^)]*' /tmp/pre))"
  bash /t/install.sh > /tmp/o1 2>&1; rc=$?; grep -E 'switching|replacing|installed|^E:' /tmp/o1
  chk "exit 0 (got $rc)" [ $rc = 0 ]
  chk "vibeserver installed" [ -n "$(dpkg-query -W -f='${Version}' vibeserver 2>/dev/null)" ]
  chk "vibesdr.list corrected" [ "$(cat /etc/apt/sources.list.d/vibesdr.list)" = "$WANT" ]
  chk "duplicate .list commented out" grep -q '^# disabled by the VibeServer installer: deb' /etc/apt/sources.list.d/vibeserver.list
  chk "backup of it kept" grep -q '^deb \[signed-by=/etc/apt/keyrings/vibe-old.gpg\]' /etc/apt/sources.list.d/vibeserver.list.vibesdr.bak
  chk "single-stanza .sources switched off, backup kept" grep -q '^Enabled: no' /etc/apt/sources.list.d/vibe.sources
  chk "...backup is the original" [ "$(grep -c Enabled /etc/apt/sources.list.d/vibe.sources.vibesdr.bak || true)" = 0 ]
  chk "one active line" [ "$(active_lines)" = 1 ]
  if [ $HAVE_DS = 1 ]; then
    chk "mixed $DS: OS stanzas kept ($NDS), ours Enabled: no" [ "$(grep -c '^URIs:' $DS)" = $((NDS+1)) -a "$(grep -c '^Enabled: no' $DS)" = 1 ]
    awk 'BEGIN{RS=""} /vibesdr/{print "    our stanza now:"; print}' $DS | sed 's/^/      /'
  fi
  apt-get update > /tmp/post 2>&1; rc=$?
  chk "apt update clean afterwards (exit $rc, no W:/E:/N: lines)" [ $rc = 0 -a "$(grep -cE '^(W|E|N):' /tmp/post || true)" = 0 ]
  grep -E '^(W|E|N):' /tmp/post
  B=$(md5sum /etc/apt/sources.list.d/*.bak)
  bash /t/install.sh > /tmp/o2 2>&1; rc=$?
  chk "re-run exit 0, backups untouched" [ $rc = 0 -a "$B" = "$(md5sum /etc/apt/sources.list.d/*.bak)" ]
  ;;
uuadd)
  echo "--- --auto-updates on a machine that ALREADY runs unattended-upgrades (add mode)"
  apt-get -qq update >/dev/null && apt-get -qq install -y unattended-upgrades >/dev/null 2>&1
  printf 'APT::Periodic::Update-Package-Lists "1";\nAPT::Periodic::Unattended-Upgrade "1";\n' > /etc/apt/apt.conf.d/20auto-upgrades
  apt-config dump | grep -E '^Unattended-Upgrade::(Origins-Pattern|Allowed-Origins)' > /tmp/before
  bash /t/install.sh --auto-updates > /tmp/o 2>&1; rc=$?; grep -E 'automatic|on —' /tmp/o
  chk "exit 0" [ $rc = 0 ]
  chk "add mode" grep -q 'vibesdr-mode: add' /etc/apt/apt.conf.d/52vibesdr-auto-updates
  chk "no #clear in add mode" [ "$(grep -c '#clear' /etc/apt/apt.conf.d/52vibesdr-auto-updates || true)" = 0 ]
  apt-config dump | grep -E '^Unattended-Upgrade::(Origins-Pattern|Allowed-Origins)' > /tmp/after
  echo "    after:"; sed 's/^/      /' /tmp/after
  chk "every origin the machine had is still there" bash -c 'grep -vxFf /tmp/after /tmp/before | grep -c . | grep -qx 0'
  chk "ours added" grep -q 'origin=VibeSDR,label=VibeSDR,site=apt.vibesdr.net' /tmp/after
  ;;
tooold)
  bash /t/install.sh > /tmp/o 2>&1; rc=$?; tail -4 /tmp/o
  chk "refused with exit 3 (got $rc)" [ $rc = 3 ]
  chk "plain reason given" grep -q 'needs Debian 12, Ubuntu 22.04' /tmp/o
  chk "repository line and key not left behind" [ ! -e /etc/apt/sources.list.d/vibesdr.list -a ! -e /usr/share/keyrings/vibesdr.gpg ]
  ;;
nosse2)
  printf 'processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: Intel(R) Pentium(R) III CPU\nflags\t\t: fpu vme de pse tsc msr pae mce cx8 sep mtrr pge mca cmov pat pse36 mmx fxsr sse\n' > /tmp/cpuinfo
  VIBE_CPUINFO=/tmp/cpuinfo bash /t/install.sh > /tmp/o 2>&1; rc=$?; cat /tmp/o
  chk "refused with exit 4 (got $rc)" [ $rc = 4 ]
  chk "nothing written" [ ! -e /etc/apt/sources.list.d/vibesdr.list -a ! -e /usr/share/keyrings/vibesdr.gpg ]
  ;;
esac
echo "### RESULT $PRETTY_NAME $ARCH $MODE: pass=$pass fail=$fail"
