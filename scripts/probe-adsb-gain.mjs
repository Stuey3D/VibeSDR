// probe-adsb-gain.mjs <http://server> <seconds> — enter ADS-B and follow the gain rule (V12, 2026-10-26): every report's
// gain, whether the rule is still trying steps (gainBusy), its reason (gainWhy) and any feed warning (feedWarn).
// Prints one line per change and a final "settled" line. No audio socket (nothing to hear, and tests are silent).
// ★ A deadline on everything; nothing retained but the last report.
const base = process.argv[2], secs = Number(process.argv[3] || 120), stay = process.argv.includes('--stay');   // --stay: leave ADS-B on (a reconnect test)
const sid = crypto.randomUUID();
const ws = new WebSocket(base.replace('http', 'ws') + `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256&proto=1`);
let last = null, lastKey = '', reports = 0;
const t0 = Date.now();
ws.onmessage = (e) => {
  if (typeof e.data !== 'string') return;
  let m; try { m = JSON.parse(e.data); } catch { return; }
  if (m.type !== 'adsb') return;
  reports++; last = m;
  const key = `${m.gain}|${m.gainBusy}|${m.gainWhy || ''}|${m.feedWarn ? 1 : 0}`;
  if (key !== lastKey) {
    lastKey = key;
    console.log(`t=${((Date.now() - t0) / 1000).toFixed(0)}s gain ${m.gain} busy ${m.gainBusy} noise ${m.noise} msgRate ${m.msgRate}${m.gainWhy ? ' why: ' + m.gainWhy : ''}${m.feedWarn ? ' WARN: ' + m.feedWarn : ''}`);
  }
};
const opened = await Promise.race([new Promise(r => ws.onopen = () => r(true)), new Promise(r => setTimeout(() => r(false), 10000))]);
if (!opened) { console.log('probe: no socket'); process.exit(2); }
const ping = setInterval(() => { try { ws.send(JSON.stringify({ type: 'ping' })); } catch {} }, 5000);
await new Promise(r => setTimeout(r, 1000));
ws.send(JSON.stringify({ type: 'adsb', on: 1 }));
await new Promise(r => setTimeout(r, secs * 1000));
if (last) {
  const ac = last.aircraft || [];
  console.log(`settled: gain ${last.gain} busy ${last.gainBusy} auto ${last.gainAuto} noise ${last.noise} msgRate ${last.msgRate} aircraft ${ac.length} pos ${ac.filter(a => a.lat !== undefined).length} reports ${reports}`);
  console.log(`why: ${last.gainWhy || ''}`);
  console.log(`feedWarn: ${last.feedWarn || ''}`);
}
if (!stay) { try { ws.send(JSON.stringify({ type: 'adsb', on: 0 })); } catch {} }
await new Promise(r => setTimeout(r, 800));
clearInterval(ping); ws.close(); process.exit(last ? 0 : 1);
