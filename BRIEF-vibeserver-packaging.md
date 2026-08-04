# BRIEF: VibeServer — packaging it as a product

**Status:** specced 2026-08-04, not started. Stuart is leading; this brief is his plan written down.
**Premise:** the bones are all built. This is PACKAGING AND PRODUCTISING, not new DSP.
Stuart, 08-04: *"the vibeserver stuff is the big one… we have all the bones of it ready its time to
package it properly."*

The Pi 500 is being **wiped and treated as a fresh install**, so the whole path below gets proven
end to end on a machine with no history — which is the only way to know it works for a stranger.

---

## 0. THE TARGET, IN ONE PARAGRAPH

SSH into a fresh Pi. `sudo apt install vibeserver`. Type `vibeserver`. A short wizard asks four
things and exits. Open the URL it printed in a real browser. The page says *"VibeServer is not
configured — sign in with your admin password to set it up."* Sign in, pick one of two modes, fill
in a properly laid-out settings page, press **Save**. The server restarts, the page refreshes, and
you are on the VibeServer landing screen with **Start Session** and **Admin** — which work exactly
as they do today.

★★★ **Nothing beyond `apt` on the command line.** Installing may use the terminal. Configuring
must not.

---

## 1. ★★★ THE DIVISION OF LABOUR — THE TUI IS NOT A SETTINGS SCREEN ANY MORE

Stuart, 2026-08-04, on the TUI as it stands:
> *"TUI is literally just used to get the server running, I found it too unintuitive and not very
> easy to use. Bare minimum in here to get the server displaying in a real browser, then all the
> config is done in the browser with a proper visual page, properly laid out with clear easy to
> understand options and headers explaining options if needed."*

★★ **This overturns the "one config schema, THREE editors" rule** from `BRIEF-vibeserver-linux-deb.md`
§3 and `memory/vibeserver_pi_two_products.md`. The curses screen was specced as "the macOS menu bar
over SSH" and built that way — 26 fields, five sections, a scrolling list in an 80×24 window. It
is now **two editors**: the browser (everything) and the Mac menu bar (the Mac product).

★ **Delete the field list from `tui.cpp`, do not extend it.** Everything it currently edits — name,
place, country, locator, lat/lon, session limit, max-bw, max-fps, lock-rate, idle saver,
uncompressed, freq, mode, rate, gain, no-web — moves to the browser. The wizard keeps only what is
needed to make a browser reachable and safe.

### What the wizard asks — and nothing else
Three steps, one screen each, no scrolling, `Enter` to advance:

1. **Radio.** Detected, shown for confirmation. If none is found, say so plainly and say what to do
   (plug it in, then press `r` to re-scan) — do not proceed to a server that cannot receive.
2. **Admin password — MANDATORY.** Typed twice. This is the credential that unlocks the browser
   setup page, so there is no way to skip it: a blank one would leave the setup page open to
   anyone who can reach the box. ★ This is a **change from today**, where blank = nothing protected.
3. **PIN — optional.** Who may connect at all. Blank = anyone who can reach this machine.

★★ **The server's NAME is deliberately not asked here.** It was, in the first draft of this brief.
But §1a puts mDNS advertising behind setup, so nothing needs a name until the browser page — and the
URL the wizard prints is an **IP address**, which works with no name at all. Asking for it twice, or
asking here and editing there, is the two-editors drift this brief exists to kill. **The browser owns
the name.**

Then: **write the config, restart the service, print the URL, exit.**

★★★ **THE FIRST CONNECTION IS BY IP ADDRESS, ALWAYS** (Stuart, 2026-08-04). No `.local`, no
discovery, no name — mDNS is not running yet (§1a) and the name does not exist yet (§3.1b). The
wizard prints `http://192.168.x.x:PORT/` and that is the only address offered anywhere until Save.
★ So the ordering is self-consistent rather than a limitation: **IP gets you to setup; setup is what
creates the friendly name and turns discovery on.**

