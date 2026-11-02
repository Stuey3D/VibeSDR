// ★★★ THE USRP's OWNER OPTIONS AND ITS RADIO AGC — HWINFO → CONTROLS, from fake capability objects (V12, 2026-11-02).
// No radio here, so the three places that draw them are driven from the shapes the server writes:
//   1. the Setup page (usrpOptsHtml / usrpImgHtml in vibe_setup_page.h): a choice the radio did not report is not drawn;
//   2. every option key the server writes (vibe_usrp_caps.h capsJson) is read by the page, and the AGC keys by the app
//      panel and the web client;
//   3. the web client's RADIO AGC switch is drawn only when the radio accepted AGC, and says it is not VibeSDR's AGC.
//   node scripts/check-usrp-options.mjs
import { readFileSync } from 'node:fs';
const root = new URL('..', import.meta.url).pathname;
const read = (p) => readFileSync(root + p, 'utf8');
let fails = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

const page = read('android/app/src/main/cpp/vibe_setup_page.h');
const grab = (name) => { const m = page.match(new RegExp(`function ${name}\\([^)]*\\) \\{[\\s\\S]*?\\n\\}`)); ok(!!m, `setup page has ${name}()`); return m ? m[0] : ''; };
const src = ['usrpOptsHtml', 'usrpImgGuess', 'usrpImgSize', 'usrpImgHtml'].map(grab).join('\n');
const names = page.match(/const USRP_IMG_NAMES = \[[^\]]*\];/);
ok(!!names, 'setup page lists UHD\'s image names');
// eslint-disable-next-line no-new-func
const f = new Function(`${names ? names[0] : ''}\n${src}\nreturn { usrpOptsHtml, usrpImgGuess, usrpImgHtml, USRP_IMG_NAMES };`)();

// ── 1. the options card ──
const off = f.usrpOptsHtml(null, {});
ok(!/<select|<input/.test(off) && /while this radio is running/.test(off), 'radio not running: no control, and the page says when they appear');
const bare = f.usrpOptsHtml({ driver: 'usrp', clockSources: [], timeSources: [], gpsdo: false, dcOffset: false, iqBalance: false }, {});
ok(!/<select|<input/.test(bare) && /reported no/.test(bare), 'a radio that reported nothing: nothing drawn, said so');
const b210 = { driver: 'usrp', clockSources: ['internal', 'external'], timeSources: ['internal', 'external'], clock: 'external',
               refLocked: false, gpsdo: false, dcOffset: true, dcOffsetOn: true, iqBalance: true, iqBalanceOn: true,
               rxBwRange: [200000, 56000000], rxBwChoices: [200000, 1000000, 5000000, 56000000], rxBwAuto: true, rxBwHz: 2400000 };
const h = f.usrpOptsHtml(b210, { usrpClock: 'external', usrpDcOffset: false, usrpRxBw: 5000000 });
ok(/id="usrpClock"/.test(h) && /id="usrpTime"/.test(h), 'clock and time selectors drawn from the radio\'s lists');
ok(!/value="gpsdo"/.test(h), 'no GPSDO fitted → no GPSDO choice');
ok(/<option value="external" selected>/.test(h), 'the saved clock is selected');
ok(/NOT locked/.test(h), 'an unlocked external reference is said');
ok(/id="usrpDcOffset"(?![^>]*checked)/.test(h) && /id="usrpIqBalance"[^>]*checked/.test(h), 'corrections: the saved OFF and the default ON');
ok(/id="usrpRxBw"/.test(h) && /<option value="5000000" selected>/.test(h) && /AUTO/.test(h), 'bandwidth: AUTO + the radio\'s choices, the saved one selected');
const withG = f.usrpOptsHtml({ ...b210, clockSources: ['internal', 'external', 'gpsdo'], gpsdo: true, gpsLocked: true }, {});
ok(/value="gpsdo"/.test(withG) && /GPS locked/.test(withG), 'a GPSDO fitted: offered, and its lock shown');
const noBw = f.usrpOptsHtml({ ...b210, rxBwRange: undefined, rxBwChoices: [] }, {});
ok(!/id="usrpRxBw"/.test(noBw), 'a trivial bandwidth range (none published) = no bandwidth control');
const oneClk = f.usrpOptsHtml({ ...b210, clockSources: [], timeSources: [] }, {});
ok(!/id="usrpClock"/.test(oneClk) && !/id="usrpTime"/.test(oneClk), 'no clock choice reported = no selector');
const note = f.usrpOptsHtml({ ...b210, optionsNote: 'this radio has no GPSDO fitted' }, {});
ok(/no GPSDO fitted/.test(note), 'a refusal from the radio is shown in words');
const xss = f.usrpOptsHtml({ ...b210, optionsNote: '<img src=x onerror=alert(1)>' }, {});
ok(!/<img/.test(xss), 'radio-supplied text is escaped');
ok(/\.\.\.\(drv === "usrp" \? usrpCollect\(\) : \{\}\)/.test(page), 'the save sends the USRP options only for a USRP');
const coll = grab('usrpCollect');
for (const k of ['usrpClock', 'usrpTime', 'usrpDcOffset', 'usrpIqBalance', 'usrpRxBw'])
  ok(new RegExp(`if \\(\\$\\("${k}"\\)\\)`).test(coll), `usrpCollect sends ${k} only when it is drawn`);

