// Single source of truth for the app version string. Keep in sync with
// app.json `expo.version`. Displayed in the About overlay, the menu footer
// and the instance picker header.
export const APP_VERSION = '10.0 Beta 1';

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
