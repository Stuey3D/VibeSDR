#!/bin/bash
# xbuild.sh SRC OUT [extra flags] — armhf (Pi 2) static ft8_wav, the server's Release flags + the Pi's -marm/NEON.
set -e
S=/w/$1; O=/w/$2; shift 2; X="$*"
CPP=$S/android/app/src/main/cpp
mkdir -p $O/obj
F="-O3 -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard $X"
for f in $CPP/ft8_lib/ft8/*.c $CPP/ft8_lib/fft/kiss_fft.c $CPP/ft8_lib/fft/kiss_fftr.c $CPP/ft8_lib/common/monitor.c $CPP/vibedsp/third_party/pffft/pffft.c; do
  [ -f "$f" ] || continue
  arm-linux-gnueabihf-gcc $F -c -I $CPP/ft8_lib -o $O/obj/$(basename $f .c).o $f
done
arm-linux-gnueabihf-g++ -std=c++17 $F -static -I $CPP -I $CPP/ft8_lib -o $O/ft8_wav $S/tools/ft8-bench/ft8_wav.cpp $CPP/decoders/ft8_decoder.cpp $(ls $CPP/decoders/ftx_fine.cpp 2>/dev/null) $O/obj/*.o -lpthread
if [ -f $S/vibeserver/test-ft8-fast.cpp ]; then
  arm-linux-gnueabihf-g++ -std=c++17 $F -static -I $S/vibeserver -I $CPP -I $CPP/ft8_lib -o $O/test-ft8-fast $S/vibeserver/test-ft8-fast.cpp $CPP/decoders/ft8_decoder.cpp $(ls $CPP/decoders/ftx_fine.cpp 2>/dev/null) $O/obj/*.o -lpthread
fi
ls -la $O/ft8_wav