★★ The URL must be the one that actually works from another machine — `hostname -I`, the real
configured port, and never a port we cannot attribute to ourselves. The existing `listenPort()`
comment in `tui.cpp:167` records why (a bare `ss` scan once told the user to point a browser at
their own SSH daemon). **Keep that function; delete almost everything around it.**

★ Running `vibeserver` again after configuration: it should NOT re-run the wizard. Show the status
block (running / radio / URL / since) and a line saying settings live in the browser, at that URL —
plus the recovery actions below. That is the whole screen.

### 1a. ★★★ mDNS STAYS OFF UNTIL SETUP IS DONE — SO THE APP NEVER MEETS AN UNCONFIGURED SERVER
Stuart, 2026-08-04:
> *"we simply only enable mDNS AFTER initial setup. A user would have to manually add the IP to the
> app, which — after seeing the warning in the TUI advising the server cannot be set up with the app
> at this time — is on them."*

★★ **This removes the problem rather than papering over it.** An unconfigured server does not
advertise, so it cannot be discovered, so it cannot appear in the app's server list. The only way to
reach one from the app is to type its IP in by hand, having just been told in the TUI not to. That
is a deliberate act against a clear instruction, and it is a fair place to stop engineering.

★ **Discovery is therefore the signal that a server is ready**, which is a stronger guarantee than a
message explaining that it is not.

