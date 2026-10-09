// ★★ THE ANTENNA SWITCH SETUP RULES — src/services/antennaSwitch.ts (2026-10-09). Lite's server screen saves these; the
// Linux setup page carries a JS copy of the same rules (vibe_setup_page.h swServerFields — one rule, two readers).
import { EMPTY_SWITCH, parseSwitchCfg, serverFields, setAntennaCount, splitBroker, type AntennaSwitchCfg } from '../src/services/antennaSwitch.ts';
import fs from 'node:fs';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

const relays = [
  { label: 'Sky Loop', cmd: 'cmnd/antsw/POWER1', state: 'stat/antsw/POWER1', on: 'ON', off: 'OFF' },
  { label: 'VHF Vertical', cmd: 'cmnd/antsw/POWER2', state: 'stat/antsw/POWER2', on: 'ON', off: 'OFF' },
  { label: 'Relay 3', cmd: 'cmnd/antsw/POWER3', state: 'stat/antsw/POWER3', on: 'ON', off: 'OFF' },
];
const base: AntennaSwitchCfg = { ...EMPTY_SWITCH, enabled: true, broker: '192.168.86.77:1883', deviceName: 'Generic Antenna Switch Example', relays };

ok(serverFields(EMPTY_SWITCH).antennaSwitch === '', 'no switch: nothing is sent');
ok(serverFields({ ...base, antennas: [] }).antennaSwitch === '', 'a switch with no antennas yet: nothing is sent (no half-built selector)');
const two = setAntennaCount(base, 2);
ok(two.antennas.length === 2 && two.antennas[0].name === 'Sky Loop' && two.antennas[1].relay === 1,
   'two antennas: named from the relays\' own names, one relay each');
ok(setAntennaCount(base, 9).antennas.length === 3 && setAntennaCount(base, 1).antennas.length === 2,
   'the count stays between 2 and the relays the switch has');
ok(setAntennaCount(base, 3).antennas[2].name === 'Antenna 3', 'a relay with only a default name gets "Antenna n"');
const cfg = { ...two, antennas: [{ name: 'Sky Loop', relay: 0, bands: '0-30MHz' }, { name: 'VHF Vertical', relay: 1, bands: '30MHz+' }], locked: true };
const f = serverFields(cfg);
const sw = JSON.parse(f.antennaSwitch);
ok(sw.host === '192.168.86.77' && sw.port === 1883 && sw.antennas.length === 2 && sw.antennas[1].cmd === 'cmnd/antsw/POWER2',
   'the server is sent the broker and each antenna\'s topics');
ok(f.antennaMap === '0-30MHz Sky Loop, 30MHz+ VHF Vertical', `the per-band list is built from each antenna's bands: "${f.antennaMap}"`);
ok(f.antennaLocked === true, 'the admin lock goes with it');
ok(serverFields({ ...cfg, antennas: [{ ...cfg.antennas[0], bands: '0-2MHz, 2-30MHz' }, cfg.antennas[1]] }).antennaMap
   === '0-2MHz Sky Loop, 2-30MHz Sky Loop, 30MHz+ VHF Vertical', 'several bands for one antenna: one entry each');
ok(serverFields({ ...cfg, enabled: false }).antennaSwitch === '', 'switched off: nothing is sent');
ok(JSON.stringify(splitBroker('mqtt.local')) === JSON.stringify({ host: 'mqtt.local', port: 1883 }), 'a broker without a port is 1883');
ok(parseSwitchCfg('{not json').enabled === false && parseSwitchCfg(JSON.stringify(cfg)).antennas[1].name === 'VHF Vertical',
   'a stored config reads back; a corrupt one is "no switch", never a crash');
// ★ The setup page's copy builds the per-band list the same way.
const page = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
ok(/named\.flatMap\(\(a\) => a\.bands\.split\(","\)/.test(page) && /\+ " " \+ a\.name\.trim\(\)\)\)\.join\(", "\)/.test(page),
   'the Linux setup page builds the per-band list by the same rule');

console.log(`test_antenna_switch_cfg: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
