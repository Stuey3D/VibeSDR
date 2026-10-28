#!/bin/bash
# test_first_update_inner.sh — vibeserver-maintenance's `first-update` (a new card's ONE update check, 2026-10-28), run for
# real: real apt, real dpkg, a real HTTP repository on 127.0.0.1 answering as apt.vibesdr.net. Runs INSIDE a throwaway
# Debian bookworm container with NO network (test_first_update_docker.sh starts it) — never on a real box.
#
# Proves, each from a fresh start:
#   · online + newer          → "updated", the new version installed, and ONLY vibeserver (another upgradable package
#                               stays as it was);
#   · online + same version   → "up to date", nothing installed;   · repository has only an OLDER one → never a downgrade;
#   · offline (nothing answers) → "offline" + the "proceeding with setup" line, nothing installed;
#   · a repository that hangs → stopped at the time limit, "offline", nothing installed;
#   · a failed download       → "failed", old version intact; asked again → NOT run again (once means once);
#   · Cancel during the download → "cancelled", old version intact, dpkg clean, apt usable straight after;
#   · Cancel pressed before the helper even started → "cancelled";
#   · Cancel while dpkg is installing → ignored, dpkg finishes: "updated" (never a half-installed package);
#   · a box that was not set up from a new card (no arm file) → nothing, no record;
#   · a beta card → the newer beta; a beta card with no newer beta → up to date, never down to stable.
set -u
SRC=/src/vibeserver/linux
HELPER=/usr/lib/vibeserver/vibeserver-maintenance
REC=/var/lib/vibeserver-pocket/first-update
CANCEL=/var/lib/vibeserver/first-update.cancel
ARM=/etc/vibeserver-pocket/first-update
LOG=/var/lib/vibeserver-maint/maintenance.log
W=/tmp/ft
ARCH=$(dpkg --print-architecture)
pass=0; fail=0
ok() { if [ "$1" = 0 ]; then echo "  ok    $2"; pass=$((pass + 1)); else echo "  FAIL  $2"; fail=$((fail + 1)); fi; }
is() { [ "$1" = "$2" ]; echo $?; }
has() { grep -qF -- "$2" "$1"; echo $?; }

