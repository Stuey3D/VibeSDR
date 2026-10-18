#!/usr/bin/env bash
# publish-apt-remote.sh — THE NORMAL WAY TO RELEASE VibeServer to apt.vibesdr.net (2026-10-18).
#
#   scripts/publish-apt-remote.sh                      # all four arches: build, index, sign, commit, PUSH
#   scripts/publish-apt-remote.sh --dry-run            # everything except the push (commits locally)
#   APT_DIR=<scratch clone> scripts/publish-apt-remote.sh --dry-run --test-rev 0~remote1
#                                                      # rehearse the whole pipeline at a test revision
#
# ★★★ WHO DOES WHAT (Stuart, 2026-10-18: "the docker build can we do that remotely on the lenovo going forward
#     that has loads of space"). The Mac's Docker VM is gone. So:
#       the LENOVO (192.168.86.13)  BUILDS all four packages (scripts/build-debs-remote.sh — amd64/i386 native,
#                                   armhf/arm64 cross, all in bookworm containers) and INDEXES them: a copy of the
#                                   apt repository is synced over, scripts/publish-apt.sh runs in a bookworm
#                                   container with VIBE_INDEX_ONLY=1 (pool, prune, Packages, Release, page).
#       the MAC                     runs the gates, SIGNS the Release that comes back, commits, and pushes.
#     The Mac has no dpkg tools, which is why the index cannot be built here.
# ★★★ THE SIGNING KEY NEVER LEAVES THE MAC (security audit 2026-10-03). It is read from
#     ~/Documents/VibeSDR-keys (VIBESERVER_SIGNING_KEY) into a THROWAWAY GNUPGHOME that is deleted on every exit,
#     success or failure. Nothing key-shaped is ever written to the Lenovo — it only ever sees an UNSIGNED Release.
# ★★ THE BACKUP is scripts/publish-apt-docker.sh, for when the Lenovo is unavailable (it builds on the Mac and
#    needs colima recreated first). Both routes run the same publish-apt.sh, so the rules cannot drift:
#    per-arch revision high-water mark, KEEP=3 prune per arch, Release naming EVERY architecture in the pool,
#    install.sh copied to the root, the page written from the signed indexes.
# ★★ ALL FOUR ARCHITECTURES BY DEFAULT — a stale architecture is invisible from the publishing end (apt on the
#    machine left behind reports no updates and no error). --arch exists for a genuine emergency.
# ★ The Lenovo is a LIVE RECEIVER: nice 19, ≤4 jobs, --rm containers; ssh/rsync fail within a minute if its
#   Wi-Fi drops. Nothing is published unless every step got through, and nothing is pushed on --dry-run.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${VIBE_REMOTE_HOST:-192.168.86.13}"
RSRC="${VIBE_REMOTE_SRC_DIR:-vibesdr-src}"
RAPT="${VIBE_REMOTE_APT_DIR:-vibesdr-apt}"     # the Lenovo's working copy of the repository (no .git)
APT_DIR="${APT_DIR:-$HOME/VibeServer}"
KEY_FILE="${VIBESERVER_SIGNING_KEY:-$HOME/Documents/VibeSDR-keys/vibesdr-apt-signing-key.asc}"
GPG="${GPG:-/opt/homebrew/bin/gpg}"
GPGV="$(dirname "$GPG")/gpgv"
GPGCONF="$(dirname "$GPG")/gpgconf"
ARCHES="amd64 arm64 armhf i386"
DRY_RUN=""
TEST_REV=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run)  DRY_RUN=1 ;;
    --test-rev) shift; TEST_REV="$1" ;;
    --arch)     shift; ARCHES="${1//,/ }" ;;
    *) echo "usage: $0 [--dry-run] [--test-rev 0~tag] [--arch 'amd64 arm64 armhf i386']"; exit 1 ;;
  esac
  shift
done
for A in $ARCHES; do
  case "$A" in amd64|arm64|armhf|i386) ;; *) echo "!! unknown architecture $A"; exit 1;; esac
