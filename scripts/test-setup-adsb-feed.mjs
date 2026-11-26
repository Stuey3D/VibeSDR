// ★★ V12 beta96 SHARE ADS-B WITH FLIGHT TRACKERS — the Setup page's real afRowsFrom / afBlob / afRender (vibe_setup_page.h)
// against stand-in elements, and the app's own copy (src/utils/adsbFeed.ts, run through tsx by test_adsb_feed.ts) held to
// the same answers by the JSON fixtures below. ★★ beta96 (Stuart 11-26) SERVER-LEVEL: the card is on the server tab, drawn
// only where the server HAS ADS-B (a radio that could run it, or an ADS-B feed); OFF by default; every refusal in words; the
// FlightAware lines echo the port. And the AIRCRAFT FEEDS card (server-level sources): its form's defaults, refusals, list.
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
const lib = new Function(`${constOf('AF_PRESETS')}${constOf('AF_UUID')}${grab('afRowsFrom')}${grab('afBlob')}${grab('adsbLandOffer')}${grab('afHasAdsb')}
  const ADSB_MIN_RATE = 2400000; return { AF_PRESETS, afRowsFrom, afBlob, adsbLandOffer, afHasAdsb };`)();
const { AF_PRESETS, afRowsFrom, afBlob, adsbLandOffer, afHasAdsb } = lib;

// ── the markup ──
ok(/<div class="card hide" id="adsbFeedCard">/.test(src), 'its OWN card, hidden until afRender says the server has ADS-B');
{ const srv = src.indexOf('id="serverPane"'), card = src.indexOf('id="adsbFeedCard"'), feeds = src.indexOf('id="feedsCard"'), comm = src.indexOf('id="communityCard"'), rad = src.indexOf('id="radioPane"');
  ok(srv > 0 && comm > srv && feeds > comm && card > feeds && (rad < srv ? true : card < rad), 'server tab: community map → aircraft feeds → share with flight trackers (the Sharing/feeds area)'); }
ok(!/id="beastAddBox"|id="beastCard"|id="feedersCard"/.test(src), 'the feed RADIO\'s box and tab card, and the per-radio summary, are gone (feeds are not radios)');
ok(src.includes('id="afServeOwn"') && /data-af="own"/.test(src), 'own-only switches: the Beast output and each destination');
for (const id of ['afServe', 'afPort', 'afOpen', 'afPush', 'afRows', 'afAdd', 'afStatus', 'afKeepNote'])
  ok(src.includes(`id="${id}"`), `#${id} is on the card`);
