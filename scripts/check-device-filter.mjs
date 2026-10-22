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
// ★★★ ICOM (docs/ICOM-SUPPORT-PLAN.md Phase 4, decided 2026-10-22): NO Icom id in either filter. The IC-7300's CI-V
// port is a generic CP2102 (10c4:ea60) — a filter entry would offer this app for every GPS and ESP32 board — and no
// native-USB Icom (VID 0x0c26) is supported yet. The owner starts an Icom server from the app. See VibeIcom.kt.
const isIcomId = (x) => { const [v, p] = x.split(':').map(Number); return (v === 0x10c4 && (p === 0xea60 || p === 0xea70)) || v === 0x0c26; };
ok(!base.some(isIcomId) && !v26.some(isIcomId), 'no Icom / CP210x id in either device filter (VibeIcom.kt)');
// ★ VibeIcom.KNOWN_MODELS = the `known = true` rows of icom_models.h — the app offers exactly what the engine accepts.
const icomKt = read('android/app/src/main/java/com/vibesdr/app/VibeIcom.kt');
const km = icomKt.match(/KNOWN_MODELS\s*=\s*arrayOf\(([^)]*)\)/);
const ktModels = km ? [...km[1].matchAll(/"([^"]+)"/g)].map((x) => x[1]).sort() : [];
const modelsH = read('android/app/src/main/cpp/icom_models.h');
const hKnown = [...modelsH.matchAll(/\{0x[0-9A-Fa-f]{2},\s*"([^"]+)",\s*(true|false)\s*,/g)].filter((x) => x[2] === 'true').map((x) => x[1]).sort();
ok(hKnown.length > 0 && JSON.stringify(ktModels) === JSON.stringify(hKnown), `VibeIcom.KNOWN_MODELS (${ktModels.join(', ')}) = icom_models.h known rows (${hKnown.join(', ')})`);
// ★ And the USB ids: Kotlin's CP210x PIDs + Icom VID = icom_transport.cpp's, so the app never offers a device the engine refuses.
const tr = read('android/app/src/main/cpp/icom_transport.cpp');
const hex = (re, src) => { const m = src.match(re); return m ? parseInt(m[1], 16) : NaN; };
const ktCp = (icomKt.match(/CP210X_PIDS\s*=\s*intArrayOf\(([^)]*)\)/) || [, ''])[1].split(',').map((x) => parseInt(x.trim(), 16)).filter((n) => !Number.isNaN(n)).sort();
const cCp = [hex(/kCp2102Pid\s*=\s*0x([0-9a-fA-F]+)/, tr), hex(/kCp2105Pid\s*=\s*0x([0-9a-fA-F]+)/, tr)].sort();
ok(JSON.stringify(ktCp) === JSON.stringify(cCp), `VibeIcom.CP210X_PIDS = icom_transport.cpp's (${cCp.map((n) => n.toString(16)).join(', ')})`);
ok(hex(/ICOM_VID\s*=\s*0x([0-9a-fA-F]+)/, icomKt) === hex(/kIcomVid\s*=\s*0x([0-9a-fA-F]+)/, tr), 'VibeIcom.ICOM_VID = icom_transport.cpp kIcomVid');
console.log(fails ? `\n${fails} failed` : '\nall passed');
process.exit(fails ? 1 : 0);
