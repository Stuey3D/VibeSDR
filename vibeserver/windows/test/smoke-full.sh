#!/bin/bash
# ★ Wine smoke test of the FRONT DOOR (Full mode): a config with one RTL-SDR, so vibeserver.exe on :48000 must
#   start that radio as its OWN PROCESS (CreateProcess + the kill-on-close Job Object) and route to it over the
#   named-pipe hand-off. Wine has no USB, so the radio itself cannot open a dongle — what is tested is the
#   process model: the door answers, the radio process exists, and when the door is stopped the radio goes too.
cd /b
export WINEDEBUG=-all
mkdir -p "/root/.wine/drive_c/ProgramData/VibeServer"
cat > "/root/.wine/drive_c/ProgramData/VibeServer/config.json" <<'JSON'
{ "configured": true, "fullMode": true, "sharing": "public", "name": "Wine test", "adminPass": "wine-test-only",
  "radios": [ { "serial": "00000001", "driver": "rtlsdr", "label": "Test RTL", "enabled": true, "configured": true } ] }
JSON
( timeout 150 wine ./vibeserver.exe --serve > /tmp/door.log 2>&1 ) &
for i in $(seq 1 90); do curl -s -o /dev/null http://127.0.0.1:48000/ && break; sleep 1; done
sleep 15
echo "== GET / on the front door:"; curl -s -o /tmp/door.html -w 'HTTP %{http_code}, %{size_download} bytes\n' http://127.0.0.1:48000/
echo "== processes (vibeserver.exe):"; ps -eo pid,args | grep -c '[v]ibeserver.exe' | sed 's/^/   count: /'
echo "== /radio/1/ through the door:"; curl -s -o /dev/null -w 'HTTP %{http_code}\n' --max-time 10 http://127.0.0.1:48000/radio/1/ || true
echo "== door log:"; grep -E 'USB|front door|started radio|radio |hand|48000|pipe|could not' /tmp/door.log | head -25
pkill -INT -f 'vibeserver.exe --serve' ; sleep 1; wineserver -k 2>/dev/null; sleep 2
echo "== after stopping: $(ps -eo args | grep -c '[v]ibeserver.exe') vibeserver.exe left"
