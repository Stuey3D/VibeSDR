// ★★ THE RATE SWITCH IS GENERAL NOW, AND ADS-B 3.2 MS/s IS AN RTL'S ALONE (V12) — vibe_setup_page.h modeRateShow().
// "Change the sample rate when a mode needs it" keeps the config key dabRateBoost (DAB 2.048, ADS-B 2.4); beneath it the
// adsbExtreme tick-box is drawn ONLY for an RTL (AGENTS.md: a control that only works on one radio is not drawn on the
// others) and is live only while the switch is ticked. Runs the page's REAL modeRateShow against stand-in elements.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };
const fn = src.match(/\nfunction modeRateShow\(r\) \{[\s\S]*?\n\}\n/);
if (!fn) { console.log('   FAIL modeRateShow not found in the page'); process.exit(1); }

ok(/<span>Change the sample rate when a mode needs it<\/span>/.test(src), 'the switch reads "Change the sample rate when a mode needs it"');
ok(/DAB needs 2\.048 MS\/s and ADS-B 2\.4 MS\/s/.test(src) && /goes back to this span when the\s+mode ends/.test(src),
   'its note names DAB 2.048 and ADS-B 2.4, and that the base rate comes back');
ok(!/Let DAB borrow/.test(src), 'the old DAB-only label is gone');
ok(/id="adsbExtreme"/.test(src) && /may drop samples at 3\.2 MS\/s and the dongle runs hotter/.test(src),
   'the ADS-B 3.2 MS/s tick-box is on the page with its warning');
ok(/\$\("adsbExtreme"\)\.addEventListener\("change", \(\) => \{ radio\(\)\.adsbExtreme = \$\("adsbExtreme"\)\.checked; \}\)/.test(src),
   'its change handler writes radio().adsbExtreme (saved with the radio, as dabRateBoost is)');

// ★ The server reads both keys back per radio — the round trip the page relies on.
const cfg = fs.readFileSync(new URL('../vibeserver/vibeserver_config.cpp', import.meta.url), 'utf8');
ok((cfg.match(/B\("adsbExtreme", r\.adsbExtreme\)/g) || []).length >= 2 && (cfg.match(/B\("dabRateBoost", r\.dabRateBoost\)/g) || []).length >= 2,
   'vibeserver_config.cpp reads and writes adsbExtreme beside dabRateBoost');

function els() {
  const e = {};
  const mk = () => ({ disabled: false, checked: false, style: { opacity: '', display: '' },
    _hide: false, classList: { toggle(c, on) { if (c === 'hide') e._last._hide = !!on; } } });
  for (const id of ['dabRateBoost', 'dabRateBoostRow', 'dabRateBoostNote', 'dabRateBoostNo',
                    'adsbExtremeBox', 'adsbExtreme', 'adsbExtremeRow']) e[id] = mk();
  return e;
}
function run(r, HW = null) {
  const e = els();
  const $ = (id) => { e._last = e[id]; return e[id]; };
  new Function('$', 'HW', 'r', `${fn[0]}\nmodeRateShow(r);`)($, HW, r);
  return e;
}

let e = run({ driver: 'rtlsdr', dabRateBoost: true, adsbExtreme: true });
ok(!e.adsbExtremeBox._hide && !e.adsbExtreme.disabled && e.adsbExtreme.checked, 'RTL, switch on, extreme saved → shown, live, ticked');
e = run({ driver: 'rtl', dabRateBoost: false, adsbExtreme: true });
ok(!e.adsbExtremeBox._hide && e.adsbExtreme.disabled && e.adsbExtremeRow.style.opacity === '0.45',
   'RTL, switch off → shown but greyed (it needs the switch)');
e = run({ driver: 'rtlsdr' });
ok(!e.adsbExtremeBox._hide && !e.adsbExtreme.checked && !e.dabRateBoost.checked, 'nothing saved → both unticked (never the default)');
for (const d of ['sdrplay', 'hackrf', 'airspy', 'airspyhf', 'icom']) {
  e = run({ driver: d, dabRateBoost: true, adsbExtreme: true });
  ok(e.adsbExtremeBox._hide, `${d}: the 3.2 MS/s option is not drawn`);
}
e = run({ driver: 'airspyhf', dabRateBoost: true });
ok(e.dabRateBoost.disabled && !e.dabRateBoost.checked && e.dabRateBoostNo.style.display === '', 'Airspy HF+: the switch is greyed with the reason');
e = run({ driver: 'rtlsdr', dabRateBoost: true, adsbExtreme: true }, { dabHw: false, offline: false });
ok(e.dabRateBoost.disabled && e.adsbExtremeBox._hide, 'a running radio that says it cannot → the switch greyed and the option hidden');

console.log(fail ? `\n${fail} FAILED` : '\nall good');
process.exit(fail ? 1 : 0);
