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
2. The user connects and reaches the configuration page **by address, like a router's config page**
   — `http://vibeserver.local/` where mDNS resolves, with the raw IP always documented as the
   fallback.
3. **First entry FORCES setting an admin password.** That password is the credential for both the
   Pi and VibeServer (but see §5 — the Pi's OS credential must not be held by the server process).
4. From there the page offers full configuration of both the appliance and the receiver.

### ★ 2.0 The rule: the web page is the baseline, the app is a shortcut

**Every appliance must be fully setup-able and administrable from a browser alone**, by address, with
no VibeSDR installed anywhere. That is the path for desktop users, for anyone who has not installed
the app, and for anyone on a platform we do not ship to. It is the guarantee.

§2.1's app-assisted setup is a **convenience layered on top** — it must never become the only way in,
and no configuration option may exist solely in the app. If the two ever diverge, the browser page
is authoritative.

### ★ 2.1 Setup from the VibeSDR app (the shortcut)

A NO-CAPTIVE-PORTAL design does not have to mean typing IP addresses. Most of the machinery already
exists:

- the shim already advertises the `_vibesdr._tcp` service (Android NsdManager) **and** answers a
  hostname A record via `mdns_responder.cpp` (`vibesdr.local`), and
- the app already browses for it in `src/services/mdns.ts`, which already carries a TXT-record
  convention (`pin`).

So: an appliance that has never been configured advertises itself as **unconfigured** (a TXT flag
alongside the existing ones). Open VibeSDR on a phone joined to the same network — including the
Pi's own hotspot, where mDNS works fine because it is link-local — and the app offers:

> **Unconfigured VibeServer found — set it up?**

…walking the user through admin password and network choice in the native UI, with the browser page
remaining the full-featured surface for later administration. This is strictly better than a captive
portal: a real app, no cut-down webview, no OS probe-URL trickery, and it works identically whether
the box is on its own hotspot or already on the home network.

**Caveats to design for:**
- **First-claim security.** An unconfigured appliance advertising "set me up" can be claimed by
  anyone on the link. Fine on a home network or the box's own hotspot; not fine on shared/public
  Wi-Fi. Advertise setup mode ONLY while genuinely unconfigured, and consider a bounded window
  after boot or a physical confirmation before a claim is accepted. First claim wins, and once
  claimed the flag disappears.
- **iOS Local Network permission** is required to browse — preflight it with an explanation screen
  (same requirement as the macOS brief §3 notes for Bonjour).
- **`.local` is not universal** (varies by Android version, older Windows). The IP address route of
  step 2 must always work and must be printed in the docs and on the app's setup screen.

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

## 4. ★ No captive portal — and why (decision, 2026-07-22)

A captive portal was the original design and was **scrapped**. Recorded so it is not revived:

- The window a captive portal opens is **not a full browser**. It is a cut-down webview, most
  restrictive on iOS, where WebSockets and much JavaScript are unreliable or blocked outright and
  the OS may close the window at any moment. The SDR client is entirely WebSocket-driven, so it
  could never have been served there — the portal could only ever have been a stub that handed the
  user elsewhere.
- It also required answering the OS probe URLs (`captive.apple.com/hotspot-detect.html`,
  `connectivitycheck.gstatic.com/generate_204`, `msftconnecttest.com/connecttest.txt`) from the AP's
  DNS/HTTP responder — fiddly, per-OS, and fragile.

The replacement is §2: a plain address like a router, plus §2.1's mDNS-assisted setup from the app,
which delivers the "it just appears" experience the captive portal was reaching for, in a real
browser and a real app.

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

## 5.1 ★ SSH — OFF by default, opt-in, and hardened when on

Pi owners expect SSH; an appliance that ships with it open does not deserve their trust. Stuart's
call is to leave it off, and that is right — with one correction to the reasoning, because it
changes the mitigation.

Running an SSH *server* does not let the Pi reach out to anything. The real exposure is the reverse:
an authenticated user gets **port forwarding**, turning the appliance into a jump host into the rest
of the owner's LAN. That is a concrete capability we can simply remove.

- **Default OFF.** Also matches Raspberry Pi OS's own default since 2016, so it surprises nobody —
  and an appliance shipping with SSH on and a shared password is how appliances join botnets.
- **Opt-in toggle in Compute Hardware & Network** (§3) — OS-level, so it sits on the ISO side of the
  boundary, never in `VibeServerConfig`.
- **When enabled, forwarding stays off:** `AllowTcpForwarding no`, `PermitTunnel no`,
  `GatewayPorts no`. Keeps the shell people actually want; removes the jump-host capability.
- **Prefer key-only auth** (`PasswordAuthentication no` once a key is installed). A user-chosen
  password on a box that may face a network is the weak link, and §5 already avoids holding the OS
  credential in the server.
- Pi OS's existing convention — an empty `ssh` file on the boot partition enabling it at boot — pairs
  naturally with the §6 recovery route.

**Knock-on:** macOS brief §5 counts SSH/file editing as the third of "three editors". On the
appliance that third editor is OPTIONAL, so the browser page must be complete on its own — which is
exactly the baseline rule in §2.0.

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

1. Cold boot with no known network → `VibeServer` AP appears; joining it, `http://vibeserver.local/`
   loads the config page on iOS, Android and macOS, and the documented IP works on a host where
   `.local` does not resolve.
2. With VibeSDR installed and joined to that AP, the app discovers the appliance and offers to set
   it up; completing setup clears the unconfigured flag, and a second phone no longer sees the
   offer.
3. First entry cannot be dismissed without setting an admin password.
4. **Browser-only parity:** the entire first-boot setup and all later administration complete from a
   browser with no VibeSDR installed on any device.
5. Joining a home network from the page works, survives reboot, and the AP stands down while the
   box has a real network — and comes back if that network disappears.
6. The VibeServer half of the page is verifiably identical to the Mac app's web config page (same
   bundle, same schema) — a config file moved between a Mac and the Pi is accepted by both.
7. Power settings measurably change consumption on a battery bank; throttling does not glitch audio
   for connected clients.
8. Recovery from the boot partition restores access on a box whose password is unknown.
9. **SSH:** absent by default (port closed on a fresh image); enabling it from Compute Hardware
   grants a shell but REFUSES port forwarding — verified by an attempted `ssh -L` tunnel failing.
