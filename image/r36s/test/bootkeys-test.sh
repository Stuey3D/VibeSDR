#!/bin/sh
# bootkeys-test.sh — INSIDE the image root on the R36S: vibe-bootkeys' evdev path without a person. A uinput pad
# named r36s_Gamepad holds Start + Select; vibe-bootkeys (pointed at it) must write the flag. Then the same with no
# keys held (the REAL pad, nobody touching it): no flag.
R=/run/vibe-bootkeys-test
rm -rf $R; mkdir -p $R
python3 /opt/test/fake-keys.py pad-hold 6 &
P=$!
sleep 7.5
DEV=/dev/input/$(for e in /sys/class/input/event*; do [ "$(cat $e/device/name)" = r36s_Gamepad ] && basename $e; done | sort -V | tail -1)
echo "fake pad: $DEV (Start + Select held)"
VIBE_HANDHELD_RUN=$R VIBE_BOOTKEYS_DEV=$DEV python3 -I /usr/lib/vibesdr-r36s/vibe-bootkeys
[ -f $R/reset-radio ] && echo "PASS held: flag written: $(cat $R/reset-radio)" || echo "FAIL held: no flag"
wait $P
rm -f $R/reset-radio
REAL=/dev/input/$(for e in /sys/class/input/event*; do [ "$(cat $e/device/name)" = r36s_Gamepad ] && basename $e; done | sort -V | head -1)
echo "real pad: $REAL (nobody pressing)"
VIBE_HANDHELD_RUN=$R VIBE_BOOTKEYS_DEV=$REAL python3 -I /usr/lib/vibesdr-r36s/vibe-bootkeys
[ -f $R/reset-radio ] && echo "FAIL idle: flag written" || echo "PASS idle: no flag"
