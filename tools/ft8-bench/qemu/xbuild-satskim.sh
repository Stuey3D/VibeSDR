#!/bin/bash
# xbuild-satskim.sh SRC OUT — ★★★ beta97 the satellite skimmer's bench (tools/ft8-bench/sat_skim_bench.cpp) for armhf (the Pi 2's
# flags: -O3 -marm -mfpu=neon-vfpv4), static — run inside vibeserver-cross:bookworm-armhf, e.g.
#   docker run --rm -v "$PWD:/w/src" -v "$TMPDIR:/w/out" vibeserver-cross:bookworm-armhf bash /w/src/tools/ft8-bench/qemu/xbuild-satskim.sh src out
# then under qemu-arm (tools/ft8-bench/qemu/qrun.sh counts its instructions) or on the board itself.
set -e
S=/w/$1; O=/w/$2
CPP=$S/android/app/src/main/cpp
mkdir -p $O/sobj
F="-O3 -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard"
for f in $CPP/ft8_lib/ft8/*.c $CPP/ft8_lib/fft/kiss_fft.c $CPP/ft8_lib/fft/kiss_fftr.c $CPP/ft8_lib/common/monitor.c $CPP/vibedsp/third_party/pffft/pffft.c; do
  arm-linux-gnueabihf-gcc $F -c -I $CPP/ft8_lib -o $O/sobj/$(basename $f .c).o $f
done
arm-linux-gnueabihf-g++ -std=c++17 $F -static -I $CPP -I $CPP/ft8_lib -o $O/sat_skim_bench_armhf $S/tools/ft8-bench/sat_skim_bench.cpp \
  $CPP/decoders/sat_skim.cpp $CPP/decoders/digi_tuner.cpp $CPP/decoders/ft8_decoder.cpp $CPP/decoders/ftx_fine.cpp $O/sobj/*.o -lpthread
ls -la $O/sat_skim_bench_armhf
