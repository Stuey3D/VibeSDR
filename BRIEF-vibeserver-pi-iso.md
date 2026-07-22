# BRIEF: VibeServer Pi ISO — the headless appliance

**Project:** VibeSDR / VibeServer
**Author:** Stuart Carr (Stuey3D), dictated 2026-07-22
**Depends on:** `files/BRIEF-vibeserver-protocol-foundations.md` (incl. its 2026-07-22 two-level
access amendment) and `files/BRIEF-vibeserver-macos.md` (the shared web config page).
**Status:** DESIGN. Not started. The Pi *desktop* install is a separate, trivial case — see §1.

---

## 1. Two Pi products, and only one of them is work

1. **Pi desktop install** — Raspberry Pi OS with a monitor and keyboard. Install the app, use the
   standard settings, done. Nothing Pi-specific to build: ARM + POSIX + the same CMake target the
   Mac build already uses.
2. **Pi ISO appliance** — headless, field-portable, no screen. **This brief.** Everything below is
   ISO-only; none of it belongs in VibeServer's own configuration.

## 2. First boot

1. Boot. If **no network connection is established**, VibeServer raises an **open Wi-Fi hotspot**
   named `VibeServer`.
2. The user connects, and the configuration page appears as a **captive network page** — the same
   experience as hotel Wi-Fi.
3. **First entry FORCES setting an admin password.** That password is the credential for both the
   Pi and VibeServer (but see §5 — the Pi's OS credential must not be held by the server process).
4. From there the page offers full configuration of both the appliance and the receiver.

## 3. The configuration page has two halves

- **Compute Hardware & Network** *(ISO-only)* — everything about the box: Wi-Fi (host an AP vs join
  a home network), and OS-level concerns including **power management**. A user running from a
  battery bank may deliberately throttle the Pi to extend runtime, so CPU governor, and sensible
  extras like HDMI/LED/USB power, belong here.
- **VibeServer** *(portable)* — **byte-identical to every other VibeServer**, whether the host is
  macOS, Android or headless Linux. Same schema, same page, same behaviour.

★ The boundary is the point. VibeServer's config document stays portable and host-agnostic; the
appliance adds a section of its own, backed by the ISO's own config, and never leaks wifi or power
fields into `VibeServerConfig`.

## 4. ★ Captive portals are a RESTRICTED browser — design around it

The auto-opening page works by answering the OS probe URLs (`captive.apple.com/hotspot-detect.html`,
`connectivitycheck.gstatic.com/generate_204`, `msftconnecttest.com/connecttest.txt`) from the AP's
DNS/HTTP responder. The window that opens is **not a full browser**: it is a cut-down webview, most
restrictive on iOS, where WebSockets and much JavaScript are unreliable or blocked outright, and the
window can be closed by the OS at any moment.

Therefore the captive page MUST be a **lightweight setup page only** — set the admin password,
choose AP-or-join, save — and then tell the user to open a real browser at the server's address for
the full client and the management UI. Serving the SDR client (WebSocket spectrum + audio) into a
captive webview will not work; do not attempt it.

## 5. ★ One password, two systems — the sharp edge

"The password for the Pi and VibeServer" is right as a *user-facing* promise, but the implementation
must not take it literally:

- Setting a Linux account password from a web form requires elevated privilege in the service — a
  real attack surface on a box that may face a public network.
- If the SDR's admin password IS the OS credential, brute-forcing the radio becomes an OS compromise
  rather than merely an SDR one.

Recommended: the first-boot flow *sets both* from one prompt, but VibeServer keeps only its own
hashed copy and never holds or proxies the OS credential thereafter. Network-side lockout stays as
per foundations §6 (nonce/HMAC + backoff).

## 6. ★ Recovery — a headless box must not be brickable

No screen, no keyboard: if Wi-Fi setup fails or the password is forgotten, the user has no way in.
A documented recovery path is mandatory, and the conventional answer is a file on the **boot
partition** (readable from any machine that can mount the SD card) that resets the password and/or
forces AP mode on next boot.

## 7. Out of scope

- The Pi desktop install (§1.1) — no work needed.
- Multi-receiver support; SDRplay or other front ends.
- The public demo server (`BRIEF-public-demo-server.md`) — related, but a different product.

## 8. Acceptance criteria (draft)

1. Cold boot with no known network → `VibeServer` AP appears; joining it raises the captive page on
   iOS, Android and macOS.
2. First entry cannot be dismissed without setting an admin password.
3. Joining a home network from the page works, survives reboot, and the AP stands down while the
   box has a real network — and comes back if that network disappears.
4. The VibeServer half of the page is verifiably identical to the Mac app's web config page (same
   bundle, same schema) — a config file moved between a Mac and the Pi is accepted by both.
5. Power settings measurably change consumption on a battery bank; throttling does not glitch audio
   for connected clients.
6. Recovery from the boot partition restores access on a box whose password is unknown.
