#!/bin/bash
# ★ Wine test of the TV-stick SWITCH and START-WITH-WINDOWS in vibeserver.exe itself, with a FAKED device list
#   (VIBE_WIN_FAKE_USB — the un-elevated server's test seam; Wine has no USB). What it proves: a fresh `--serve` is a
#   new server with the setup page (not "no SDR found"); ONLY the TV stick and the no-driver stick are offered (console
#   and /vibeserver/windows/state); the routes are admin-only; SWITCH runs the elevated helper through ShellExecuteEx
#   "runas" and the helper's report comes back over the pipe — and the helper REFUSES, because it scans Windows' real
#   device list itself (there is no such device under Wine) and never trusts the server's; NOT NOW; an arrival is
#   noticed; the helper refuses a bad command line; start-at-sign-in on and off (idempotent); start-at-boot via the
#   helper, as far as Wine's schtasks goes.
# ★ Silent by construction: no audio anywhere.
cd /b
export WINEDEBUG=-all
PD=/root/.wine/drive_c/ProgramData/VibeServer
rm -rf "$PD"; mkdir -p "$PD"
PASS=wine-test-only
FAKE=/tmp/fake-usb.txt
cat > $FAKE <<'EOF'
# instanceId|service|class|problem|infPath|ours|parentId
USB\VID_0BDA&PID_2838&MI_00\7&2C1A0B3&0&0000|RTL2832UBDA|MEDIA|0|oem12.inf|0|USB\VID_0BDA&PID_2838\00000001
USB\VID_0BDA&PID_2838&MI_01\7&2C1A0B3&0&0001|HidUsb|HIDClass|0|input.inf|0|USB\VID_0BDA&PID_2838\00000001
USB\VID_0BDA&PID_2838&MI_00\7&AA&0&0000||USB|28||0|USB\VID_0BDA&PID_2838\00000002
USB\VID_0BDA&PID_2838&MI_00\7&BB&0&0000|WinUSB|USBDevice|0|oem40.inf|0|USB\VID_0BDA&PID_2838\00000003
USB\VID_0BDA&PID_2838&MI_00\7&CC&0&0000|WinUSB|USBDevice|0|winusb.inf|1|USB\VID_0BDA&PID_2838\00000004
EOF
export VIBE_WIN_FAKE_USB='Z:\tmp\fake-usb.txt'
ok=0; bad=0
check() { if eval "$2"; then ok=$((ok+1)); echo "PASS  $1"; else bad=$((bad+1)); echo "FAIL  $1"; fi; }
auth() {   # the setup page's HMAC handshake: vs_admin_nonce + HMAC-SHA256(password, nonce)
  local n; n=$(curl -s http://127.0.0.1:48000/vibeserver/auth | sed -n 's/.*"nonce":"\([^"]*\)".*/\1/p')
  local t; t=$(printf %s "$n" | openssl dgst -sha256 -hmac "$PASS" -r | cut -d' ' -f1)
  echo "vs_admin_nonce=$n&vs_admin_auth=$t"
}
state() { curl -s "http://127.0.0.1:48000/vibeserver/windows/state?$(auth)"; }
post() { curl -s -o /tmp/post.json -w '%{http_code}' -X POST --data "$2" "http://127.0.0.1:48000/vibeserver/windows/$1?$(auth)"; }
# ★ Ids as JSON-escaped text (one backslash written as two), so a body is just {"id":"$ID"}.
ID_TV='USB\\VID_0BDA&PID_2838&MI_00\\7&2C1A0B3&0&0000'
ID_NODRV='USB\\VID_0BDA&PID_2838&MI_00\\7&AA&0&0000'
ID_ZADIG='USB\\VID_0BDA&PID_2838&MI_00\\7&BB&0&0000'
postid() { post "$1" "{\"id\":\"$2\"}"; }
waitjob() { for i in $(seq 1 90); do state > /tmp/state.json; grep -q '"running":false,"done":true' /tmp/state.json && return 0; sleep 1; done; return 1; }

( timeout 400 wine ./vibeserver.exe --serve --admin-pass "$PASS" > /tmp/sw.log 2>&1 ) &
for i in $(seq 1 120); do curl -s -o /dev/null http://127.0.0.1:48000/ && break; sleep 1; done
sleep 3

echo "== first run"
check "fresh --serve starts as a new server (no 'no SDR found')" "! grep -q 'no SDR found' /tmp/sw.log && grep -q 'starting as a new server' /tmp/sw.log"
check "admin password stored for the boot task" "grep -q 'admin password stored' /tmp/sw.log"
curl -s http://127.0.0.1:48000/ > /tmp/page.html
check "GET / is the setup page with the Windows cards" "grep -q 'id=\"winDongles\"' /tmp/page.html && grep -q 'id=\"winAutoCard\"' /tmp/page.html"
check "UsbDk is not chosen by default" "grep -q 'USB path = WinUSB' /tmp/sw.log"

echo "== the offer (console)"
grep -E 'Windows is using|Windows has no driver|→ open' /tmp/sw.log | sed 's/^/   /'
check "console offers the TV stick (serial 00000001)" "grep -q 'Windows is using this dongle as a TV tuner (.*serial 00000001)' /tmp/sw.log"
check "console offers the no-driver stick (00000002)" "grep -q 'Windows has no driver for this dongle (.*serial 00000002)' /tmp/sw.log"
check "console says nothing about the Zadig'd / our sticks (03, 04)" "! grep -qE 'TV tuner.*0000000[34]|no driver.*0000000[34]' /tmp/sw.log"
check "console never mentions Zadig in an offer" "! grep -E 'Switch it to SDR' /tmp/sw.log | grep -qi zadig"

echo "== the routes"
check "hello is open" "curl -s http://127.0.0.1:48000/vibeserver/windows/hello | grep -q '\"windows\":true'"
check "state without admin = 401" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:48000/vibeserver/windows/state)\" = 401 ]"
state > /tmp/state.json
sed 's/},{/}\n{/g' /tmp/state.json | sed -n 's/.*"serial":"\([^"]*\)","state":"\([^"]*\)","offer":\([a-z]*\),"switchedByUs":\([a-z]*\).*"why":"\([^"]*\)".*/   \2 offer=\3 ours=\4 \1 — \5/p'
sed -n 's/.*"autostart":\({[^}]*}\).*/   autostart: \1/p' /tmp/state.json; echo
offered() { sed 's/},{/}\n{/g' /tmp/state.json | grep '"offer":true' | sed -n 's/.*"serial":"\([^"]*\)".*/\1/p' | sort | tr '\n' ' '; }
ours() { sed 's/},{/}\n{/g' /tmp/state.json | grep '"switchedByUs":true' | sed -n 's/.*"serial":"\([^"]*\)".*/\1/p' | tr '\n' ' '; }
check "state: exactly the TV + no-driver sticks are offered" '[ "$(offered)" = "00000001 00000002 " ]'
check "state: only our own switch has Give back" '[ "$(ours)" = "00000004 " ]'
check "state: the IR interface is not listed" "! grep -q 'MI_01' /tmp/state.json"
check "switch a Zadig'd stick = 409 (already usable)" '[ "$(postid switch "$ID_ZADIG")" = 409 ] && grep -q "already usable" /tmp/post.json'
check "give back a stick we did not switch = 409" '[ "$(postid giveback "$ID_ZADIG")" = 409 ]'
check "NOT NOW hides the no-driver stick" '[ "$(postid notnow "$ID_NODRV")" = 200 ] && state | grep -q "\"notNow\":\[\"USB"'

