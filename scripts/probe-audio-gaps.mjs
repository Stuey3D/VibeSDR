// ★★★ MEASURE THE SYMPTOM, NOT A PROXY. A stutter IS a gap in the audio stream, so hold a real
// listener open and timestamp every frame. dspCpu is context; the gap is the fault.
// ★ Node 26's BUILT-IN WebSocket — no 'ws' import, so this runs from anywhere (see memory).
// ★ HTTP + WS only, never ssh: an ssh login to the Pi 2 spawns systemd --user + dbus +
//   mpris-proxy, a burst measured at ~160% CPU on this box, which is itself a cause of stutter.
const base = 'http://192.168.86.129:48001';
const SID  = 'stutter-' + Math.random().toString(16).slice(2, 10);
const MIN  = Number(process.argv[2] || 25);
const wsUrl = (p) => base.replace(/^http/, 'ws') + p;

const spec = new WebSocket(wsUrl(`/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=1024`));
await new Promise((res, rej) => { spec.onopen = res; spec.onerror = rej; });
spec.onmessage = () => {};
spec.send(JSON.stringify({ type: 'tune', frequency: 96.1e6, mode: 'wfm' }));

const audio = new WebSocket(wsUrl(`/ws/audio?user_session_id=${SID}&codec=opus`));
let last = 0, frames = 0, worst = 0;
const gaps = [];
audio.onmessage = () => {
  const t = Date.now(); frames++;
  if (last) { const g = t - last; if (g > worst) worst = g; if (g > 150) gaps.push({ at: new Date(t).toISOString().slice(11, 19), ms: g }); }
  last = t;
};
await new Promise((res) => { audio.onopen = res; audio.onerror = res; });
console.log(`listening for ${MIN} min — reporting audio gaps > 150 ms`);

const t0 = Date.now(); let shown = 0;
while (Date.now() - t0 < MIN * 60000) {
  await new Promise((r) => setTimeout(r, 5000));
  while (shown < gaps.length) {
    const g = gaps[shown++];
    let cpu = '?', drops = '?';
    try { const d = await (await fetch(base + '/vibeserver.json', { signal: AbortSignal.timeout(3000) })).json(); cpu = d.dspCpu; drops = d.iqDrops; } catch {}
    console.log(`  GAP ${String(g.ms).padStart(5)} ms at ${g.at}   dspCpu(now)=${cpu} iqDrops=${drops}`);
  }
}
const mins = (Date.now() - t0) / 60000;
console.log(`\ndone: ${frames} frames in ${mins.toFixed(1)} min, ${gaps.length} gaps > 150 ms, worst ${worst} ms`);
/* ★★★ A VERDICT MUST NOT FIRE ON ONE BLIP. The first version said "STUTTER REPRODUCED" for
 *  gaps.length > 0, and then reported exactly ONE gap of 152 ms in 60,016 frames — 2 ms over an
 *  arbitrary threshold — as if it were the fault. That is a detector that cannot say "clean",
 *  which is worse than no detector at all. Audible stuttering is REPEATED breaks; judge on RATE
 *  and SIZE, and say plainly when the stream was good. */
const perHour = gaps.length / (mins / 60);
const bad = gaps.filter(g => g.ms > 400).length;
console.log(bad > 0 || perHour > 6
  ? `★ STUTTER REPRODUCED with nobody logged in — ${perHour.toFixed(1)} gaps/hour, ${bad} over 400 ms.`
  : `★ ESSENTIALLY CLEAN — ${gaps.length} gap(s) in ${mins.toFixed(0)} min (${perHour.toFixed(1)}/hour), worst ${worst} ms.`);
spec.close(); audio.close(); process.exit(0);
