// probe-mode-rates.mjs <http://server> — the owner's rate switch end to end (V12): ADS-B refused at 1.2 MS/s with the
// reason, offered once the switch is on, 1.2 -> 2.4 on entry, ONE change each way on the DAB <-> ADS-B handover, back
// to the base rate on leaving. Run against fake-rtl-tcp --adsb at --rate 1200000 (see scripts/test-adsb-mode.sh).
const base = process.argv[2];
const j0 = await (await fetch(base + '/vibeserver.json')).json();
console.log('1. at start: adsb', j0.adsb, j0.adsbWhy ? '— ' + j0.adsbWhy : '');
const ws = new WebSocket(base.replace('http','ws') + `/ws/user-spectrum?user_session_id=${crypto.randomUUID()}&mode=binary8&bins=512&proto=1&dab=2`);
let rate = 0, ac = 0, dabOn = false, errs = [];
ws.onmessage = (e) => { if (typeof e.data !== 'string') return; let m; try { m = JSON.parse(e.data); } catch { return; }
  if (m.type === 'hwinfo' && m.rateNow) rate = m.rateNow;
  if (m.type === 'adsb') ac = (m.aircraft || []).length;
  if (m.type === 'dab' || m.type === 'dab_list') dabOn = true;
  if (m.type === 'dab_off') dabOn = false;
  if (/_error$/.test(m.type)) errs.push(m.type + ': ' + m.why); };
await new Promise(r => ws.onopen = r);
setInterval(() => ws.send('{"type":"ping"}'), 5000);
const wait = (ms) => new Promise(r => setTimeout(r, ms));
await wait(1500);
ws.send(JSON.stringify({ type: 'dabBoost', on: 1 })); await wait(1500);
const j1 = await (await fetch(base + '/vibeserver.json')).json();
console.log('2. switch on: adsb', j1.adsb, 'dabBoost', j1.dabBoost);
ws.send(JSON.stringify({ type: 'adsb', on: 1 })); await wait(8000);
console.log('3. ADS-B: rate', rate, 'aircraft', ac);
ws.send(JSON.stringify({ type: 'dab', on: 1, channel: '12B' })); await wait(6000);
console.log('4. -> DAB: rate', rate, 'dab', dabOn);
ws.send(JSON.stringify({ type: 'adsb', on: 1 })); await wait(8000);
console.log('5. -> ADS-B: rate', rate, 'aircraft', ac, 'dab', dabOn);
ws.send(JSON.stringify({ type: 'adsb', on: 0 })); await wait(4000);
console.log('6. leave: rate', rate);
if (errs.length) console.log('errors', errs);
process.exit(0);
