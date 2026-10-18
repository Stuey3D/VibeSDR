#!/bin/bash
# Part of tools/atom-bench (docs/ATOM-PERFORMANCE-2026-10-18.md). Runs on the Lenovo from ~/atomwork; see images.sh.
# tests.sh BUILD PLATFORM — vibedsp suites (cmake), scripts/run-tests.sh, and the vibeserver ctest targets,
# all in the bookworm build image, nice 19, 4 CPUs. i386 compiles the hand-built tests with the package's
# SSE2 flags (a g++ wrapper) so they exercise the same kernels the i386 package ships.
set -uo pipefail
n=$1 p=$2; img=vibeserver-build:bookworm-${p/386/i386}
W=$HOME/atomwork; O=$W/tests/$n; mkdir -p $O
docker run --rm --platform linux/$p --cpus 4 -v $HOME/atom-src:/src:ro -v $W/b/$n:/b -v $O:/out -v $W/wrap:/wrap:ro -v $W/tools:/tools:ro $img /bin/bash -c '
  set -uo pipefail
  if [ '"$p"' = 386 ]; then mkdir -p /wrap; printf "#!/bin/sh\nexec /usr/bin/g++ -march=i686 -msse2 -mfpmath=sse \"\$@\"\n" > /wrap/g++;
     printf "#!/bin/sh\nexec /usr/bin/gcc -march=i686 -msse2 -mfpmath=sse \"\$@\"\n" > /wrap/cc; chmod +x /wrap/*; export PATH=/wrap:$PATH; fi
  cp -a /src /v; cd /v
  # 1. vibedsp suites
  T=/v/android/app/src/main/cpp/vibedsp/test
  nice -n 19 cmake -S $T -B /tmp/vt -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=$(command -v g++) -DCMAKE_C_COMPILER=$(command -v cc) > /out/vdsp-cmake.log 2>&1
  nice -n 19 cmake --build /tmp/vt -j4 > /out/vdsp-build.log 2>&1 || echo "VDSP BUILD FAILED"
  python3 - <<PY > /dev/null 2>&1 || true
PY
  cd /tmp/vt
  # spectrum vectors need node; skip that one if absent
  for t in ./*_tests; do
    if [ $t = ./vibedsp_spectrum_processor_tests ] && [ ! -f spectrum_in.bin ]; then echo "$t NOT RUN (needs node vectors)"; continue; fi
    nice -n 19 timeout 900 $t > /out/$(basename $t).log 2>&1; echo "$t exit=$? $(tail -1 /out/$(basename $t).log | cut -c1-80)"; done
  cd /v
  # 2. hand-built suites
  TMPDIR=/out nice -n 19 bash scripts/run-tests.sh > /out/run-tests.log 2>&1; echo "run-tests exit=$?"
  grep -E "passed|failed|did not build|SUITE FAILED" /out/run-tests.log | tail -8
  # 3. ctest targets of the server build
  nice -n 19 cmake --build /b -j4 > /out/all-build.log 2>&1 || { echo "ALL BUILD FAILED"; tail -20 /out/all-build.log; }
  cd /b/vs && nice -n 19 ctest --output-on-failure > /out/ctest.log 2>&1; echo "ctest exit=$?"; tail -4 /out/ctest.log
' 2>&1 | tee $O/summary.txt
