#!/bin/bash
# The three Atom test packages: PGO-trained, built through scripts/build-x86-debs.sh's own layout.
# Run from the Mac (it drives scripts/build-x86-debs.sh); the training half runs pgo-train.sh on the Lenovo.
set -uo pipefail
cd "$(dirname "$0")/../.."
export VIBE_X86_SRC_DIR=atom-debsrc
T32="-march=i686 -msse2 -mfpmath=sse -mtune=bonnell"
gen_train() { # arch pgodir label  (+ env for flags)
  VIBE_PGO=generate VIBE_PGO_DIR=atomwork/$2 nice -n 10 scripts/build-x86-debs.sh --rev 1+pgo --arch $1 2>&1 | tail -2
  p=$1; [ $p = i386 ] && p=386
  ssh 192.168.86.13 "cd ~/atomwork && rm -rf pg-$3 && cp -a ~/vibeout/$1/pgogen pg-$3 && ./pgo-train.sh \$HOME/atomwork/pg-$3 \$HOME/atomwork/$2 $p 2>&1 | grep -E 'exit|specFps|^[0-9]'"
}
VIBE_EXTRA_CFLAGS="-mtune=bonnell" gen_train amd64 pgo-deb-amd64-t amd64-t
VIBE_X86_32_FLAGS="$T32" gen_train i386 pgo-deb-i386-t i386-t
VIBE_X86_32_FLAGS="" gen_train i386 pgo-deb-i386-g i386-g
echo "==== USE BUILDS"
VIBE_EXTRA_CFLAGS="-mtune=bonnell" VIBE_PGO=use VIBE_PGO_DIR=atomwork/pgo-deb-amd64-t nice -n 10 scripts/build-x86-debs.sh --rev 1+atom2 --arch amd64 2>&1 | tail -4
VIBE_X86_32_FLAGS="$T32" VIBE_PGO=use VIBE_PGO_DIR=atomwork/pgo-deb-i386-t nice -n 10 scripts/build-x86-debs.sh --rev 1+atom2 --arch i386 2>&1 | tail -4
VIBE_PGO=use VIBE_PGO_DIR=atomwork/pgo-deb-i386-g nice -n 10 scripts/build-x86-debs.sh --rev 1+atom2g --arch i386 2>&1 | tail -4
echo DEBS-DONE
