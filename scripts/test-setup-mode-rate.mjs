// ★★ THE RATE SWITCH IS GENERAL NOW (V12) — vibe_setup_page.h modeRateShow().
// "Change the sample rate when a mode needs it" keeps the config key dabRateBoost (DAB 2.048, ADS-B 2.4). The RTL-only
// "ADS-B at 3.2 MS/s (extreme)" tick-box that hung off it is REMOVED (Stuart, 2026-10-24: on the Pi 2 it dropped samples
// and heard fewer aircraft) — this suite keeps it gone. In its place (same evening, his OWRX rate): "Use 2.56 MS/s sample
// rate for ADS-B (may drop some samples)", RTL only, its OWN switch (not hung off the rate switch). Runs the page's REAL
// modeRateShow against stand-in elements.
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

// ★ The 3.2 MS/s option stays gone everywhere a reader could meet it; the 2.56 one is carried everywhere.
const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8');
for (const [p, what] of [['../android/app/src/main/cpp/vibe_setup_page.h', 'the setup page'],
                         ['../android/app/src/main/cpp/local_sdr_shim.cpp', 'the shim'],
                         ['../vibeserver/vibeserver_config.cpp', 'the server config'],
                         ['../vibeserver/main.cpp', 'the server'],
                         ['../src/screens/ServerModeScreen.tsx', 'the app’s server screen'],
                         ['../android/app/src/main/java/com/vibesdr/app/VibeServerBoot.kt', 'the Android boot']]) {
  const t = read(p);
  ok(!/adsbExtreme|AdsbExtreme|3\.2 MS\/s \(extreme\)/.test(t), `${what} has no 3.2 MS/s "extreme"`);
  ok(/adsb256|Adsb256/.test(t), `${what} carries adsb256`);
}
ok(/<span>Use 2\.56 MS\/s sample rate for ADS-B \(may drop some samples\)<\/span>/.test(src), 'the page reads "Use 2.56 MS/s sample rate for ADS-B (may drop some samples)"');
ok(/rtl && g_adsb256\.load\(std::memory_order_relaxed\) && !g_rateGuard\.blocked\(\)\) return 2\.56e6;/.test(read('../android/app/src/main/cpp/local_sdr_shim.cpp')),
   'the shim: an RTL with it on takes 2.56 MS/s in ADS-B, unless the rate guard holds a fall-over');
const cfg = read('../vibeserver/vibeserver_config.cpp');
ok((cfg.match(/B\("dabRateBoost", r\.dabRateBoost\)/g) || []).length >= 2, 'vibeserver_config.cpp still reads and writes dabRateBoost');

function els() {
  const e = {};
  const mk = () => ({ disabled: false, checked: false, style: { opacity: '', display: '' },
    _hide: false, classList: { toggle(c, on) { if (c === 'hide') e._last._hide = !!on; } } });
  for (const id of ['dabRateBoost', 'dabRateBoostRow', 'dabRateBoostNote', 'dabRateBoostNo', 'adsb256Box', 'adsb256', 'adsb256Row']) e[id] = mk();
  return e;
}
function run(r, HW = null) {
  const e = els();
  const $ = (id) => { e._last = e[id]; if (!e[id]) throw new Error(`modeRateShow touched #${id}`); return e[id]; };
  new Function('$', 'HW', 'r', `${fn[0]}\nmodeRateShow(r);`)($, HW, r);
  return e;
}

let e = run({ driver: 'rtlsdr', dabRateBoost: true });
ok(!e.dabRateBoost.disabled && e.dabRateBoost.checked, 'RTL, switch saved on → live and ticked');
e = run({ driver: 'rtlsdr' });
ok(!e.dabRateBoost.checked, 'nothing saved → unticked (never the default)');
e = run({ driver: 'airspyhf', dabRateBoost: true });
ok(e.dabRateBoost.disabled && !e.dabRateBoost.checked && e.dabRateBoostNo.style.display === '', 'Airspy HF+: the switch is greyed with the reason');
e = run({ driver: 'rtlsdr', dabRateBoost: true }, { dabHw: false, offline: false });
ok(e.dabRateBoost.disabled, 'a running radio that says it cannot → the switch greyed');

e = run({ driver: 'rtlsdr', adsb256: true });
ok(!e.adsb256Box._hide && !e.adsb256.disabled && e.adsb256.checked, 'RTL, 2.56 saved on, the rate switch OFF → still live and ticked (its own switch)');
e = run({ driver: 'sdrplay', adsb256: true });
ok(e.adsb256Box._hide, 'an RSP: the 2.56 option is not drawn (AGENTS.md: one radio only)');
e = run({ driver: 'rtlsdr' });
ok(!e.adsb256.checked, 'nothing saved → 2.56 unticked (never the default)');

console.log(fail ? `\n${fail} FAILED` : '\nall good');
process.exit(fail ? 1 : 0);
