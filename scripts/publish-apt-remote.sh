#!/usr/bin/env bash
# publish-apt-remote.sh — THE NORMAL WAY TO RELEASE VibeServer to apt.vibesdr.net (2026-10-18).
#
#   scripts/publish-apt-remote.sh                      # all four arches: build, index, sign, commit, PUSH
#   scripts/publish-apt-remote.sh --dry-run            # everything except the push (commits locally)
#   APT_DIR=<scratch clone> scripts/publish-apt-remote.sh --dry-run --test-rev 0~remote1
#                                                      # rehearse the whole pipeline at a test revision
#   scripts/publish-apt-remote.sh --suite beta         # a V12 BETA: suite `beta` only (2026-10-22)
#
# ★★★ TWO SUITES (2026-10-22, "Receive beta updates"). --suite beta publishes into dists/beta + pool/beta, which only
#     a server whose owner ticked Receive beta updates has in its sources; the stable suite is not touched (and a stable
#     publish never touches beta). Signed with the same key. GUARDED BOTH WAYS, because the mistake is invisible from
#     here and lands on every machine: a beta suite takes only a prerelease version (12.0.0~beta1), and the stable
#     suite REFUSES a ~beta version — that would offer the beta to every server in the world.
#
# ★★★ WHO DOES WHAT (Stuart, 2026-10-25, on the new M5 Pro: "keep X86 builds on the Lenovo for speed, but the mac
#     for everything else as using the lenovo was causing some delays whilst it was working"). So:
#       the LENOVO (192.168.86.13)  BUILDS amd64 + i386, natively, in bookworm containers — nothing else.
#       the MAC                     BUILDS arm64 (native) + armhf (cross) in colima AT THE SAME TIME
#                                   (scripts/build-debs-remote.sh), then asks publish-apt.sh which revision each
#                                   architecture is owed, INDEXES (publish-apt.sh VIBE_INDEX_ONLY=1 in the arm64
#                                   bookworm image, straight into $APT_DIR), runs the gates, SIGNS, commits, pushes.
#     The apt repository never leaves the Mac now; only the source tree goes to the Lenovo, and only the two x86
#     packages come back. (2026-10-18 → 10-25 the Lenovo built and indexed all four — the old Mac had no room.)
#     If the Lenovo is down: `--arch 'arm64 armhf'` publishes the ARM half (and warns, as any partial publish does).
# ★★★ THE SIGNING KEY NEVER LEAVES THE MAC (security audit 2026-10-03). It is read from
#     ~/Documents/VibeSDR-keys (VIBESERVER_SIGNING_KEY) into a THROWAWAY GNUPGHOME that is deleted on every exit,
#     success or failure. Nothing key-shaped is ever written to the Lenovo — it only ever sees an UNSIGNED Release.
# ★★ THE BACKUP for a Mac whose Docker will not run is VIBE_ARM_ON_LENOVO=1 (the Lenovo cross-builds the ARM
#    half too, as it did 10-18 → 10-25). publish-apt-docker.sh is the older all-on-the-Mac route. Every route
#    runs the same publish-apt.sh, so the rules cannot drift:
#    per-arch revision high-water mark, KEEP=3 prune per arch, Release naming EVERY architecture in the pool,
#    install.sh copied to the root, the page written from the signed indexes.
# ★★ ALL FOUR ARCHITECTURES BY DEFAULT — a stale architecture is invisible from the publishing end (apt on the
#    machine left behind reports no updates and no error). --arch exists for a genuine emergency.
# ★ The Lenovo is a LIVE RECEIVER: nice 19, ≤4 jobs, --rm containers; ssh/rsync fail within a minute if its
#   Wi-Fi drops. Nothing is published unless every step got through, and nothing is pushed on --dry-run.
# ★ The Mac's Docker is colima (8 CPUs, 12 GB, 100 GB disk, in ~/.colima/default/colima.yaml); started if it is not.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${VIBE_REMOTE_HOST:-192.168.86.13}"
RSRC="${VIBE_REMOTE_SRC_DIR:-vibesdr-src}"
APT_DIR="${APT_DIR:-$HOME/VibeServer}"
KEY_FILE="${VIBESERVER_SIGNING_KEY:-$HOME/Documents/VibeSDR-keys/vibesdr-apt-signing-key.asc}"
GPG="${GPG:-/opt/homebrew/bin/gpg}"
GPGV="$(dirname "$GPG")/gpgv"
GPGCONF="$(dirname "$GPG")/gpgconf"
ARCHES="amd64 arm64 armhf i386"
DRY_RUN=""
TEST_REV=""
SUITE="stable"
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run)  DRY_RUN=1 ;;
    --test-rev) shift; TEST_REV="$1" ;;
    --arch)     shift; ARCHES="${1//,/ }" ;;
    --suite)    shift; SUITE="${1:-}" ;;
    *) echo "usage: $0 [--dry-run] [--test-rev 0~tag] [--arch 'amd64 arm64 armhf i386'] [--suite stable|beta]"; exit 1 ;;
  esac
  shift
