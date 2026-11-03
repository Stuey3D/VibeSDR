// ★★ THE OWNER'S PORT NAMES, THE CLIENTS' HALF — src/utils/antennaPortNames.ts (V12, 2026-11-03). The app, the web client
// and the setup page (a JS copy, antPortLabel / antPortNameClamp in vibe_setup_page.h) must read the same label from the
// same hwinfo `antennaNames`; the server's own rule is portnames::display (vibeserver/test-port-names.cpp, same cases).
import { clampPortName, portLabel, portNamesFrom, PORT_NAME_MAX } from '../src/utils/antennaPortNames.ts';
import fs from 'node:fs';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
console.log('test_antenna_port_names');

const names = portNamesFrom({ RX2: 'Discone', 'RX2 (A)': 'FM Yagi', 'Hi-Z': '  Long \t wire ', B: 7, C: '' });
ok(portLabel('RX2', names) === 'Discone (RX2)', 'Discone (RX2)');
ok(portLabel('RX2 (A)', names) === 'FM Yagi · RX2 (A)', 'FM Yagi · RX2 (A) — no nested brackets');
ok(portLabel('Hi-Z', names) === 'Long wire (Hi-Z)', 'whitespace in a received name collapsed');
ok(portLabel('B', names) === 'B' && portLabel('C', names) === 'C', 'a non-string or empty name reads as the bare port');
ok(portLabel('TX/RX', names) === 'TX/RX' && portLabel('A', null) === 'A', 'no name / no names at all: the bare port');
ok(Object.keys(portNamesFrom(['A'])).length === 0 && Object.keys(portNamesFrom('x')).length === 0
   && Object.keys(portNamesFrom(undefined)).length === 0, 'anything but an object is no names');
ok(portLabel('A', portNamesFrom({ A: '<b>x</b>' })) === '<b>x</b> (A)', 'the owner\'s text is kept as TEXT (drawn with textContent / React text, never HTML)');
ok(clampPortName('x'.repeat(100)).length === PORT_NAME_MAX, 'an owner\'s typing is cut to 40 bytes');
ok(new TextEncoder().encode(clampPortName('Ω'.repeat(30))).length === 40, '…in whole characters (20 × Ω)');
ok(new TextEncoder().encode(clampPortName('x' + 'Ω'.repeat(30))).length === 39, '…an odd cut backs off to the last whole glyph');

// ★ The setup page's copy of the label and clamp rules, run as the page runs them.
const page = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const grab = (name: string) => { const m = page.match(new RegExp(`function ${name}\\([^)]*\\) \\{[\\s\\S]*?\\n\\}`)); return m ? m[0] : ''; };
const src = ['const ANT_PORT_NAME_MAX = 40;', grab('antPortNamesOf'), grab('antPortLabel'), grab('antPortNameClamp'), grab('antPortNameEdit')].join('\n');
ok(!!grab('antPortLabel') && !!grab('antPortNameClamp') && !!grab('antPortNameEdit'), 'the setup page carries the label, clamp and edit rules');
// eslint-disable-next-line no-new-func
const pg = new Function(`${src}; return { antPortLabel, antPortNameClamp, antPortNameEdit, antPortNamesOf };`)() as any;
for (const [port, nm] of [['RX2', 'Discone'], ['RX2 (A)', 'FM Yagi'], ['Hi-Z', 'Long wire'], ['Tuner 2 50Ω', 'Dipole']] as const)
  ok(pg.antPortLabel(port, { [port]: nm }) === portLabel(port, { [port]: nm }), `the setup page reads ${port} the same: ${pg.antPortLabel(port, { [port]: nm })}`);
ok(pg.antPortNameClamp('Ω'.repeat(30)) === clampPortName('Ω'.repeat(30)), 'the setup page clamps the same');
const r: any = {};
pg.antPortNameEdit(r, 'A', '  Dipole  ');
pg.antPortNameEdit(r, 'B', 'Vertical');
ok(r.antennaNames === JSON.stringify({ A: 'Dipole', B: 'Vertical' }), `edits land on the radio as the stored JSON: ${r.antennaNames}`);
pg.antPortNameEdit(r, 'A', '');
pg.antPortNameEdit(r, 'B', '   ');
ok(r.antennaNames === '', 'clearing every box stores nothing at all');
ok(/antennaNames: radio\(\)\.antennaNames \|\| ""/.test(page), 'the setup page saves antennaNames with the radio');

console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
