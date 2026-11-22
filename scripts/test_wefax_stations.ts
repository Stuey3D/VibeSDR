/**
 * WEFAX STATIONS — ONE LIST (V12 2026-11-22). The server's android/app/src/main/cpp/vibe_wefax_stations.h is the source
 * (the NWS "Worldwide Marine Radiofacsimile Broadcast Schedules"); the Setup page and the Android screen draw its JSON.
 * The clients' own hints must agree with it:
 *   • decoderBands.ts WEFAX_KHZ (where the WEFAX key is "near" on the spectrum) — exactly the server's frequencies;
 *   • knownSignals.ts DWD_FAX / NORTHWOOD_FAX (the labelled stations) — the server's DWD and Northwood lists;
 *   • wefaxAlign.ts NORTHWOOD_KHZ (Northwood's slant) and BORDER_KHZ (the stations that draw a border) — in the list.
 * Run: node --no-warnings scripts/test_wefax_stations.ts
 */
import { readFileSync } from 'node:fs';
import { decoderNear } from '../src/utils/decoderBands.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const read = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const nums = (s: string) => s.split(',').map((x) => x.trim()).filter(Boolean).map(Number);

const h = read('android/app/src/main/cpp/vibe_wefax_stations.h');
const st = new Map<string, number[]>();
for (const m of h.matchAll(/\{ "(\w+)",\s+"[^"]*",\s+"[^"]*",\s+"[^"]*",\s+\{ ([0-9., ]+) \} \}/g)) st.set(m[1], nums(m[2]));
const all = [...st.values()].flat();
ok(st.size === 19 && all.length === 68, `the server lists 19 stations, 68 frequencies (${st.size}, ${all.length})`);
const same = (a: number[], b: number[]) => a.length === b.length && a.every((x, i) => Math.abs(x - b[i]) < 0.005);
const listIn = (src: string, name: string) => { const m = new RegExp(`const ${name} = \\[([^\\]]*)\\]`).exec(src); return m ? nums(m[1]) : []; };

const db = read('src/utils/decoderBands.ts');
ok(same(listIn(db, 'WEFAX_KHZ'), all), 'decoderBands.ts WEFAX_KHZ is exactly the server\'s list, in its order');
ok(decoderNear('wefax', 4233100) && decoderNear('wefax', 8038100) && decoderNear('wefax', 12627350) && !decoderNear('wefax', 5807000 - 1900),
   'the WEFAX key is near Boston 4233.1, Northwood 8038.1, Guangzhou 12627.35 USB dials — not Wellington (stopped 2023)');
const ks = read('src/utils/knownSignals.ts');
ok(same(listIn(ks, 'DWD_FAX'), st.get('dwd')!) && same(listIn(ks, 'NORTHWOOD_FAX'), st.get('gya')!), 'knownSignals.ts DWD_FAX / NORTHWOOD_FAX are the server\'s DWD and Northwood');
const wa = read('src/utils/wefaxAlign.ts');
ok(same(listIn(wa, 'NORTHWOOD_KHZ'), st.get('gya')!), 'wefaxAlign.ts NORTHWOOD_KHZ is the server\'s Northwood');
ok(listIn(wa, 'BORDER_KHZ').length > 0 && listIn(wa, 'BORDER_KHZ').every((k) => all.some((x) => Math.abs(x - k) < 0.005)),
   'wefaxAlign.ts BORDER_KHZ (DWD, Athens 4482.9 / 8106.9) are all in the server\'s list');
ok(st.get('svj4')!.join() === '4482.9,8106.9' && st.get('hsw64')!.join() === '7396.9', 'the schedule\'s carrier-quoted stations are converted to centres (Athens +1.9, Bangkok +1.9)');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