done
[ "$ARCHES" = "amd64 arm64 armhf i386" ] || echo "!! WARNING: publishing only '$ARCHES' — the others stay on their old version"

# ★★★ A TEST REVISION NEVER REACHES THE REAL REPOSITORY. `0~…` sorts below every real revision so it could not
#     shadow one, but it has no business in the pool at all: --dry-run is required, and so is a scratch APT_DIR.
if [ -n "$TEST_REV" ]; then
  case "$TEST_REV" in 0~*) ;; *) echo "!! --test-rev must start 0~ (it then sorts below every real revision)"; exit 1;; esac
  [ -n "$DRY_RUN" ] || { echo "!! --test-rev needs --dry-run"; exit 1; }
  [ "$(cd "$APT_DIR" 2>/dev/null && pwd -P)" != "$(cd "$HOME/VibeServer" 2>/dev/null && pwd -P)" ] \
    || { echo "!! --test-rev into ~/VibeServer refused — clone it to a scratch directory and set APT_DIR"; exit 1; }
fi

# ── gates, all before a minute is spent building ────────────────────────────────────────────────
# ★★★ THE COMPATIBILITY GATE (docs/PROTOCOL.md, BRIEF-v11-compatibility §8) — as publish-apt-docker.sh.
if [ -n "${VIBE_COMPAT_TARGET:-}" ]; then
  echo "==> compat gate against $VIBE_COMPAT_TARGET"
  bash "$SRC_DIR/scripts/compat/run.sh" "$VIBE_COMPAT_TARGET" || { echo "!! compat gate FAILED — not publishing"; exit 1; }
elif [ -n "${VIBE_COMPAT_SKIP:-}" ]; then
  echo "==> compat gate SKIPPED: $VIBE_COMPAT_SKIP"
else
  echo "!! no VIBE_COMPAT_TARGET (a server on the candidate build) and no VIBE_COMPAT_SKIP=<reason> — refusing to publish"; exit 1
fi
# ★★★ THE WEB PAGE IS A GENERATED FILE, AND A STALE ONE SHIPS SILENTLY (2026-10-18; see publish-apt-docker.sh).
_page="$SRC_DIR/android/app/src/main/cpp/vibe_web_page.h"
( cd "$SRC_DIR" && node scripts/build-web.mjs >/dev/null ) || { echo "!! scripts/build-web.mjs failed — not publishing"; exit 1; }
if ! git -C "$SRC_DIR" diff --quiet -- "$_page"; then
  echo "!! vibe_web_page.h was out of date with web/client — it has been rebuilt; review and commit it, then publish"
  exit 1
fi
bash -n "$SRC_DIR/scripts/install.sh" || { echo "!! scripts/install.sh has a syntax error"; exit 1; }

# ★★ THE KEY, checked before the long build — and the installer must name it (KEY_FPR in scripts/install.sh),
#    or the one-liner would refuse the repository on every machine.
[ -x "$GPG" ] && [ -x "$GPGV" ] || { echo "!! no gpg/gpgv at $GPG (brew install gnupg, or set GPG)"; exit 1; }
[ -r "$KEY_FILE" ] || { echo "!! no signing key at $KEY_FILE (set VIBESERVER_SIGNING_KEY)"; exit 1; }
[ "$(grep -c 'PRIVATE KEY BLOCK' "$KEY_FILE" || true)" -gt 0 ] || {
  echo "!! $KEY_FILE is not a secret key — it looks like the PUBLIC one (KEY.gpg cannot sign anything)"; exit 1; }
