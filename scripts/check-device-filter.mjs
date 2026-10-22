// ★★ THE TWO DEVICE FILTERS MUST AGREE (2026-10-22). res/xml/device_filter.xml is what Android reads below 8.0;
// res/xml-v26/device_filter.xml is the same list plus the SDRplay RSPs, whose driver needs API 26. A radio added
// to one and not the other is offered on some phones and not others — so every <usb-device> of the base file must
// be in the v26 one, the v26 one may add ONLY vendor 7671 (0x1df7), and its RSP list must match VibeRsp.kt's.
import { readFileSync } from 'node:fs';
const root = new URL('..', import.meta.url).pathname;
const read = (p) => readFileSync(root + p, 'utf8');
const ids = (xml) => [...xml.matchAll(/<usb-device\s+vendor-id="(\d+)"\s+product-id="(\d+)"/g)].map((m) => `${m[1]}:${m[2]}`);
const base = ids(read('android/app/src/main/res/xml/device_filter.xml'));
const v26 = ids(read('android/app/src/main/res/xml-v26/device_filter.xml'));
let fails = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
ok(base.every((x) => v26.includes(x)), `every radio in xml/ (${base.length}) is in xml-v26/ too`);
const extra = v26.filter((x) => !base.includes(x));
ok(extra.every((x) => x.startsWith('7671:')), `xml-v26/ adds only SDRplay (vendor 7671): ${extra.length} entries`);
ok(!base.some((x) => x.startsWith('7671:')), 'xml/ (below Android 8.0) offers no RSP');
const kt = read('android/app/src/main/java/com/vibesdr/app/VibeRsp.kt');
const m = kt.match(/RSP_PIDS\s*=\s*intArrayOf\(([^)]*)\)/);
const ktPids = m ? m[1].split(',').map((s) => parseInt(s.trim(), 16)).filter((n) => !Number.isNaN(n)).sort() : [];
const xmlPids = extra.map((x) => parseInt(x.split(':')[1], 10)).sort();
ok(JSON.stringify(ktPids) === JSON.stringify(xmlPids), `VibeRsp.RSP_PIDS (${ktPids.length}) matches xml-v26's RSPs (${xmlPids.length})`);
console.log(fails ? `\n${fails} failed` : '\nall passed');
process.exit(fails ? 1 : 0);
