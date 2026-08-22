# BRIEF — the tunnel switch and vibeserver.vibesdr.net

**Status:** design, 2026-08-22. Nothing built.
**Supersedes nothing — it COMPLETES [BRIEF-public-directory.md](BRIEF-public-directory.md).**
That brief designed the directory for a **port-forwarded** server and is still correct for one.
This is the half it explicitly put out of scope: *"KiwiSDR also offers a proxy for people who
cannot port forward at all (the CGNAT answer)… out of scope for v1."*

**Origin:** Stuart, 2026-08-22 — *"I want an android phone on the website as a demo"*, and then the
larger point: *"we now have people using VibeServer and this will be an ideal way of getting them
online and into the directory."*

---

## ★★★ WHY THE TUNNEL EXISTS: "discovery only, data direct" CANNOT WORK FOR A PHONE

The agreed shape was a tunnel for **discovery** with the **data handled directly**, to stay off the
Cloudflare free limits. That is the right instinct and it is already the design in
`BRIEF-public-directory.md` — but it assumes a listener can *reach* the server once the directory
has told them where it is.

**A phone on mobile data is behind CGNAT. There is no direct address to hand out and no port to
forward.** For that server the tunnel either carries the audio or it carries nothing.

So the two paths are not alternatives, they are the two flavours of listing that brief already
names, and the address is what differs:

| | address | data path | who it is |
|---|---|---|---|
| **Permanent** | operator's DDNS + forwarded port | **direct** — never touches us | the loft Pi, the Mac mini |
| **Tunnelled** | `https://<id>.trycloudflare.com` | through Cloudflare | **the phone, and anyone behind CGNAT** |

★★ **The free-limit worry was aimed at the wrong resource.** Checked 2026-08-22: Tunnels are free at
**any traffic volume**; the caps are **1000 tunnels per account** and 100 edge connections per
tunnel, and there is **no "disproportionate non-HTML content" clause** in the current self-serve
terms. Bandwidth is not the thing to design around. **Whose account the tunnels live on is.**

---

## ★★★ THE DECISION: QUICK TUNNELS — NOBODY'S ACCOUNT

Stuart, 2026-08-22. `cloudflared` is run with **no login at all** and Cloudflare hands back a random
`https://<words>.trycloudflare.com`.

**Why this and not named tunnels on our account:**

- ★★★ **The 1000-tunnel cap never applies to us**, because the tunnels are not ours. Named tunnels
  would make our account the ceiling on how many VibeServers can ever be listed.
- ★★★ **We do not become the transit provider.** With named tunnels, every listener's audio crosses
  our Cloudflare account and we own the moderation and terms exposure for whatever anyone streams.
  The brief already flags moderation as *"the hidden cost"*; this keeps it from also being a
  liability.
- ★ **No recurring cost, and nothing to administer.** Consistent with the project rule of no
  recurring costs.

**★★★ THE UGLY HOSTNAME IS A NON-ISSUE, AND STUART'S ANSWER IS THE WHOLE REASON WHY.**

> *"we simply add a box for a Public Server name… it is called something like Dave's VibeServer,
> Manchester, GB. The address doesn't matter at that point as you won't be typing it directly as
> the directory does the hard work."*

★★★ **THE ADDRESS IS A FIELD, NOT AN IDENTITY.** Nobody reads `brave-tomato-lemon-fox.trycloudflare.com`
because nobody ever types it: the directory and the picker carry **the operator's chosen name and
their location**, and the URL travels underneath as data. This is what makes a rotating hostname
survivable rather than fatal, and it is already how `BRIEF-public-directory.md` is built — the
listing is keyed on a **server-issued id + key**, and re-registers.

★★★ **AND IT IS WHY ONE EXISTING RULE MAY NEVER BE RELAXED.** [[client_infers_server_decisions]]:
bookmarks key on the server's `instance` id, **never the URL**. That was learned when one server
reached two ways became two servers. Here it is sharper — **a tunnelled server bookmarked by URL
breaks on every single restart.** Any new code that stores a tunnelled address as identity is a bug
at the moment it is written.

