// ★★ V12 ADS-B AS THE LANDING + "KEEP ADS-B RUNNING WHEN NOBODY IS CONNECTED" — the setup page's real adsbLandOffer and
// adsbLandShow (vibe_setup_page.h) against stand-in elements. AGENTS.md: the toggle is drawn only where ADS-B can run on
// this radio; where only a setting stands in the way the page says which; the keep-running box lives only beneath the
// choice, is never ticked by default, and when ticked the Background Decoding and release cards say the radio is ADS-B's.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };
const grab = (name) => {
  const m = src.match(new RegExp(`\\nfunction ${name}\\([^)]*\\) \\{[\\s\\S]*?\\n\\}\\n`));
  if (!m) { console.log(`   FAIL ${name} not found in the page`); process.exit(1); }
  return m[0];
};
const offerSrc = grab('adsbLandOffer'), showSrc = grab('adsbLandShow');
const minRate = (src.match(/const ADSB_MIN_RATE = (\d+);/) || [])[1];
ok(minRate === '2400000', 'the page knows ADS-B needs 2.4 MS/s');
const offer = new Function(`const ADSB_MIN_RATE = ${minRate};\n${offerSrc}\nreturn adsbLandOffer;`)();

// ── the rule ──
const F = (o = {}) => ({ users: 1, rate: 2400000, boost: false, blocked: false, ...o });
let o = offer({ driver: 'rtlsdr' }, null, F());
ok(o.hw && o.offer, 'RTL at 2.4 MS/s: offered');
o = offer({ driver: 'airspyhf' }, null, F());
ok(!o.hw && !o.offer && o.why === '', 'Airspy HF+ (cannot reach 1090 MHz): not drawn, nothing said');
o = offer({ driver: 'icom' }, null, F());
ok(!o.hw, 'Icom: not drawn');
o = offer({ driver: 'rtlsdr' }, { adsbHw: false, offline: false }, F());
ok(!o.hw, 'a running radio that says it cannot (adsbHw false): not drawn');
o = offer({ driver: 'airspyhf' }, { adsbHw: true, offline: false }, F());
ok(o.hw && o.offer, 'the running radio’s own answer wins over the driver guess');
o = offer({ driver: 'sdrplay', mode: 'locked' }, null, F({ users: 5 }));
ok(!o.hw, 'a locked centre with its own-VFO listeners: not drawn (ADS-B would move the window)');
o = offer({ driver: 'rtlsdr' }, null, F({ rate: 2048000 }));
ok(o.hw && !o.offer && /2\.4 MS\/s/.test(o.why) && /Change the sample rate when a mode needs it/.test(o.why),
   'RTL at 2.048 MS/s without the rate switch: hidden, and the reason names both fixes');
o = offer({ driver: 'rtlsdr' }, null, F({ rate: 2048000, boost: true }));
ok(o.offer, '…with the rate switch ticked: offered');
o = offer({ driver: 'rtlsdr' }, null, F({ blocked: true }));
ok(o.hw && !o.offer && /switched off/.test(o.why), 'ADS-B blocked on the radio: hidden, and said');

// ── the drawing ──
function els() {
  const e = {};
  const mk = (id) => ({ id, checked: false, textContent: '', style: { opacity: '', display: '' }, _hide: true,
    classList: { toggle(c, on) { if (c === 'hide') e[id]._hide = !!on; } } });
  for (const id of ['adsbLandWrap', 'adsbLandOn', 'adsbLandBox', 'adsbKeep', 'adsbLandNote', 'adsbPlainHead',
                    'bgAdsbNote', 'bgProfiles', 'releaseAdsbNote']) e[id] = mk(id);
  return e;
}
function show(r, off) {
  const e = els();
  const $ = (id) => e[id] || null;
  const on = new Function('$', 'r', 'o', 'plainLandingWords', `${showSrc}\nreturn adsbLandShow(r, o);`)($, r, off, () => '96.1 MHz WFM');
  return { e, on };
}
const yes = { hw: true, offer: true, why: '' };
let s = show({ driver: 'rtlsdr' }, yes);
ok(!s.e.adsbLandWrap._hide && !s.e.adsbLandOn.checked && s.e.adsbLandBox._hide && !s.on,
   'offered, nothing saved: the toggle shows, unticked, and keep-running is hidden beneath it');
s = show({ driver: 'rtlsdr', landingAdsb: true }, yes);
ok(s.on && !s.e.adsbLandBox._hide && !s.e.adsbKeep.checked, 'ADS-B landing on: keep-running appears, NOT ticked');
ok(s.e.adsbPlainHead.style.display === '', 'and the plain landing is labelled as EXIT ADS-B’s fallback');
ok(s.e.bgAdsbNote._hide && s.e.releaseAdsbNote._hide, 'background decoding and the release are untouched without keep-running');
s = show({ driver: 'rtlsdr', landingAdsb: true, adsbKeepRunning: true }, yes);
ok(s.e.adsbKeep.checked, 'keep-running saved: ticked');
ok(!s.e.bgAdsbNote._hide && s.e.bgProfiles.style.opacity === '.45', '★ the Background Decoding profiles are greyed, and the note says ADS-B owns the idle radio');
ok(!s.e.releaseAdsbNote._hide, '★ the release card says the radio is never let go');
s = show({ driver: 'rtlsdr', adsbKeepRunning: true }, yes);
ok(!s.e.adsbKeep.checked && s.e.bgAdsbNote._hide, 'keep-running without the landing means nothing');
s = show({ driver: 'rtlsdr', landingAdsb: true }, offer({ driver: 'rtlsdr' }, null, F({ rate: 1024000 })));
ok(s.e.adsbLandWrap._hide && !s.on && /2\.4 MS\/s/.test(s.e.adsbLandNote.textContent) && /96\.1 MHz WFM/.test(s.e.adsbLandNote.textContent),
   'a saved landing the rate now rules out: hidden, the reason, and where listeners start instead');
s = show({ driver: 'airspyhf', landingAdsb: true }, offer({ driver: 'airspyhf' }, null, F()));
ok(s.e.adsbLandWrap._hide && s.e.adsbLandNote.textContent === '', 'an HF+ is told nothing at all');

// ── the copy ──
ok(/<span>Keep ADS-B running when nobody is connected<\/span>/.test(src), 'the tick box reads "Keep ADS-B running when nobody is connected"');
ok(/It costs CPU all the time<\/b> \(about 13&nbsp;% of a Pi&nbsp;3 core\)/.test(src), 'its note states the CPU trade-off in one plain line');
ok(/landingAdsb: !!radio\(\)\.landingAdsb,\s*\n\s*adsbKeepRunning: !!radio\(\)\.landingAdsb && !!radio\(\)\.adsbKeepRunning,/.test(src),
   'the save sends both, keep-running never without the landing');

console.log(fail ? `\n${fail} FAILED` : '\nall good');
process.exit(fail ? 1 : 0);
