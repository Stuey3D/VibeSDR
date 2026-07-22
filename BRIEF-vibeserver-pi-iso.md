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

## 5.2 ★ Updates — APT, and NO update server to run

The concern: a desktop app can be updated by hand, but an ISO cannot, and we do not want to operate
update infrastructure. Stuart's instinct — get into the APT system so `sudo apt update && sudo apt
upgrade` does it for us — is the right answer, and it is cheaper than it sounds.

**A Debian repository is STATIC FILES.** `dists/`, `pool/`, a `Packages.gz` index and a GPG-signed
`Release`. No daemon, no database, no runtime cost. Host it on the same Cloudflare account already
serving the website (Pages or R2) — which keeps the "no recurring costs while this is a hobby" rule
intact. `apt` then works because it IS apt, not an imitation of it.

**The ISO stops being an update channel.** The image is only the initial install; after first boot
the appliance is Raspberry Pi OS plus our package, so APT updates both the OS and VibeServer. The
image is re-cut occasionally for NEW installs, not to deliver fixes to existing ones.

**Where to host it — GitHub Pages is fine, and free.** ★ Note the two senses of "repo": a GitHub
repo is a GIT repository; APT needs a DEBIAN repository, a specific file tree served over HTTP. You
cannot point apt at a git repo. But because that tree is static, GitHub Pages serves it happily —
which is precisely how Docker, VS Code and others distribute. Bake into the image:

```
# /etc/apt/sources.list.d/vibeserver.list
deb https://<org>.github.io/vibeserver-apt stable main
```

plus our public key in `/etc/apt/keyrings/`. Cloudflare Pages/R2 works identically; pick whichever
is less friction. ★ **GitHub *Releases* is NOT an apt repo** — attached `.deb` files have no index,
so apt cannot see them. It must be a generated tree.

**What it costs:**
- Build a `.deb` from the CMake output (`nfpm` or `dpkg-deb`); the binary is already self-contained.
- **Hold a signing key, and keep holding it.** The one genuine ongoing responsibility, and the part
  that hurts if it is lost — plan its storage and its rotation story before the first release.
- Ship the image with our repo key and a `sources.list.d` entry already installed.

**Also:**
- `unattended-upgrades` for security patches, so a field box nobody logs into still gets OS fixes.
- A field appliance on its own hotspot has NO internet and simply will not update. Fine — but
  Compute Hardware & Network must show the installed version and when it last checked, and offer
  "update now" when a network is present, rather than silently doing nothing.

### 5.2.1 Updates in the web config GUI (Stuart, 2026-07-22)

Compute Hardware & Network gains an **Updates** section covering the whole box — OS packages and
VibeServer together, since apt does not distinguish:

- **Policy:** off · security only · everything.
- **Schedule:** daily / weekly / monthly, with a chosen time of day.
- **Client warning:** because the server knows an update is due, connected clients are told
  *"the server is carrying out an update, your connection may drop"* rather than simply losing audio.

**Implementation notes that shape the design:**

- **Drive `unattended-upgrades`; do not reimplement apt.** The three policy levels map onto its
  `Allowed-Origins` (security-only vs everything) and the schedule onto a systemd timer. We WRITE
  ITS CONFIG. Anything we run ourselves dies when the service restarts (below).
- ★ **We cannot narrate our own restart.** When the update contains VibeServer, its `postinst`
  restarts the service — killing the process that is showing progress and warning clients. So the
  run must be owned by something that outlives us (the timer), and our part is: ANNOUNCE BEFORE,
  and DETECT AFTER (a "was I just restarted by an upgrade?" marker on start, to log or confirm).
  Clients reconnect on their own; they already do.
- ★ **The warning is a PROTOCOL message**, not a web-page banner — the phone, the watch and the web
  client must all render it. That makes it a `files/BRIEF-vibeserver-protocol-foundations.md` item
  (a server `notice` to all clients), not something added here.
- **Do not defer indefinitely for listeners.** "Never update while someone is connected" means a
  busy server never updates. Warn, allow a grace period, then proceed.
- **Reboots need their own consent.** Kernel/firmware updates require one, and an unannounced reboot
  of a remote appliance is far worse than a service blip. Detect `/var/run/reboot-required`, surface
  "reboot needed" in the GUI, and keep automatic reboot OPT-IN with its own scheduled time.
- **A failed upgrade must not brick it.** apt does not roll back. At minimum the §6 recovery route
  covers it; consider pinning a known-good version so a bad build can be stepped back to.

### 5.2.2 The update sequence, and why not blue/green

**The sequence:**

1. An update is available → **download it**. Nothing is blocked and nobody notices; apt fetches
   everything before it configures anything.
2. **Clients connected?** Send the 5-minute warning notice (§5.2.1). **None connected?** Proceed
   immediately — there is nobody to inconvenience.
3. Install and restart. Downtime is **seconds**, not minutes: a systemd restart plus reopening USB
   and starting the DSP, measured at roughly three seconds on the Mac build.
4. Clients reconnect on their own.

★ **Do NOT block new connections during the download.** Downloading disrupts nothing; only the
install does. Refusing connections for the whole download costs availability for no benefit. Refuse
(with "update in progress") only across the install itself, if at all — it is seconds.

**Blue/green was considered and rejected: ONE DONGLE, ONE OWNER.** The idea of holding the new
version, starting it, letting it take over and then cleaning up the old one cannot work here —
libusb claims the RTL-SDR exclusively (proven the hard way: `rtl_tcp` holding the device produced
`usb_claim_interface error -3`), and the listen port is likewise exclusive. The old process must
FULLY RELEASE the radio before the new one can open it, so the handover is still a gap, and
hand-rolling A/B deployment against a single receiver buys nothing that step 3 does not already give.

★ **But steal blue/green's ONE genuine benefit: rollback.** Its real value was never the handover,
it was that the old version still exists if the new one will not start — precisely the gap flagged
above, since apt has no rollback. Cheap 80% version: **keep the previous `.deb`**, health-check the
new service after restart, and if it fails to come up within ~60s, reinstall the old package
automatically and report it prominently in the GUI. A failed update must never cost the owner their
receiver.

### 5.2.3 Supervision and handover — the goals are right, the mechanism should differ

Two refinements were proposed: (a) the OLD process stays alive, verifies the NEW one truly took
over, then cleans itself up — or reasserts itself and leaves an error in the admin section; and
(b) OLD releases the RADIO first but keeps serving the web side, so arrivals see "an update is
happening", handing over the ports once NEW is confirmed good.

Both GOALS are right and are kept: **arrivals must not meet a dead server**, and **a failed update
must self-heal**. The mechanisms below achieve them with far less machinery.

**Why not "OLD supervises NEW":** dpkg replaces the binary in place and `postinst` restarts the
service, so systemd kills OLD as part of the upgrade — it does not get to adjudicate. Keeping it
alive means preventing our own package from restarting the service and moving orchestration
elsewhere. And "OLD reasserts itself" needs the old BINARY, which has just been overwritten — so a
copy must be kept regardless. At that point the kept copy (the previous `.deb`, §5.2.2) is doing the
real work and the surviving process adds nothing.

★ **The thing that outlives the swap should be the UPDATE SCRIPT owned by the systemd timer.** It
survives our restart by design — that is the whole point of §5.2.1's "we cannot narrate our own
restart" — so it is the natural owner of the post-restart health check and of the rollback.

★ **Use systemd SOCKET ACTIVATION instead of a port handover.** systemd owns the listening socket
and passes it to whichever process is running, so during the swap incoming connections QUEUE IN THE
KERNEL rather than being refused. Someone arriving mid-update simply waits the few seconds and is
then served normally — no error, no custom "updating" page, and no two-process port negotiation to
write or debug. It is strictly better than the proposed handover, for less code.

The only thing socket activation does not provide is an explanatory page during the gap. For a
~3-second window, where connected clients already had a 5-minute warning (§5.2.1) and reconnect
automatically, that is not worth a bespoke handover protocol. If the gap ever grows (a slow Pi, a
large migration), revisit with a tiny fallback unit serving a static "updating" page on the same
socket — still far simpler than two live servers negotiating.

**Rejected:** a bespoke self-updater (needs its own hosting and signing anyway, and re-invents what
apt already does well) and image-based A/B updaters such as RAUC/Mender (robust, but far too heavy
for this).

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
9. **Updates:** `sudo apt update && sudo apt upgrade` on a running appliance fetches and installs a
   newer VibeServer from the static repo, verifies its signature, and restarts the service without
   losing configuration. An appliance with no internet reports its version and last-checked date
   rather than failing.
10. **Update policy:** setting security-only vs everything, and a weekly 03:00 schedule, is
    reflected in the system's own `unattended-upgrades`/timer config — verified by inspecting it, not
    just our UI. Connected clients receive the update notice before the service restarts, and
    reconnect afterwards without user action.
11. **Update resilience:** a deliberately broken package fails its health check and the previous
    version is reinstalled automatically, with the failure reported in the admin section. A client
    connecting during the restart window is held and then served, rather than refused.
12. **SSH:** absent by default (port closed on a fresh image); enabling it from Compute Hardware
   grants a shell but REFUSES port forwarding — verified by an attempted `ssh -L` tunnel failing.