// ── the image section ──
ok(f.usrpImgGuess('USRP B200mini') === 'usrp_b200mini_fpga.bin' && f.usrpImgGuess('USRP B210') === 'usrp_b210_fpga.bin'
   && f.usrpImgGuess('Ettus USRP B200/B210 (30AA038)') === 'usrp_b210_fpga.bin', 'the file name a radio needs, from what it calls itself');
const img = f.usrpImgHtml({ dir: '/etc/vibeserver/uhd-images', files: [{ name: 'usrp_b210_fpga.bin', bytes: 5207532, mtime: 1, kind: '7-series (Kintex/Artix) image — for clone boards' }] },
                          'This board needs its maker\'s FPGA image — upload usrp_b210_fpga.bin', 'USRP B210', null);
ok(/usrp_b210_fpga\.bin/.test(img) && /7-series/.test(img) && /Remove/.test(img), 'the owner\'s file is listed with its kind and a Remove button');
ok(/not running: This board needs its maker/.test(img), 'why the radio is not running is shown on its card');
ok(/Use this computer's UHD images/.test(img), 'the "use this computer\'s UHD images" action is there');
const scan = f.usrpImgHtml({ files: [] }, '', 'USRP B210', { systemDirs: ['/usr/share/uhd/images'], system: [
  { from: 0, dir: '/usr/share/uhd/images', name: 'usrp_b210_fpga.bin', bytes: 5207532, mtime: 1, same: false, kind: '7-series' },
  { from: 0, dir: '/usr/share/uhd/images', name: 'usrp_b200_fw.hex', bytes: 514346, mtime: 1, same: true, kind: 'FX3 firmware' }] });
ok(/differs from VibeServer/.test(scan) && /usrpImgAdopt\(0, 'usrp_b210_fpga\.bin'\)/.test(scan), 'a system file that differs can be used');
ok(!/usrpImgAdopt\(0, 'usrp_b200_fw\.hex'\)/.test(scan), 'one identical to ours is not offered');
ok(f.USRP_IMG_NAMES.length === 5 && f.USRP_IMG_NAMES.every((n) => /^usrp_b2\d\d(mini)?_(fpga\.bin|fw\.hex)$/.test(n)), 'the page offers only UHD\'s names');
const caps = read('android/app/src/main/cpp/vibe_usrp_caps.h');
for (const n of f.USRP_IMG_NAMES) ok(caps.includes(`"${n}"`), `the server accepts ${n} too`);

// ── 2. every option key written is read ──
const capsJson = caps.slice(caps.indexOf('inline std::string capsJson'));
for (const k of ['clockSources', 'timeSources', 'clock', 'time', 'refLocked', 'gpsdo', 'gpsLocked', 'dcOffset', 'dcOffsetOn',
                 'iqBalance', 'rxBwRange', 'rxBwChoices', 'rxBwHz', 'optionsNote']) {
  ok(capsJson.includes(`\\"${k}\\"`) || capsJson.includes(`("${k}"`), `server writes radio.${k}`);
  ok(new RegExp(`rc\\.${k}\\b`).test(src), `setup page reads radio.${k}`);
}
const panel = read('src/components/LocalHardwarePanel.tsx');
const web = read('web/client/src/main.ts');
ok(/p\.radio\?\.hwAgc/.test(panel) && /p\.radio\?\.agcOn/.test(panel), 'app panel: the radio AGC switch from hwAgc / agcOn');
ok(/p\.radio\?\.clockSources/.test(panel), 'app panel: the reference shown only when the radio has a choice');

// ── 3. the web client's RADIO AGC ──
const fn = web.match(/function applyUsrpCaps\([\s\S]*?\n\}/);
ok(!!fn, 'web client has applyUsrpCaps()');
ok(fn && /row\.hidden = !caps\?\.hwAgc/.test(fn[0]), 'web: RADIO AGC drawn only when the radio accepted AGC');
ok(fn && /setHwGain\(0, true\)/.test(fn[0]), 'web: ON = the AUTO end on the wire, as the app sends');
ok(/if \(isUsrp\) \{ applyUsrpCaps\(caps\)/.test(web), 'web: applied for a USRP only');
const html = read('web/client/index.html');
ok(/id="usrpCtls" hidden/.test(html) && /not VibeSDR's AGC/.test(html), 'web: the switch is named as the radio\'s own, not VibeSDR\'s');
ok(/body\.askLocked #usrpCtls/.test(html) && /#usrpCtls/.test(web.match(/ASK_SHIELD_SEL = [^;]*;/)[0]), 'web: shielded with the other receiver settings');

console.log(fails ? `\n${fails} FAILED` : '\nall USRP option checks pass');
process.exit(fails ? 1 : 0);
