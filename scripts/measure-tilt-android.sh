#!/usr/bin/env bash
# Tilt lighting — the §5.2 budget, measured on an Android device over adb (BRIEF-lighting-and-vfd-glass §5.2).
#
#   scripts/measure-tilt-android.sh [seconds] [adb-serial]
#     seconds     per run (default 60)
#     adb-serial  default 192.168.86.111:36408 (Stuart's XCover 4S — the slowest supported Android)
#
# BEFORE RUNNING: the app (com.vibesdr.app, a build with tilt in it) is OPEN on a receiver, on the SILVER or
# BLACK chassis, MOTION EFFECTS on, controls shown, waterfall running, screen on. Leave the phone FLAT AND STILL
# for the whole script. Nothing is installed or changed on the device: the runs only switch the measurement mode
# with a link (services/tiltLight.ts):  vibesdr://debug/tilt/off | synthetic | auto
#
# Three runs, the same length each:
#   OFF        tilt forced off — the baseline (fixed light, no sensor subscription)
#   SYNTHETIC  a deterministic slow tilt through the SAME path as the sensor — always moving, so the write gate
#              runs at its full rate: the worst case for frame time
#   AUTO       the real sensor with the phone STILL — what a listener's idle phone costs (the CPU line)
# and it prints each figure against the budget:
#   UI-thread frame time  ≤ +0.5 ms      GPU frame time  ≤ +0.5 ms
#   JS thread: 0 React renders from tilt ([tilt] log: providerRenders must not rise with tilt)
#   CPU, phone still      ≤ +1 %
# The 30-minute battery check is separate (adb shell dumpsys batterystats --reset, then …/batterystats after a
# 30-minute run each way) — it only catches a large regression, as the brief says.
#
# ★ gfxinfo measures the app's HWUI frames (React Native views, and Skia canvases drawn through them). If the Skia
#   canvases render on their own surface on this device, their GPU cost shows in SurfaceFlinger rather than here —
#   the script also prints `dumpsys SurfaceFlinger --latency` frame counts for a cross-check; Perfetto
#   (`record_android_trace`) is the tool if the two disagree.
# ★ Restore afterwards: the script ends by sending vibesdr://debug/tilt/auto (and a relaunch is 'auto' anyway).
set -uo pipefail
SECS="${1:-60}"
DEV="${2:-192.168.86.111:36408}"
PKG=com.vibesdr.app
A=(adb -s "$DEV")

pid() { "${A[@]}" shell pidof "$PKG" 2>/dev/null | tr -d '\r' | awk '{print $1}'; }
P=$(pid)
[ -n "$P" ] || { echo "!! $PKG is not running on $DEV — open it on a receiver first"; exit 1; }
HZ=$("${A[@]}" shell getconf CLK_TCK 2>/dev/null | tr -d '\r'); HZ=${HZ:-100}

ticks() { "${A[@]}" shell cat /proc/"$P"/stat 2>/dev/null | awk '{print $14+$15}'; }
mode()  { "${A[@]}" shell am start -a android.intent.action.VIEW -d "vibesdr://debug/tilt/$1" "$PKG" >/dev/null 2>&1; }
pct()   { # $1 = file, $2 = label regex → ms
  grep -E "^$2" "$1" | head -1 | sed -E 's/.*: *([0-9.]+)ms.*/\1/'; }

run() { # $1 = mode → writes /tmp-ish files
  local m=$1 out; out=$(mktemp -t tilt-$m.XXXX)
  echo "==> $m: switching, settling 5 s, then $SECS s"
  mode "$m"; sleep 5
  "${A[@]}" shell dumpsys gfxinfo "$PKG" reset >/dev/null 2>&1
  "${A[@]}" logcat -c 2>/dev/null
  local t0 t1; t0=$(ticks)
  sleep "$SECS"
  t1=$(ticks)
  "${A[@]}" shell dumpsys gfxinfo "$PKG" > "$out" 2>/dev/null
  "${A[@]}" logcat -d -s ReactNativeJS:I 2>/dev/null | grep "\[tilt\]" > "$out.log"
  local cpu; cpu=$(awk -v a="$t0" -v b="$t1" -v hz="$HZ" -v s="$SECS" 'BEGIN{printf "%.2f", (b-a)/hz/s*100}')
  local p50 p90 g50 g90 frames
  p50=$(pct "$out" "50th percentile"); p90=$(pct "$out" "90th percentile")
  g50=$(pct "$out" "50th gpu percentile"); g90=$(pct "$out" "90th gpu percentile")
  frames=$(grep -m1 "Total frames rendered" "$out" | awk -F': ' '{print $2}')
  local renders writes
  renders=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^providerRenders=/){split($i,a,"="); s+=a[2]}} END{print s+0}' "$out.log")
  writes=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^writes=/){split($i,a,"="); s+=a[2]}} END{print s+0}' "$out.log")
  echo "$m cpu=$cpu frames=${frames:-?} ui50=${p50:-?} ui90=${p90:-?} gpu50=${g50:-?} gpu90=${g90:-?} renders=$renders writes=$writes"
  eval "R_${m}_cpu=$cpu R_${m}_ui50=${p50:-0} R_${m}_ui90=${p90:-0} R_${m}_g50=${g50:-0} R_${m}_g90=${g90:-0} R_${m}_rend=$renders R_${m}_wr=$writes"
}

run off
run synthetic
run auto
mode auto

d() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%+.2f", b-a}'; }
verdict() { awk -v v="$1" -v lim="$2" 'BEGIN{print (v+0 <= lim+0) ? "PASS" : "FAIL"}'; }
echo
echo "── §5.2 budget (synthetic vs off; CPU: auto-still vs off) ─────────────────────────────"
dui=$(d "$R_off_ui90" "$R_synthetic_ui90");  echo "UI frame time  p90 $dui ms   (≤ +0.5)  $(verdict "$dui" 0.5)"
dg=$(d "$R_off_g90" "$R_synthetic_g90");     echo "GPU frame time p90 $dg ms   (≤ +0.5)  $(verdict "$dg" 0.5)"
dc=$(d "$R_off_cpu" "$R_auto_cpu");          echo "CPU, still        $dc %    (≤ +1)    $(verdict "$dc" 1)"
echo "React renders from tilt: off=$R_off_rend synthetic=$R_synthetic_rend auto=$R_auto_rend (must not rise with tilt)  writes: synthetic=$R_synthetic_wr auto=$R_auto_wr"
echo "SurfaceFlinger cross-check (frames in its latency buffer): $("${A[@]}" shell dumpsys SurfaceFlinger --latency 2>/dev/null | wc -l | tr -d ' ')"
echo
echo "▶ Record the rung this device ends on (and these numbers) in src/constants/tiltLight.ts, the ladder comment."