done
case "$SUITE" in stable|beta) ;; *) echo "!! --suite must be stable or beta"; exit 1;; esac
echo "==> suite: $SUITE"
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
# ★ The revision lookup and the index run in this Mac's Docker, whatever is being built where.
command -v docker >/dev/null || { echo "!! no docker on this Mac (brew install colima docker)"; exit 1; }
docker info >/dev/null 2>&1 || { echo "==> starting colima"; colima start >/dev/null || { echo "!! colima would not start"; exit 1; }; }
[ "$(docker info --format '{{.Architecture}}')" = aarch64 ] || { echo "!! this Mac's Docker is not arm64 — refusing"; exit 1; }

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
    echo "!! stopped after indexing — $APT_DIR is modified and UNSIGNED/uncommitted. To discard:"
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

# ★ No link check here: build-debs-remote.sh checks the Lenovo before it starts, and only when an x86
#   architecture is being built.
UIDGID="$(id -u):$(id -g)"
# ── 1. which revision each architecture is owed ────────────────────────────────────────────────
# ★★ ASKED OF publish-apt.sh ITSELF (--next-rev), so the high-water-mark rule exists once. Plain bookworm-slim:
#    the question needs only grep and sed, and it is asked before any build image exists.
RUNC=(docker run --rm --user "$UIDGID" -e HOME=/tmp -v "$SRC_DIR":/src:ro)
declare_ver() { eval "VER_$1=\$2"; }
getver() { eval "echo \"\${VER_$1:-}\""; }
for A in $ARCHES; do
  v="$("${RUNC[@]}" -v "$APT_DIR":/apt:ro -e APT_DIR=/apt -e VIBE_PUBLISH_ARCH="$A" -e VIBE_SUITE="$SUITE" \
         debian:bookworm-slim bash /src/scripts/publish-apt.sh --next-rev | tail -1)"
  case "$v" in [0-9]*-[0-9]*) ;; *) echo "!! [$A] could not read the next revision (got '$v')"; exit 1;; esac
  declare_ver "$A" "$v"
  echo "==> [$A] next version: $v"
done
UPSTREAM="$(getver "${ARCHES%% *}")"; UPSTREAM="${UPSTREAM%-*}"
# ★★★ THE SUITE MUST MATCH THE VERSION — see the header. Checked before a minute is spent building.
case "$SUITE:$UPSTREAM" in
  beta:*~*)         ;;
  beta:*)           echo "!! $UPSTREAM is not a prerelease — a beta is versioned like 12.0.0~beta1 (VIBESERVER_PRERELEASE)"; exit 1 ;;
  stable:*~beta*)   echo "!! $UPSTREAM is a BETA — it would be offered to every server. Publish it with --suite beta"; exit 1 ;;
esac
if [ -n "$TEST_REV" ]; then
  for A in $ARCHES; do declare_ver "$A" "$UPSTREAM-$TEST_REV"; done
  echo "==> TEST revision: building and indexing $UPSTREAM-$TEST_REV for every architecture (dry run, scratch repository)"
fi

# ── 2. build: x86 on the Lenovo, ARM here — one call per distinct revision (normally one for all four) ──
REVS=""
for A in $ARCHES; do r="$(getver "$A")"; r="${r##*-}"; case " $REVS " in *" $r "*) ;; *) REVS="$REVS $r";; esac; done
for r in $REVS; do
  as=""; for A in $ARCHES; do v="$(getver "$A")"; [ "${v##*-}" = "$r" ] && as="$as $A"; done
  echo "==> building revision $r for:$as"
  VIBE_REMOTE_HOST="$HOST" VIBE_REMOTE_SRC_DIR="$RSRC" bash "$SRC_DIR/scripts/build-debs-remote.sh" --rev "$r" --arch "${as# }"
done

# ── 3. index HERE (publish-apt.sh, VIBE_INDEX_ONLY — the same step the Lenovo used to run) ──────
# ★ One architecture after another in ONE container: they share Packages/Release/index.html, and the LAST pass
#   writes the Release that names every architecture in the pool. It writes straight into $APT_DIR, as this
#   user (--user), so git sees the result with nothing to sync back.
PREBUILT="$SRC_DIR/vibeserver/build/prebuilt"
LOOP=""
for A in $ARCHES; do
  v="$(getver "$A")"
  [ -f "$PREBUILT/vibeserver_${v}_$A.deb" ] || { echo "!! [$A] vibeserver_${v}_$A.deb is not in $PREBUILT"; exit 1; }
  LOOP="$LOOP VIBE_PREBUILT_DEB=/debs/vibeserver_${v}_$A.deb VIBE_PUBLISH_ARCH=$A bash /src/scripts/publish-apt.sh;"
done
TESTENV=(); [ -z "$TEST_REV" ] || TESTENV=(-e "VIBE_TEST_REV=$TEST_REV")
docker image inspect vibeserver-build:bookworm-arm64 >/dev/null 2>&1 || \
  (cd "$SRC_DIR/vibeserver/linux" && nice -n 10 docker build --platform linux/arm64 -q \
     -f Dockerfile.build -t vibeserver-build:bookworm-arm64 . >/dev/null)