# ── the box ──────────────────────────────────────────────────────────────────────────────────────────────────
rm -f /etc/apt/sources.list.d/*.sources /etc/apt/sources.list
groupadd -r vibeserver 2>/dev/null || true
mkdir -p /usr/lib/vibeserver /var/lib/vibeserver /etc/vibeserver-pocket /var/lib/vibeserver-pocket "$W/debs"
install -m 0755 "$SRC/vibeserver-maintenance" "$HELPER"
echo '127.0.0.1 apt.vibesdr.net' >> /etc/hosts

mkdeb() {   # <name> <version> [preinst sleep s] [payload KB]
  local d="$W/build/$1-$2"
  rm -rf "$d"; mkdir -p "$d/DEBIAN" "$d/usr/share/$1"
  printf 'Package: %s\nVersion: %s\nArchitecture: %s\nMaintainer: test <t@example.org>\nDescription: test package\n' \
    "$1" "$2" "$ARCH" > "$d/DEBIAN/control"
  if [ "${3:-0}" != 0 ]; then printf '#!/bin/sh\nsleep %s\n' "$3" > "$d/DEBIAN/preinst"; chmod 0755 "$d/DEBIAN/preinst"; fi
  head -c "$(( ${4:-4} * 1024 ))" /dev/urandom > "$d/usr/share/$1/blob"
  dpkg-deb -Zxz -z0 --build "$d" "$W/debs/$1_$2_$ARCH.deb" >/dev/null
}
mkdeb vibeserver 11.0.14-1
mkdeb vibeserver 11.0.15-1
mkdeb vibeserver 11.0.16-1 6            # dpkg takes 6 s on it: time to press Cancel while it installs
mkdeb vibeserver 11.0.17-1 0 3072       # 3 MB of noise, served slowly: time to press Cancel while it downloads
mkdeb vibeserver 12.0.0~beta3-1
mkdeb vibeserver 12.0.0~beta4-1
mkdeb otherpkg 1.0-1
mkdeb otherpkg 1.1-1

mkrepo() {   # <stable debs…> -- <beta debs…>
  local R="$W/repo" suite=stable f
  rm -rf "$R"; mkdir -p "$R/pool/stable" "$R/pool/beta"
  for f in "$@"; do
    if [ "$f" = -- ]; then suite=beta; continue; fi
    cp "$W/debs/$f" "$R/pool/$suite/"
  done
  for suite in stable beta; do
    mkdir -p "$R/dists/$suite/main/binary-$ARCH"
    ( cd "$R" && dpkg-scanpackages --multiversion "pool/$suite" /dev/null 2>/dev/null ) > "$R/dists/$suite/main/binary-$ARCH/Packages"
    ( cd "$R/dists/$suite" && apt-ftparchive -o "APT::FTPArchive::Release::Suite=$suite" \
        -o "APT::FTPArchive::Release::Codename=$suite" -o "APT::FTPArchive::Release::Architectures=$ARCH" \
        -o APT::FTPArchive::Release::Components=main release . > Release.new && mv Release.new Release )
  done
}

# ★ socket + threading only: the build image's python3 is the minimal one (no http.server).
cat > "$W/server.py" <<'PY'
import os, socket, sys, threading, time
MODE = sys.argv[1]; ROOT = sys.argv[2]
def handle(c):
    try:
        req = b""
        while b"\r\n\r\n" not in req:
            d = c.recv(4096)
            if not d: return
            req += d
        if MODE == "hang":
            time.sleep(3600); return
        path = req.split(b" ")[1].decode().split("?")[0].replace("%7e", "~").replace("%7E", "~")
        p = os.path.normpath(os.path.join(ROOT, path.lstrip("/")))
        if not p.startswith(ROOT) or not os.path.isfile(p) or (path.endswith(".deb") and MODE == "nodeb"):
            c.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); return
        data = open(p, "rb").read()
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(data))
        if path.endswith(".deb") and MODE == "slow":
            for i in range(0, len(data), 4096):
                c.sendall(data[i:i + 4096]); time.sleep(0.1)          # ~40 KB/s
        else:
            c.sendall(data)
    except Exception:
        pass
    finally:
        try: c.close()
        except Exception: pass
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 80)); s.listen(16)
while True:
    c, _ = s.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
PY
SRV=""
serve() { stop_serve; python3 "$W/server.py" "$1" "$W/repo" & SRV=$!; sleep 0.7; }
stop_serve() { [ -n "$SRV" ] && { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }; SRV=""; }

fresh() {   # <installed vibeserver version> [beta]
  stop_serve
  dpkg -i "$W/debs/vibeserver_$1_$ARCH.deb" "$W/debs/otherpkg_1.0-1_$ARCH.deb" >/dev/null 2>&1
  rm -f "$REC" "$CANCEL" "$LOG" /etc/apt/sources.list.d/vibesdr*.list
  rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*.deb
  echo "deb [trusted=yes] http://apt.vibesdr.net stable main" > /etc/apt/sources.list.d/vibesdr.list
  [ "${2:-}" = beta ] && echo "deb [trusted=yes] http://apt.vibesdr.net beta main" > /etc/apt/sources.list.d/vibesdr-beta.list
  : > "$ARM"
}
ask() { echo first-update > /var/lib/vibeserver/maintenance.request; timeout 600 "$HELPER" > "$W/out" 2>&1; }
ask_bg() { echo first-update > /var/lib/vibeserver/maintenance.request; timeout 600 "$HELPER" > "$W/out" 2>&1 & BG=$!; }
state() { sed -n 's/^state=//p' "$REC" 2>/dev/null; }
field() { sed -n "s/^$1=//p" "$REC" 2>/dev/null; }
ver() { dpkg-query -W -f='${Version}' "$1" 2>/dev/null; }
wait_state() { local n=0; while [ "$(state)" != "$1" ] && [ $n -lt 600 ]; do sleep 0.1; n=$((n + 1)); done; [ "$(state)" = "$1" ]; }
dpkg_clean() { [ -z "$(dpkg --audit 2>&1)" ] && apt-get -qq check >/dev/null 2>&1; }

echo "first-update — online, a newer VibeServer"
mkrepo vibeserver_11.0.15-1_$ARCH.deb vibeserver_11.0.16-1_$ARCH.deb otherpkg_1.0-1_$ARCH.deb otherpkg_1.1-1_$ARCH.deb
fresh 11.0.15-1; serve ok; ask
ok "$(is "$(state)" updated)" "state updated  [$(state)]"
ok "$(is "$(ver vibeserver)" 11.0.16-1)" "11.0.16-1 installed  [$(ver vibeserver)]"
ok "$(is "$(field from)/$(field to)" 11.0.15-1/11.0.16-1)" "the record says from 11.0.15-1 to 11.0.16-1"
ok "$(is "$(ver otherpkg)" 1.0-1)" "ONLY vibeserver: otherpkg 1.1 was available and stays at 1.0  [$(ver otherpkg)]"
ok "$(has "$W/out" 'Updated to VibeServer 11.0.16-1.')" "the log says \"Updated to VibeServer 11.0.16-1.\""
ok "$(has "$W/out" '__VIBESERVER_DONE__ 0')" "the admin page's done marker, exit 0"
ask
ok "$(has "$W/out" 'already run once')" "asked again: refused, already run once"
ok "$(is "$(state)" updated)" "…and the record is unchanged"

echo "first-update — online, already the newest"
mkrepo vibeserver_11.0.15-1_$ARCH.deb
fresh 11.0.15-1; serve ok; ask
ok "$(is "$(state)" uptodate)" "state uptodate  [$(state)]"
ok "$(has "$W/out" 'VibeServer is up to date (11.0.15-1).')" "the log says up to date"
ok "$(is "$(ver vibeserver)" 11.0.15-1)" "nothing installed"
ok "$(is "$(ls /var/lib/apt/lists | grep -c Packages)" 0)" "the quick look wrote no apt lists to the root (a throwaway directory)"

echo "first-update — the repository has only an OLDER one"
mkrepo vibeserver_11.0.14-1_$ARCH.deb
fresh 11.0.15-1; serve ok; ask
ok "$(is "$(state)" uptodate)" "state uptodate — never a downgrade  [$(state)]"
ok "$(is "$(ver vibeserver)" 11.0.15-1)" "11.0.15-1 still installed"

echo "first-update — offline"
mkrepo vibeserver_11.0.15-1_$ARCH.deb vibeserver_11.0.16-1_$ARCH.deb
fresh 11.0.15-1; stop_serve; t0=$(date +%s); ask; t1=$(date +%s)
ok "$(is "$(state)" offline)" "state offline  [$(state)]"
ok "$(has "$W/out" 'VibeServer cannot check for updates')" "the log says it cannot check — proceeding with setup"
ok "$(is "$(ver vibeserver)" 11.0.15-1)" "nothing installed"
ok "$([ $((t1 - t0)) -lt 30 ]; echo $?)" "and quickly: $((t1 - t0)) s"
ask
ok "$(has "$W/out" 'already run once')" "★ never retried by itself: a second request is refused"

echo "first-update — the repository accepts and never answers"
fresh 11.0.15-1; serve hang; t0=$(date +%s); VIBE_FIRST_CHECK_SECS=8 ask; t1=$(date +%s)
ok "$(is "$(state)" offline)" "state offline  [$(state), why=$(field why)]"
ok "$(is "$(field why)" timeout)" "why=timeout"
ok "$([ $((t1 - t0)) -lt 25 ]; echo $?)" "stopped at the limit: $((t1 - t0)) s"
ok "$(is "$(ver vibeserver)" 11.0.15-1)" "nothing installed"

echo "first-update — the download fails"
fresh 11.0.15-1; serve nodeb; ask
ok "$(is "$(state)" failed)" "state failed  [$(state), why=$(field why)]"
ok "$(is "$(ver vibeserver)" 11.0.15-1)" "11.0.15-1 intact"
ok "$(has "$W/out" 'Update it later from the admin page')" "the log points to the admin page"
ok "$(dpkg_clean; echo $?)" "dpkg and apt clean"
serve ok; ask
ok "$(has "$W/out" 'already run once')" "★ NOT retried, even with the repository back"
ok "$(is "$(ver vibeserver)" 11.0.15-1)" "…still 11.0.15-1 (the owner's Update button is the way now)"

echo "first-update — Cancel during the download"
mkrepo vibeserver_11.0.15-1_$ARCH.deb vibeserver_11.0.17-1_$ARCH.deb
fresh 11.0.15-1; serve slow; ask_bg
wait_state downloading; sleep 2
ok "$(is "$(state)" downloading)" "downloading the slow 3 MB package"
touch "$CANCEL"; t0=$(date +%s); wait "$BG"; t1=$(date +%s)
ok "$(is "$(state)" cancelled)" "state cancelled  [$(state)]  after $((t1 - t0)) s"
ok "$(is "$(ver vibeserver)" 11.0.15-1)" "11.0.15-1 intact"
ok "$(dpkg_clean; echo $?)" "dpkg and apt clean — no half-installed package"
ok "$([ ! -e "$CANCEL" ]; echo $?)" "the cancel file is removed"
ok "$(! pgrep -x apt-get >/dev/null; echo $?)" "no apt-get left running"
ok "$(apt-get -qq update >/dev/null 2>&1; echo $?)" "apt works straight after (no lock left)"

echo "first-update — Cancel pressed before the helper started"
mkrepo vibeserver_11.0.15-1_$ARCH.deb vibeserver_11.0.16-1_$ARCH.deb
fresh 11.0.15-1; serve ok; touch "$CANCEL"; ask
ok "$(is "$(state)" cancelled)" "state cancelled  [$(state)]"
ok "$(is "$(ver vibeserver)" 11.0.15-1)" "11.0.15-1 intact"

echo "first-update — Cancel while dpkg installs"
fresh 11.0.15-1; serve ok; ask_bg
wait_state installing; touch "$CANCEL"; wait "$BG"
ok "$(is "$(state)" updated)" "dpkg was let finish: updated  [$(state)]"
ok "$(is "$(ver vibeserver)" 11.0.16-1)" "11.0.16-1 installed whole"
ok "$(dpkg_clean; echo $?)" "dpkg clean"
ok "$(has "$W/out" 'cannot be cancelled')" "the log said the install cannot be cancelled"

echo "first-update — not a new card"
fresh 11.0.15-1; serve ok; rm -f "$ARM"; ask
ok "$([ ! -e "$REC" ]; echo $?)" "no arm file: nothing run, no record"
ok "$(is "$(ver vibeserver)" 11.0.15-1)" "nothing installed"

echo "first-update — a beta card"
mkrepo vibeserver_11.0.16-1_$ARCH.deb -- vibeserver_12.0.0~beta3-1_$ARCH.deb vibeserver_12.0.0~beta4-1_$ARCH.deb
fresh 12.0.0~beta3-1 beta; serve ok; ask
ok "$(is "$(ver vibeserver)" 12.0.0~beta4-1)" "the newer beta  [$(ver vibeserver)]"
mkrepo vibeserver_11.0.16-1_$ARCH.deb -- vibeserver_12.0.0~beta3-1_$ARCH.deb
fresh 12.0.0~beta3-1 beta; serve ok; ask
ok "$(is "$(state)" uptodate)" "no newer beta: up to date  [$(state)]"
ok "$(is "$(ver vibeserver)" 12.0.0~beta3-1)" "never down to 11.0.16 stable"

stop_serve
echo "first-update: $pass passed, $fail failed"
[ "$fail" = 0 ]
