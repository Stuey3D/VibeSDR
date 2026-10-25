#!/bin/bash
# qrun.sh BIN TAG ARGS... — run under qemu-arm with the instruction counter (TAG.insn) + symbols (TAG.syms)
B=$1; T=$2; shift 2
cd /w
s=$(date +%s%N)
INSN_OUT=/w/q/$T.insn qemu-arm -plugin /w/q/insncount.so $B "$@" > /w/q/$T.out 2> /w/q/$T.err
e=$(date +%s%N)
arm-linux-gnueabihf-nm -n --defined-only -C $B | awk '$2 ~ /[tTwW]/' > /w/q/$T.syms
echo "$T wall $(( (e - s) / 1000000 )) ms; $(grep TOTAL /w/q/$T.insn)"