### ★ THE APP-SIDE MESSAGE IS NOW A NICETY, NOT A REQUIREMENT
An earlier draft of this brief made it required. With mDNS gated it is not — the residual cases are
narrow (a hand-typed IP; a previously-configured server that has been reset from the recovery
console and is still in someone's saved list). Keep the "not set up yet — open this in a browser"
message if it is cheap, but it is no longer what stands between us and a broken-looking app.

### ★★ THE TUI IS ALSO THE RECOVERY CONSOLE
Stuart, 2026-08-04:
> *"the TUI needs to remain a backup to reset the admin password etc if required as that can only be
> accessed on the server itself or via SSH anyway."*

★★★ **This is the lock-out escape hatch, and it is the one thing the browser cannot be.** The
browser setup page is gated on the admin password; forget it and there is no way in — on a headless
box in a loft that is a reinstall. So the second screen keeps exactly these:

- **Reset the admin password.** Set a new one and restart. No old password required.
- **Reset / clear the PIN.**
- **Reset to unconfigured** — clears `configured`, so the next browser visit is the setup page again.
- Restart the service, and the status block.

★★ **And the security argument is sound, not a compromise:** reaching this screen already requires a
shell on the machine — physical access or SSH. Anyone who has that can read `config.json`, stop the
service or reinstall the package anyway, so **SSH access is a strictly stronger credential than the
admin password**. Demanding the forgotten password here would protect nothing and lock out only the
legitimate owner.

★ It stays a **recovery** console, not a settings screen. Credentials and reset only — the moment a
range, a mode or a demodulator list appears here we are back to two editors of the same settings and
the drift that follows. If it can be done in the browser, it is not on this screen.

★★ **And realistically it is a LAST RESORT after first setup** (Stuart) — so budget the effort
accordingly. Plain, obvious and correct beats polished: four labelled actions, confirm before each,
say what happened. **The design effort goes into the browser page**, which is what an owner actually
uses. Do not let this screen grow back into the 26-field list it is replacing.

---

## 2. ★★★ THE FOUNDATION: CONFIG BECOMES A FILE THE SERVER WRITES

This is the biggest change and everything else sits on it.

**Today:** `/etc/vibeserver/vibeserver.conf` is a systemd `EnvironmentFile` holding one line —
`VIBESERVER_ARGS="--flags…"`. The TUI composes that string (`buildArgs()`) and re-parses it
(`loadConf()`). It is a command line pretending to be a config file.

**Required:** a *browser page* saves settings. That means structured config the server reads at
boot and **rewrites on Save**, with a schema rich enough for a checkbox list of blocked
demodulators — which does not survive a flag string.

- **`/etc/vibeserver/config.json`** — the single source of truth. Owned by the service user,
  written atomically (temp file + rename; the existing `saveConf()` already uses that discipline
  and the reason is in the comment at `tui.cpp:146`).
- **CLI flags become an override**, not the storage. They stay for scripts and development —
  `BRIEF-vibeserver-linux-deb.md` §2 already says the flags keep working and are just no longer
  the front door. Precedence: **flags > config.json > defaults.**
- **`vibeserver.conf` stays** as the systemd EnvironmentFile for genuine boot-time arguments, but
  the normal state is empty.
- ★ **A config the server writes must survive a package upgrade.** `config.json` is NOT a dpkg
  conffile — it is state. Keep it out of the package payload and have `postinst` create it only if
  absent.

### The `configured` flag is NOT `adminSet`
Today `GET /` returns `{"admin": true|false}` and the whole model is *"no admin password ⇒ nothing
is protected"* (`local_sdr_shim.cpp:795`). The new model needs a **separate** boolean:

- `adminSet` — is there a password. After the wizard this is **always true**.
- `configured` — has the owner been through the browser setup and pressed Save.

`configured: false` is what drives the landing page in §3. ★ Both must be published on the identity
response, and **older app/Mac/watch clients must tolerate the new field** — they read this endpoint
(`local_sdr_shim.cpp:3403` records that positive identity replaced landing-page sniffing, so the
shape of this response is load-bearing for every client we ship).

---

## 3. THE BROWSER: UNCONFIGURED LANDING → SETUP → RUNNING

### 3.1 First visit, unconfigured
A page that says **"VibeServer is not configured. Use your admin password to set it up now."** and
one password field. Nothing else — no Start Session, no spectrum. The server has a radio and a
password but no policy, and until the owner sets one it should not be serving strangers.

Auth reuses what exists: the nonce/HMAC challenge-response behind `admin_unlock`
(`local_sdr_shim.cpp:2765`) and `g_vsAuthState.verify`. **No new credential mechanism.**

### 3.1a ★★ SETUP IS BROWSER-ONLY IN THIS BUILD — SAY SO, IN BOTH PLACES
Stuart, 2026-08-04:
> *"right now the apps won't support setting up a vibeserver that is in first use mode… if a user is
> SSH'ing into a Linux build to run the TUI chances are they will have a browser anyway. We simply
> put a line in the TUI saying please use a browser to continue setup, in-app setup from the VibeSDR
> app coming soon."*

The reasoning holds: anyone with an SSH client has a browser. So **the app does not get a setup flow
in this release** — but it must not pretend the gap does not exist.

- **In the TUI**, on the final wizard screen next to the URL: *"Open this address in a browser to
  finish setup. Setup from the VibeSDR app is coming soon."* One line, where the user already is.
- ★★ **In the APP, and this is the part that is easy to miss** — the wizard is not the only way to
  meet an unconfigured server. A saved server, a directory entry or a friend's box can all be
  `configured: false`, and the app reaches them without ever seeing the TUI. It must read the flag
  and say **"This server has not been set up yet — open `http://…` in a browser to finish setup"**,
  with the address shown and tappable. It must NOT present a connection failure, an empty spectrum
  or a PIN prompt: a server that is working exactly as designed must never look broken.
- ★ That app-side message is the ONLY app work this release requires for first-use, and it is small.
  Skipping it is what turns "coming soon" into "the app is broken against the new server."

### 3.1b ★★ THE FIRST QUESTIONS ON THE SETUP PAGE: DISCOVERY AND NAME
Before the mode choice, because they decide how anyone reaches this box at all. Stuart's wording:

> **Do you want to allow advertising of VibeServer to local network clients?**  ( ● Yes  ○ No )
> **Give your server a friendly name to identify it.**  `VibeServer: Pi500`
> *You can reach your server at* **`vibeserver-pi500.local`**

★★★ **Showing the converted `.local` name live, as they type, is the whole point** — Stuart: *"so no
surprises."* The user types a display name with capitals, spaces and punctuation; mDNS gets a
lowercased, hyphenated, ASCII-only label. If we convert silently, the address that actually works is
one the owner has never seen, and they will type the pretty one and conclude discovery is broken.
**Derive and display it in the same place, from the same rule.**

★★ **Derive it ONCE, at the server, and publish the result** — do not reimplement the conversion in
the page and again in the responder. `memory/wire_value_derived_both_ends.md` is exactly this bug:
a value derived independently at two ends drifts, and the drift is silent.

### ★★★ SHOW THE NAME CURRENTLY HELD, NOT THE NAME REQUESTED
Stuart, 2026-08-04: *"the mDNS shows the name it has currently obtained, so that a user knows exactly
which mDNS address is their server."*

★★ **This is a requirement, not an edge case.** The requested name and the held name differ more
often than they look:
- **Collisions.** Two Pis both called "VibeServer" cannot both hold `vibeserver.local`; the responder
  suffixes the loser (`vibeserver-2.local`). Two VibeServers on one network is not exotic — it is a
  Pi 500 in the shack and a Zero 2 W up the tree, which is our own roadmap.
- **Conversion.** Capitals, spaces and punctuation do not survive into an mDNS label.
- **Timing.** The name is not *held* until the responder has probed and won it, which is after Save.

★ So: display the name **read back from the responder once it has settled**, refresh it if it changes
underneath us, and never render a locally-computed guess as though it were fact. A user who is told
`vibeserver.local` and reaches someone else's Pi has been actively misled — worse than not being told.
★ Until the responder reports a held name, show the **IP address**, which is always true.

★ **"No" must mean no.** Choosing not to advertise leaves the server reachable by IP, and the page
should say so and show that address, so the choice does not read as "turn my server off".

★ Precedent: the Android app already offers this friendly-name entry, so the concept and the wording
should match it rather than invent a parallel vocabulary. `mdns_responder.cpp` already has the
`__linux__` path.

### 3.1c ★★ WHEN THE SERVER IS FULL: A COUNTDOWN AND A QUEUE POSITION — BOTH MODES
Stuart, 2026-08-04:
> *"for the landing page, and it applies for both modes: if time limits have been set and a client
> lands and all radios (single user per radio) or slots (multiple users) are taken, then a countdown
> to next free slot should show, and if anybody is ahead of them in the queue."*

★★★ **A refusal with no information is the worst screen we can show.** "Server busy" gives a user
nothing to decide with, so they hammer reconnect — which is also the behaviour our own cooldown then
punishes. *"Free in 4:12, you are 2nd in the queue"* turns a dead end into a wait, and a wait is
something a person will actually sit through.

★ **The data already exists** — `occupantSince` and the session limit are both tracked in
`local_sdr_shim.cpp`, and the warning bits (`occupantWarned`) prove we already compute time
remaining. Nothing new needs measuring; it needs publishing.

- **Only meaningful when a time limit is set.** With no limit there is no honest countdown — say
  "in use, no fixed session limit" rather than invent a number. ★ A countdown we cannot honour is
  worse than none: it will be wrong, and the user will be watching it.
- **Mode A (one user per radio):** the countdown is to that one occupant's expiry.
- **Mode B (multi-user):** to the earliest-expiring occupied slot.
- **Queue position** means the server must actually hold a queue — arrival order, with entries
  expiring if the waiter leaves. ★ And if we show a position we must honour it: handing the freed
  slot to whoever reconnects fastest, while showing someone "1st in the queue", is a lie the user
  can see. **This is the real work in this item** — the countdown is arithmetic, the queue is state.
- ★ Precedent worth reading first: **Kiwi publishes exactly this and we currently discard it**
  (`memory/backend_limits_probed.md`). We would be shipping what we already fail to consume.
### ★★★ THE POST-EXPIRY GRACE PERIOD STAYS — DO NOT LOSE IT TO THE QUEUE
Stuart, 2026-08-04:
> *"we must keep our grace period for the IP after the time limit expires so that the same user
> cannot immediately rejoin, hogging the slot — we have that already in the Mac and Android apps."*

✅ **It exists and is not going anywhere:** `cooldownUntil` in `local_sdr_shim.cpp` — an
address → expiry map, checked in `acceptWs` **before** occupancy, with the reason already recorded
in the source: *"Someone serving a cooldown must be refused even when the radio is FREE; that is the
entire point of it. Checking occupancy first would let them straight back in the instant their own
timeout freed the slot."* Loopback is exempt. **Keep that ordering.**

★★ **But the queue and the cooldown must be designed together or they contradict each other.** The
failure is easy to write and obvious to a user: a waiter is shown *"1st in the queue"*, the slot
frees, they reconnect — and are refused for cooldown, or beaten to it by someone who never queued.
Two rules keep them honest:
- **A user serving a cooldown is not queued**, and is shown their cooldown, not a queue position.
- **The freed slot goes to the head of the queue**, not to the fastest reconnect. If we show a
  position we have promised an order, and a promise the server does not keep is worse than no
  information at all.

★ Related: `memory/kiwi_reconnect_etiquette.md` and `memory/third_party_receiver_etiquette.md` —
we already hold ourselves to fairness-vs-liveness rules on other people's receivers; this is the
same question on our own.

### 3.2 The mode choice
Two cards, chosen once (changeable later from Admin):

**A — One user per radio.** The existing personal-receiver behaviour, named: one listener at a time,
with the full settings surface the Mac and Android apps have today.

★★ **The browser page still configures all of it.** Stuart, 2026-08-04: *"browser page configures
everything, as having it visually is much easier — that is why the Android setup works so well,
people love how easy it is."* So Mode A is **not** "the server sets nothing and the app does it all";
it is the same visual settings page, showing the Mode A surface instead of the Mode B one. An owner
who picks Mode A gets a page, not a shrug.
★ The connecting client can still change what it always could at runtime — this is about where the
**server's** defaults and policy are set, and the answer is the same page for both modes.

**B — Multi-user, locked range.** The owner sets the policy; listeners get what they are given.
This is `g_vsLockedCentre > 0` today — every gate in the shim already keys off it — promoted to an
explicit, named mode rather than an emergent consequence of one setting.

### 3.3 The Mode B settings page
Properly laid out, grouped, with a short explanatory line under any heading that needs one. In
order:

| Group | Settings |
|---|---|
| **Range** | Centre frequency · Sample rate (⇒ shows the resulting coverage, e.g. "2.5 – 10.5 MHz") |
| **New listeners** | Landing frequency · Landing mode |
| **Available modes** | Demodulator + decoder checklist — see §4 |
| **Listeners** | How many users · Time limit per session |
| **Hardware** | Gain / AGC / bias-T / direct sampling / calibration — **fully locked to clients in this mode**, so they are set here or nowhere |
| **Other** | Uncompressed audio · idle saver · max frame rate · the rest of the Mode A server settings that still apply |

★★ **The hardware group must branch on `radio.driver`** — AGENTS.md: *"a control that only works on
one radio should not be there."* RTL has a gain list, the Airspy HF+ has no variable gain at all
(attenuator + preamp), the SDRplay RSP uses IF gain reduction. Draw the right control per driver
(as `LocalHardwarePanel` does) or draw none. Do not render a dead one.

A **Save** button at the bottom → write `config.json` → restart the service → the page reconnects
and lands on the normal VibeServer landing screen with **Start Session** and **Admin**, which
behave as they do now.

---

## 4. ★★ THE BLOCKED-DEMODULATOR LIST IS THREE JOBS, NOT A CHECKBOX

Every demodulator and decoder is listed, **all ticked by default**; the owner unticks what to block.
The two motivating cases are Stuart's:

- **No point in WFM and Advanced RDS on an HF server** — irrelevant clutter.
- **Prevents a client accidentally selecting WFM stereo, which is CPU-heavy** — on a 20-listener Pi
  one careless click is a real cost.

The three jobs:

1. **Publish** the permitted set as a field on the identity/config response.
2. **Enforce** it server-side. A client can always send `mode=wfm` regardless of what the UI drew —
   an unenforced list is decoration. Refuse and say why.
3. **Hide** the blocked entries in every client that reads the field — app, web client, watch.

★★★ Job 3 is the one that gets skipped, and skipping it is the AGENTS.md failure mode exactly: a
mode that is visible and refused reads as *"the feature is broken"*, not *"the owner blocked it"*.
Same rule as `FmdxSettings`: never offer a control whose every use is a no-op.

★ Note the coupling: **Advanced RDS depends on WFM.** Unticking WFM must untick and disable
Advanced RDS, not leave an orphan that can never fire.

---

## 5. ★★★ THE BLOCKER — FIX BEFORE SHIPPING THE UI THAT SELLS IT

**`BUG-vibeserver-broadcast-blocks.md`** — blocking sends under a **global mutex on the DSP
thread**, so **one slow listener freezes every other one**. Meanwhile `--users 20` is advertised.

Mode B's headline is multi-user. Putting a **"How many users"** field on a settings page is us
selling the exact capability this bug breaks — and unlike the demo, a shipped product gets pointed
at listeners on bad connections, which is precisely the trigger. ★★ Multi-listener fan-out is
**built but unproven**: a probe has never joined as a second listener.

**Fix it as step one**, then prove it with two real listeners, one deliberately throttled.

---

## 6. `apt install vibeserver` MEANS AN APT REPOSITORY

`sudo apt install vibeserver` with no path implies a repo, not a downloaded file.

- ✅ **The `.deb` already builds** — CPack `DEB` generator is wired in `vibeserver/CMakeLists.txt:253`,
  with `postinst`/`prerm`/`postrm`, a systemd unit, a udev rule and `shlibdeps`.
- ❌ **`apt.vibesdr.net` does not exist.** A Debian repo is just static files (`Packages`, `Release`,
  `InRelease`, the `.deb`s) plus a GPG signing key — and the website already deploys to Cloudflare
  via `npx wrangler deploy`, so it is the same mechanism serving different files.
- ★★★ **The payoff is `sudo apt upgrade`.** For a box in a loft that is the difference between
  updates happening and not.
- ★ Build **natively on the Pi**. Cross-compiling is where the days go.

---

## 7. ORDER OF WORK

1. **Fix the broadcast blocker** (§5) and prove it with two listeners.
2. **`config.json` + read/write + flag precedence + `configured`** (§2). Nothing else can start.
3. **Strip the TUI to the wizard** (§1).
4. **Unconfigured landing page + admin sign-in** (§3.1).
5. **Mode choice + the Mode B settings page** (§3.2–3.3).
6. **Blocked demodulators — publish, enforce, hide** (§4).
6b. **App: the "not set up yet" message** for `configured: false` servers (§3.1a). Small, but it is
   the difference between "setup is browser-only for now" and "the app broke against the new server."
7. **Build the `.deb` on the fresh Pi; stand up `apt.vibesdr.net`** (§6).
8. **Wipe the Pi and walk the whole path as a stranger** — `apt install` → wizard → browser → Save
   → listen. Anything that needs explaining on that walk is a bug in this brief, not in the user.

★ Step 8 is the acceptance test. It is also the only honest one.

---

## 8. ★★★ THE DESIGN STANDARD: THE ANDROID SETUP IS THE BAR

Stuart, 2026-08-04, on why everything goes in the browser:
> *"having it visually is much easier — that is why the Android setup works so well, people love how
> easy it is."*

★★ **So the reference implementation is not a config file with a skin on it — it is the Android
setup flow.** Before designing any screen here, go and look at what Android actually does and why it
lands: grouped, visual, plain words, sensible defaults already filled in, and no concept the user has
to hold in their head from one screen to the next. Match that, and match its vocabulary.

★ This is also the standard the current TUI failed — it exposed every CLI flag as a labelled row and
called that a settings screen. **A list of every option is not a design.** The browser page must
group, explain and default; a heading with one clear sentence under it beats six perfectly-labelled
fields.

## 8b. OPEN QUESTIONS

1. ✅ **RESOLVED 08-04 — Mode A gets the full browser page too.** See §3.2.
2. **When is the Pi wiped?** After the wipe there is no Linux target to test the current binary
   against, so the `.deb` for step 7 should ideally be built before or immediately as part of it.

---

## 8a. ★★★ THE ENDGAME — NOT THIS RELEASE, BUT DO NOT BUILD AGAINST IT

Stuart, 2026-08-04:
> *"at some point we will have to figure out how to make the server fully configurable from the app
> with no SSH, no TUI etc — so a user could download a Pi ISO, plug a radio into it and power it up,
> a captive wifi is displayed, they connect their phone to it, and then in VibeSDR the full setup is
> there including admin password setup. But that is for another day when we ship a full VibeServer
> ISO for our Pi Zero 2 W in a tree setup."*

**Explicitly out of scope.** It belongs with `BRIEF-vibeserver-pi-iso.md`, and it needs things this
release does not (a captive-portal AP mode, Wi-Fi provisioning, an app-side setup flow). ★ The
Zero 2 W up a tree is also the case that makes it *necessary* rather than nice: there is no keyboard,
no screen and no SSH-able network until the box has been told which network to join.

### ★★ THE ONE DECISION TODAY THAT DECIDES WHETHER THAT IS EASY OR A REWRITE

**Make the browser setup page a CLIENT of a config API — do not special-case it.**

The page should `GET` the current config and `POST` the new one over a documented endpoint, behind
the existing admin nonce/HMAC auth, with the server validating and persisting. The HTML is then one
consumer of that API, and the app later becomes a second one **with no server-side work at all.**

★★★ The failure mode to avoid is the obvious shortcut: form handling wired straight into the request
router, validation living in the page's JavaScript, `config.json` written from inside the HTML
handler. That ships identically today and makes app-side setup a from-scratch rebuild of every rule —
and, worse, a *second* implementation of the validation, which is the two-editors drift this brief
already exists to kill, moved from settings to setup.

★ It costs nothing now. The page has to talk to the server either way; this only fixes *how*.
★ Corollary: **every question in the wizard must also be answerable over that API** — admin password
included. The TUI stays the recovery console for a box you can reach, but it must never be the only
place a given setting can be set, or the ISO story is blocked on it.

## 9. THINGS THIS BRIEF DELIBERATELY DOES NOT DO
- No new DSP. The channelizer, zoom spectrum, per-client DSP and idle-grace are all built and
  measured — see `BRIEF-per-client-dsp-and-zoom-fft.md` and the memory notes.
- No ISO appliance, no captive portal, no app-side setup flow — see §8a for the shape and for the
  one decision here that keeps it cheap. `BRIEF-vibeserver-pi-iso.md` builds on this brief; it does
  not belong in it.
- No change to the Mac menu-bar app, which stays its own front end.

## 10. WHEN THIS LANDS, FIX THE COPY THAT SAYS WHERE THINGS ARE
Per AGENTS.md — this moves every server control from a terminal to a browser, so the grep list
applies in full: `AboutOverlay.tsx`, `sdrTour`, `pickerTour`, the watch tutorial sheets,
`website/index.html`, plus `vibeserver/linux/INSTALL.md` and `README.md`. **Verify the whole
sentence, not the part you came to fix.**
