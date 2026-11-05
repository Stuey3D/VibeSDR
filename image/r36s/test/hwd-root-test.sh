#!/bin/sh
# hwd-root-test.sh — INSIDE the image root on the R36S: vibe-hwd with Debian's python 3.13 / alsa-utils / chpasswd,
# the socket's owner, the SSH password path (hash kept, never logged), set time, and vibe-clock save/restore + hwclock.
S=/run/hwd-root-test; rm -rf $S; mkdir -p $S/state
VIBE_HWD_DRYRUN=1 VIBE_STATE_DIR=$S/state VIBE_HWD_SOCK=$S/hwd.sock python3 -I /usr/lib/vibesdr-r36s/vibe-hwd > $S/hwd.log 2>&1 &
H=$!
python3 /opt/test/hwd-client.py $S/hwd.sock 2>&1 | grep -v "event:" | cut -c1-150
echo "--- socket"; ls -l $S/hwd.sock
echo "--- ssh password"
python3 /opt/test/hwd-client.py $S/hwd.sock raw '{"set":"ssh-password","password":"correct horse battery"}'
python3 /opt/test/hwd-client.py $S/hwd.sock raw '{"get":"ssh"}'
echo "hash kept: $(cut -c1-12 $S/state/ssh/root.hash)… ($(stat -c %a $S/state/ssh/root.hash))"
grep -c "correct horse" $S/hwd.log $S/state/ssh/root.hash /etc/shadow 2>/dev/null | sed 's/^/password text found in /'
echo "--- set time (to now)"
NOW=$(($(date +%s) * 1000))
python3 /opt/test/hwd-client.py $S/hwd.sock raw "{\"set\":\"time\",\"utcMs\":$NOW}"
echo "--- vibe-clock"
VIBE_STATE_DIR=$S/state /usr/lib/vibesdr-r36s/vibe-clock save; echo "saved: $(cat $S/state/clock)"
hwclock --show --utc 2>&1 | head -1
echo "rtc since_epoch $(cat /sys/class/rtc/rtc0/since_epoch), system $(date +%s)"
VIBE_STATE_DIR=$S/state /usr/lib/vibesdr-r36s/vibe-clock restore; echo "restore: ok (no jump expected)"
echo "$(( $(date +%s) + 3600 )) $(( $(date +%s) + 3600 ))" > $S/state/clock.fake
echo "--- hwd log"; cat $S/hwd.log
kill $H
