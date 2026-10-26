#!/bin/bash
# Wine smoke test of vibeserver.exe: start it on an rtl_tcp stand-in, fetch the web client, check a WebSocket upgrade.
cd /b
apt-get install -y python3 >/dev/null 2>&1 || true
python3 /t/fake_rtl_tcp.py 1234 & 
sleep 1
export ProgramData='C:\ProgramData'
( timeout 120 wine ./vibeserver.exe --tcp 127.0.0.1:1234 --port 48000 --mode wfm --freq 100000000 > /tmp/vs.log 2>&1 ) &
for i in $(seq 1 60); do curl -s -o /dev/null http://127.0.0.1:48000/ && break; sleep 1; done
echo "== GET / :"; curl -s -o /tmp/index.html -w 'HTTP %{http_code}, %{size_download} bytes, %{content_type}\n' http://127.0.0.1:48000/
grep -o '<title>[^<]*' /tmp/index.html | head -1
echo "== WebSocket upgrade (/ws/audio):"
curl -s -i -N --max-time 4 -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' "http://127.0.0.1:48000/ws/audio?codec=opus" 2>/dev/null | head -c 600 | strings | head -8
sleep 8
echo "== server log (head/tail):"; head -40 /tmp/vs.log; echo ...; tail -15 /tmp/vs.log
