// ★★ V12 beta96 SHARE ADS-B WITH FLIGHT TRACKERS — the Setup page's real afRowsFrom / afBlob / afRender (vibe_setup_page.h)
// against stand-in elements, and the app's own copy (src/utils/adsbFeed.ts, run through tsx by test_adsb_feed.ts) held to
// the same answers by the JSON fixtures below. AGENTS.md: the card is drawn only where this radio could run ADS-B; OFF by
// default (nothing stored for a radio that never set it); every refusal in words; the FlightAware lines echo the port.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
let fail = 0, pass = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); c ? pass++ : fail++; };
const grab = (name) => {
  const m = src.match(new RegExp(`\\n(?:async )?function ${name}\\([^)]*\\) \\{[\\s\\S]*?\\n\\}\\n`));
  if (!m) { console.log(`   FAIL ${name} not found in the page`); process.exit(1); }
  return m[0];
};
const constOf = (name) => { const m = src.match(new RegExp(`\\nconst ${name} = [\\s\\S]*?;\\n`)); if (!m) { console.log(`   FAIL ${name}`); process.exit(1); } return m[0]; };
const lib = new Function(`${constOf('AF_PRESETS')}${constOf('AF_UUID')}${grab('afRowsFrom')}${grab('afBlob')}${grab('adsbLandOffer')}
  const ADSB_MIN_RATE = 2400000; return { AF_PRESETS, afRowsFrom, afBlob, adsbLandOffer };`)();
const { AF_PRESETS, afRowsFrom, afBlob, adsbLandOffer } = lib;

// ── the markup ──
ok(/<div class="card hide" id="adsbFeedCard">/.test(src), 'its OWN card, hidden until afRender says this radio can run ADS-B');
for (const id of ['afServe', 'afPort', 'afOpen', 'afPush', 'afRows', 'afAdd', 'afStatus', 'afKeepNote'])
  ok(src.includes(`id="${id}"`), `#${id} is on the card`);
