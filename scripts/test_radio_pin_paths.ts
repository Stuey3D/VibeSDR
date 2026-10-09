// ★★★ A RADIO WITH A PIN OF ITS OWN IS NEVER OPENED WITHOUT IT (2026-10-09). Stuart: Buddy listed the Pi 500's Airspy
// HF+ (radio PIN, no master PIN) as open, the phone connected without asking, and the SDR screen had no waterfall and no
// audio — every socket refused. Measured: the door's /vibeserver/auth said required:false, the radio's
// /r/<id>/vibeserver/auth required:true. Source checks on the three paths that went round the radio's PIN:
import fs from 'node:fs';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const read = (f: string) => fs.readFileSync(new URL('../' + f, import.meta.url), 'utf8');

const picker = read('src/screens/InstancePickerScreen.tsx');
ok(/vibeServerNeedsPin\(target\)\.catch\(\(\) => vibeServerNeedsPin\(door\)\)/.test(picker),
   'a watch-chosen radio: the PIN question goes to the RADIO, the door only as a fallback');

const sdr = read('src/screens/SDRScreen.tsx');
ok(/d\.radios\.length === 1 && !worthReading && d\.radios\[0\]\.pinLocked !== true/.test(sdr),
   'a door of one PIN radio is not adopted silently');
ok(/door\.radios\.some\(\(r\) => r\.pinLocked === true\)\)/.test(sdr), '…it waits on the card, where the PIN box is');

const app = read('App.tsx');
ok(/pin: r\.pinLocked === true/.test(app), 'the phone tells Buddy which radios have a PIN');
const dir = read('ios/VibeSDRWatch/SDRDirectory.swift');
ok(/var pin: Bool\?/.test(dir), 'Buddy reads it — optional, so an older phone still decodes');
const row = read('ios/VibeSDRWatch/InstancePickerView.swift');
ok(/if r\.pin == true \{[\s\S]{0,120}lock\.fill/.test(row) && /PIN required/.test(row), 'Buddy shows the lock and "PIN required"');

// ★★★ …AND ADMIN MODE OPENS EVERYTHING (Stuart, 2026-10-09: "admin mode is King"). The server already did (vsAuthOk:
//     admin above both PINs); the three pickers still drew a PIN radio locked for the admin.
ok(/const gated = r\.pinLocked === true && !unlockedRadios\[r\.id\] && !adminAuthQ;/.test(sdr), 'app: a PIN radio is open to the admin');
ok(/\{!adminAuthQ && door\.radios\.some\(\(r\) => r\.pinLocked === true/.test(sdr), 'app: no PIN box for the admin');
const web = read('web/client/src/main.ts');
ok(/return radioPinLocked\(r\) && !unlockedRadios\.has\(radioKey\(r\)\) && !inAdminMode\(\);/.test(web), 'web: a PIN radio is open in admin mode');
const jr = read('spike/WristSDR/WristSDR/ContentView.swift');
ok(/let needsRadioPin = r\.pinLocked && !link\.unlockedRadios\.contains\(r\.id\) && !link\.adminArmed/.test(jr), 'Jr: a PIN radio is open to the admin');
const shim = read('android/app/src/main/cpp/local_sdr_shim.cpp');
ok(/bool vsAuthOk[\s\S]{0,2500}if \(adminOkFor\(reqLine, sock\)\) return true;/.test(shim), 'server: the socket gate lets the admin in before any PIN');

console.log(`test_radio_pin_paths: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
