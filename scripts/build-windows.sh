#!/usr/bin/env bash
# Build VibeServer for Windows on this Mac — reproducibly, in containers — and package the Milestone 1 test zip.
#
#   scripts/build-windows.sh            deps (pinned, cached) + vibeserver.exe + vibe-usbprobe.exe + every test
#   scripts/build-windows.sh --test     … then run the unit tests and a server smoke test under Wine
#   scripts/build-windows.sh --zip      … then make vibeserver/windows/out/vibeserver-windows-x64-test1.zip
#   scripts/build-windows.sh --clean    remove the build tree first (the deps cache is kept)
#   ARCH=aarch64 scripts/build-windows.sh   Windows on ARM (same image, NEON path) — builds; untested
#
# ★★★ THE TOOLCHAIN IS PINNED BY DIGEST: mstorsjo/llvm-mingw 20261006 (clang 23.1.3, MinGW-w64, UCRT). A new
#     compiler is a new test rig; a moving tag would make two builds of one commit different programs.
# ★★ Everything static (docs/WINDOWS-PORT-PLAN.md §4, Stuart's rule): the .exe files import only Windows' own
#    DLLs — KERNEL32, WS2_32, IPHLPAPI, SETUPAPI, the UCRT that ships with Windows 10+. The script CHECKS that.
# ★★ Gentle on the Mac (other work runs beside it): nice 10, 3 CPUs to the container, ninja -j3, one build.
# ★ Outputs live under vibeserver/windows/{deps,out} (gitignored). Nothing is signed, uploaded or pushed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WIN="$ROOT/vibeserver/windows"
IMAGE="mstorsjo/llvm-mingw@sha256:3f535cae24312a5bb25968ef670ea070e422da01f3f642bac550bca781bac29e"
WINE_IMAGE="vibe-wine-test:m1"
ARCH="${ARCH:-x86_64}"
TRIPLE="$ARCH-w64-mingw32"
OUT="$WIN/out/build-$TRIPLE"
ZIPNAME="vibeserver-windows-${ARCH/x86_64/x64}-test1"
DO_TEST=0; DO_ZIP=0; CLEAN=0
for a in "$@"; do
  case "$a" in --test) DO_TEST=1;; --zip) DO_ZIP=1;; --clean) CLEAN=1;;
               *) echo "unknown option $a"; exit 2;; esac
done
[ $CLEAN = 1 ] && rm -rf "$OUT"
run() {  # run <command…> in the toolchain container, repo at /src
  nice -n 10 docker run --rm --cpus 3 -e JOBS=3 -v "$ROOT":/src -w /src "$IMAGE" bash -c "$*"
}
rel() { echo "/src/${1#"$ROOT"/}"; }

echo "==> deps ($TRIPLE)"
run "bash vibeserver/windows/build-deps.sh $TRIPLE /src/vibeserver/windows/deps"

echo "==> configure + build ($TRIPLE)"
run "set -e
     [ -f $(rel "$OUT")/build.ninja ] || cmake -S vibeserver -B $(rel "$OUT") -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/src/vibeserver/windows/windows-toolchain.cmake -DVIBE_WIN_TRIPLE=$TRIPLE \
        -DVIBE_WIN_DEPS=/src/vibeserver/windows/deps/$TRIPLE/prefix > /tmp/cfg.log 2>&1 || { tail -40 /tmp/cfg.log; exit 1; }
     ninja -C $(rel "$OUT") -j3 all"

