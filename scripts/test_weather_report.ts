// test_weather_report.ts — the clients' half of the RTTY weather reports (src/utils/weatherReport.ts, V12 2026-10-23).
// The server translates (vibe_synop.h); the clients only check the 0x08 frame and place it. Pins: the frame the SERVER
// writes (pasted from test-synop-host.cpp's output) parses; anything malformed is dropped, never half-shown; the app's
// box markers survive the scrollback cap cutting them in half; a marker can only ever come from the client.
import { parseWeatherFrame, cleanWeatherReport, splitWxText, stripWxMarkers, wxMarker, WX_OPEN, WX_CLOSE } from '../src/utils/weatherReport.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };

const frame = (json: string, op = 0x08) => {
  const b = new TextEncoder().encode(json);
  const u = new Uint8Array(5 + b.length);
  u[0] = op; u[1] = (b.length >>> 24) & 255; u[2] = (b.length >>> 16) & 255; u[3] = (b.length >>> 8) & 255; u[4] = b.length & 255;
  u.set(b, 5);
  return u;
};
// ★ Exactly what the server sent in test-synop-host.cpp (vibe_synop.h Report::json).
const SERVER = '{"type":"weather_report","kind":"synop","raw":"10147 31/97 72315 10092 40213","line":"10147 · Hamburg, Germany · 06:00 UTC · wind 230° 15 kt (28 km/h) · 9.2 °C · 1021.3 hPa","unreadable":0,"details":["Station 10147: Hamburg, Germany","Observed on day 23 of the month at 06:00 UTC","No precipitation","Visibility 10 km","Manned station","Cloud cover 7/8","Wind 230° 15 kt (28 km/h)","Air temperature 9.2 °C","Pressure at sea level 1021.3 hPa"]}';

// 1. The server's frame.
{
  const r = parseWeatherFrame(frame(SERVER));
  ok(!!r && r.kind === 'synop' && r.line.startsWith('10147 · Hamburg, Germany · 06:00 UTC'), 'the server\'s own frame parses');
  ok(!!r && r.raw === '10147 31/97 72315 10092 40213' && r.details.length === 9 && r.unreadable === 0, '...raw, details and count intact');
}
// 2. Malformed frames are dropped whole.
{
  ok(parseWeatherFrame(frame(SERVER, 0x07)) === null, 'another op: not a report');
  ok(parseWeatherFrame(frame(SERVER).subarray(0, 40)) === null, 'a truncated frame: dropped');
  ok(parseWeatherFrame(frame('{"type":"weather_report","kind":"synop"')) === null, 'not JSON: dropped');
  ok(parseWeatherFrame(frame('{"type":"digital_spot","kind":"synop","line":"x"}')) === null, 'another type: dropped');
  ok(parseWeatherFrame(frame('{"type":"weather_report","kind":"metar","line":"x"}')) === null, 'an unknown kind: dropped');
  ok(parseWeatherFrame(frame('{"type":"weather_report","kind":"ship","line":""}')) === null, 'no line: dropped');
  ok(parseWeatherFrame(new Uint8Array([8, 0, 0, 0])) === null, 'shorter than its header: dropped');
}
// 3. Field by field: control characters and marker characters never reach the box; caps.
{
  const r = cleanWeatherReport({ type: 'weather_report', kind: 'ship', line: 'DBBH\n\u0007 · 54.3°N', raw: 7, details: ['a', 5, '', 'b'], unreadable: 1e9 });
  ok(!!r && r.line === 'DBBH   · 54.3°N' && r.raw === '' && r.details.join('|') === 'a|b' && r.unreadable === 99,
     `cleaned: ${JSON.stringify(r)}`);
  const long = cleanWeatherReport({ type: 'weather_report', kind: 'synop', line: 'x'.repeat(5000), details: Array(100).fill('d') });
  ok(!!long && long.line.length === 400 && long.details.length === 40, 'a huge line is capped, and so is the detail list');
}
// 4. The app's markers.
{
  const t = 'AAXX 23064\n10384 41/96 82508=\n' + wxMarker(3) + '10147 31/97=\n' + wxMarker(4);
  const p = splitWxText(t);
  ok(p.length === 4 && 'text' in p[0] && 'id' in p[1] && (p[1] as { id: number }).id === 3 && 'id' in p[3], 'text · report 3 · text · report 4');
  ok(splitWxText('plain RTTY').length === 1, 'no marker: one text part');
  ok(splitWxText('').length === 0, 'empty: nothing');
  // ★ The scrollback cap (SDRScreen appendDecoderText) can cut a marker in half at either end.
  const head = splitWxText('12' + WX_CLOSE + 'next text');
  ok(head.length === 1 && 'text' in head[0] && (head[0] as { text: string }).text === 'next text', 'a marker that lost its opener: its remains are dropped');
  const tail = splitWxText('text ' + WX_OPEN + '12');
  ok(tail.length === 1 && (tail[0] as { text: string }).text === 'text ', 'a marker that lost its closer: dropped');
  const bad = splitWxText('a' + WX_OPEN + 'x9' + WX_CLOSE + 'b');
  ok(bad.length === 2 && bad.every((q) => 'text' in q), 'a marker that is not a number: no report drawn');
  ok(stripWxMarkers('RY' + WX_OPEN + '5' + WX_CLOSE + 'RY') === 'RY5RY', 'decoder text can never carry a marker in: stripped');
  ok(stripWxMarkers('plain') === 'plain', '...and plain text is untouched');
}
console.log(`weather reports: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
