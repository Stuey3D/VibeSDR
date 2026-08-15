#!/usr/bin/env node
// check-versions.mjs — the app must give ONE answer to "what version is this?".
//
// ★★★ IT HAS GIVEN THREE, MORE THAN ONCE, AND EACH TIME IT COST A RELEASE.
//   · 10.0.2 was built, installed and tested announcing itself as 10.0.1, because app.json moved
//     and constants/version.ts did not. Stuart found it in a diagnostics dump.
//   · On 2026-08-15 the App Store build came out as 10.2 while the app's own About overlay, its
//     User-Agent and the Android build all said 10.3 — because app.json DOES NOT REACH THE iOS
//     BUILD. MARKETING_VERSION lives in the pbxproj, and only `expo prebuild` would copy it
//     across, which this project deliberately never runs. Stuart had been asking "how come it is
//     10.3, I thought we were on 10.2" — and TestFlight had been agreeing with him for days.
//   · Jr carried 1.2 in its project, 1.3 in one User-Agent and 1.0 in the other.
//
// ★★ A VERSION STRING THAT LIES IS NOT COSMETIC. It is sent to third-party receivers, where
//    operators write filter rules against it and are entitled to refuse us by a name that is true;
//    and it is stamped on every diagnostics dump, so it decides whether tomorrow's crash report
//    gets read as a shipping bug or one in the build under test.
//
// ★ This is a GREP, not a build. It is cheap, it runs in the suite, and it fails loudly — which is
//   the only thing that would have caught any of the three.
import { readFileSync } from 'node:fs';

const read = (p) => readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');
let bad = 0;
const fail = (m) => { console.log(`   FAIL ${m}`); bad++; };
const ok   = (m) => console.log(`   ok   ${m}`);

// ── The phone/tablet app: four files, four different ways of being wrong ────────────────────────
const appJson = JSON.parse(read('app.json')).expo.version;
const constTs = read('src/constants/version.ts').match(/APP_VERSION\s*=\s*'([^']+)'/)?.[1];
const gradle  = read('android/app/build.gradle').match(/versionName\s+"([^"]+)"/)?.[1];
// Every configuration, not the first: a Debug/Release pair that disagree ships one of them.
const iosAll  = [...read('ios/VibeSDR.xcodeproj/project.pbxproj')
  .matchAll(/MARKETING_VERSION = ([0-9][^;]*);/g)].map((m) => m[1].trim());

console.log('\nOne version, everywhere it is written down');
console.log(`   .. app.json ${appJson} · version.ts ${constTs} · gradle ${gradle} `
          + `· ios ${[...new Set(iosAll)].join('/')}`);

if (new Set(iosAll).size !== 1) {
  fail(`the iOS project disagrees with ITSELF across configurations: ${iosAll.join(', ')}`);
} else ok('every iOS build configuration agrees');

const want = appJson;
for (const [name, got] of [['constants/version.ts', constTs], ['android build.gradle', gradle],
                           ['ios MARKETING_VERSION', iosAll[0]]]) {
  if (got !== want) {
    fail(`${name} says ${got}, app.json says ${want}`
       + (name.startsWith('ios') ? '  ★★★ THIS IS THE ONE THE APP STORE SHOWS' : ''));
  } else ok(`${name} agrees (${got})`);
}

// ── Jr tracks the phone's minor ─────────────────────────────────────────────────────────────────
// ★ Stuart, 2026-08-15: "if the main app is 10.3 the Jr needs to be 1.3 as I want to keep the .
//   releases in line." A stated convention, so it is checkable — and the pair is meant to be
//   legible at a glance, which only works if it is actually kept.
const jrAll = [...read('spike/WristSDR/WristSDR.xcodeproj/project.pbxproj')
  .matchAll(/MARKETING_VERSION = ([0-9][^;]*);/g)].map((m) => m[1].trim());
if (new Set(jrAll).size !== 1) fail(`Jr's configurations disagree: ${jrAll.join(', ')}`);
else {
  const jrMinor = jrAll[0].split('.')[1], appMinor = want.split('.')[1];
  if (jrMinor !== appMinor) {
    fail(`Jr is ${jrAll[0]} but the app is ${want} — the point releases are meant to track`);
  } else ok(`Jr ${jrAll[0]} tracks the app's .${appMinor}`);
}

console.log(bad ? `\nFAILED ${bad}\n` : '\nall good\n');
process.exit(bad ? 1 : 0);
