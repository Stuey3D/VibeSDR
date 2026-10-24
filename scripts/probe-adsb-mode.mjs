// probe-adsb-mode.mjs <http://server> — ADS-B mode end to end (V12): the vibeserver.json flag, entering, the aircraft
// reports, a tune refused while in it, leaving and the receiver given back. See scripts/test-adsb-mode.sh.
// ★ An audio socket is opened and COUNTED, never played (tests are silent): ADS-B must send no audio at all (Stuart,
// 2026-10-24: "in ADSB mode the audio needs to be muted"), and audio must come back after leaving.
const base = process.argv[2];
const j = await (await fetch(base + '/vibeserver.json')).json().catch(() => ({}));
console.log('vibeserver.json adsb =', j.adsb, j.adsbWhy ? `(${j.adsbWhy})` : '', 'modes', JSON.stringify(j.modes));
const sid = crypto.randomUUID();
const ws = new WebSocket(base.replace('http', 'ws') + `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=1024&proto=1`);
let reports = 0, last = null, off = false, cfgAfter = null, errs = [];
ws.onmessage = (e) => {
  if (typeof e.data !== 'string') return;
  let m; try { m = JSON.parse(e.data); } catch { return; }
  if (m.type === 'adsb') { reports++; last = m; }
  if (m.type === 'adsb_error') errs.push(m.why);
  if (m.type === 'adsb_off') off = true;
  if (m.type === 'config' && off && !cfgAfter) cfgAfter = m;
  if (m.type === 'config' && !globalThis.cfg0) globalThis.cfg0 = m;
};
await new Promise(r => ws.onopen = r);
const au = new WebSocket(base.replace('http', 'ws') + `/ws/audio?user_session_id=${sid}${process.env.VIBE_AUDIO_OPUS ? "&codec=opus" : ""}`);
let audioBytes = 0; au.binaryType = 'arraybuffer';
au.onmessage = (e) => { if (typeof e.data !== 'string') audioBytes += e.data.byteLength; };
setInterval(() => { try { ws.send(JSON.stringify({ type: 'ping' })); } catch {} }, 5000);
await new Promise(r => setTimeout(r, 1500));
console.log('before: centre', globalThis.cfg0?.center_freq ?? globalThis.cfg0?.centerFreq, 'freq', globalThis.cfg0?.frequency);
console.log('audio before ADS-B', audioBytes > 0 ? 'flowing' : 'NONE');
ws.send(JSON.stringify({ type: 'adsb', on: 1 }));
let adsbAudio0 = -1;
for (let t = 0; t < 12; t++) {
  await new Promise(r => setTimeout(r, 1000));
  if (t === 2) adsbAudio0 = audioBytes;   // ★ settled: the entry's last in-flight frames are past
  if (last && t % 5 === 4) {
    const ac = last.aircraft || [];
    console.log(`t=${t + 1}s reports ${reports} aircraft ${ac.length} with pos ${ac.filter(a => a.lat !== undefined).length} msgs ${last.messages} msgRate ${last.msgRate} noise ${last.noise} dropped ${last.dropped} capture ${last.capture}`);
  }
}
console.log('audio during ADS-B', audioBytes - adsbAudio0, 'bytes');
const offAudio0 = audioBytes;
if (errs.length) console.log('errors:', errs);
ws.send(JSON.stringify({ type: 'tune', frequency: 100000000 }));   // must be refused while in ADS-B
await new Promise(r => setTimeout(r, 500));
ws.send(JSON.stringify({ type: 'adsb', on: 0 }));
await new Promise(r => setTimeout(r, 2500));
console.log('audio after ADS-B', audioBytes > offAudio0 ? 'flowing' : 'NONE');
console.log('adsb_off', off, 'after: centre', cfgAfter?.center_freq ?? cfgAfter?.centerFreq, 'freq', cfgAfter?.frequency);
if (last) console.log('sample aircraft:', JSON.stringify((last.aircraft || []).find(a => a.flight && a.lat !== undefined)));
ws.close(); au.close(); process.exit(0);
