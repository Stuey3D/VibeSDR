#!/bin/bash
# test_recordBuffer_swift.sh — the iOS/Mac RECORD BUFFER ring (ios/VibeSDR/RecordBufferRing.swift), headless.
#  1. storage: mono when L == R, stereo when not, Int16, trimmed to the capacity, Off frees it;
#  2. ORDER: the same two-queue hand-off VibePowerModule uses (snapshot + arm in ONE audioQ block, then the
#     prefix and the live buffers through a serial writer queue) writes every sample exactly once, in order,
#     while the "audio thread" keeps producing — and the audio thread never waits for the writer.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cat > "$T/main.swift" <<'SWIFT'
import Foundation
var fail = 0
func check(_ ok: Bool, _ m: String) { if ok { print("ok   \(m)") } else { fail += 1; print("FAIL \(m)") } }

// ── 1. storage ──
let r = RecordBufferRing(rate: 48_000)
var L = [Float](repeating: 0, count: 480), R = [Float](repeating: 0, count: 480)
check(!r.isOn, "Off by default")
r.append(left: &L, right: &R, frames: 480)
check(r.seconds == 0, "Off stores nothing")
r.setCapacity(seconds: 1)
for i in 0..<480 { L[i] = Float(i) / 480; R[i] = L[i] }
r.append(left: &L, right: &R, frames: 480)
check(r.snapshot().first?.channels == 1 && r.bytesHeld == 960, "L == R → mono Int16 (960 B for 480 frames)")
R[7] = -0.5
r.append(left: &L, right: &R, frames: 480)
check(r.snapshot().last?.channels == 2 && r.bytesHeld == 960 + 1920, "L ≠ R → stereo interleaved")
check(r.snapshot().last!.samples[15] == Int16(-0.5 * 32767), "stereo R sample lands at 2i+1")
for _ in 0..<200 { r.append(left: &L, right: &R, frames: 480) }
check(r.seconds >= 1 && r.seconds < 1 + 480.0 / 48_000, "trimmed to the 1 s capacity (\(r.seconds))")
r.setCapacity(seconds: 0.5)
check(r.seconds >= 0.5 && r.seconds < 0.5 + 480.0 / 48_000, "shrinking trims the oldest")
r.clear()
check(r.seconds == 0 && r.snapshot().isEmpty, "clear empties")
r.setCapacity(seconds: 0)
check(!r.isOn && r.bytesHeld == 0, "Off frees")
// 60 s mono / stereo memory
let big = RecordBufferRing(rate: 48_000); big.setCapacity(seconds: 60)
var bl = [Float](repeating: 0.1, count: 960), br = bl
for _ in 0..<3100 { big.append(left: &bl, right: &br, frames: 960) }
print(String(format: "     60 s mono: %.2f MB", Double(big.bytesHeld) / 1e6))
check(big.bytesHeld <= 5_800_000, "60 s mono ≤ 5.8 MB")
br[0] = 0.2
for _ in 0..<3100 { big.append(left: &bl, right: &br, frames: 960) }
print(String(format: "     60 s stereo: %.2f MB", Double(big.bytesHeld) / 1e6))
check(big.bytesHeld <= 11_600_000, "60 s stereo ≤ 11.6 MB")

// ── 2. order across the two queues ──
// Each buffer carries a running sample counter in L. The writer appends what it is handed; at the end the
// written stream must be one unbroken run: prefix (from the ring) then live, no gap, no repeat.
let audioQ = DispatchQueue(label: "audio"), writeQ = DispatchQueue(label: "write")
let ring = RecordBufferRing(rate: 48_000); ring.setCapacity(seconds: 2)
var armed = false
var written: [Int16] = []       // writeQ only
var counter = 0                 // audioQ only
var maxAudioBlockNs: UInt64 = 0
func produce(_ n: Int) {        // audioQ: one buffer, as deliver() sees it
  var l = [Float](repeating: 0, count: n)
  for i in 0..<n { l[i] = Float((counter + i) % 30000) / 32768 }
  counter += n
  let t0 = DispatchTime.now().uptimeNanoseconds
  if armed { let copy = l; writeQ.async { for v in copy { written.append(Int16(v * 32768)) } } }
  ring.append(left: &l, right: &l, frames: n)
  maxAudioBlockNs = max(maxAudioBlockNs, DispatchTime.now().uptimeNanoseconds - t0)
}
for _ in 0..<150 { audioQ.sync { produce(480) } }          // 1.5 s of audio before the tap
var prefixSeconds = 0.0
audioQ.async {                                              // startRecording's audioQ block
  let snap = ring.snapshot(); prefixSeconds = ring.seconds; armed = true
  writeQ.async {
    usleep(50_000)                                          // a slow file open + encode: the audio thread must not care
    for c in snap { for i in 0..<c.frames { written.append(c.samples[i * c.channels]) } }
  }
}
let t0 = DispatchTime.now().uptimeNanoseconds
for _ in 0..<100 { audioQ.async { produce(480) } }          // 1 s live, while the prefix is being written
audioQ.sync {}
let produceNs = DispatchTime.now().uptimeNanoseconds - t0
writeQ.sync {}
check(abs(prefixSeconds - 1.5) < 0.011, "prefix = what the ring held (\(prefixSeconds) s)")
var unbroken = written.count == (150 + 100) * 480
for i in 1..<written.count where unbroken {
  let a = Int(written[i - 1]), b = Int(written[i])
  let d = ((b - a - 1) % 30000 + 30000) % 30000   // the ring stores Int16 at ×32767: ±1 is rounding
  if d > 1 && d < 29999 { unbroken = false; print("     break at \(i): \(a) → \(b)") }
}
check(unbroken, "prefix then live: every sample once, in order (\(written.count) samples)")
check(produceNs < 50_000_000, "the audio queue did not wait for the 50 ms writer (\(produceNs / 1_000_000) ms for 100 buffers)")
print(fail == 0 ? "recordBuffer swift: all pass" : "recordBuffer swift: \(fail) FAILED")
exit(fail == 0 ? 0 : 1)
SWIFT
nice xcrun swiftc -O -o "$T/t" ios/VibeSDR/RecordBufferRing.swift "$T/main.swift"
"$T/t"