GNUPGHOME="$(mktemp -d "${TMPDIR:-/tmp}/vibe-gnupg.XXXXXX")"; export GNUPGHOME
chmod 700 "$GNUPGHOME"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/vibe-publish.XXXXXX")"
SYNCED_BACK=""
cleanup() {
  "$GPGCONF" --kill all >/dev/null 2>&1 || true
  rm -rf "$GNUPGHOME" "$WORK"
  if [ -n "$SYNCED_BACK" ] && [ "${_ok:-}" != 1 ]; then
    echo "!! stopped after the index came back — $APT_DIR is modified and UNSIGNED/uncommitted. To discard:"
    echo "!!   git -C $APT_DIR reset --hard && git -C $APT_DIR clean -fd"
  fi
}
trap cleanup EXIT
"$GPG" --batch --quiet --import "$KEY_FILE"
FPR="$("$GPG" --batch --with-colons --list-secret-keys | awk -F: '$1=="fpr"{print $10; exit}')"
[ -n "$FPR" ] || { echo "!! no secret key imported from $KEY_FILE"; exit 1; }
echo "$FPR:6:" | "$GPG" --batch --quiet --import-ownertrust
[ "$(grep -c "^KEY_FPR=\"$FPR\"" "$SRC_DIR/scripts/install.sh" || true)" = 1 ] \
  || { echo "!! scripts/install.sh does not name the signing key ($FPR) — fix KEY_FPR there first"; exit 1; }
echo "==> signing key $FPR (throwaway keyring on this Mac only)"

# ── the repository clone (the copy that must survive lives HERE) ───────────────────────────────
[ -d "$APT_DIR/.git" ] || { echo "!! $APT_DIR is not a clone of the apt repository"
  echo "   git clone --depth 1 https://github.com/Stuey3D/VibeServer.git $APT_DIR"; exit 1; }
[ -z "$(git -C "$APT_DIR" status --porcelain)" ] || { echo "!! $APT_DIR has uncommitted changes — refusing"; exit 1; }
# ★ A stale clone would compute revisions that are already public. ff-only: never merge anything here.
git -C "$APT_DIR" pull --ff-only -q || { echo "!! could not fast-forward $APT_DIR — refusing"; exit 1; }

# ── the link ───────────────────────────────────────────────────────────────────────────────────
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=4"
rsh() {
  local rc=0
  # shellcheck disable=SC2086
  ssh $SSH_OPTS "$HOST" "$@" || rc=$?
  [ "$rc" != 255 ] || echo "!! ssh to $HOST failed or DROPPED (exit 255) — nothing was published" >&2
  return "$rc"
}
rsync_host() { rsync -a --timeout=60 -e "ssh $SSH_OPTS" "$@" || { echo "!! rsync with $HOST failed (exit $?) — nothing was published" >&2; return 1; }; }
rsh true || { echo "!! cannot reach $HOST"; exit 1; }
echo "==> $HOST: $(rsh uptime)"
HUID="$(rsh id -u)"; HGID="$(rsh id -g)"

# ── 1. the tree and the repository go over ─────────────────────────────────────────────────────
VIBE_REMOTE_HOST="$HOST" VIBE_REMOTE_SRC_DIR="$RSRC" bash "$SRC_DIR/scripts/build-debs-remote.sh" --sync-only
echo "==> syncing $APT_DIR (without .git) to $HOST:~/$RAPT"
rsync_host --delete --exclude .git "$APT_DIR/" "$HOST:$RAPT/"

# ── 2. which revision each architecture is owed ────────────────────────────────────────────────
# ★★ ASKED OF publish-apt.sh ITSELF (--next-rev), so the high-water-mark rule exists once. Plain bookworm-slim:
#    the question needs only grep and sed, and it is asked before any build image exists.
RUNC="docker run --rm --user $HUID:$HGID -e HOME=/tmp -v \$HOME/$RSRC:/src:ro"
declare_ver() { eval "VER_$1=\$2"; }
getver() { eval "echo \"\${VER_$1:-}\""; }
for A in $ARCHES; do
  v="$(rsh "$RUNC -v \$HOME/$RAPT:/apt:ro -e APT_DIR=/apt -e VIBE_PUBLISH_ARCH=$A debian:bookworm-slim \
            bash /src/scripts/publish-apt.sh --next-rev" | tail -1)"
  case "$v" in [0-9]*-[0-9]*) ;; *) echo "!! [$A] could not read the next revision (got '$v')"; exit 1;; esac
  declare_ver "$A" "$v"
  echo "==> [$A] next version: $v"