ok(/receiver-type other/.test(src) && /receiver-host/.test(src) && /receiver-port/.test(src), 'the FlightAware lines (piaware-config receiver-type other / host / port)');
ok(/MLAT \(multilateration\) is not supported/.test(src), 'MLAT: said NOT supported');
ok(/Nothing about your listeners/.test(src) && /never sends this receiver&rsquo;s position|never sends this receiver's position/.test(src), 'the privacy lines');
ok(AF_PRESETS.map((p) => p.host).join(',') === 'feed.adsb.lol,feed.adsb.fi,feed.airplanes.live,feed.map.flights,feed1.adsbexchange.com', 'the presets');
// ★ 11-26 ADS-B Exchange: its feed client's own target (adsbexchange/feedclient configure.sh: feed1.adsbexchange.com,30004,
//   beast_reduce_out, --uuid-file) — a UUID we make when ticked, as the others
ok(JSON.stringify(AF_PRESETS.find((p) => /adsbexchange/.test(p.host))) === '{"name":"ADS-B Exchange","host":"feed1.adsbexchange.com","port":30004,"uuid":"auto"}', 'the ADS-B Exchange preset: feed1:30004, UUID made for you');
ok(/ADS-B Exchange:<\/b> your aircraft reach their map/.test(src) && /MLAT and their feeder stats page need their own feed program/.test(src), 'the card says ADS-B Exchange MLAT and stats need their client');

// ── off by default ──
let rows = afRowsFrom({});
ok(rows.length === 5 && rows.every((r) => !r.on), 'a server that never set it: every preset unticked');
ok(afBlob(false, 30005, false, false, rows, false).blob === '', 'and nothing is stored (the config stays as before)');
ok(JSON.parse(afBlob(false, 30005, false, false, rows, true).blob).serve === false, 'once set, switched off is stored as off');

// ── what a save stores ──
rows = afRowsFrom({});
rows[0].on = true; rows[0].uuid = '0F3C3A2B-1234-4abc-9def-0123456789AB';
let out = afBlob(true, '30005', false, true, rows, false);
ok(out.blob === '{"serve":true,"port":30005,"open":false,"push":true,"feeds":[{"host":"feed.adsb.lol","port":30004,"on":true,"uuid":"0f3c3a2b-1234-4abc-9def-0123456789ab"}]}',
   `adsb.lol ticked + the Beast port: ${out.blob}`);
const back = afRowsFrom(JSON.parse(out.blob));
ok(back[0].on && back[0].uuid === '0f3c3a2b-1234-4abc-9def-0123456789ab' && !back[1].on, 'and it reads back into the same rows');
rows = afRowsFrom({}); rows[0].on = true; rows[0].own = true;
out = afBlob(true, 30005, false, true, rows, false, true);
ok(out.blob === '{"serve":true,"port":30005,"open":false,"push":true,"serveOwn":true,"feeds":[{"host":"feed.adsb.lol","port":30004,"on":true,"own":true}]}', `own-only: stored only when set (${out.blob})`);
ok(afRowsFrom(JSON.parse(out.blob))[0].own === true, '…and read back');
rows = afRowsFrom({ feeds: [{ host: '192.168.1.40', port: 30004, on: false }] });
ok(rows.length === 6 && rows[5].host === '192.168.1.40' && !rows[5].on, 'a custom destination keeps its row (ticked or not)');
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

// ── the gate: the server HAS ADS-B (a radio that could run it — the landing's own rule — or an ADS-B feed) ──
const F = { users: 1, rate: 2400000, boost: false, blocked: false };
ok(adsbLandOffer({ driver: 'rtlsdr' }, null, F).hw === true, 'an RTL could run ADS-B');
ok(adsbLandOffer({ driver: 'airspyhf' }, null, F).hw === false && adsbLandOffer({ driver: 'icom' }, null, F).hw === false, 'an Airspy HF+ or an Icom could not');
ok(afHasAdsb({ radios: [{ driver: 'rtlsdr' }] }, []) === true, 'a server with an RTL: the card is drawn');
ok(afHasAdsb({ radios: [{ driver: 'airspyhf' }, { driver: 'icom' }] }, []) === false, 'HF+ and Icom only, no feeds: not drawn (AGENTS.md)');
ok(afHasAdsb({ radios: [{ driver: 'airspyhf' }] }, [{ fmt: 'beast', dir: 'connect', host: 'pi', port: 30005 }]) === true, '…with a Beast feed: drawn');
ok(afHasAdsb({ radios: [{ driver: 'airspyhf' }] }, [{ fmt: 'sbs', dir: 'listen', port: 32010, label: 'hfdl' }]) === false, '…with only an HFDL feed: not drawn (HFDL never goes to these sites)');

// ── afRender on stand-in elements ──
const els = {};
const el = (id) => els[id] || (els[id] = { id, checked: false, value: '', textContent: '', innerHTML: '', dataset: {},
  classList: { s: new Set(), toggle(c, on) { on ? this.s.add(c) : this.s.delete(c); }, contains(c) { return this.s.has(c); }, add(c) { this.s.add(c); }, remove(c) { this.s.delete(c); } },
  addEventListener() {}, querySelectorAll() { return []; } });
const render = new Function('$', 'adsbLandForm', 'location', 'esc', 'afNewUuid', `
  ${constOf('AF_PRESETS')}${grab('adsbLandOffer')}${grab('afHasAdsb')} const ADSB_MIN_RATE = 2400000;
  let afCfg = arguments[5], afRowsModel = arguments[6], feedsModel = arguments[7];
  ${grab('afRowsDraw')}${grab('afRender')} afRender(); return afRowsModel;`);
const draw = (cfg, serve, push, port, feeds = []) => {
  for (const k of Object.keys(els)) delete els[k];
  el('afServe').checked = serve; el('afPush').checked = push; el('afPort').value = String(port);
  render(el, () => F, { hostname: 'pi3.local' }, (s) => String(s), () => 'x', cfg, afRowsFrom({}), feeds);
  return els;
};
let e = draw({ radios: [{ driver: 'rtlsdr' }] }, true, false, 30105);
ok(!e.adsbFeedCard.classList.contains('hide'), 'rendered for a server with an RTL');
ok(!e.afServeBox.classList.contains('hide') && e.afPushBox.classList.contains('hide'), 'the Beast options open under their switch only');
ok(e.afPortEcho.textContent === '30105' && e.afHost.textContent === 'pi3.local', 'the FlightAware lines name this server and its port');
ok(!e.afKeepNote.classList.contains('hide'), 'no radio keeps ADS-B running, no feed: "only while ADS-B runs" is said');
ok(/feed\.adsb\.lol/.test(e.afRows.innerHTML) && /map\.flights/.test(e.afRows.innerHTML), 'the preset rows are drawn');
e = draw({ radios: [{ driver: 'rtlsdr', landingAdsb: true, adsbKeepRunning: true }] }, false, true, 30005);
ok(e.afKeepNote.classList.contains('hide') && !e.afPushBox.classList.contains('hide'), 'a radio keeps ADS-B running: the note goes; the site list opens under its switch');
e = draw({ radios: [{ driver: 'airspyhf' }] }, false, false, 30005, [{ fmt: 'beast', dir: 'connect', host: 'pi', port: 30005 }]);
ok(!e.adsbFeedCard.classList.contains('hide') && e.afKeepNote.classList.contains('hide'), 'an HF+ with a Beast feed: drawn, and no keep-running note (feeds always run)');
e = draw({ radios: [{ driver: 'airspyhf' }] }, false, false, 30005);
ok(e.adsbFeedCard.classList.contains('hide'), 'an Airspy HF+ alone: the card stays hidden');

// ── the shared fixture the app's copy is held to (scripts/test_adsb_feed.ts reads the same file) ──
const fx = JSON.parse(fs.readFileSync(new URL('./testdata/adsb-feed-cases.json', import.meta.url), 'utf8'));
for (const c of fx) {
  const rs = afRowsFrom(c.stored ? JSON.parse(c.stored) : {});
  for (const [i, p] of Object.entries(c.edit || {})) Object.assign(rs[Number(i)], p);
  const got = afBlob(c.serve, c.port, c.open, c.push, rs, !!c.stored);
  ok(c.blob !== undefined ? got.blob === c.blob : (got.error || '').includes(c.error), `fixture "${c.name}": ${got.blob ?? got.error}`);
}
// ── ★★ the AIRCRAFT FEEDS card (server-level sources) ──
const fl = new Function(`${grab('beastAddrWhy')}${grab('feedsRowsFrom')}${grab('feedsDefaults')}${grab('feedsTitle')}${grab('feedsFromForm')}${grab('feedsBlob')}
  return { beastAddrWhy, feedsRowsFrom, feedsDefaults, feedsTitle, feedsFromForm, feedsBlob };`)();
const why = fl.beastAddrWhy;
ok(why('192.168.86.12:30005') === '' && why('127.0.0.1:30005') === '' && why('piaware.local') === '' && why('[fd00::12]:30005') === '',
   'IPv4:port, a name, [IPv6]:port are taken');
ok(/IPv4/.test(why('192.168.86:30005')) && /IPv4/.test(why('300.1.1.1')), 'a short or out-of-range IPv4 is refused');
ok(/port/.test(why('pi:99999')) && /e\.g\./.test(why('')) && /not a host/.test(why('my pi')), 'bad port, empty, a space: refused in words');
ok(JSON.stringify(fl.feedsDefaults('beast')) === '{"dir":"connect","port":30005,"label":"adsb"}', 'Beast defaults: connect, 30005, ADS-B');
ok(JSON.stringify(fl.feedsDefaults('sbs')) === '{"dir":"listen","port":32010,"label":"hfdl"}', 'BaseStation defaults: listen, 32010, HFDL (dumphfdl)');
let r1 = fl.feedsFromForm('beast', 'connect', '192.168.86.12', '', 'hfdl', 'PiAware', []);
ok(JSON.stringify(r1.src) === '{"fmt":"beast","dir":"connect","label":"adsb","host":"192.168.86.12","port":30005,"name":"PiAware"}', `Stuart's PiAware: ${JSON.stringify(r1.src)} (Beast is always ADS-B)`);
let r2 = fl.feedsFromForm('sbs', 'listen', '', '32010', 'hfdl', 'UberSDR HFDL', [r1.src]);
ok(JSON.stringify(r2.src) === '{"fmt":"sbs","dir":"listen","label":"hfdl","port":32010,"name":"UberSDR HFDL"}', `Stuart's dumphfdl: ${JSON.stringify(r2.src)}`);
ok(/already listens on 32010/.test(fl.feedsFromForm('beast', 'listen', '', '32010', '', '', [r2.src]).error || ''), 'two on one port refused');
ok(/1024 to 65535/.test(fl.feedsFromForm('sbs', 'listen', '', '80', 'hfdl', '', []).error || ''), 'port 80 refused');
ok(/IPv4/.test(fl.feedsFromForm('beast', 'connect', '192.168.86:30005', '', '', '', []).error || ''), 'a bad address refused');
ok(/at most 8/.test(fl.feedsFromForm('sbs', 'listen', '', '40000', 'hfdl', '', Array.from({ length: 8 }, (_, i) => ({ dir: 'connect', port: i }))).error || ''), 'a ninth refused');
const blob = fl.feedsBlob([r1.src, r2.src]);
ok(JSON.stringify(fl.feedsRowsFrom(blob)) === JSON.stringify([r1.src, r2.src]) && fl.feedsBlob([]) === '' && fl.feedsRowsFrom('junk').length === 0, 'stored and read back; none = ""; junk = none');
ok(fl.feedsTitle({ fmt: 'sbs', dir: 'listen', port: 32010, label: 'hfdl' }) === 'HFDL feed on port 32010' && fl.feedsTitle({ name: 'PiAware' }) === 'PiAware', 'what the list calls them');
ok(/id="feedLabelWrap" class="hide"/.test(src) && /feedLabelWrap"\)\.classList\.toggle\("hide", !sbs\)/.test(src), 'the label is asked only for BaseStation (the form shows only what the format needs)');
ok(/--output decoded:basestation:tcp:address=/.test(src), 'the card gives dumphfdl\'s own output option');
console.log(`test-setup-adsb-feed: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
