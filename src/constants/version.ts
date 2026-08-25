// Single source of truth for the app version string. Keep in sync with
// app.json `expo.version`. Displayed in the About overlay, the menu footer
// and the instance picker header.
// ★★★ NO "Beta" IN A SHIPPING BUILD. This read '10.0 Beta 1' and is displayed in THREE places a
//     user sees — the About overlay, the menu footer and the instance picker header — so build 17
//     went to App Store review announcing itself as a beta to everyone upgrading from 6.1. Caught
//     by Stuart minutes after submission; the submission was cancelled and the build replaced.
// ★★ The pre-release suffix belongs on the GITHUB release (which is correctly tagged
//     v10.0.0-beta.1 and flagged pre-release), NOT in the app's own version string.
// ★★ AND IT HAS TO BE BUMPED WITH app.json, WHICH 10.0.2 FORGOT. The whole of 10.0.2 was built,
//     installed and tested announcing itself as "10.0.1" — in the About overlay, the menu footer,
//     the instance picker header AND the diagnostics dump. Stuart spotted it from a diagnostics
//     file: "you never changed the version numbers in the app itself." It also sent the wrong
//     User-Agent to every third-party receiver, and it very nearly cost a crash diagnosis: the
//     dump said 10.0.1, so the crash looked like a SHIPPING bug rather than one in the build under
//     test. A version string that lies is not cosmetic — it misroutes the next investigation.
// ★ Now matches app.json's expo.version exactly, as the note below has always asked.
//   NOTE: USER_AGENT derives from this, so it moves 'VibeSDR/10.0' -> 'VibeSDR/10.0.0'. That is
//   "only the version moves", which is what operators' filter rules are written to tolerate.
// ★★★ AND IT DRIFTED AGAIN. app.json went to 10.2 and this stayed at 10.0.2, so the About overlay,
//     the menu footer, the picker header and the diagnostics dump all announced a version that has
//     not shipped for two releases — and every third-party receiver (FM-DX, OWRX, Kiwi) was told
//     "VibeSDR/10.0.2" by a 10.2 app. Seen in the owner's own connection log on 2026-08-14, which
//     is exactly how the last one was caught. A version string that lies misroutes the next
//     investigation: the dump names a SHIPPING build for a bug that is in the one under test.
// ★★★ FOUR PLACES, NOT THREE — AND THE FOURTH IS THE ONE THAT DRIFTED THIS TIME. The 10.5 release
//     commit set out to fix this for good and said so in its own message: "VERSIONS, ALL THREE
//     PLACES THAT HAVE DRIFTED BEFORE: app.json, src/constants/version and android/app/build.gradle".
//     It listed three and there are four. ios/VibeSDR.xcodeproj's MARKETING_VERSION stayed at 10.4,
//     so the next iOS build would have announced 10.4 while the same release on Android announced
//     10.5 — the exact fault the paragraphs above are about, in the one place the checklist did not
//     name. Stuart caught it by simply remembering the number: "I thought we were on 10.4".
//  ★★ THE LIST, IN FULL. Bump ALL of these together:
//       1. app.json                          expo.version
//       2. src/constants/version.ts          APP_VERSION (this line)
//       3. android/app/build.gradle          versionName (and versionCode)
//       4. ios/VibeSDR.xcodeproj             MARKETING_VERSION x4 — the app AND the embedded
//                                            Buddy watch app, which Apple requires to match
//  ★ NOT spike/WristSDR: Jr is a SEPARATE APP on its own version train (1.4) and must not follow.
export const APP_VERSION = '10.5';

/**
 * How we introduce ourselves to SOMEBODY ELSE'S receiver.
 *
 * ★★★ Sent on every connection to a third-party server — FM-DX, OpenWebRX,
 * KiwiSDR — so an operator can see who we are and allow or block us BY NAME.
 *
 * Until 2026-07-29 we sent nothing at all: FM-DX and OWRX opened a bare
 * `new WebSocket(url)` with no User-Agent. An FM-DX operator asked on the
 * FMDX.org Discord how to stop VibeSDR reaching his public server and the
 * honest answer was "you can't cleanly, because it doesn't say who it is."
 * That is not a position to be in. A client that consumes volunteer receivers
 * should be trivially identifiable, and refusing us should be one filter rule.
 *
 * ★ Keep it STABLE and greppable. Operators write rules against this string;
 * changing its shape breaks their rules, so only the version moves.
 */
export const USER_AGENT = `VibeSDR/${APP_VERSION.split(' ')[0]} (+https://vibesdr.net)`;

// ★ VibeSDR Jr announces itself SEPARATELY (see spike/WristSDR — Swift side).
//   The watch is a different client with different behaviour on a shared radio,
//   so an operator can allow one and refuse the other:
//       VibeSDR Jr/1.0 (+https://vibesdr.net)