done
UPSTREAM="$(getver "${ARCHES%% *}")"; UPSTREAM="${UPSTREAM%-*}"
if [ -n "$TEST_REV" ]; then
  for A in $ARCHES; do declare_ver "$A" "$UPSTREAM-$TEST_REV"; done
  echo "==> TEST revision: building and indexing $UPSTREAM-$TEST_REV for every architecture (dry run, scratch repository)"
fi

# ── 3. build on the Lenovo — one call per distinct revision (normally one call for all four) ───
REVS=""
for A in $ARCHES; do r="$(getver "$A")"; r="${r##*-}"; case " $REVS " in *" $r "*) ;; *) REVS="$REVS $r";; esac; done
for r in $REVS; do
  as=""; for A in $ARCHES; do v="$(getver "$A")"; [ "${v##*-}" = "$r" ] && as="$as $A"; done
  echo "==> building revision $r for:$as"
  VIBE_REMOTE_HOST="$HOST" VIBE_REMOTE_SRC_DIR="$RSRC" bash "$SRC_DIR/scripts/build-debs-remote.sh" --rev "$r" --arch "${as# }"
done

# ── 4. index on the Lenovo (publish-apt.sh, VIBE_INDEX_ONLY — no key there) ─────────────────────
# ★ One architecture after another in ONE container: they share Packages/Release/index.html, and the LAST pass
#   writes the Release that names every architecture in the pool.
LOOP=""
for A in $ARCHES; do
  v="$(getver "$A")"
  rsh "test -f ~/vibeout/$A/vibeserver_${v}_$A.deb" || { echo "!! [$A] vibeserver_${v}_$A.deb is not in ~/vibeout/$A on $HOST"; exit 1; }
  LOOP="$LOOP VIBE_PREBUILT_DEB=/debs/$A/vibeserver_${v}_$A.deb VIBE_PUBLISH_ARCH=$A bash /src/scripts/publish-apt.sh;"
done
TESTENV=""; [ -z "$TEST_REV" ] || TESTENV="-e VIBE_TEST_REV=$TEST_REV"
rsh "docker image inspect vibeserver-build:bookworm-amd64 >/dev/null 2>&1 || \
       (cd ~/$RSRC/vibeserver/linux && nice -n 19 docker build --platform linux/amd64 --build-arg DEPS_NICE=19 -q \
          -f Dockerfile.build -t vibeserver-build:bookworm-amd64 . >/dev/null)"
echo "==> indexing on $HOST:"
rsh "$RUNC --cpus 2 -v \$HOME/$RAPT:/apt -v \$HOME/vibeout:/debs:ro -e APT_DIR=/apt -e VIBE_INDEX_ONLY=1 $TESTENV \
       vibeserver-build:bookworm-amd64 nice -n 19 bash -euo pipefail -c '$LOOP'" | sed 's/^/    /'

# ── 5. the result comes back — dists/, the pool's additions and removals, the page ────────────
echo "==> bringing the index and pool back to $APT_DIR"
SYNCED_BACK=1
rsync_host --delete --exclude .git "$HOST:$RAPT/" "$APT_DIR/"

# ── 6. sign HERE ───────────────────────────────────────────────────────────────────────────────
DIST="$APT_DIR/dists/stable"
rm -f "$DIST/InRelease" "$DIST/Release.gpg"
"$GPG" --batch --yes --default-key "$FPR" --clearsign -o "$DIST/InRelease" "$DIST/Release"
"$GPG" --batch --yes --default-key "$FPR" --armor --detach-sign -o "$DIST/Release.gpg" "$DIST/Release"
# ★ KEY.gpg: re-exported, but the committed file is KEPT when it holds exactly the same key — Homebrew's gpg
#   armours differently from bookworm's, and a churned KEY.gpg in every commit would hide a real key change.
"$GPG" --batch --armor --export "$FPR" > "$WORK/KEY.gpg"
if [ -f "$APT_DIR/KEY.gpg" ] && [ "$("$GPG" --batch --with-colons --show-keys "$APT_DIR/KEY.gpg" 2>/dev/null | grep -E '^(pub|sub|fpr|uid)')" \
     = "$("$GPG" --batch --with-colons --show-keys "$WORK/KEY.gpg" | grep -E '^(pub|sub|fpr|uid)')" ]; then
  echo "==> KEY.gpg unchanged (same key)"
