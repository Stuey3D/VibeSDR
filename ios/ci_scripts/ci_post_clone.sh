#!/bin/sh
# Xcode Cloud post-clone step for VibeSDR (Expo / React Native bare workflow).
#
# Installs Node + JS deps + CocoaPods after Xcode Cloud clones the repo, so the
# archive can bundle the JS and link the pods.
#
# WHY XCODE CLOUD: our local build machine is on a BETA macOS (27.x); App Store
# Connect rejects binaries built on a beta OS (ITMS-90111), even with stable
# Xcode 26.6. Xcode Cloud builds on Apple's STABLE macOS images. Keep using this
# until macOS 27 ships stable.
set -e

# Run natively on arm64. Xcode Cloud runners are Apple Silicon, but the script
# can land in an x86_64/Rosetta shell (Intel Homebrew at /usr/local), which makes
# CocoaPods refuse ("Do not use pod install from inside Rosetta2"). Re-exec once
# under arm64 so brew/node/pods are all native and consistent.
if [ "$(uname -m)" != "arm64" ] && [ -x /usr/bin/arch ]; then
  echo "--- re-exec under arm64 (was $(uname -m)) ---"
  exec /usr/bin/arch -arm64 /bin/sh "$0" "$@"
fi

# Prefer arm64 Homebrew.
if [ -x /opt/homebrew/bin/brew ]; then
  eval "$(/opt/homebrew/bin/brew shellenv)"
fi

echo "--- ci_post_clone: ensuring Node is available ($(uname -m)) ---"
if ! command -v node >/dev/null 2>&1; then
  echo "Node not found; installing via Homebrew…"
  brew install node || brew link --overwrite node || true
else
  echo "Node already present."
fi
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
# ★★ HOMEBREW IS NOT GUARANTEED TO DELIVER NODE. Run 299 (2026-09-17, the workflow pinned to Xcode
#    26.6) landed on a runner where `brew install node` ended in "No such keg: /usr/local/Cellar/node"
#    and the archive died at `node: command not found`. The official tarball from nodejs.org needs
#    nothing from the image: fetched for this runner's architecture into $HOME and put on PATH.
# ★★ PINNED AND HASH-CHECKED (audit 2026-10-03). It was `latest-v22.x` piped straight into tar, so
#    whatever nodejs.org served that minute ran our build — unchecked. Now ONE version, downloaded to
#    a file, its SHA-256 compared with the line for that exact filename in the release's
#    SHASUMS256.txt, and only then unpacked. Any failure stops the build: fail closed.
#    ★ Bump NODE_FALLBACK_VER deliberately (react-native wants ^22.13.0 on the 22 line).
if ! command -v node >/dev/null 2>&1; then
  NODE_FALLBACK_VER=v22.20.0
  ARCH=$(uname -m); case "$ARCH" in arm64) NARCH=arm64 ;; *) NARCH=x64 ;; esac
  NODE_BASE="https://nodejs.org/dist/$NODE_FALLBACK_VER"
  TAR="node-$NODE_FALLBACK_VER-darwin-$NARCH.tar.gz"
  NODE_DL=$(mktemp -d)
  echo "Homebrew gave no Node; fetching $NODE_BASE/$TAR"
  curl -fsSL --retry 3 "$NODE_BASE/SHASUMS256.txt" -o "$NODE_DL/SHASUMS256.txt"
  WANT=$(awk -v f="$TAR" '$2 == f {print $1; exit}' "$NODE_DL/SHASUMS256.txt")
  if [ -z "$WANT" ]; then
    echo "!!! $TAR is not listed in $NODE_BASE/SHASUMS256.txt — refusing"
    exit 1
  fi
  curl -fsSL --retry 3 "$NODE_BASE/$TAR" -o "$NODE_DL/$TAR"
  GOT=$(shasum -a 256 "$NODE_DL/$TAR" | awk '{print $1}')
  if [ "$GOT" != "$WANT" ]; then
    echo "!!! SHA-256 MISMATCH for $TAR: got $GOT, SHASUMS256.txt says $WANT — refusing"
    exit 1
  fi
  echo "$TAR SHA-256 verified ($GOT)"
  mkdir -p "$HOME/node"
  tar -xzf "$NODE_DL/$TAR" -C "$HOME/node" --strip-components=1
  rm -rf "$NODE_DL"
  export PATH="$HOME/node/bin:$PATH"
fi

