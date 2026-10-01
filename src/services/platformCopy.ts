/**
 * platformCopy — the app's own words, made fit for the platform they are shown on.
 *
 * ★★★ iOS MUST NOT NAME ANDROID (App Review guideline 2.3.10: no other mobile platforms in the app or its
 * metadata). The About screen's release notes, feature list and credits grew up describing VibeServer,
 * which an iPhone cannot host, and mention Android two dozen times (Stuart, 2026-10-01: "hide them").
 *   1. A few lines carry Android inside a sentence an iPhone user still wants (the VibeServer pitch, the
 *      V11 "everywhere" list, the credits): REWRITES give those their iOS wording.
 *   2. Anything still naming Android after that is a sentence ABOUT Android (a fix on Android, a feature
 *      of the Android app): it is dropped whole on iOS. Nothing an iPhone user can do is lost.
 * Android and every other platform get the text unchanged.
 * Pure (no React Native): scripts/test_platformCopy.ts runs it over every string the About screen shows.
 */

/** [Android wording, iOS wording] — exact substrings, applied before any sentence is dropped. */
export const IOS_REWRITES: ReadonlyArray<readonly [string, string]> = [
  ['Plug an RTL‑SDR or an Airspy HF+ into your Android phone — or run VibeServer on a Mac, where it also drives SDRplay RSP receivers — and it turns that radio',
   'Run VibeServer on a Mac, a Raspberry Pi or a Linux PC — with an RTL‑SDR, an Airspy or an SDRplay RSP receiver plugged in — and it turns that radio'],
  ['on a Raspberry Pi, a Linux PC, a Mac, an Android phone and VibeServer Lite', 'on a Raspberry Pi, a Linux PC and a Mac'],
  ['the Pi, Linux, the Mac, Android and Lite', 'the Pi, Linux and the Mac'],
  [' Built in, with a small patch to open the radio from an Android USB handle, so it works the moment it is plugged in with nothing to install.', ' Built in.'],
  ['Built from source for Android, VibeServer Lite and the Mac; Cloudflare’s own release on Linux.', 'Built from source for the Mac; Cloudflare’s own release on Linux.'],
  ['the HOST already has — Android’s, Apple’s, or an ffmpeg the owner installed —', 'the HOST already has — Apple’s, or an ffmpeg the owner installed —'],
  // V4: on an iPhone the radio is a networked rtl_tcp server, never a USB dongle in the phone.
  ['Local SDR hardware \u2014 VibeSDR now runs a radio on-device. Plug an RTL-SDR into an Android phone over USB (\u201cLocal Hardware\u201d), or connect to a networked rtl_tcp server from either platform, and the app',
   'rtl_tcp \u2014 connect to a networked rtl_tcp server and the app'],
  [' (iOS and Android)', ''],
  ['(lock screen, Apple Watch, Android Auto, headphones)', '(lock screen, Apple Watch, headphones)'],
];

const OTHER_PLATFORM = /Android/;

/** Sentences, each with its trailing space; a lone em dash between clauses travels with the clause after it. */
function sentences(text: string): string[] {
  return text.match(/[^.!?]+(?:[.!?]+[”’"')]*|$)\s*/g) ?? [text];
}

/** `text` as it should read on `os` ('ios' | 'android' | …). */
export function platformCopy(text: string, os: string): string {
  if (os !== 'ios' || !OTHER_PLATFORM.test(text)) return text;
  let t = text;
  for (const [from, to] of IOS_REWRITES) t = t.split(from).join(to);
  if (!OTHER_PLATFORM.test(t)) return t;
  const kept = sentences(t).filter(s => !OTHER_PLATFORM.test(s));
  return kept.join('')
    .replace(/\s*—\s*$/u, '')          // a clause dash left dangling at the end
    .replace(/^\s*—\s*/u, '')          // …or at the start
    .replace(/\s{2,}/g, ' ')
    .trim();
}
