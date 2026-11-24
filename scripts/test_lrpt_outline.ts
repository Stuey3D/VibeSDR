// test_lrpt_outline.ts — V12 METEOR LRPT's MAP OUTLINE, the clients' rule (src/utils/lrptOutline.ts — the app and the web
// page): the default (OFF — Stuart: "map outline should be a toggle"), the remembered choice, the key's words, the
// outline JSON parsed defensively, the live message, a gallery item's "geo" / "gw", the outline items left out of every
// list, and DOWNLOAD following the switch. The server side is vibeserver/test-lrpt-geo.cpp.
import { OUTLINE_DEFAULT_ON, OUTLINE_KEY, outlineFromStored, outlineToStored, outlineKeyLabel, outlineWhyLine, parseOutline,
         parseLiveOutline, outlineOfItem, isOutlineItem, outlineDownloadUrl, outlineLabelSize } from '../src/utils/lrptOutline';

let fail = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

ok(OUTLINE_DEFAULT_ON === false, 'the outline is OFF until the viewer turns it on (one constant)');
ok(outlineFromStored(null) === OUTLINE_DEFAULT_ON && outlineFromStored('junk') === OUTLINE_DEFAULT_ON, 'nothing stored = the default');
ok(outlineFromStored(outlineToStored(true)) === true && outlineFromStored(outlineToStored(false)) === false, 'the choice round-trips');
ok(OUTLINE_KEY === 'vibe.lrptOutline', 'one storage key');
ok(outlineKeyLabel(false) === 'MAP OUTLINE: OFF' && outlineKeyLabel(true) === 'MAP OUTLINE: ON', 'the key reads as a switch');
ok(outlineWhyLine("the satellite's clock reads 3 h 0 min ahead of ours") === "No map outline: the satellite's clock reads 3 h 0 min ahead of ours", 'the reason line');

// an outline as vibe_lrpt_overlay.h writes it
const js = '{"v":1,"w":1568,"h":792,"scale":1.0000,"sat":"Meteor-M2-4","norad":59051,"start":1795442805.129,"end":1795442927.012,'
  + '"periodMs":1231.14,"clockSkewSec":-1.42,"corners":[[54.2,20.4],[48.1,-19.4],[61.2,20.8],[54.1,-25.9]],"centre":[56.4024,-2.7444],'
  + '"coast":[[10,20,30,40,50,60],[1,2]],"lake":[],"border":[[5,5,6,6]],"grid":[[0,100,1568,110]],"labels":[[4,98,"55°N"]]}';
const o = parseOutline(js);
ok(!!o && o.w === 1568 && o.h === 792 && o.sat === 'Meteor-M2-4', 'a real outline parses');
ok(!!o && o.coast === 'M10 20L30 40L50 60' && o.lines.coast.length === 1, 'a one-point line is dropped; the path is built');
ok(!!o && o.border === 'M5 5L6 6' && o.grid === 'M0 100L1568 110' && o.lake === '', 'every layer');
ok(!!o && o.labels.length === 1 && o.labels[0].text === '55°N', 'the grid label');
ok(!!o && !!o.centre && Math.abs(o.centre[0] - 56.4024) < 1e-6, 'the centre');
ok(parseOutline('{"v":2,"w":1,"h":1}') === null && parseOutline('not json') === null && parseOutline({ v: 1, w: -1, h: 5 }) === null, 'not an outline = null');
{ const j = parseOutline({ v: 1, w: 10, h: 10, coast: [[1, 'x', 2, 3, NaN, 4, 5, 6]], labels: [[1, 2, 'x'.repeat(40)], ['a', 2, 'b']] });
  ok(!!j && j.coast === 'M2 3L5 6' && j.labels.length === 0, 'junk inside is skipped, never drawn'); }
ok(parseLiveOutline({ why: 'too little of the pass to place it on the map' }).why.startsWith('too little') && parseLiveOutline({ why: 'x' }).outline === null, 'a live "why"');
ok(parseLiveOutline(JSON.parse(js)).outline?.w === 1568, 'a live outline');

ok(outlineOfItem({ geo: '2026-11-23_140645_lrptgeo.json' }).geo === '2026-11-23_140645_lrptgeo.json', "a picture names its outline");
ok(outlineOfItem({ geo: '../config.json"<' }).geo === '', 'a strange name is not fetched');
ok(outlineOfItem({ gw: 'no orbit data' }).why === 'no orbit data' && outlineOfItem({}).geo === '' && outlineOfItem({}).why === '', 'the reason; an older picture has neither');
ok(isOutlineItem({ k: 'lrptgeo' }) && !isOutlineItem({ k: 'lrpt' }), 'outline items are left out of the lists');
ok(outlineDownloadUrl('/vibeserver/gallery/file/a.png', true, true) === '/vibeserver/gallery/file/a.png?download=1&outline=1', 'DOWNLOAD with the switch ON = the burnt-in copy');
ok(outlineDownloadUrl('/f/a.png?auth=x', false, true) === '/f/a.png?auth=x&download=1', 'OFF = the plain picture');
ok(outlineDownloadUrl('/f/a.png', true, false) === '/f/a.png?download=1', 'no outline = plain whatever the switch');
ok(Math.abs(outlineLabelSize(0.5) - 22) < 1e-9, 'labels ~11 screen px at any size');

console.log(fail ? `${fail} FAILED` : 'all passed');
process.exit(fail ? 1 : 0);