echo "--- node/npm versions ---"
node --version
npm --version

# JS dependencies.
#
# THE FALLBACK MUST START FROM A CLEAN TREE. It was `npm ci || npm install`, and that is a
# trap: npm ci runs `postinstall` (patch-package) as its LAST step, so a ci that dies late
# leaves node_modules ALREADY PATCHED. The `npm install` fallback then runs patch-package a
# second time over a patched tree — and our expo-modules-jsi patch CREATES a file, so
# re-applying it fails ("Failed to apply patch"), which fails the postinstall, which fails
# the whole script under `set -e`. The fallback could never have succeeded.
#
# So the retry deletes node_modules first. It is also worth SAYING that ci failed and why,
# because the fallback used to hide it — the build only ever reported the second, confusing
# error, never the first, real one.
# ★ REVERTED 2026-08-04: --legacy-peer-deps was added for the react-native-tvos alias, which has
#   now been reverted (it broke the shipping iPhone app). Plain npm ci is the known-good path.
cd "$CI_PRIMARY_REPOSITORY_PATH"
echo "--- installing JS dependencies (npm ci) ---"
if ! npm ci; then
  echo "--- npm ci FAILED (see above). Retrying from a clean node_modules… ---"
  rm -rf node_modules
  npm install
fi

# CocoaPods. RN 0.86 fetches a prebuilt "reactnative-dependencies" tarball from
# Maven Central during pod install, which can transiently reset the connection.
# Retry a few times so a flaky download doesn't fail the whole build.
cd "$CI_PRIMARY_REPOSITORY_PATH/ios"
echo "--- installing CocoaPods (with retries) ---"
n=0
until [ "$n" -ge 4 ]; do
  if pod install; then
    echo "--- pod install succeeded ---"
    break
  fi
  n=$((n + 1))
  echo "pod install failed (attempt $n/4) — retrying in 15s…"
  sleep 15
done
if [ "$n" -ge 4 ]; then
  echo "pod install failed after 4 attempts"
  exit 1
fi

# ★★★ REGENERATE THE EMBEDDED BASEMAP, so a committed artefact can never go stale against its
#     source. src/generated/mapdataBundle.ts is tier0+tier1 of assets/mapdata/v1 turned into a TS
#     module, and it is COMMITTED on purpose: Xcode Cloud builds a fresh clone, so an ignored
#     generated file would fail the build outright rather than degrade.
# ★★ Committed AND regenerated is the point — the commit guarantees the build works, this line
#    guarantees the bytes match assets/mapdata/v1. Without it the two drift silently and the app
#    ships last month's coastlines while the repo shows this month's.
# ★ The map is why this matters: the app is the basemap source for every engine it drives
#   (UberSDR, KiwiSDR, a local dongle), so a stale bundle is a wrong map everywhere at once.
# ✗✗✗ AND IT MUST NOT BE FATAL. This ran with `|| exit 1`, and builds 332 and 333 failed with
#     ONE error and ZERO warnings apiece — against 653 warnings on the build before it, which is
#     what a run that actually reaches the compiler looks like. Zero warnings means it died in a
#     pre-build step, and this was the only pre-build step that changed.
# ★★ The bundle is COMMITTED and current, so regenerating it here is a safety net against drift,
#    not a requirement to build. Turning a safety net into a hard failure is how a nicety takes the
#    whole release down — and it cost two cloud builds and blocked Stuart from testing the maps at
#    all, which was the entire point of the builds.
# ★ Still loud on failure: a silent fallback to a stale bundle is the other way to get this wrong.
# ✗✗✗ AND THE ACTUAL BUG WAS THE WORKING DIRECTORY. `pod install` above cds into ios/, so this
#     ran `node scripts/gen-mapdata-source.mjs` from INSIDE ios/ — where that path does not exist.
#     It failed with "cannot find module", the `|| exit 1` fired, and the build died before
#     compiling a single file. I appended a relative path to a script that had changed directory
#     three steps earlier and never looked at what the cwd was by then.
echo "--- regenerating the embedded basemap ---"
cd "$CI_PRIMARY_REPOSITORY_PATH"
if node scripts/gen-mapdata-source.mjs; then
  echo "--- basemap regenerated ---"
else
  echo "!!! map bundle generation failed — building with the COMMITTED bundle instead."
  echo "!!! that is safe (it is in git and current), but find out why before it drifts."
fi

echo "--- ci_post_clone: done ---"
