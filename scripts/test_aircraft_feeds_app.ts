/* test_aircraft_feeds_app.ts — ★★ V12 beta96 the phone's AIRCRAFT FEEDS card rules (src/utils/aircraftFeeds.ts, the Setup page's
 * feedsFromForm / feedsTitle / beastAddrWhy) and the AIR & SEA MAP limits' words. Silent, no network. */
import { feedFromForm, feedsBlob, parseFeeds, feedTitle, feedAddrWhy, feedDefaults, feedStatusLines, mapViewersOf, mapTimeMinOf, mapCostLine, mapViewersLine, feedFmtWord, feedTestWords, feedFindWords, feedKey, feedWithSite, feedIsAdsb } from '../src/utils/aircraftFeeds';
let pass = 0, fail = 0;
const ok = (c: boolean, w: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${w}`); };
console.log('── the phone\'s aircraft feeds card ──');
ok(feedDefaults('beast').dir === 'connect' && feedDefaults('beast').port === 30005 && feedDefaults('sbs').dir === 'listen' && feedDefaults('sbs').port === 32010 && feedDefaults('sbs').label === 'hfdl', 'defaults: Beast connect :30005; BaseStation listen :32010 HFDL (the Setup page\'s)');
const pi = feedFromForm('beast', 'connect', '192.168.86.12:30005', '', 'hfdl', 'PiAware', []);
ok(!!pi.src && pi.src.host === '192.168.86.12' && pi.src.port === 30005 && pi.src.label === 'adsb' && pi.src.name === 'PiAware', 'Stuart\'s PiAware: Beast, connect, forced ADS-B');
const hf = feedFromForm('sbs', 'listen', '', '32010', 'hfdl', 'UberSDR HFDL', [pi.src!]);
ok(!!hf.src && hf.src.port === 32010 && hf.src.label === 'hfdl' && !hf.src.host, 'Stuart\'s UberSDR HFDL: BaseStation, listen 32010, HFDL');
ok(feedFromForm('sbs', 'listen', '', '32010', 'hfdl', '', [hf.src!]).error === 'another feed already listens on 32010', 'two listening on one port: refused');
ok(/1024 to 65535/.test(feedFromForm('sbs', 'listen', '', '80', 'hfdl', '', []).error || ''), 'a listen port under 1024: refused');
ok(/IPv4/.test(feedAddrWhy('192.168.1.300')) && /1 to 65535/.test(feedAddrWhy('host:99999')) && feedAddrWhy('[fe80::1]:30005') === '', 'addresses: a bad IPv4, a bad port, IPv6 in brackets');
ok(feedFromForm('beast', 'connect', 'pi.local', '', 'adsb', '', []).src?.port === 30005, 'no port given: the format\'s usual one');
const many = Array.from({ length: 8 }, (_, i) => feedFromForm('sbs', 'listen', '', String(40000 + i), 'vdl2', '', []).src!);
ok(/at most 8/.test(feedFromForm('beast', 'connect', '10.0.0.1', '', 'adsb', '', many).error || ''), 'at most 8 feeds');
const blob = feedsBlob([pi.src!, hf.src!]);
ok(JSON.stringify(parseFeeds(blob)) === JSON.stringify([pi.src, hf.src]) && feedsBlob([]) === '', 'the stored text round-trips; none = ""');
ok(parseFeeds('junk').length === 0 && parseFeeds('[{"fmt":"beast","dir":"connect","port":30005}]').length === 0 && parseFeeds('[{"fmt":"beast","label":"hfdl","dir":"connect","host":"h","port":1}]')[0].label === 'adsb', 'junk read as nothing; a connect without a host dropped; Beast is always ADS-B');
ok(feedTitle(pi.src!) === 'PiAware' && feedTitle({ fmt: 'sbs', dir: 'listen', port: 32010, label: 'hfdl' }) === 'HFDL feed on port 32010', 'titles (the Setup page\'s)');
ok(feedStatusLines({ sources: [{ title: 'PiAware', label: 'ADS-B', line: 'connected, 1500 messages a second' }] })[0] === 'PiAware (ADS-B): connected, 1500 messages a second', 'the running server\'s line per source');
ok(mapViewersOf('') === 20 && mapViewersOf('0') === 0 && mapViewersOf('5000') === 1000 && mapTimeMinOf('x') === 0 && mapTimeMinOf('30') === 30, 'the map limits: 20 by default, 0 = none, clamped');
ok(/At 20 viewers that is about 38 KB\/s/.test(mapCostLine(20)) && /no limit/.test(mapCostLine(0)), `the cost line ("${mapCostLine(20).slice(0, 90)}…")`);
ok(mapViewersLine({ mapViewers: { now: 2, max: 20 } }) === '2 viewers on the map now of 20' && mapViewersLine({}) === '', 'the viewers-now line');
console.log('── ★★ beta96 follow-up: the UberSDR HFDL format, TEST and FIND (the engine\'s answers, vibe_feed_test.h toJson) ──');
const ub = feedFromForm('uberhfdl', 'listen', '192.168.86.11', '', 'adsb', 'UberSDR HFDL', []);
ok(!!ub.src && ub.src.dir === 'connect' && ub.src.port === 8080 && ub.src.label === 'hfdl' && ub.src.host === '192.168.86.11', `an UberSDR: always connect, 8080, HFDL (${JSON.stringify(ub.src)})`);
ok(parseFeeds('[{"fmt":"uberhfdl","dir":"listen","host":"u","port":8080,"label":"adsb"}]')[0]?.dir === 'connect' && parseFeeds('[{"fmt":"uberhfdl","host":"u","port":8080}]')[0]?.label === 'hfdl', 'stored: read back as connect + HFDL whatever it says');
ok(feedFmtWord('uberhfdl') === 'UberSDR HFDL' && feedFmtWord('sbs') === 'BaseStation' && feedFmtWord('beast') === 'Beast', 'the format words (the Setup page\'s)');
ok(feedTestWords({ ok: true, verdict: 'ok', line: 'Connected — UberSDR HFDL, 23 aircraft (14 with positions)', hint: '' }).text === '✓ Connected — UberSDR HFDL, 23 aircraft (14 with positions)', 'a passing test');
ok(feedTestWords({ ok: false, verdict: 'refused', line: 'Nothing is listening on 192.168.86.13:32010', hint: 'That address is this server.' }).text === '✗ Nothing is listening on 192.168.86.13:32010 — That address is this server.', 'a failing test, with its hint');
ok(!feedTestWords(null).ok && /did not answer/.test(feedTestWords(null).text), 'no answer: said, never a pass');
const found = feedFindWords({ found: true, line: 'Found: PiAware ADS-B (Beast, port 30005) — 112 aircraft', hits: [
  { src: { fmt: 'beast', dir: 'connect', host: '192.168.86.12', port: 30005, label: 'adsb', name: 'PiAware' }, name: 'PiAware', line: 'PiAware ADS-B (Beast, port 30005) — 112 aircraft' },
  { src: { fmt: 'beast', dir: 'connect', port: 30005 }, name: 'junk', line: 'a connect without a host' }], tried: [] });
ok(found.hits.length === 1 && found.hits[0].src.host === '192.168.86.12' && found.hits[0].name === 'PiAware' && found.text.startsWith('Found: PiAware'), 'FIND: a ready-to-add source (junk dropped by the same parser)');
const none = feedFindWords({ found: false, line: 'Nothing found on 192.168.86.13 — tried 30005: nothing listening', hint: 'Is it on?', hits: [] });
ok(none.hits.length === 0 && none.text === 'Nothing found on 192.168.86.13 — tried 30005: nothing listening Is it on?', 'nothing found: what was tried, and the hint');
ok(feedKey(pi.src!) === 'beast|connect|192.168.86.12|30005|adsb', 'the key a TEST result is kept under');
console.log('── ★★ b96-2 "Not at this site" (Stuart 11-27, docs/v12/RANGE-FEED.md) ──');
ok(feedIsAdsb(pi.src!) && !feedIsAdsb(hf.src!) && feedIsAdsb({ fmt: 'sbs', dir: 'listen', port: 30003, label: 'adsb' }), 'ADS-B: a Beast feed, or BaseStation labelled ADS-B');
const piOff = feedWithSite(pi.src!, true);
ok(piOff.offsite === true && pi.src!.offsite === undefined, 'the tick marks an ADS-B feed off-site (a copy)');
ok(feedWithSite(hf.src!, true).offsite === undefined && feedWithSite(piOff, false).offsite === undefined, '…never an HFDL one (the outline is ADS-B only); unticked = no key at all');
ok(feedsBlob([piOff, hf.src!]) === JSON.stringify([{ ...pi.src!, offsite: true }, hf.src]), 'stored as "offsite":true on that feed only (the engine\'s vibe_feeds.h SourceCfg::offsite)');
const back = parseFeeds(feedsBlob([piOff, hf.src!]));
ok(back[0].offsite === true && back[1].offsite === undefined, 'read back: off-site kept');
ok(parseFeeds('[{"fmt":"sbs","dir":"listen","port":32010,"label":"hfdl","offsite":true}]')[0].offsite === undefined
   && parseFeeds('[{"fmt":"beast","dir":"connect","host":"h","port":30005,"offsite":"yes"}]')[0].offsite === undefined
   && parseFeeds('[{"fmt":"beast","dir":"connect","host":"h","port":30005}]')[0].offsite === undefined, 'only true on an ADS-B feed counts; an old config (no key) = at this site');
console.log(`\ntest_aircraft_feeds_app: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