ok(/receiver-type other/.test(src) && /receiver-host/.test(src) && /receiver-port/.test(src), 'the FlightAware lines (piaware-config receiver-type other / host / port)');
ok(/MLAT \(multilateration\) is not supported/.test(src), 'MLAT: said NOT supported');
ok(/Nothing about your listeners/.test(src) && /never sends this receiver&rsquo;s position|never sends this receiver's position/.test(src), 'the privacy lines');
ok(AF_PRESETS.map((p) => p.host).join(',') === 'feed.adsb.lol,feed.adsb.fi,feed.airplanes.live,feed.map.flights', 'the presets');
ok(!AF_PRESETS.some((p) => /adsbexchange/.test(p.host)), 'no ADS-B Exchange preset (its published route is its own feed program)');

// ── off by default ──
let rows = afRowsFrom({});
ok(rows.length === 4 && rows.every((r) => !r.on), 'a radio that never set it: every preset unticked');
ok(afBlob(false, 30005, false, false, rows, false).blob === '', 'and nothing is stored (the radio entry stays as before)');
ok(JSON.parse(afBlob(false, 30005, false, false, rows, true).blob).serve === false, 'once set, switched off is stored as off');

// ── what a save stores ──
rows = afRowsFrom({});
rows[0].on = true; rows[0].uuid = '0F3C3A2B-1234-4abc-9def-0123456789AB';
let out = afBlob(true, '30005', false, true, rows, false);
ok(out.blob === '{"serve":true,"port":30005,"open":false,"push":true,"feeds":[{"host":"feed.adsb.lol","port":30004,"on":true,"uuid":"0f3c3a2b-1234-4abc-9def-0123456789ab"}]}',
   `adsb.lol ticked + the Beast port: ${out.blob}`);
const back = afRowsFrom(JSON.parse(out.blob));
ok(back[0].on && back[0].uuid === '0f3c3a2b-1234-4abc-9def-0123456789ab' && !back[1].on, 'and it reads back into the same rows');
rows = afRowsFrom({ feeds: [{ host: '192.168.1.40', port: 30004, on: false }] });
ok(rows.length === 5 && rows[4].host === '192.168.1.40' && !rows[4].on, 'a custom destination keeps its row (ticked or not)');
ok(JSON.parse(afBlob(false, 30005, false, true, rows, true).blob).feeds[0].on === false, '…and is stored unticked');

// ── refusals in words ──
rows = afRowsFrom({}); rows[3].on = true;
ok(/map\.flights needs the station UUID/.test(afBlob(false, 30005, false, true, rows, false).error || ''), 'map.flights ticked without its UUID: refused, says why');
rows[3].uuid = 'abc';
ok(/not a UUID/.test(afBlob(false, 30005, false, true, rows, false).error || ''), 'a malformed UUID: refused');
ok(/1024 to 65535/.test(afBlob(true, 80, false, false, afRowsFrom({}), false).error || ''), 'port 80: refused');
rows = afRowsFrom({ feeds: [{ host: 'bad host', port: 1 }] });
ok(/not a host/.test(afBlob(false, 30005, false, true, rows, true).error || ''), 'a host with a space: refused');
rows = afRowsFrom({ feeds: Array.from({ length: 9 }, (_, i) => ({ host: `h${i}.org`, port: 1 })) });
ok(/at most 8/.test(afBlob(false, 30005, false, true, rows, true).error || ''), 'nine sites: refused');

// ── the gate: the landing's own rule ──
const F = { users: 1, rate: 2400000, boost: false, blocked: false };
ok(adsbLandOffer({ driver: 'rtlsdr' }, null, F).hw === true, 'an RTL: the card is drawn');
ok(adsbLandOffer({ driver: 'airspyhf' }, null, F).hw === false && adsbLandOffer({ driver: 'icom' }, null, F).hw === false,
   'an Airspy HF+ or an Icom: not drawn (AGENTS.md)');

// ── afRender on stand-in elements ──
const els = {};
const el = (id) => els[id] || (els[id] = { id, checked: false, value: '', textContent: '', innerHTML: '', dataset: {},
  classList: { s: new Set(), toggle(c, on) { on ? this.s.add(c) : this.s.delete(c); }, contains(c) { return this.s.has(c); }, add(c) { this.s.add(c); }, remove(c) { this.s.delete(c); } },
  addEventListener() {}, querySelectorAll() { return []; } });
const render = new Function('$', 'HW', 'adsbLandForm', 'radio', 'location', 'esc', 'afNewUuid', `
  ${constOf('AF_PRESETS')}${grab('adsbLandOffer')} const ADSB_MIN_RATE = 2400000;
  let afRadio = arguments[7], afRowsModel = arguments[8];
  ${grab('afRowsDraw')}${grab('afRender')} afRender(); return afRowsModel;`);
const draw = (r, serve, push, port) => {
  for (const k of Object.keys(els)) delete els[k];
  el('afServe').checked = serve; el('afPush').checked = push; el('afPort').value = String(port);
  render(el, null, () => F, () => r, { hostname: 'pi3.local' }, (s) => String(s), () => 'x', r, afRowsFrom({}));
  return els;
};
let e = draw({ driver: 'rtlsdr' }, true, false, 30105);
ok(!e.adsbFeedCard.classList.contains('hide'), 'rendered for an RTL');
ok(!e.afServeBox.classList.contains('hide') && e.afPushBox.classList.contains('hide'), 'the Beast options open under their switch only');
ok(e.afPortEcho.textContent === '30105' && e.afHost.textContent === 'pi3.local', 'the FlightAware lines name this server and its port');
ok(!e.afKeepNote.classList.contains('hide'), 'without keep-running: "only while ADS-B is running" is said');
ok(/feed\.adsb\.lol/.test(e.afRows.innerHTML) && /map\.flights/.test(e.afRows.innerHTML), 'the preset rows are drawn');
e = draw({ driver: 'rtlsdr', landingAdsb: true, adsbKeepRunning: true }, false, true, 30005);
ok(e.afKeepNote.classList.contains('hide') && !e.afPushBox.classList.contains('hide'), 'with keep-running the note goes; the site list opens under its switch');
e = draw({ driver: 'airspyhf' }, false, false, 30005);
ok(e.adsbFeedCard.classList.contains('hide'), 'an Airspy HF+: the card stays hidden');

// ── the shared fixture the app's copy is held to (scripts/test_adsb_feed.ts reads the same file) ──
const fx = JSON.parse(fs.readFileSync(new URL('./testdata/adsb-feed-cases.json', import.meta.url), 'utf8'));
for (const c of fx) {
  const rs = afRowsFrom(c.stored ? JSON.parse(c.stored) : {});
  for (const [i, p] of Object.entries(c.edit || {})) Object.assign(rs[Number(i)], p);
  const got = afBlob(c.serve, c.port, c.open, c.push, rs, !!c.stored);
  ok(c.blob !== undefined ? got.blob === c.blob : (got.error || '').includes(c.error), `fixture "${c.name}": ${got.blob ?? got.error}`);
}
// ── ★★ the Beast feed radio's address check (the page's beastAddrWhy; the server checks again) ──
const why = new Function(`${grab('beastAddrWhy')} return beastAddrWhy;`)();
ok(why('192.168.86.12:30005') === '' && why('127.0.0.1:30005') === '' && why('piaware.local') === '' && why('[fd00::12]:30005') === '',
   'IPv4:port, a name, [IPv6]:port are taken');
ok(/IPv4/.test(why('192.168.86:30005')) && /IPv4/.test(why('300.1.1.1')), 'a short or out-of-range IPv4 is refused');
ok(/port/.test(why('pi:99999')) && /e\.g\./.test(why('')) && /not a host/.test(why('my pi')), 'bad port, empty, a space: refused in words');
ok(/id="beastAddOpen"/.test(src) && /ADS-B Beast feed \(from another receiver\)/.test(src), 'ADD RADIO offers "ADS-B Beast feed (from another receiver)"');
ok(/value="127\.0\.0\.1:30005"/.test(src), 'its address starts at 127.0.0.1:30005');
// ★ the tab: every SDR card hidden, the name, its own card and the flight-tracker card kept
{
  const mk = (id, extra = '') => ({ id, dataset: {}, cls: new Set(extra ? [extra] : []), inner: id === 'name' ? '#radioName' : '',
    classList: { toggle(c, on) { on ? this.o.cls.add(c) : this.o.cls.delete(c); }, contains(c) { return this.o.cls.has(c); }, add(c) { this.o.cls.add(c); }, remove(c) { this.o.cls.delete(c); } },
    querySelector(q) { return q === '#radioName' && this.inner ? {} : null; } });
  const cards = [mk('name'), mk('beastCard', 'hide'), mk('usageCard'), mk('startCard'), mk('adsbFeedCard')];
  for (const c of cards) c.classList.o = c;
  const pane = { querySelectorAll: () => cards, querySelector: () => cards.find((c) => c.id !== 'beastCard') };
  const fields = { radioPane: pane, beastAddr: { value: '' }, beastAddrMsg: { textContent: '' }, beastStatus: { textContent: '' } };
  const apply = new Function('$', 'beastStatusPoll', `${grab('beastTabApply')} return beastTabApply;`)((id) => fields[id], () => {});
  apply({ driver: 'beast', beastIn: '192.168.86.12:30005', serial: 'beast-1' });
  const shown = cards.filter((c) => !c.cls.has('hide')).map((c) => c.id).join(',');
  ok(shown === 'name,beastCard,adsbFeedCard', `a Beast feed tab: only the name, its own card and the flight-tracker card: ${shown}`);
  ok(fields.beastAddr.value === '192.168.86.12:30005', 'its address is shown');
  apply({ driver: 'rtlsdr' });
  const back = cards.filter((c) => !c.cls.has('hide')).map((c) => c.id).join(',');
  ok(back === 'name,usageCard,startCard,adsbFeedCard', `an SDR's tab after it: every card it had is back (${back})`);
}
console.log(`test-setup-adsb-feed: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