echo "== SWITCH the TV stick: the helper runs (Wine: no UAC), reports over the pipe, and refuses (no such device in Windows' own list)"
code=$(postid switch "$ID_TV"); echo "   POST switch → $code $(cat /tmp/post.json)"
check "switch accepted (202)" "[ $code = 202 ]"
c2=$(postid switch "$ID_NODRV"); echo "   a second SWITCH while the first waits → $c2 (409 if the first is still running)"
waitjob; sed -n 's/.*"job":\({.*}\),"autostart".*/   job: \1/p' /tmp/state.json; echo
check "the helper's answer came back over the pipe: refused, not plugged in" "grep -q 'refused: that device is not plugged in' /tmp/state.json"
check "nothing was changed (job ok=false)" "grep -q '\"ok\":false' /tmp/state.json"
check "the helper wrote its audit log" "grep -q -- '--winusb-bind' $PD/vibeserver-helper.log"

echo "== an arrival"
echo 'USB\VID_0BDA&PID_2838&MI_00\8&11&0&0000|RTL2832UBDA|MEDIA|0|oem12.inf|0|USB\VID_0BDA&PID_2838\V4L00001' >> $FAKE
sleep 8
check "a TV stick plugged in later is offered on the console" "grep -q 'TV tuner (.*serial V4L00001)' /tmp/sw.log"