echo "==> what the .exe files need from Windows"
# ★★ The rule, checked: any DLL outside Windows itself is a dependency a user would have to install.
run "for e in vibeserver.exe vibe-usbprobe.exe; do
       dlls=\$(llvm-readobj --coff-imports $(rel "$OUT")/\$e | sed -n 's/^ *Name: //p' | sort -u)
       echo \"   \$e: \$(echo \$dlls | tr '\n' ' ')\"
       bad=\$(echo \"\$dlls\" | grep -viE '^(kernel32|ws2_32|iphlpapi|setupapi|cfgmgr32|advapi32|shell32|ole32|bcrypt|psapi|version|newdev|user32|api-ms-win-crt-[a-z0-9-]+)\.dll\$' || true)
       [ -z \"\$bad\" ] || { echo \"!! \$e imports a DLL that is not part of Windows: \$bad\"; exit 1; }
     done"

if [ $DO_TEST = 1 ]; then
  [ "$ARCH" = x86_64 ] || { echo "!! --test runs x64 builds only (Wine on amd64)"; exit 1; }
  if ! docker image inspect "$WINE_IMAGE" >/dev/null 2>&1; then
    echo "==> building the Wine test image ($WINE_IMAGE, amd64 via qemu)"
    nice -n 10 docker build --platform linux/amd64 -t "$WINE_IMAGE" -f "$WIN/test/Dockerfile.wine" "$WIN/test" | tail -2
  fi
  echo "==> unit tests under Wine"
  wrun() { nice -n 10 docker run --rm --platform linux/amd64 --cpus 3 -v "$OUT":/b -v "$WIN/test":/t "$WINE_IMAGE" bash "$@"; }
  wrun /t/run-tests-wine.sh || TESTS_FAILED=1
  echo "==> server smoke test under Wine"
  wrun /t/smoke.sh | grep -E '^(==|HTTP|<title>|Upgrade|Sec-WebSocket|\[VibeLocalSDR\] (RTL-TCP started|audio WS|capture clean|clock)|VibeServer: (USB|failed))'
  [ "${TESTS_FAILED:-0}" = 0 ] || { echo "!! a unit test failed under Wine (see above)"; exit 1; }
fi

if [ $DO_ZIP = 1 ]; then
  echo "==> packaging $ZIPNAME.zip"
  STAGE="$WIN/out/$ZIPNAME"
  rm -rf "$STAGE" "$WIN/out/$ZIPNAME.zip"; mkdir -p "$STAGE/licences"
  cp "$OUT/vibeserver.exe" "$OUT/vibe-usbprobe.exe" "$STAGE/"
  cp "$ROOT/docs/WINDOWS-M1-TEST.md" "$STAGE/WINDOWS-M1-TEST.md"
  cp "$WIN/README-test.txt" "$STAGE/README.txt"
  cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
  P="$WIN/deps/$TRIPLE/prefix"; C="$ROOT/android/app/src/main/cpp"
  cp "$P/COPYING.libusb"    "$STAGE/licences/libusb (LGPL-2.1).txt"
  cp "$P/COPYING.librtlsdr" "$STAGE/licences/librtlsdr (GPL-2.0).txt"
  cp "$P/COPYING.opus"      "$STAGE/licences/libopus (BSD-3).txt"
  cp "$C/libairspyhf/LICENSE" "$STAGE/licences/libairspyhf (BSD-3).txt"
  cp "$C/libairspy/LICENSE"   "$STAGE/licences/libairspy (BSD-3, MIT).txt"
  cp "$C/libhackrf/COPYING"   "$STAGE/licences/libhackrf (GPL-2.0).txt"
  cp "$C/ft8_lib/LICENSE"     "$STAGE/licences/ft8_lib (MIT).txt"
  # ★ Windows' own Notepad copes with LF, but CRLF is what every Windows reader expects.
  for f in "$STAGE"/*.txt "$STAGE"/*.md "$STAGE"/licences/*.txt; do perl -pi -e 's/\r?\n/\r\n/' "$f"; done
  # ★ Reproducible-ish: fixed timestamps, sorted entries, no extra attributes.
  find "$STAGE" -exec touch -t 202610260000 {} +
  ( cd "$WIN/out" && find "$ZIPNAME" | sort | zip -X -q -9 "$ZIPNAME.zip" -@ )
  rm -rf "$STAGE"
  ls -l "$WIN/out/$ZIPNAME.zip" | awk '{print "   "$5" bytes  "$9}'
  echo "   sha256 $(shasum -a 256 "$WIN/out/$ZIPNAME.zip" | awk '{print $1}')"
fi
echo "==> done: $OUT"
