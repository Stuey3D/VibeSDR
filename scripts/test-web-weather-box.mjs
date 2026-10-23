/**
 * test-web-weather-box.mjs — the WEB RTTY box draws the server's weather reports UNDER the raw text they came from,
 * collapsed, as text only (V12, 2026-10-23).
 *
 * ★ Runs the REAL decoders.ts (0x08 → onWeatherReport, RTTY only) and weatherBox.ts (the box's nodes) in jsdom, the way
 *   main.ts's onText / onWeatherReport use them. Silent: no audio, no network.
 */
import * as esbuild from 'esbuild';
import { JSDOM } from 'jsdom';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let checks = 0, failures = 0;
const ok = (c, w) => { checks++; if (!c) failures++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${w}`); };

const dom = new JSDOM('<!doctype html><div id="decText"></div>');
globalThis.window = dom.window; globalThis.document = dom.window.document; globalThis.Node = dom.window.Node;
globalThis.location = { protocol: 'http:', host: '127.0.0.1:48111', hostname: '127.0.0.1', port: '48111' };
globalThis.localStorage = { getItem: () => null, setItem: () => {}, removeItem: () => {} };
const sockets = [];
class FakeWS {
  static OPEN = 1;
  constructor(url) { this.url = url; this.readyState = 0; this.sent = []; sockets.push(this); }
  send(s) { this.sent.push(JSON.parse(s)); }
  close() { this.readyState = 3; this.onclose?.(); }
  open() { this.readyState = 1; this.onopen?.(); }
}
globalThis.WebSocket = FakeWS;

const bundle = async (entry) => {
  const b = await esbuild.build({ entryPoints: [path.join(root, entry)], bundle: true, write: false, format: 'esm',
                                  platform: 'browser', target: 'es2020', logLevel: 'silent' });
  return import('data:text/javascript;base64,' + Buffer.from(b.outputFiles[0].text).toString('base64'));
};
const box = await bundle('web/client/src/weatherBox.ts');
const dec = await bundle('web/client/src/decoders.ts');

const enc = (op, json) => { const b = Buffer.from(json); const u = Buffer.alloc(5 + b.length); u[0] = op; u.writeUInt32BE(b.length, 1); b.copy(u, 5); return u.buffer.slice(u.byteOffset, u.byteOffset + u.length); };
const text = (t) => { const b = Buffer.from(t); const u = Buffer.alloc(13 + b.length); u[0] = 1; u.writeUInt32BE(b.length, 9); b.copy(u, 13); return u.buffer.slice(u.byteOffset, u.byteOffset + u.length); };
const REPORT = { type: 'weather_report', kind: 'synop', raw: '10384 41/96 82508 10118', unreadable: 0,
                 line: '10384 · Tempelhof, Germany · 06:00 UTC · wind 250° 8 kt (15 km/h) · 11.8 °C',
                 details: ['Visibility 4 km', '<img src=x onerror=alert(1)>'] };

// The box, driven as main.ts drives it: onText appends to decTextNode(el), onWeatherReport appends weatherReportEl(r).
const el = document.getElementById('decText');
const seen = [];
const dc = new dec.DecoderClient('127.0.0.1:48111', { query: '' }, {
  onText: (t) => { box.decTextNode(el).appendData(t); },
  onWeatherReport: (r) => { seen.push(r); el.appendChild(box.weatherReportEl(r)); box.trimWxBox(el, 8000, 2000); },
}, 'sess-wx000001');
dc.connect();
dc.attach('rtty', { shift: 450 });
const ws = sockets[0]; ws.open();
ws.onmessage({ data: text('AAXX 23064\n10384 41/96 82508 10118=\n') });
ws.onmessage({ data: enc(0x08, JSON.stringify(REPORT)) });
ws.onmessage({ data: text('10385 NIL=\n') });
ok(seen.length === 1 && seen[0].line.startsWith('10384 · Tempelhof'), 'RTTY: the 0x08 frame reaches onWeatherReport');
const kids = [...el.childNodes];
ok(kids.length === 3 && kids[0].nodeType === 3 && kids[1].nodeName === 'DETAILS' && kids[2].nodeType === 3,
   `text · report · text, in arrival order (${kids.map((k) => k.nodeName).join(' ')})`);
ok(kids[0].data.endsWith('10118=\n') && kids[2].data === '10385 NIL=\n', 'the report sits UNDER the raw groups it came from; later text goes under it');
const d = kids[1];
ok(!d.open && d.querySelector('summary').textContent === REPORT.line, 'collapsed to its one line');
ok(d.querySelector('.wxRepBody').textContent.includes('Visibility 4 km') && d.querySelector('.wxRepRaw').textContent === REPORT.raw,
   'opened: the details, then the raw groups');
ok(!d.querySelector('img') && d.textContent.includes('<img src=x'), 'text only — markup from the air is shown, never parsed');
// Other decoders never get a report; a malformed frame is dropped.
ws.onmessage({ data: enc(0x08, '{"type":"weather_report","kind":"synop"') });
ok(seen.length === 1, 'a malformed 0x08 is dropped');
dc.attach('navtex', {});
ws.onmessage({ data: enc(0x08, JSON.stringify(REPORT)) });
ok(seen.length === 1, 'NAVTEX: a 0x08 is not a weather report there');
// The cap: text and reports trimmed from the top, the newest kept.
for (let i = 0; i < 80; i++) { box.decTextNode(el).appendData(`${10000 + i} 41/96 82508=\n`.repeat(10)); el.appendChild(box.weatherReportEl({ ...REPORT, line: 'R' + i })); box.trimWxBox(el, 8000, 2000); }
const reps = el.querySelectorAll('.wxRep');
let total = 0; for (const n of el.childNodes) if (n.nodeType === 3) total += n.length;
ok(reps.length <= 64 && reps[reps.length - 1].querySelector('summary').textContent === 'R79', `capped at 64 reports, newest kept (${reps.length})`);
ok(total <= 10000, `text capped (${total} characters)`);
// A clear (textContent = '') leaves one node again: the long-standing single-node path.
el.textContent = ''; box.decTextNode(el).appendData('RYRY');
ok(el.childNodes.length === 1 && el.textContent === 'RYRY', 'after CLR: one text node, as before');
console.log(`\n   ${checks - failures} of ${checks} passed`);
process.exit(failures ? 1 : 0);