echo "== the helper's own refusals (run directly)"
wine ./vibeserver.exe --winusb-bind 'USB\VID_046D&PID_C077\5&1234&0&2' --reply '\\.\pipe\vibeserver-helper-1-ab' > /dev/null 2>&1; rc=$?
check "helper with no server listening refuses (rc=$rc)" "[ $rc -ne 0 ] && tail -3 $PD/vibeserver-helper.log | grep -q 'not listening'"
wine ./vibeserver.exe --winusb-bind 'USB\VID_0BDA&PID_2838&MI_00\7&1&0&0000' --reply '\\.\pipe\vibeserver-helper-1-ab' --serve > /dev/null 2>&1; rc=$?
check "helper with an extra argument refuses (rc=$rc)" "[ $rc -ne 0 ] && tail -3 $PD/vibeserver-helper.log | grep -q 'unexpected argument'"
wine ./vibeserver.exe --winusb-bind 'USB\VID_0BDA&PID_2838&MI_00\7" & calc' --reply '\\.\pipe\vibeserver-helper-1-ab' > /dev/null 2>&1; rc=$?
check "helper with a quote in the id refuses (rc=$rc)" "[ $rc -ne 0 ] && tail -3 $PD/vibeserver-helper.log | grep -q 'not a USB device instance id'"

echo "== start with Windows"
code=$(post autostart '{"mode":"signin"}'); echo "   signin → $code $(cat /tmp/post.json)"
check "start at sign-in: on, no prompt" "[ $code = 200 ] && wine reg query 'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run' /v VibeServer 2>/dev/null | grep -q -- '--autostart signin'"
code=$(post autostart '{"mode":"signin"}')
check "asking again changes nothing (idempotent)" "[ $code = 200 ] && grep -q unchanged /tmp/post.json"
code=$(post autostart '{"mode":"off"}')
check "off removes it" "[ $code = 200 ] && ! wine reg query 'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run' /v VibeServer >/dev/null 2>&1"
code=$(post autostart '{"mode":"off"}')
check "off again: unchanged" "[ $code = 200 ] && grep -q unchanged /tmp/post.json"
code=$(post autostart '{"mode":"bogus"}')
check "an unknown mode = 409" "[ $code = 409 ]"
code=$(post autostart '{"mode":"boot"}'); echo "   boot → $code $(cat /tmp/post.json)"
waitjob; sed -n 's/.*"job":\({.*}\),"autostart".*/   job: \1/p' /tmp/state.json; echo
echo "   (Wine's schtasks: $(wine schtasks /Query /TN VibeServer >/dev/null 2>&1 && echo 'task present' || echo 'no task'))"
check "start at boot went through the helper and reported back" "grep -q '\"op\":\"autostart\"' /tmp/state.json && grep -q '\"done\":true' /tmp/state.json"

pkill -INT -f 'vibeserver.exe --serve'; sleep 1; wineserver -k 2>/dev/null; sleep 2

echo "== the keeper (what start-at-boot / at-sign-in runs): it starts the server, logs to a file, restarts it"
( timeout 200 wine ./vibeserver.exe --autostart signin > /dev/null 2>&1 ) &
for i in $(seq 1 120); do curl -s -o /dev/null http://127.0.0.1:48000/ && break; sleep 1; done
check "the keeper started the server (port 48000 answers)" "curl -s -o /dev/null http://127.0.0.1:48000/vibeserver/windows/hello"
check "the server's output goes to vibeserver.log" "grep -q 'keeper (signin): starting VibeServer' $PD/vibeserver.log && grep -q 'front door\\|VibeServer:' $PD/vibeserver.log"
pkill -KILL -f 'vibeserver.exe --serve'; sleep 25
check "killed, it is started again by the keeper" "grep -q 'keeper: VibeServer stopped' $PD/vibeserver.log && curl -s -o /dev/null http://127.0.0.1:48000/vibeserver/windows/hello"
pkill -f -- '--autostart signin'; sleep 1; wineserver -k 2>/dev/null; sleep 1
echo "Switch smoke: $ok passed, $bad failed"
[ $bad -eq 0 ]