**What we actually give up:**

- ★★ Cloudflare calls Quick Tunnels non-production and rate-limits them. **This is the risk spike 2
  must MEASURE**, not assume.
- ★ A restarted tunnel means the directory is briefly advertising a dead address. Handled, twice
  over: the server re-registers with its new URL, and the picker probes before offering it.

★ **Named tunnels stay the upgrade path**, not the v1: if Quick Tunnels prove too flaky, the same
switch can issue a token instead, and nothing above the address field changes.

---

## ★★★ ANDROID: TWO THINGS THAT DECIDE WHETHER THIS SHIPS

Stuart wants this in the build that goes to Play — *"building it now also means when we submit to
google its already in place"* — which is right ([[android_server_gui_parity]]: uploading again
re-queues review). But it puts two things on the critical path that must be settled BEFORE the
submission, not after a rejection.

### 1. ★★★ cloudflared CAN run on Android — BUILD IT FROM SOURCE, DO NOT PATCH A BINARY

`GOOS=android GOARCH=arm64` builds fine. The failure is at runtime and it is **DNS**: Go's resolver
reads `/etc/resolv.conf`, which **does not exist on Android**, so every tunnel dial fails with
`lookup … connection refused` (cloudflared issue #425). The fix in that issue writes
`/system/etc/resolv.conf` — **root only, therefore useless to us.**

The circulating unrooted workaround **patches the string `/etc/resolv.conf` inside the shipped
binary** to a same-length path and feeds a nameserver on stdin.

★★★ **DO NOT DO THAT. cloudflared is Apache-2.0 AND WE ARE COMPILING IT OURSELVES ANYWAY.** We
already have to build for `android/arm64`, so the resolver is fixed **in Go source** — a few lines
installing a custom `net.Resolver` that dials `1.1.1.1:53` directly — and then built. Reviewable,
survives version bumps, and no hex-editing of somebody else's artefact.

★★ **This was the largest risk in this brief and building from source very largely removes it.** It
is still spike 1, because "compiles" is not "dials the edge from a phone on 5G" — but it is now a
spike we expect to pass rather than one we expect to fight.

★★ **Package it as `libcloudflared.so` in `nativeLibraryDir`.** `filesDir` is not executable on
modern Android; files shipped as `lib*.so` are. Needs `useLegacyPackaging`/`extractNativeLibs` so it
lands on disk as a real file rather than staying compressed inside the APK.

★★ **SIZE — measured, 2026-08-22.** The published `cloudflared-linux-arm64` is **37.4 MB raw**
(19 MB as a `.deb`), so an Android arm64 build is the same order. Two levers, in this order:

- ★ **arm64 only.** The switch does not appear on armeabi-v7a — AGENTS.md: absent, never inert.
- ★★ **A Play on-demand feature module** if that is still too much: only users who actually flick
  the switch ever download it. ▶ **UNCONFIRMED:** this plausibly also answers the policy rule below,
  since the code then arrives **from Google Play** rather than "a source other than Google Play" —
  but Google's position on that was not found in writing. Verify before relying on it.
  ★ React Native + dynamic feature modules is fiddly; native libs in a module need ReLinker.

### 2. ★★★ Play policy — three rules, and one sentence that matters

- ★★★ **BUNDLE IT. NEVER DOWNLOAD IT.** *"Apps that download executable code, such as dex files or
  native code, from a source other than Google Play are prohibited."* Fetching cloudflared on first
  use — the obvious way to keep the APK small — is a **policy violation**, not a trade-off.
- ★★★ **"Apps that facilitate proxy services to third parties may only do so in apps where that is
  the primary, user-facing core purpose."** ★★ Read carefully, **we are not that**: the tunnel
  carries the user's **own** receiver to listeners the user chose to invite. No third party's
  traffic passes through it. But a reviewer skimming for a bundled tunnel daemon will not make that
  distinction for us — **the Play Console justification has to say it in one sentence**: *"exposes
  the user's own radio receiver, which this app is, for public listening at the user's explicit
  request."*
- ★★★ **DO NOT TOUCH `VpnService`.** Device-level VPN is a far stricter policy and we need none of
  it — cloudflared is an ordinary outbound process. Staying off that API keeps us out of that
  policy entirely.
- ★★ **NO NEW FOREGROUND SERVICE TYPE. Ride the EXISTING one.** `VibeStreamService.kt` already runs one for the
  server. Adding a second FGS type means a second Play declaration and justification to be rejected
  over. The tunnel is part of "the server is running"; it belongs in that service's lifetime.

---

## ★★★ SECURITY — TWO GATES ON THE SWITCH, NOT TWO TO-DOS

Stuart, 2026-08-22: *"I want whatever we add to be secure."* Flicking this switch puts the whole
VibeServer HTTP/WebSocket surface on the open internet. Two things must be true **before the switch
can be turned on**, enforced in code, not written in a help page.

### 1. ★★ THE TUNNEL MAKES EVERY LISTENER 127.0.0.1 — THE CURE EXISTS, THE SWITCH MUST APPLY IT

`cloudflared` connects to the server **from loopback**, so without the trusted-proxy list every
listener arrives as `127.0.0.1` and the **session limit, the IP cooldown and the one-address rule
all switch themselves off** — at the moment the server becomes public.

✅ **This is ALREADY BUILT AND IT WAS BUILT FOR THIS.** Stuart, 2026-08-22: *"isn't that what the
reverse proxy box is for?"* — it is. `vibe_proxy.h` opens with *"Put VibeServer behind
nginx/Caddy/**a tunnel** and every connection arrives from the proxy"*, and `trustedProxies`
(`vibeserver_config.h:410`) is that box. **Do not design anything here.**

✅ **And the cloudflared header bug is already closed.** Issue #1426: a client's own
`X-Forwarded-For` can reach the origin, making the leftmost entry attacker-chosen.
`clientAddress()` walks the chain **right to left, skipping trusted hops** — *"the client can
prepend whatever it likes, so the LEFTMOST entry is forgeable even through a trusted proxy."*
★ Cloudflare sends `X-Forwarded-For` as well as `CF-Connecting-IP`, so no new header support is
needed either.

★★ **THE ONLY WORK IS THAT THE SWITCH MUST FILL THE BOX IN ITSELF** — add loopback when the tunnel
starts, remove it when it stops. The box is manual and its failure is **silent**: flick the switch
without knowing to type `127.0.0.1` and the server works perfectly while every protection it relies
on is off. A one-line wiring job on an existing mechanism, but it belongs in the same code path as
the tunnel, not in a help page. ★ Loopback ONLY — widening it is the forgery this all guards against.

★ **Android-only wrinkle, noted not acted on:** any local app can reach loopback on a phone, so a
malicious app on the same device could forge an address once loopback is trusted. Not a concern on
desktop.

### 2. ✅ SESSION LIMITS ARE **NOT** MANDATORY — and ★★ the admin password gap is on OUR platforms

`BRIEF-public-directory.md` floated *"a listing should arguably REQUIRE a session limit"*.
❌ **REJECTED, Stuart 2026-08-22:** *"session limits shouldn't be mandatory — if a user is happy to
have unlimited connection then that is their choice."*

★ **The code already agreed:** `sessionLimitMin` is *"0 = unlimited (the default, and what every
private receiver wants)"*. And the abuse this was reaching for — one person occupying a whole
receiver site — is already answered by the **one-address rule, which defaults TRUE**. Do not
re-propose this.

★★ **The real gap is the admin password, and it is on the two platforms this brief is about.**
Stuart: *"admin password is already mandatory now"* — true, and the comments say so
(`"the wizard makes it mandatory"`, line 7787). **But `g_vsNativeSetup` means the wizard is NEVER
SERVED on macOS and Android**, *"because it would be a second way to configure one server"* — so on
precisely the hosts with native GUIs, the thing that enforces the password never runs. Line 1035:
*"Empty = no admin password set, and then nothing is protected."*

★★★ **Harmless on a LAN, NOT harmless once listed.** So the gate is not a new policy — it is
checking that a password is actually set before the switch can be turned on, **because on Android
the usual enforcement was bypassed by design.** ★ Say it at the switch, with the field to fix it.

### ★ What the tunnel gets us for free

- **Real TLS, real certificate.** A port-forwarded home server cannot have one — no CA issues for a
  bare address — so its listeners get "Not Secure" and its admin password crosses in clear.
  ★★ **On the listener's security the tunnel is the STRONGER option, not the weaker one.**
- **The home address is never published at all.** Not the IP, not a DDNS name.
- ★★ It also dissolves the mixed-content limitation — see the directory page section.

## Where it goes in the code — ONE shim, not two

★★★ `vibeserver/CMakeLists.txt:93` compiles `android/app/src/main/cpp/local_sdr_shim.cpp`. **The
desktop daemon and the Android server are the same shim.** The listing state, the `/vibeserver.json`
`dirProof` challenge-response, and the ping timer are written **once, there** — not per platform.

What is genuinely per-platform is only the two things the shim cannot do:

| | desktop (Linux/Mac) | Android |
|---|---|---|
| **HTTPS to vibesdr.net** | ★ **`curl`, the existing precedent** — `solar.cpp:88`, `eibi.cpp:211`, whose comment already reasons this out: *"the daemon has no TLS stack of its own… adding libcurl to the build for that would be a poor trade."* | Kotlin/**OkHttp** — no curl on Android. Matches `BRIEF-public-directory.md`'s *"the directory client lives in the PLATFORM layer"*. |
| **running cloudflared** | bundled binary — ★ Apache-2.0, so we may redistribute it; `cloudflared-darwin-arm64` and `-linux-arm64` are both published, so Apple Silicon and the Pi are covered, and the new amd64 apt build is too | `libcloudflared.so` + the DNS patch above |

★★ **The `BRIEF-public-directory.md` reason for keeping this out of the shim was TLS, and `curl`
already answers it on desktop.** The *tunnel process* still has to be launched per platform.

### ★★★ IT IS ONE SWITCH, BECAUSE THE SERVER ALREADY KNOWS EVERYTHING

Stuart, 2026-08-22: *"a user can setup a local vibeserver in seconds on android — plug in sdr, use
as server, enter admin password, serve. Advertise on VibeSDR.net being a simple yes or no switch
keeps in line with this."*

★★★ **NO NEW BOXES ARE NEEDED, AND THAT IS NOT A COMPROMISE — CHECK THE CONFIG:**
`vibeserver_config.h:363` already holds `place, country, locator, lat, lon` — *"the SITE — one
machine, one location"* — and line 364 holds `name`, *"the machine's name, shown above the list"*.
**The directory's every field is already there**, set during that seconds-long setup.

```
Name  [ Stuey3d Pi500 ]
  [x] Advertise on VibeSDR.net
      Others can find and listen to this server at
      stuey3d-pi500.vibeserver.vibesdr.net
```

★★★ **LOCATION IS THE OPERATOR'S MAIDENHEAD SQUARE, NEVER THE ADDRESS.** Geolocating a tunnelled
server puts **Cloudflare's edge** on the map, not the operator — the entry would read "Frankfurt"
for a receiver in Manchester. Already forbidden on privacy grounds; with a tunnel it is also
simply WRONG.

### ★★★ THE SHAREABLE ADDRESS — `stuey3d-pi500.vibeserver.vibesdr.net`

Stuart, 2026-08-22: *"The friendly address name is derived from the friendly name entered in the
name box above the advertise to public switch **exactly like we do with mdns**."*

✅ **VERIFIED FREE, 2026-08-22.** I first said the two-level form would need paid Advanced
Certificate Manager. **That was wrong.** Tested by asking the edge what it serves per SNI:

| SNI | certificate served |
|---|---|
| `dave.vibesdr.net` | `vibesdr.net`, `*.vibesdr.net` |
| `dave.vibeserver.vibesdr.net` | `vibesdr.net`, `vibeserver.vibesdr.net`, **`*.vibeserver.vibesdr.net`** |

Creating the `vibeserver.vibesdr.net` custom domain caused Cloudflare to issue a cert covering one
wildcard level beneath it. **Both forms cost nothing**, and proxied wildcard DNS is free on all
plans. ★ Chosen: **`*.vibeserver.vibesdr.net`** — Stuart: *"I want them to be identified as
vibeserver addresses"*, and it keeps user-chosen names out of the apex beside `demo` and `www`.
▶ **Confirm that cert keeps RENEWING** — it appeared as a side effect, not something we asked for,
and if it lapsed every shared link breaks at once.

★★★ **A REDIRECT, NEVER A PROXY.** The Worker looks the name up and issues a **302 to the current
tunnel URL** — one request per click, not per byte. Proxying would put every listener's audio
through our Worker's 100k requests/day and make us the transit provider, which this design has
refused twice. ★★ Stuart, on the ugly hostname reappearing in the address bar after the redirect:
*"not an issue as long as it appears nice and neatly on the servers page"*.

★★★ **AND THIS IS WHAT MAKES A QUICK TUNNEL SHAREABLE AT ALL.** The tunnel hostname rotates on
every restart; `stuey3d-pi500.vibeserver.vibesdr.net` does not. **The friendly name is the stable
identity the rotating address never was** — a real argument for building it, not a nicety.

#### Reuse `mdnsLabel()`. Do not write a second slug rule.

`vibeserver_config.cpp:156` already does exactly this: lowercase, non-alphanumerics collapsed to a
single dash, trimmed, **capped at 63 — one DNS label**. Already DNS-correct.
★★ And `vibe_setup_page.h:859` already MIRRORS it in JS and shows the derived address live under
the name box — *"the address they are given is the address that works"*. **The public address line
slots into `addr()` beside the `.local` line.** The UX Stuart is asking for is already built, for
mDNS.

★ There are already TWO implementations of this one rule (C++ and JS). A third consumer makes a
divergence worse — **add a test asserting they agree** rather than a fourth copy.

#### ★★★ Three traps, two of them already scars

1. ★★★ **REFUSE the `"vibeserver"` FALLBACK — DO NOT PUBLISH IT.** `mdnsLabel("")` returns
   `"vibeserver"`, and `main.cpp:1290` carries the scar: an unnamed laptop **took
   `vibeserver.local` away from the Pi and SSH to it started failing**. Publicly it is worse — the
   first unnamed server would take `vibeserver.vibeserver.vibesdr.net` and every other unnamed one
   collides with it. ★ "No name yet" and "not ready to advertise" are ONE state, exactly as mDNS
   already treats them.
2. ★★★ **DERIVE THE SLUG ONCE, AT REGISTRATION, AND STORE IT.** `vibeserver_config.h:78` already
   says the `.local` label is *"DERIVED from it, once"*. For a **shared public** address the rule
   is stronger: if Dave renames his server, **every link his friends already have must keep
   working.** Never recompute the slug on a ping.
3. ★★★ **THE HOLD IS AS LONG AS THE LISTING WAS USED, capped at a week, floored at an hour.**
   A flat week has a nasty failure Stuart spotted at once: someone who reinstalls loses the id and
   key, re-registers, and is **blocked from their own name by their own dead entry** — offered
   "dave1" because of a ghost they cannot delete. Proportional holding means an experiment gives
   its name up within the hour while an established receiver survives a holiday or a dead SD card.
   ★ Turning the switch OFF still frees the name immediately, via delist. **Verified live
   2026-08-22:** two servers both away 2 h — one that ran 5 min released, one that ran 30 days held.
4. ★★ **Collisions are first-come.** The second Dave is offered `dave` + his locator —
   `daveio92nh` — which the app can propose automatically, because
   `vibeserver_config.h:363` already holds `locator`.

### ★★ MOVING AN ADDRESS TO A NEW DEVICE — a ONE-TIME TOKEN, not the credential

Stuart's design, 2026-08-22: a **Transfer to new device** button that warns it will deactivate the
listing, **cycles the server's unique ID**, and hands over a small file to import elsewhere.

★★ **Cycling the ID on export is the load-bearing part** — the old device is out the moment the
button is pressed, so the original and the new one can never both be live.

★★★ **BUT THE FILE MUST NOT CONTAIN THE id AND key.** Stuart spotted the hole himself: *"the only
issue is if someone then tries to use that on multiple devices."* The key IS the identity (the
original brief: *"anyone holding it can take over the listing"*), so a credential file is a bearer
token — two devices importing it both ping the same row, **each overwriting the other's tunnel
URL**, and the listing flips between them every 15 minutes. Not dangerous; very hard to diagnose.

★★★ **SO THE FILE CARRIES A SHORT-LIVED, SINGLE-USE `transferToken`.** The first device to import
it exchanges it for a **freshly minted id and key**, and the token is burned server-side in the
same transaction. A second import gets *"this transfer has already been used"*. The hole stops
being a caveat and becomes impossible.

- ★★ **The file stops being dangerous once used.** A credential file left in Downloads is a
  standing risk for ever; a burned token is worthless.
- ★ **It expires** (24 h), so a transfer that never happened does not stay live.
- ★ **The old device can cancel it** before it is claimed.
- ★★ **The old device must SAY what happened** — *"this server's listing was transferred to another
  device"* — not merely stop appearing. Same rule as the sleeping-server 503: state it, never leave
  it to be inferred.

★ **Complements the hold, does not replace it.** Transfer covers the PLANNED move (new phone,
rebuilt Pi); the usage-proportional hold covers the UNPLANNED one, where the device died before
anybody pressed anything.

### The switch, in BOTH GUIs — and this is where we have form

*"In the app a simple switch in the server page called **List on VibeSDR.net**"*, which creates the
tunnel and registers it.

★★★ **It has to land in `vibe_setup_page.h` AND `ServerModeScreen.tsx` in the SAME change.** These
two were only just brought to parity ([[android_server_gui_parity]]) and that work produced three
traps that this switch will walk straight into:

- ★★★ **A `class="hide"` row needs its unhide in the SAME edit** — "VibeAGC" shipped **invisible for
  a whole release**. There is a test for this now; the switch must be in it.
- ★★★ **One missing `@ReactMethod` returns `undefined`, not an error** — it has now happened twice.
  Every new bridge call for this switch gets checked against the Kotlin side before it is believed.
- ★★★ **A `useCallback` dep list is a list you must remember, so it WILL be wrong.** Read the
  listing state through a ref, as the settings now are.

★ And per AGENTS.md: **the switch must not appear where it cannot work.** No cloudflared for this
ABI, or no radio attached — then it is absent, not inert.

---

## vibesdr.net/… — the public page

Stuart's design, 2026-08-22. **`vibeserver.vibesdr.net`.**

1. **A map at the top.** Per `BRIEF-public-directory.md`: Leaflet, `leaflet.terminator` for the
   greyline (★★ that is *information* for a DXer, not decoration), Maidenhead-square positions,
   clustered markers, and shape-as-well-as-colour so the red/green legend survives colour blindness.
2. **A list underneath, collapsed by country** — ★ *"like the app sorts UberSDR and KiwiSDR into"*.
   The grouping the picker already uses, so the site and the app agree.
3. **A server entry expands to its radios and capabilities, the way the landing page does** — the
   shape `website/worker.js` already assembles for `/api/demo`: label, driver, coverage, mode,
   listeners/max, `freeInSec`, shared-or-exclusive.
   ★★★ **NO SERIAL NUMBERS**, exactly as that Worker already strips them.

★★★ **THE TUNNEL FIXES THE MIXED-CONTENT PROBLEM, AND THAT IS A REAL BONUS.**
`BRIEF-public-directory.md` had to concede: *"The page can only LINK to a server: VibeSDR.net is
HTTPS, listed servers are plain HTTP, so the browser blocks any fetch."* **A tunnelled server is
HTTPS with a real certificate**, so for those entries the directory page can fetch live status
directly in the browser — no Worker round-trip, no stale record. Plain-HTTP port-forwarded entries
keep the old limitation. ★ Two classes of entry on one page; the page must not assume either.

★ Storage is **D1, never KV** — the 15-minute ping is 96 writes/day/server and KV's free tier is
1,000 writes/day *total*. And **not Durable Objects**: they require the paid plan. This is settled
in `BRIEF-public-directory.md` and is not reopened here.

---

## In the app — a VibeServer directory, top of the list

*"a new directory for VibeServer and that is top of the list"* — which is already the plan
(`BRIEF-public-directory.md`: *"give VibeServers top spot, above OWRX / Kiwi / UberSDR"*).

★★ **The client decides what is reachable NOW.** The directory says who is listed; the picker probes
and greys out what does not answer. ★ For tunnelled entries this matters more, not less: a Quick
Tunnel that has been restarted is *listed at an address that is already dead*, and only the probe
knows.

---

## Build order — spike 1 is a gate, not a step

### ✅ SPIKE 1, HALF DONE — 2026-08-22. THE BUILD WORKS.

Run on this Mac with Go 1.27.0 (`brew install go`), against `cloudflared` master `d1df798`:

- ✅ **`GOOS=android GOARCH=arm64 CGO_ENABLED=0` builds clean**, in 9 seconds, no patches needed to
  compile.
- ✅ **`/etc/resolv.conf` IS in the resulting binary** — confirmed with `strings`. The failure in
  issue #425 is real and would have hit us.
- ✅ **The source fix is written and verified in the build**:
  `cmd/cloudflared/dns_android.go`, `//go:build android`, an `init()` installing a
  `net.DefaultResolver` whose `Dial` ignores resolv.conf and goes straight to 1.1.1.1 / 1.0.0.1 /
  8.8.8.8. Confirmed present: `go list` for `GOOS=android` includes the file, the binary hash
  changes, and the nameserver strings appear.
  ★ Keep it as a **patch file applied to a pinned upstream tag**, not a fork to maintain.
- ✅ **SIZE IS BETTER THAN FEARED: 29.0 MB stripped** (`-ldflags "-s -w"`), not the 37 MB of the
  published unstripped Linux binary. ★★ **arm64-only may well be enough on its own** — the Play
  on-demand feature module, and its unconfirmed policy question, can probably be dropped.

▶ **STILL TO DO, and it is the half that decides it:** run it **on the Moto, on 5G not wifi**.
★ It needs no app packaging to test — `adb push` to `/data/local/tmp`, `chmod +x`, run it from
`adb shell`. That directory is executable, so the tunnel can be proven end to end **before any
Android build work at all**. (Attempted 2026-08-22: no device attached.)

### The rest

1. ★★★ **Does our source-patched cloudflared dial the edge from the Moto?** Build
   `GOOS=android GOARCH=arm64` with the custom `net.Resolver`, ship it as `libcloudflared.so`, run a
   Quick Tunnel against the real VibeServer — **on 5G, not wifi**, because CGNAT is the whole point.
   Measure the real APK cost while there.
   **If this fails, Android is cut from this release and the desktop switch ships alone** — an
   acceptable outcome, but only if we learn it BEFORE the Play submission, not after a rejection.
2. ★★ **How stable is a Quick Tunnel over hours?** Leave one up on the Pi overnight against the real
   server. Rate limits and reconnect behaviour are claims until measured.
3. D1 schema + `register`/`ping`/`delist` + the DNS-vs-source-IP check, per
   `BRIEF-public-directory.md`. ★ Tunnelled entries **skip** the DNS-vs-source-IP check — the
   hostname is Cloudflare's, not the operator's, so it proves nothing. Their proof is the
   `dirProof` challenge-response over the tunnel, which is now over **HTTPS**.
4. The shim: listing state, `dirProof`, ping timer. Once, shared.
5. The switch in both GUIs, same change, with the hide-test extended — and the loopback
   trusted-proxy wiring in that same change, per the security gates above.
6. `vibeserver.vibesdr.net` — map, country list, expanding entries.
7. The picker's directory source, top of the list, with the reachability probe.

## ★ The phone demo does NOT depend on any of this

Stuart's immediate want — an Android phone on the website — needs **no code at all**. `cloudflared`
already runs on the Pi for `demo.vibesdr.net` (`website/worker.js`); add a second hostname to that
tunnel's ingress pointing at the Moto's LAN IP and port. Config only, no account limits consumed,
and it can be live today. ★★ **Keep the two decoupled** — otherwise a demo phone on the website is
gated on a Play review.

## Open questions

1. Does a Quick Tunnel survive an Android Doze cycle, or does the switch need the wake lock
   `VibeWifiLock` already provides?
2. Whose terms is a listener agreeing to when the audio crosses Cloudflare on a tunnel nobody owns?
3. Do we show a tunnelled server its own ugly hostname, or only ever "Listed"?
4. Inherited from `BRIEF-public-directory.md`, still unanswered: should listing REQUIRE a session
   limit?

---

## Appendix — the DDNS + Let's Encrypt route, and why it is NOT v1

Stuart's original plan, 2026-08-22: *"have the user enter their DDNS address into VibeServer and
then enable the switch to publish it to the directory — the main concern was HTTPS."* Recorded
because it remains the right answer for anyone who wants **no third party in the data path at all**,
and because two things were established while checking it.

★★★ **`freemyip.com` IS on the Public Suffix List** (verified against `publicsuffix.org` on
2026-08-22, alongside `duckdns.org`, `ddns.net`, `no-ip.org`). This matters more than it looks:
Let's Encrypt limits issuance to **50 certificates per registered domain per week**, so a DDNS
provider that were NOT on the list would have **every one of its users sharing a single bucket**,
and issuance would start failing for our users through no fault of their own. On the major providers
it does not. ★ Any implementation must still degrade gracefully when it does.

★★★ **The blocker is not the certificate, it is that VIBESERVER HAS NO TLS SERVER.** Getting a cert
is the easy half; terminating TLS on the listening socket does not exist today. `eibi.cpp:14`
reasons out why even the *outbound* path uses `curl` rather than linking a stack.

| way | cost |
|---|---|
| **mbedTLS in the shim** | The only one that fits the architecture — lands in the shared shim, so Linux, Mac **and Android** all gain HTTPS at once, and nothing else on the table gives the Android server TLS at all. ★★ The work is an ACME client in C++: JSON over HTTPS is easy, the JWS/ECDSA signing is not. |
| **Caddy in front** | Least code by far — ACME and TLS in ~2 lines of config. But a second daemon to supervise, and a Go binary, so on desktop it costs about what bundling cloudflared costs. **Non-starter on Android.** |
| **nginx + certbot** | Most moving parts, worst fit for a one-switch feature. |

★★★ **AND THAT IS THE ARGUMENT THAT SETTLED IT.** If the HTTPS cure needs a bundled Go binary on
desktop anyway, then Caddy-for-TLS and cloudflared cost roughly the same — and **cloudflared
delivers strictly more**: TLS, no port forwarding at all, and the address never published. The DDNS
route still needs **two** ports forwarded (443, plus 80 for the ACME challenge, which some ISPs
block), still publishes where the operator lives, and goes dark if a renewal lapses.

★★ **The requirement it cannot satisfy at all:** Stuart asked for port forwarding *"as long as we
can hide the home IP address"*. **Those are mutually exclusive.** A direct connection requires the
listener to know the address — that is what "direct" means. Hiding it requires something in the
middle, which is the tunnel. ★ Publishing a DDNS *hostname* rather than an IP is not hiding either;
it is one DNS lookup away. It keeps the raw address out of a search index, nothing more.

★ Revisit this appendix only if Quick Tunnel rate limits (spike 2) turn out to be intolerable.
