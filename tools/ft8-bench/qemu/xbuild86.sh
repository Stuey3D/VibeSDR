#!/bin/bash
# xbuild86.sh SRC OUT ARCH(amd64|i386) [extra] — static x86 ft8_wav + test-ft8-fast, the server's x86 flags
set -e
S=/w/$1; O=/w/$2; A=$3; shift 3; X="$*"
CPP=$S/android/app/src/main/cpp
if [ $A = amd64 ]; then CC=x86_64-linux-gnu-gcc; CXX=x86_64-linux-gnu-g++; F="-O3"; ST=""; else CC=i686-linux-gnu-gcc; CXX=i686-linux-gnu-g++; F="-O3 -march=i686 -msse2 -mfpmath=sse -mtune=generic"; ST="-static"; fi
F="$F $X"
mkdir -p $O/obj
for f in $CPP/ft8_lib/ft8/*.c $CPP/ft8_lib/fft/kiss_fft.c $CPP/ft8_lib/fft/kiss_fftr.c $CPP/ft8_lib/common/monitor.c $CPP/vibedsp/third_party/pffft/pffft.c; do
  [ -f "$f" ] || continue
  $CC $F -c -I $CPP/ft8_lib -o $O/obj/$(basename $f .c).o $f
done
$CXX -std=c++17 $F $ST -I $CPP -I $CPP/ft8_lib -o $O/ft8_wav $S/tools/ft8-bench/ft8_wav.cpp $CPP/decoders/ft8_decoder.cpp $(ls $CPP/decoders/ftx_fine.cpp 2>/dev/null) $O/obj/*.o -lpthread
[ -f $S/vibeserver/test-ft8-fast.cpp ] && $CXX -std=c++17 $F $ST -I $S/vibeserver -I $CPP -I $CPP/ft8_lib -o $O/test-ft8-fast $S/vibeserver/test-ft8-fast.cpp $CPP/decoders/ft8_decoder.cpp $(ls $CPP/decoders/ftx_fine.cpp 2>/dev/null) $O/obj/*.o -lpthread
ls -la $O/ft8_wav