else
  cp "$WORK/KEY.gpg" "$APT_DIR/KEY.gpg"; echo "==> KEY.gpg re-exported"
fi
echo "==> signed InRelease + Release.gpg with $FPR"

# ── 7. verify what apt will verify, with nothing but the published KEY.gpg ─────────────────────
"$GPG" --batch --dearmor < "$APT_DIR/KEY.gpg" > "$WORK/key.kbx.gpg"
"$GPGV" --quiet --keyring "$WORK/key.kbx.gpg" "$DIST/InRelease" 2>"$WORK/v1" || { cat "$WORK/v1"; echo "!! InRelease does not verify"; exit 1; }
"$GPGV" --quiet --keyring "$WORK/key.kbx.gpg" "$DIST/Release.gpg" "$DIST/Release" 2>"$WORK/v2" || { cat "$WORK/v2"; echo "!! Release.gpg does not verify"; exit 1; }
echo "==> InRelease and Release.gpg verify against KEY.gpg"
# ★★ Every index the Release lists must match its SHA256 line, and every package an index lists must match ITS
#    SHA256 — the pool travelled over the network twice, and apt refuses the lot on one mismatch.
awk '/^SHA256:/{f=1;next} /^[A-Za-z]/{f=0} f&&NF==3{print $1, $3}' "$DIST/Release" | while read -r sum path; do
  [ -f "$DIST/$path" ] || continue
  [ "$(shasum -a 256 "$DIST/$path" | awk '{print $1}')" = "$sum" ] || { echo "!! $path does not match the Release"; exit 1; }
done
for d in "$DIST/main"/binary-*; do
  awk '/^Filename:/{f=$2} /^SHA256:/{print $2, f}' "$d/Packages" | while read -r sum f; do
    [ "$(shasum -a 256 "$APT_DIR/$f" | awk '{print $1}')" = "$sum" ] || { echo "!! $f does not match $(basename "$d")/Packages"; exit 1; }
  done
done
grep -q "^Architectures: $(cd "$DIST/main" && ls -d binary-* | sed 's/^binary-//' | sort | tr '\n' ' ' | sed 's/ $//')\$" "$DIST/Release" \
  || { echo "!! Release does not list every architecture in dists/: $(grep ^Architectures: "$DIST/Release")"; exit 1; }
echo "==> every index and package matches its checksum; Release lists $(sed -n 's/^Architectures: //p' "$DIST/Release")"

# ── 8. what the repository will offer ──────────────────────────────────────────────────────────
echo "==> the repository will offer:"
for d in "$DIST/main"/binary-*; do
  printf '      %-8s %s\n' "$(basename "$d" | sed 's/^binary-//')" \
    "$(grep -oE '^Version: [0-9][^ ]*' "$d/Packages" | awk '{print $2}' | sort -V | tail -1)"
done

# ── 9. commit (always), push (never on --dry-run) ──────────────────────────────────────────────
VERS=""; for A in $ARCHES; do VERS="$VERS $(getver "$A")"; done
VERS="$(echo "$VERS" | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ' ' | sed 's/ $//')"
git -C "$APT_DIR" add -A
git -C "$APT_DIR" -c user.name="VibeSDR release" -c user.email="packages@vibesdr.net" \
    commit -q -m "vibeserver $VERS ($ARCHES)${TEST_REV:+ — TEST, never pushed}" || { echo "!! nothing to commit"; exit 1; }
_ok=1
echo "==> committed in $APT_DIR: $(git -C "$APT_DIR" log --oneline -1)"
git -C "$APT_DIR" show --stat --format= HEAD | tail -1
if [ -n "$DRY_RUN" ]; then
  echo "==> dry run: nothing pushed. $APT_DIR holds the signed result."
  exit 0
fi
echo "==> pushing"
git -C "$APT_DIR" push
echo "==> published: https://apt.vibesdr.net/  (GitHub Pages takes a few seconds before apt sees it)"
