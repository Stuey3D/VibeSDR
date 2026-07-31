// Single source of truth for the app version string. Keep in sync with
// app.json `expo.version`. Displayed in the About overlay, the menu footer
// and the instance picker header.
// ★★★ NO "Beta" IN A SHIPPING BUILD. This read '10.0 Beta 1' and is displayed in THREE places a
//     user sees — the About overlay, the menu footer and the instance picker header — so build 17
//     went to App Store review announcing itself as a beta to everyone upgrading from 6.1. Caught
//     by Stuart minutes after submission; the submission was cancelled and the build replaced.
// ★★ The pre-release suffix belongs on the GITHUB release (which is correctly tagged
//     v10.0.0-beta.1 and flagged pre-release), NOT in the app's own version string.
// ★ Now matches app.json's expo.version exactly, as the note below has always asked.
//   NOTE: USER_AGENT derives from this, so it moves 'VibeSDR/10.0' -> 'VibeSDR/10.0.0'. That is
//   "only the version moves", which is what operators' filter rules are written to tolerate.
export const APP_VERSION = '10.0.1';

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