echo "==> indexing on this Mac:"
SYNCED_BACK=1
"${RUNC[@]}" --cpus 2 -v "$APT_DIR":/apt -v "$PREBUILT":/debs:ro -e APT_DIR=/apt -e VIBE_INDEX_ONLY=1 -e VIBE_SUITE="$SUITE" \
  ${TESTENV[@]+"${TESTENV[@]}"} vibeserver-build:bookworm-arm64 nice -n 10 bash -euo pipefail -c "$LOOP" | sed 's/^/    /'

# ── 4. sign HERE ───────────────────────────────────────────────────────────────────────────────
# ★ THIS SUITE's Release only; the other suite's signed files came back unchanged and stay as they were.
DIST="$APT_DIR/dists/$SUITE"
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

# ── 5. verify what apt will verify, with nothing but the published KEY.gpg ─────────────────────
"$GPG" --batch --dearmor < "$APT_DIR/KEY.gpg" > "$WORK/key.kbx.gpg"
"$GPGV" --quiet --keyring "$WORK/key.kbx.gpg" "$DIST/InRelease" 2>"$WORK/v1" || { cat "$WORK/v1"; echo "!! InRelease does not verify"; exit 1; }
"$GPGV" --quiet --keyring "$WORK/key.kbx.gpg" "$DIST/Release.gpg" "$DIST/Release" 2>"$WORK/v2" || { cat "$WORK/v2"; echo "!! Release.gpg does not verify"; exit 1; }
echo "==> InRelease and Release.gpg verify against KEY.gpg"
# ★★ Every index the Release lists must match its SHA256 line, and every package an index lists must match ITS
#    SHA256 — two of the packages travelled over the network, and apt refuses the lot on one mismatch.
awk '/^SHA256:/{f=1;next} /^[A-Za-z]/{f=0} f&&NF==3{print $1, $3}' "$DIST/Release" | while read -r sum path; do
  # ★ The Release lists ITSELF, with the checksum of a half-written file: publish-apt.sh runs
  #   `apt-ftparchive release dists/stable > dists/stable/Release`, so the file exists (truncated) while it is
  #   being scanned. Long-standing in the live repository too (a 219-byte "Release" line) and harmless — apt
  #   never fetches that entry — so it is skipped here rather than reported.
  case "$path" in Release|InRelease|Release.gpg) continue;; esac
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

# ── 6. what the repository will offer ──────────────────────────────────────────────────────────
echo "==> the repository will offer ($SUITE suite):"
for d in "$DIST/main"/binary-*; do
  printf '      %-8s %s\n' "$(basename "$d" | sed 's/^binary-//')" \
    "$(grep -oE '^Version: [0-9][^ ]*' "$d/Packages" | awk '{print $2}' | sort -V | tail -1)"
done

# ── 6b. THE BUNDLED FFmpeg'S CORRESPONDING SOURCE, beside the packages (V12, 2026-10-29) ──────────
# ★★★ LGPL-2.1: shipping FFmpeg's binary (lib/vibeserver/ffmpeg) is allowed because its exact source and the way we built
#     it are offered with it. They go into the repository itself, so whoever can fetch the package can fetch them:
#     https://apt.vibesdr.net/source/ffmpeg/<version>/ — the release tarball and its signature, our build script and
#     build image, the licence, the notice, BUILDINFO (every configure line) and the binaries' checksums. Same files on
#     every publish, so git stores them once. A package with no source beside it is not published.
FFSRC="$SRC_DIR/tools/ffmpeg-linux/bin"
FFVER="$(sed -n 's/^ffmpeg \([0-9.]*\) .*/\1/p' "$FFSRC/BUILDINFO" 2>/dev/null)"
[ -n "$FFVER" ] && [ -f "$FFSRC/source/ffmpeg-$FFVER.tar.xz" ] && [ -f "$FFSRC/source/build-ffmpeg-linux.sh" ] \
  || { echo "!! the bundled FFmpeg's source is not in $FFSRC/source — refusing to publish packages without it"; exit 1; }
mkdir -p "$APT_DIR/source/ffmpeg/$FFVER"
cp "$FFSRC/source/"* "$FFSRC/COPYING.LGPLv2.1" "$FFSRC/NOTICE.ffmpeg" "$FFSRC/BUILDINFO" "$FFSRC/manifest.sha256" \
   "$APT_DIR/source/ffmpeg/$FFVER/"
echo "==> FFmpeg $FFVER source + build script beside the packages: source/ffmpeg/$FFVER/"

# ── 7. commit (always), push (never on --dry-run) ──────────────────────────────────────────────
VERS=""; for A in $ARCHES; do VERS="$VERS $(getver "$A")"; done
VERS="$(echo "$VERS" | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ' ' | sed 's/ $//')"
git -C "$APT_DIR" add -A
git -C "$APT_DIR" -c user.name="VibeSDR release" -c user.email="packages@vibesdr.net" \
    commit -q -m "vibeserver $VERS ($ARCHES)$([ "$SUITE" = beta ] && echo ' — BETA suite')${TEST_REV:+ — TEST, never pushed}" || { echo "!! nothing to commit"; exit 1; }
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
