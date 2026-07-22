# BRIEF: VibeServer — multi-radio, the landing page, and the admin GUI

**Project:** VibeSDR / VibeServer
**Author:** Stuart Carr (Stuey3D), dictated 2026-07-22
**Builds on:** `VibeServer-MultiClient-Brief.md` (the *why*: one physical radio per user),
`files/BRIEF-vibeserver-protocol-foundations.md` + its two-level access amendment (the control
token, admin role, hardware policy), `files/BRIEF-vibeserver-macos.md` (the Mac app).
**Status:** DESIGN. Not started.

---

## 1. The shape, in Stuart's words

> "Port 48000 is the landing page. Radio 1 is 48001, Radio 2 is 48002, etc. Different radios have
> their own DSP engine, so a user never has to fight or share settings with another user. If the
> user on Radio 1 has to throttle the FPS and reduce the bins/FFT, but Radio 2 has enough power and
> bandwidth for full quality, then user 1's reduction must not affect user 2. Effectively each radio
> is its own separate VibeServer, just served by a landing screen at 48000, all from the same
> machine."

This is the deployment shape of the decision already taken in `VibeServer-MultiClient-Brief.md` §0:
an RTL-SDR sees only ~2.4 MHz around one LO, so there is nothing to slice between users. N dongles
serve N controlling users, and quality is guaranteed rather than shared.

## 1.1 ★★ THE TEST: multi-radio should be INVISIBLE when it should be

Stuart: *"if a server is only running one profile with 4 radios — all the same antenna and
capability — then the server operates as it does now, just in the back end a user is being assigned
a radio. The only difference is a server gets 4 user slots instead of 1."*

That is the whole design in a sentence, and it is the test to hold the implementation to.

| | Today | One pool of 4 |
|---|---|---|
| Address | `host:48000` | `host:48000` |
| Ports forwarded | 1 | 1 |
| Splash / picker | none | **none** |
| What a listener does | connects, listens | connects, listens |
| Capacity | 1 listener | **4 listeners** |

Nothing a user sees changes. They do not choose a radio, do not know whether they are on 1 of 1 or
1 of 4, and cannot tell which. The allocation is a back-end fact.

**The complexity earns its place only where it buys something:** more listeners on identical radios
(no UI at all), or a genuine choice between DIFFERENT receivers (a picker, because picking wrongly
would waste the user's time). Anything else — a picker over identical radios, a serial shown to a
listener, a splash for one pool — is the feature leaking into a place it does not belong.

Occupancy follows the same logic: "full" means every radio in that POOL is busy
(`VibeServer-MultiClient-Brief.md` §4's three states), and it is per-pool — an HF listener is
blocked when the HF pool is full even if four VHF radios sit idle.

## 2. ★ ONE PROCESS PER RADIO — isolation by construction

**Take "each radio is its own separate VibeServer" literally: run one `vibeserver` process per
radio.** This is the central decision of this brief.

**Why it must be processes, not objects.** `LocalSdrShim` is a SINGLETON —
`LocalSdrShim& LocalSdrShim::instance() { static LocalSdrShim inst; return inst; }` — and every
VibeServer setting is a file-scope global: `g_vsPort`, `g_vsSecret`, `g_vsMaxFftRate`,
`g_vsMaxBandwidth`, `g_vsLockedRate`, `g_vsForceIdle`, `g_serveOnLan`, and more. Running two radios
in one process therefore means de-singletoning the core AND moving a dozen-plus globals into
per-instance state — invasive surgery on code Android also ships, for no user-visible gain.

**What processes buy, beyond avoiding that work:**

- **The isolation requirement is met by the OPERATING SYSTEM, not by our diligence.** User 1's FPS,
  FFT size, bandwidth, gain, squelch and sample rate are in a different address space from user 2's.
  There is no code path by which one can affect the other — no shared global anyone might add later
  and forget to key by radio. Requirements guaranteed by construction do not regress.
- **Fault isolation.** A dongle that wedges, a USB error, a crash — takes one radio down, not the
  server. On a headless Pi in a field, that is the difference between "one receiver is out" and "the
  site is down".
- **Independent restart.** Radio 2 can be restarted without interrupting the person listening on
  Radio 1. Config changes to one radio need not disturb the other.
- The existing single-radio code stays exactly as it is, which is the code that already works.

**Cost, stated honestly:** each process carries its own copy of the DSP state and the ~250 KB web
page, plus its own threads. Trivial on a Mac; worth measuring on a Pi before promising 4+ radios
there. Measure before claiming a number.

## 3. The hub at 48000

`vibeserver --hub` — the same binary, a different mode. It:

1. **Serves the landing page** at `GET /`: every configured radio, its name, whether it is free, in
   use or offline, and a link to its port. This is the "splash screen for choosing a radio".
2. **Supervises the children.** Spawns one `vibeserver` per enabled radio on 48001, 48002 …, watches
   them, restarts a crashed one, and stops them cleanly on quit.
3. **Owns the shared config** (§5) and hands each child its slice.
4. **Serves the admin surface** (§6).

Being the same binary matters: the Mac GUI and the headless Pi run the *same* hub, so the landing
page and admin behave identically on both. The Mac app becomes a window onto the hub rather than a
second implementation of it — the same rule already applied to the web config page.

**Ports.** 48000 hub, then 48001+ in configuration order. Explicit, predictable, and easy to
port-forward. A taken port FAILS LOUDLY for that radio (as `setVibeServerPort` already does) and the
hub reports it — never silent drift, which would break a forward or a saved bookmark.

## 3.0 ★ 48000 IS THE UNIVERSAL ENTRY

Stuart: *"port 48000 is our universal entry then — and if a user only has one radio it works as it
does now; if multiple radios it shows the splash screen."*

One address to remember, one port to forward, one thing to advertise, whatever the setup:

- **One radio** → 48000 goes straight to it. No splash, no choosing, no change from today's
  behaviour. The overwhelmingly common case must not pay for a feature it does not use.
- **Several radios** → 48000 shows the splash, and the choice sets the session (§3.1).

★ **The hub always runs**, even for a single radio — it is the entry point, the admin surface and
the Bonjour advertiser, so having it present-or-absent depending on radio count would mean two
topologies to build and test rather than one. Adding a second dongle then changes nothing
structural: the address, the forwarded port and the admin surface all stay exactly where they were,
and a splash simply starts appearing.

The cost is one extra process and one proxy hop for the single-radio case. Trivial on a Mac; on a Pi
it goes on the same measurement list as per-radio RAM (§2) before any radio count is advertised. If
it ever proves to matter there, the hub can serve a lone radio in-process — but that is an
optimisation to make with numbers in hand, not up front.

## 3.1 ★ ONE FORWARDED PORT — the hub proxies, so 48000 is all anyone opens

Stuart: *"right now this setup requires a user to forward multiple ports on their router for each
radio — is there any way of doing internal port forwarding so a server owner only has to forward
48000?"*

Yes, and it should be the default. Asking someone to forward one port per radio is a real barrier —
four radios means four forwarding rules, four chances to get it wrong, and a router UI most people
touch once a year.

**The hub becomes a reverse proxy.** It already listens on 48000; it also relays HTTP and WebSocket
traffic to the child on 48001/48002/… . A WebSocket relay is an HTTP upgrade followed by copying
bytes both ways — no parsing of the stream, so it cannot corrupt spectrum or audio.

### ★ Route by SESSION, so the paths never change

The obvious approach — a path prefix like `/r/loft/ws/user-spectrum` — breaks every existing client,
because they all build paths at the root. Instead:

1. The splash page at 48000 lists the radios. Choosing one sets a **session cookie (or token)**
   naming that radio.
2. Every subsequent request on 48000 — `GET /`, `/ws/user-spectrum`, `/ws/audio`, `/favicon.png` —
   is proxied to that session's child, **at exactly the same path**.

So the web client needs NO changes: it still asks for `/ws/user-spectrum` and still gets it. And
because a radio has one user (§1), "this browser session is on Radio 2" is a complete and honest
description of the world — there is nothing to multiplex.

### What this does and does not change

- **Isolation is untouched.** The point of one-process-per-radio was that a listener's FPS, FFT and
  gain cannot affect another's. Those still live in separate processes; the hub only moves bytes.
- **LAN access is unaffected.** Bonjour still advertises each radio directly (§4), so clients on the
  network connect straight to 48001/48002 and never touch the proxy. **Port forwarding is a REMOTE
  concern only** — which is exactly the case worth optimising, since it is the one with a router in
  it.
- **The hub becomes a single point of failure for REMOTE users.** Honest cost. A crash there takes
  remote access to every radio, where before it would have taken one. Mitigate by keeping the proxy
  dumb: route once on the session, then shovel bytes and never interpret them.
- **Loopback traffic roughly doubles** for proxied clients (in to the hub, out to the child). At
  ~120 KB/s per listener this is nothing on a Mac, and worth measuring on a Pi alongside the
  per-process RAM figure.

### Native clients

The phone and watch do not do cookies. Two options, and the brief prefers the first:

1. **Direct ports on the LAN** (already how discovery works), and for remote use let the app take a
   `?radio=<id>` query parameter that the hub honours in place of the cookie. One extra field in a
   saved server entry.
2. A path prefix, which is cleaner in URL terms and worse in every other, because every client must
   learn it.

## 4. Discovery — advertise the RADIOS, not just the hub

Each child advertises itself over Bonjour as `_vibesdr._tcp` with its own name ("Kitchen RTL-SDR",
"Loft V4"). Existing clients — phone, watch, browser — then see the radios in their picker and
connect directly, **with no client change at all**, because to a client a radio simply *is* a
VibeServer. The hub may advertise itself too, for a human arriving with a browser and no app.

★ Note the mDNS gap already found on macOS applies here: the C++ core answers hostname queries only,
so each host needs its own service registration (`NetService` on macOS, Avahi or a PTR/SRV/TXT
extension to the responder on Linux). See `vibeserver_mac_standalone` notes.

## 5. Configuration — one document, a radios array

Keep the "one config schema, three editors" decision (macOS brief §5) intact by extending the
existing document rather than sprouting per-radio files:

```jsonc
{
  "serverName": "Stuey3D",
  "adminPassword": "…",          // global: the operator (foundations amendment)
  "hubPort": 48000,
  "radios": [
    { "id": "loft", "name": "Loft V4", "device": 0, "port": 48001, "enabled": true,
      "pin": "", "maxFftRate": 0, "maxBandwidthHz": 0, "lockedRate": 0,
      "forceIdleSaver": false, "hardware": { "biasT": "admin", "gain": "open" },
      "defaults": { "centreHz": 96600000, "mode": "wfm" } }
  ]
}
```

- **Global:** server name, admin password, hub port, update policy.
- **Per radio:** everything a single VibeServer already takes — because a radio IS one.
- Identify dongles by the resolution order in §5.1 — serial, then USB port path, never index. Each
  radio stores BOTH a serial and a port path so it can be matched by whichever is unambiguous.

## 4.5 ★★★ THE MODEL: THE ANTENNA IS THE OBJECT

Stuart, arriving at it after everything above: *"I've got it, and the answer has been brutally
simple the whole time."*

**Configuration is organised around the ANTENNA, not the radio and not an abstract profile.** What
this brief kept calling a "profile" IS an antenna — coverage, band limits, who powers it, what can
hear HF through it. Every question a user must answer becomes one about their own installation,
which is the only kind of question they can answer confidently.

This supersedes the profile-centric framing in §5.2.1–§5.2.2. Those sections' CONTENT still holds —
pooling, per-radio installation facts, control policy — but the object it hangs on is the antenna.

### Simple Mode — the default, and no wizard at all

**One radio, or several identical radios on one antenna.** Nothing to configure, nothing to choose,
no splash (§1.1, §3.0). The overwhelmingly common setup never meets any of the machinery below.

### Multi-antenna mode — a wizard, run once per antenna

1. **Antenna name and frequency range.** *"80m OCF dipole, 0.5–30 MHz."* The user knows this; it is
   the fact everything else derives from.
2. **Add radios to this antenna.** A physical statement about what is plugged into what.
3. **Does one of these radios power the antenna?** If yes, which one. ★ **If their serials collide,
   this is the moment to suggest the EEPROM change** — not buried in an advanced menu, but exactly
   where the user has just told us something that only a unique serial can make reliable (§5.2
   revision, §5.0 rule 2). The reason is now obvious to them, because they are looking at it.
4. **Model-aware recommendation, offered not imposed.** *"You have 3 × RTL-SDR v4 and 1 × v3, and
   you have said this antenna covers HF. Recommended: no direct sampling on the v4s (they reach HF
   through their built-in upconverter), and automatic direct sampling on the v3."*
   - ★ **The v3 uses the Q BRANCH** (`direct_samp` mode 2). The I branch is the wrong input on that
     hardware and yields silence on HF with nothing to explain it. Our own shim already documents
     this: `0=off, 1=I, 2=Q (not needed on Blog V4)`.
   - ★ **UNVERIFIED ON HARDWARE.** Stuart runs v4s only, so the v3 recommendation has never been
     tested here — it comes from the hardware design and our own shim's comment, not from a working
     receiver. Before this wizard ships, the v3 path must be tried on an actual v3: does Q-branch
     direct sampling below ~24 MHz genuinely produce HF? Shipping hardware advice we have not seen
     work is how a user ends up with silence and no idea whether it is them or us.
   - **Unknown or clone dongles:** do not guess. Say that HF may need direct sampling enabled or an
     upconverter fitted, and let the user decide — clones lie about their descriptors (§5.2).
   - **Upconverter:** if one is fitted to a radio, this is where its offset is entered.
5. **Gain range and auto-gain.** The ceiling the owner knows this receiver overloads above, and
   whether the listener may touch it.

Repeat per antenna. Antennas that differ in coverage become the pools a listener chooses between
(§5.2.2); radios on one antenna are interchangeable and never surfaced.

### ★★ How little of this needs an EEPROM change

Stuart: *"you only have to change the EEPROM of the radio supplying the power — for the others the
serial number doesn't matter, as we identify their capabilities from their USB name."*

Correct, and the general rule is worth stating because it makes the EEPROM tool a rarity rather than
a chore:

> **A radio needs a unique serial only when it carries a PER-RADIO INSTALLATION FACT that the USB
> descriptor cannot reveal, AND it is otherwise indistinguishable from its neighbours.**

Everything else is already known without one:

- **Capability comes from the USB product string** — "Blog V4" versus "Blog V3" tells us how each
  reaches HF, so a mixed pool needs no serials at all.
- **Pooled radios are interchangeable** — if nothing distinguishes them, nothing needs identifying.

So the cases that actually require it are few:

| Setup | Serial needed? |
|---|---|
| Any number of identical radios, none powering the antenna | **No** |
| Mixed models (3 × v4 + 1 × v3), none powering the antenna | **No** — the product string separates them |
| Several IDENTICAL radios, one supplying bias-T | **Yes — that one only** |
| Mixed models where the ONE v3 supplies the power | **No** — it is already unique by model |
| Several identical radios, one with an upconverter fitted | **Yes — that one only** |

In practice that is usually **exactly one dongle, and often none**. Which is the right place to land:
the tool exists for a real hazard (§5.0), is offered at the moment that hazard appears (§4.5 step 3),
and most owners will never need to touch it.

### Why this is the right shape

- **Every question is answerable.** "Does a radio power this antenna?" is observable from where the
  user is standing. "Set the direct-sampling policy" is not.
- **The hazards arise naturally.** Bias-T is asked about because powering an antenna is an antenna
  fact — so the dangerous setting is raised at the one moment the user has the context to get it
  right, rather than left to a checkbox in a settings page.
- **The EEPROM suggestion lands where it makes sense**, immediately after the user has expressed a
  requirement that depends on it.
- **It matches how people describe their stations.** Nobody says "I have a v3 profile"; they say
  "the dipole runs to the v3, and the discone feeds the two v4s."

## 5.0 ★★ PRINCIPLE: a dodgy config must not damage hardware

Stuart, 2026-07-22: *"we want to prevent physical damage to hardware through dodgy configs."*

Almost everything here is recoverable — a wrong gain sounds bad, a wrong sample rate looks wrong, a
wrong mode is a click away from right. **Bias-T is not.** It puts DC up the feed, and the wrong
answer can leave an LNA unpowered (silently useless), push DC into a block, or set two supplies
against each other on a shared antenna. The user is usually not present when a config is applied —
it happens at boot, on a headless box, in a loft or a field.

So hardware-affecting settings obey these rules, and any future one inherits them:

1. **Default OFF.** Always, everywhere, including on a radio that matches no profile (§5.1).
2. **Never auto-restored to a device that is not provably identified.** Only a UNIQUE SERIAL earns
   automatic restoration at boot; a device known only by which socket it is in starts off and needs
   a human that session (§5.2 revision).
3. **Never inherited.** An unrecognised dongle takes safe defaults, not the settings of a radio that
   happens to be free.
4. **Always visible, with attribution.** The client shows whether it is on and who set it — never a
   control whose state the user cannot see, and never one we would silently ignore.
5. **Warn on a plausible conflict.** We cannot see the coax, so we cannot know two receivers share
   an antenna — but when more than one radio has bias-T enabled we can say so once: *"Two receivers
   are set to supply bias-T. If they share an antenna, only one should."* Cheap to say, and it names
   the exact mistake.

The test to apply to any new setting: **if this is wrong at 3am with nobody watching, what is the
worst outcome?** If the answer involves hardware rather than audio, it belongs under these rules.

## 5.1 ★★ DEVICE IDENTITY — the hard part, and the one that can damage hardware

Stuart: *"if a user has a v3 or a dongle that requires direct sampling active, and another where it
is not needed, we must not accidentally put the direct-sampling radio in the server that has direct
sampling active."* And the reason that is hard: *"with RTL-SDRs that will need an EEPROM tweak to
change the serial, otherwise they all get detected as 1 radio."*

He is right. Stock RTL-SDRs ship with the same serial (`00000001` on most, including many v3/v4
units), so **serial alone cannot distinguish them**. Getting this wrong is not cosmetic:

- **Direct sampling** applied to a dongle that does not need it = a receiver that appears broken.
- **Bias-T** applied to the wrong dongle = DC pushed up a feed that may not tolerate it. This is the
  one that can cost someone an LNA or worse, and it is why identity must be *conservative*.

### The resolution order

Identify a physical dongle by the first of these that is unambiguous:

1. **Serial** — when it is unique across the attached dongles. The best case: settings follow the
   DONGLE wherever it is plugged. Users who want this set it once with `rtl_eeprom -s`.
2. **USB port path** (bus + port numbers, from libusb, which we already link) — stable across
   replugs and reboots, because it describes the SOCKET. This is the answer when serials collide,
   and it is intuitive to explain: *"the v3 lives in the left-hand hub port."*
3. **Index** — last resort only, and never persisted, because it renumbers the moment another
   dongle is unplugged. Index is what makes settings land on the wrong radio.

Store BOTH the serial and the port path in each radio's config, and match on the best available.

### ★ An unknown dongle gets SAFE DEFAULTS, never someone else's config

If a newly-appeared dongle matches no configured radio, it must NOT inherit the settings of a radio
that happens to be free. It starts with **direct sampling off, bias-T off, automatic gain**, and is
presented in the GUI as a new device awaiting setup. Inheriting is exactly how a bias-T ends up
somewhere it should not be.

### ★ Detect the collision and SAY so

When two attached dongles report the same serial, the GUI must tell the owner plainly, once:

> Two receivers report the same serial number (`00000001`). VibeServer will tell them apart by which
> USB socket they are plugged into, so their settings follow the socket, not the dongle. If you move
> them, their settings move too. To bind settings to a specific dongle instead, give it its own
> serial with `rtl_eeprom -s`.

That is honest, actionable, and it costs the user nothing if they do not care — which, as Stuart
notes, is the common case: *"not so much a problem if a user has 4 v4s all connected to the same
antenna using the same settings."*

### 5.2 ★ RADIO PROFILES — split by CAPABILITY, and detect the model to suggest one

Stuart, 2026-07-22: *"if a user plugs a v3 in and then sees it as a v3 this makes setup
significantly easier … they set up radio profiles (but not like OWRX) so a user with 2 v4s and 2 v3s
could make a v4 profile with the direct-sampling controls locked out, and a v3 profile which has
auto-enable direct sampling below 24 MHz, or if they are using an upconverter they can enter the
offset frequency. That way the radios are split by capability."*

**The model is knowable.** The USB descriptor carries manufacturer `RTLSDRBlog` and product
`Blog V4` — `rtlsdr_get_device_usb_strings()` returns both, and the Mac app now shows them. This is
strictly better than `rtlsdr_get_device_name()`, which reports the TUNER chip ("Generic RTL2832U
OEM") and is identical across dongles that need opposite settings.

**Why capability profiles rather than per-radio fiddling.** HF works differently on different
hardware, and getting it wrong looks like a broken receiver rather than a wrong setting:

| Hardware | How it reaches HF | What the profile should do |
|---|---|---|
| **RTL-SDR v3** | Direct sampling, Q branch | Auto-enable direct sampling below ~24 MHz; expose the control |
| **RTL-SDR v4** | Built-in upconverter | Direct sampling controls LOCKED OUT — using them is simply wrong here |
| **Any + external upconverter** | External mixer | Offset frequency entered once; tuning is transparent thereafter |
| **Plain RTL2832U** | No HF | Hide HF entirely rather than offer a band it cannot hear |

A profile is therefore **a named bundle of capability facts and control policy**, applied to one or
more radios. Two v4s share one profile; the two v3s share another; nothing is typed twice.

**Deliberately NOT OWRX-style profiles.** OWRX profiles are *band/frequency* presets that switch a
shared receiver's tuning, and switching one retunes the radio for everybody. These are *hardware
capability* descriptions — they do not change frequency, they change what the client is allowed to
ask for and what the server does automatically. Nobody's listening is disturbed by another user
selecting one, because a radio has one user (§1).

**Detect, suggest, never assume.** On seeing a new dongle the GUI proposes the matching profile from
the USB product string ("This looks like an RTL-SDR Blog V4 — use the V4 profile?"). The owner
confirms. Auto-applying would be exactly the failure §5.1 exists to prevent: an unrecognised or
misidentified dongle must still land on SAFE DEFAULTS, not on a guess. Clones lie about their
descriptors, so the string is a hint, never proof.

**What a profile carries** (draft — settle when built): direct-sampling policy (`off` / `manual` /
`auto below N Hz`), upconverter offset, tunable frequency range, bias-T policy, gain range, and
which of these the listener may touch versus admin-only versus locked.

### 5.2.1 The profile GUI, as Stuart specced it

**A profile is radio capability AND installation.** Not the model alone — two identical V4s can need
different profiles because of what they are plugged into. This is why model detection may only ever
SUGGEST (§5.2): the software can read the dongle, but it cannot see the coax.

**The flow:**

1. **Create SDR profile** → name it, e.g. *VHF/UHF*.
2. Every RTL-SDR setting is present, and for each one the owner chooses: **listener may change it**,
   or **admin only**. (This is foundations §5.4's per-control policy, surfaced as a page rather than
   scattered through the UI.)
3. Set the **frequency range** — e.g. FM broadcast upwards — with the ability to **block bands**, so
   an operator can stay the right side of local law.
4. **Lock out bias-T** — this antenna is not powered.
5. **Lock out direct sampling** — no HF on this profile.
6. **Allow gain, but cap it.** The owner knows this receiver overloads above 29.6 dB, so the
   listener gets the control with a ceiling rather than an argument.
7. Save, then **add a radio to the profile** — here, the V3.

**Worked examples, all Stuart's:**

| Situation | Profiles | Why |
|---|---|---|
| One V3, unpowered VHF antenna | 1 — *VHF/UHF* | Capability and installation both match |
| Two V4s, **one** supplying bias-T to a shared powered antenna, the other behind a DC block | ~~2 profiles~~ → **1 pooled profile, bias-T set per radio** (SUPERSEDED — see §5.2.2) | To a listener they are identical; the difference is installation, which is per-radio |
| Two V4s, antenna powered EXTERNALLY | **1** — *V4 Wideband*, both radios | Identical capability and identical installation, so nothing to separate |

★ The middle case is precisely why **unique serials matter** and why the rule in §5.0 exists: serial
`00000001` supplies the power, `00000002` sits behind the block, and each is bound to its own
profile. Had they shared a serial, a reboot could swap them — an unpowered LNA and DC into a block.
With unique serials, bias-T may be restored automatically; without, it starts off (§5.0 rule 2).

**Relationships:** a profile holds MANY radios; a radio belongs to exactly ONE profile. That is what
makes the third case a single edit rather than two, and it is why profiles are worth having at all —
a user with four identical dongles on one antenna configures them once.

### 5.2.2 ★★ A PROFILE IS A POOL — and installation differences do not split it

Stuart, refining the model:

> "If all radios are equal — say all v4s on an antenna distributor, same signal, same settings — all
> 4 are assigned to one profile and the server operates blind: a user connects, here's a radio, they
> don't know if it's 1 or 4. Radio identity only matters if specific radios have specific
> requirements, so an HF-only one needs to be selectable by the user, and VHF/UHF-only ones too. In
> the scenario where both radios share the same antenna and the only difference is the bias-T, we
> could do a linked profile so both are in the same pool with the same capabilities."

This is `VibeServer-MultiClient-Brief.md` §3 — **pool vs picker is DERIVED, never selected** — with
the missing piece supplied. Restating it in these terms:

- **A profile IS a pool.** Radios sharing a profile are interchangeable to a listener, so the splash
  offers the POOL and hands out whichever radio is free. Four v4s on a distributor look like one
  entry that happens to serve four people.
- **The splash appears only when there is a real choice** — i.e. more than one pool. One pool, one
  entry, no picker (§3.0). Cards lead with COVERAGE, not hardware: *"HF 0.5–30 MHz · 80m OCF
  dipole"*, never a dongle serial.
- **Identity matters to the USER only where capability differs.** An HF-only receiver and a
  VHF/UHF-only receiver are different pools, because choosing wrongly wastes the user's time.

### ★ The correction this makes: installation differences are PER-RADIO, not per-profile

Earlier in this brief (§5.2.1) the two-v4 case — one supplying bias-T, one behind a DC block — was
described as needing TWO profiles. **That was wrong, and Stuart's "linked profile" is the better
answer.** To a listener those two radios are identical: same antenna, same coverage, same controls.
Splitting them into two pools would force a meaningless choice on someone who cannot know which to
pick, and would halve the value of having two radios — each pool of one is either free or busy,
where a pool of two absorbs a second listener.

So:

- The **profile** carries everything the LISTENER experiences: coverage, band blocks, gain ceiling,
  which controls are theirs, direct-sampling policy.
- Each **radio** carries its own INSTALLATION facts, which the listener never sees and never
  chooses: bias-T on this one and off that one, an upconverter offset if only one has it, a per-unit
  gain trim.

One pool, four radios, three of them bias-T off and one on — perfectly coherent, and invisible
where it should be.

★ This does NOT weaken §5.0: bias-T is still per-radio, still default-off, and still only
auto-restored to a radio identified by a unique serial. Being a per-radio setting inside a shared
profile is precisely why the serial matters — the pool cannot tell you which physical stick feeds
the powered antenna, but the serial can.

### ★ An unassigned radio does not serve

A dongle that is plugged in but belongs to no profile has **no policy**: no frequency limits, no
band blocks, no gain ceiling, no bias-T decision. It must therefore NOT start serving. It appears in
the GUI as a new receiver awaiting a profile, and the owner assigns it.

Serving it with defaults would mean exposing a receiver whose legal limits and hardware policy
nobody has set — which is the opposite of what the profile system is for.

### ★ Band blocks are enforced SERVER-SIDE

Frequency limits and blocked bands exist for legal and hardware reasons, so they cannot live in the
client. The client is TOLD the permitted ranges (so it can grey out what it must not offer — never
show a control we would silently ignore), and the shim REFUSES a tune outside them regardless of
what any client asks for. A modified or third-party client must not be able to tune an operator into
trouble.

### ★★ REVISED DECISION: an EEPROM serial writer, gated hard

**First position (2026-07-22):** do not build it — bricking risk, no value added over
`rtl_eeprom -s`, and the port-path fallback means nobody NEEDS a unique serial.

**Revised the same evening, by a case that breaks the reasoning.** Stuart:

> "If I have 2 RTL-SDR V4s sharing 1 antenna and I am using one of the bias-Ts to power that
> antenna and the other has a DC block on it, I don't want to create a profile with the bias-T
> enabled and one with it disabled and have the sticks switch profiles on reboot — and you then
> have a bias-T sending power into a DC block and an antenna receiving no power."

Port-path identity is stable per SOCKET, so it holds only while nothing physically moves. Here the
configuration encodes a fact about the COAX — which lead has the DC block — and if the two dongles
swap sockets, the software cannot tell, because their serials are identical. The result is a
powered LNA left dead and DC pushed into a block. That is not a convenience gap; it is a hazard
that only a unique serial removes.

(Stuart's own dongles already have unique serials, set for OpenWebRX, which requires them — further
evidence that anyone running several receivers ends up needing this.)

**Unique serials are the RECOMMENDED path, not a power-user extra.** Stuart:

> "A user would have to remember the exact USB port order every time, which is not recommended and
> not a good experience. Serial numbers coded to the radio is the foolproof way … if we can give
> the users a GUI to change the EEPROM in a foolproof and as safe as we can make it way then that
> is the best thing to do."

Port-path identity is a FALLBACK that keeps a stock setup working. It is not a good answer for
anyone running more than one dongle, because it silently transfers a hardware-safety guarantee onto
the user's memory of a cable arrangement — six months later, at boot, with nobody watching. The GUI
should therefore actively OFFER to set serials at the moment the hazard appears: two dongles with
the same serial, and different bias-T settings between their profiles.

**So: build it, as an advanced tool, gated hard.**

- **Admin password required**, and a typed confirmation — the user types the NEW SERIAL to proceed,
  not just "OK". A slip should not be able to reach the hardware.
- **Explicit warning, in plain words:** this permanently modifies the receiver; do not unplug it or
  power off the machine while it is happening; if interrupted the dongle may be unusable.
- **The radio must be STOPPED first** — never write to a device that is streaming.
- **Only the serial is touched.** Vendor, product and every other byte are written back unchanged.
- ★ **Refuse on an unrecognised EEPROM layout.** Clones exist with different layouts, and writing a
  known offset into an unknown map is precisely how a dongle is bricked. Read first, verify the
  layout matches what we expect, and refuse politely if it does not — `rtl_eeprom` remains
  available to anyone who wants to take that risk themselves.
- **Read back and verify** after writing, and say plainly whether it took.
- ★ **ONE DONGLE PLUGGED IN AT A TIME.** The single biggest foolproofing measure, and cheap: with
  two identical dongles attached, the software knows which handle it holds but the USER cannot know
  which physical stick that is — and writing the wrong serial to the wrong stick recreates exactly
  the mix-up this exists to prevent. So the flow is: *"Unplug every receiver except the one you want
  to name."* It is a one-time setup task; the friction is worth the certainty.
- **Offer a sensible name, require it typed.** Suggest `VIBE-01`, `VIBE-02`… so nobody has to invent
  a scheme, but still make the user type it to confirm. Suggestion removes the blank page; typing
  removes the slip.
- **Say that a replug is needed.** The dongle re-enumerates with its new serial only after being
  unplugged and reconnected; without that sentence the user reasonably thinks it did not work.

### ★ AND A RULE THAT PROTECTS PEOPLE WHO NEVER USE IT

The writer helps those who run it. This protects everyone else, and should exist regardless:

**Bias-T is never auto-applied at startup to a radio identified only by port path.** If identity
rests on which socket a dongle is in — i.e. serials collide — bias-T starts OFF and must be enabled
deliberately for that session. Only a radio identified by a UNIQUE SERIAL may have bias-T restored
automatically on boot.

The reasoning is Stuart's scenario exactly: the failure is silent, it is physical, and it happens at
boot when nobody is watching. Making the dangerous setting require either a provably-identified
device or a human present removes the whole class of accident. Direct sampling deserves the same
treatment on the same grounds, though its failure costs reception rather than hardware.

### Hot-plug and process assignment

Each radio's process is bound to a **resolved identity**, not to whatever appears next:

- A dongle appearing that matches a configured radio → that radio's process starts (or reclaims it),
  with that radio's config.
- A dongle appearing that matches nothing → offered as a new radio; nothing is auto-assigned.
- A dongle disappearing → that radio reports offline and its process waits for its OWN device to
  return (already implemented in the shim's hot-plug watch, which matches by serial and refuses to
  grab a different dongle).
- Two radios must never resolve to the same physical device; the hub rejects that configuration
  rather than starting two processes fighting over one dongle.

## 5.3 ★ TWO SETTINGS PANES, split by what the thing actually is

Stuart: *"we basically hide everything behind the initial server setup … we have a specific
network/DSP settings pane with all the FPS / idle / adaptive link management, and the physical SDR
hardware stuff is on its own page with the antennas in a flow chart."*

| Pane | Contains | Because |
|---|---|---|
| **Network & DSP** | Waterfall rate ceiling, idle-saver policy, adaptive link management, bandwidth ceiling, locked sample rate, port | These are about DATA and the CONNECTION. Bandwidth belongs here, not with the hardware — it is a network cost before it is a radio setting |
| **Hardware & Antennas** | The flow chart (§5.5), bias-T, PPM, direct sampling, gain range, per-radio installation facts | These are about the PHYSICAL STATION — what is plugged into what, and what it is safe to do |

Everything a listener sees is a consequence of these two, decided once at setup. Nothing that can
damage hardware, break the law, or cost the owner money is reachable from a client.

## 5.4 ★★ WHAT A LISTENER ACTUALLY GETS: gain and bandwidth. That is it.

Stuart: *"realistically all a client needs access to is the gain slider and bandwidth controls. That
is it."* And: *"if we can identify the ones that don't have bias-T, like my Nooelec NESDR v5, then we
don't even show the bias-T option."*

This is a real simplification. Foundations §5.4 imagined a per-control matrix — every hardware
setting marked open / admin / locked. In practice the honest answer is much shorter:

| Control | Who | Why |
|---|---|---|
| **Gain** | Listener, within the owner's ceiling | Genuinely varies with what they are listening to |
| **Bandwidth** | Listener, within the owner's cap | Theirs to trade against noise |
| Frequency, mode, volume, squelch, NR | Listener | Not hardware — this is just *using the radio* |
| Sample rate, PPM, AGC, direct sampling, bias-T | **Owner only, always** | Station-wide decisions with hardware or legal consequences |

So the wizard's "choose what the user can access" step (§4.5 step 5) shrinks to roughly two
questions: *may they change the gain, and up to what?* and *may they change the bandwidth, and up to
what?* Everything else is simply not a listener's concern, and does not need a policy setting at all
— which removes a whole category of configuration nobody wanted to fill in.

### ★ Direct sampling should be AUTOMATIC, not a client control

Stuart wondered whether the client might get direct-sampling mode as well. The brief's
recommendation is **no — and not admin-only either, but automatic.**

Direct sampling is not a preference, it is a MECHANISM: on a v3 it is simply how HF reaches the
tuner, so it should follow the frequency (on below ~24 MHz, off above) exactly as §4.5 step 4
configures it. A listener toggling it can only ever break their own reception — switch it off on HF
and the band goes silent; switch it on above 24 MHz and everything else does. There is no listening
situation in which the wrong setting is the one they wanted.

So it is derived, not chosen. The owner decides the POLICY once per radio in the wizard (auto below
N, always off, manual); the client sees the result and no switch. On hardware where it does not
apply at all — a v4, with its built-in upconverter — it is absent entirely, per the rule below.

★ **And hide what the hardware cannot do.** If a radio has no bias-T circuitry — several Nooelec
NESDR models, only the "SMArTee" variants carry it — the option is not disabled, it is ABSENT. Same
rule as a pinned sample rate hiding the rate picker, and the enforced idle saver locking its toggle:
never show a control we would silently ignore, and never make an owner wonder whether they simply
have not found the right checkbox.

The capability list per model (§5.2's detection table) is therefore what drives the UI, not just the
recommendations: it says what this hardware HAS, and the GUI shows only that.

## 5.5 ★★ THE GUI AFTER SETUP: a flow chart of the station

Stuart's design: once the wizard has run, the GUI is **picture-based** — antennas along the top,
a line down from each, splitting into the radios beneath. Click a radio to give it a specific
setting. Drag a radio onto a different antenna after a hardware shuffle.

His own station as the worked example — an **LZ1AQ** loop feeding **2 × RTL-SDR v4**, a **Nooelec
NESDR v5**, and an **SDRplay RSP1B**:

```
                    ┌──────────────────────────┐
                    │   ANTENNA — LZ1AQ        │
                    │   0.5 – 30 MHz           │
                    └────────────┬─────────────┘
             ┌───────────────────┼───────────────────┐
   ┌─────────┴─────────┐  ┌──────┴──────┐  ┌─────────┴─────────┐
   │  2 × RTL-SDR v4   │  │ NESDR v5    │  │  SDRplay RSP1B    │
   │  shared settings  │  │             │  │                   │
   └───────────────────┘  └─────────────┘  └───────────────────┘
```

**The branches group by TYPE, and same-type radios share one settings block.** That is the pool
(§5.2.2) made visible: two v4s are configured once, not twice, and the saving is obvious on screen
rather than being a concept the user has to hold in their head.

### What the picture gives us for free

- **Pools are visible.** A branch with two radios under it IS a pool. Nobody has to be taught the
  word.
- **Per-radio overrides have an obvious home.** Click the individual radio, not the branch — which
  is exactly the profile/installation split (§5.2.2) expressed as a click target.
- **"Unassigned radios do not serve"** becomes self-evident: a dongle attached to no antenna sits
  loose at the bottom, plainly not connected to anything. The rule needs no explanation because the
  picture already says it.
- **It accommodates non-RTL hardware without redesign** — the RSP1B is simply another branch, even
  though driver support is future work (§5.9).

### ★★ PREFER THE RADIO THAT IS ALREADY IDENTIFIABLE — do not ask for an EEPROM change you can avoid

Stuart: *"if a server owner adds a load of RTL-SDRs and tells one to be the bias-T power, and we
detect all the serials are the same, we tell them to change the EEPROM. But if one or two have
unique serials or identifiers — the one v3 among three v4s — then we highlight that radio in a
different colour and advise using that one to supply bias-T, as it already has a unique identifier
and no EEPROM mods needed."*

The best possible handling of a risky operation: **make it unnecessary.** At step 3 of the wizard
(§4.5), when the user says a radio powers the antenna:

1. **Is any attached radio already uniquely identifiable?** Either by a unique serial, or by being
   the only one of its model on that antenna (the lone v3 among v4s — §5.2's table).
2. **If yes → highlight it and recommend it**, with the reason: *"This receiver can already be told
   apart from the others, so bias-T will be applied to the right one every time. No hardware change
   needed."*
3. **If no → then, and only then**, offer the EEPROM route (§5.2 revision) for whichever radio they
   choose.

★ **BUT FILTER BY WHETHER IT CAN ACTUALLY SUPPLY BIAS-T.** Not every RTL-family dongle has the
circuitry: RTL-SDR Blog v3 and v4 do; several Nooelec NESDR models do NOT (only the "SMArTee"
variants carry it). Recommending a stick that physically cannot power an antenna would send the
owner chasing a fault that is not in the software. So the candidate list is:

> uniquely identifiable **AND** bias-T capable **AND** on this antenna

and if that list is empty, fall through to the EEPROM offer without pretending otherwise.

**It stays advice, never enforcement.** The owner may have a physical reason it must be a particular
stick — which port of the distributor it is on, cable lengths, or simply which one they can reach.
If they pick a different radio, accept it and offer the EEPROM route for that one instead.

### The visual language

Stuart: *"radios supplying bias-T power are highlighted yellow with a little lightning bolt next to
them. We could even do line diagrams of each radio so it looks like you are clicking and dragging an
RTL-SDR USB stick. Make it really visually clear to the server owner."*

**Bias-T: yellow, plus a ⚡ glyph.** ★ The glyph is not decoration — it carries the meaning WITHOUT
relying on colour. Roughly one man in twelve has some colour vision deficiency, and this is the one
state in the whole UI where "I didn't notice" has physical consequences (§5.0). Colour catches the
eye; the glyph is what actually says it. Both, always.

That also satisfies §5.0 rule 4 — hardware-affecting settings must be **visible with attribution** —
at a glance, across the whole station, rather than by opening each radio in turn.

**Three states worth showing on every radio**, matching what the menu-bar app already distinguishes:

| State | Shown as |
|---|---|
| Supplying bias-T | Yellow + ⚡ |
| In use by a listener | Solid / lit, with the listener count where a pool serves several |
| Offline (unplugged or failed) | Greyed with a warning glyph — the same condition the client banner reports |

**Line art per model.** Drawing a recognisable RTL-SDR stick, a NESDR and an RSP makes the chart a
picture of the actual station rather than a diagram of abstract boxes — and it makes a drag feel
like moving the physical thing, which is the point of the gesture. Note this is already a solved
problem here: `spike/tools/make_family_icons.py` generates the brand's green line-art family, so
per-model artwork should extend that generator rather than start a new visual language.

### ★ THE HAZARD DRAG-AND-DROP INTRODUCES

Moving a radio to a different antenna changes its **installation facts**, and one of them is
dangerous. If the dragged radio was the one supplying bias-T, that setting must **NOT** travel with
it — dropping it on another antenna would start pushing DC into a feed nobody asked to power, which
is precisely the accident §5.0 exists to prevent.

So a move:

1. **Resets hardware-affecting settings to safe defaults — bias-T is switched OFF automatically**,
   and stays off until the server owner deliberately sets it again on the new antenna (Stuart,
   confirmed). It is never carried across a move, under any circumstances.
2. **Re-asks the antenna's question:** *"Does one of these radios power this antenna?"* — because
   the answer has genuinely changed for BOTH antennas involved.
3. Keeps harmless settings (gain trim, an upconverter offset that belongs to the dongle rather than
   the feed) since they follow the hardware, not the coax.

A drag is a physical statement — *"I moved this lead"* — and the software should respond as if the
user had just rewired the station, because they have.

## 5.9 ★ WHAT IS RTL-SHAPED HERE — the assumptions that break when other SDRs arrive

Stuart: *"it will get more complex when we support other SDRs in the future, but right now with the
RTL and its variants like the Nooelec NESDR v5 it should be super simple."*

Right, and the design should stay simple for the hardware we actually support. But it rests on
assumptions that are true of RTL dongles and not of SDRs generally, and they are much cheaper to
list now than to rediscover mid-build:

| Assumption | Why it holds for RTL | What breaks |
|---|---|---|
| **One radio = one user** | ~2.4 MHz around one LO; nothing to slice between users (`VibeServer-MultiClient-Brief` §0) | An RSPdx or Airspy sees 10 MHz. Several users COULD take independent VFOs from one device — which reopens the whole pool model, not just a setting |
| **Serials collide** | RTL EEPROMs ship with `00000001` | Most other SDRs have genuinely unique serials. The EEPROM tool and the port-path fallback become RTL-only concerns |
| **HF needs direct sampling or an upconverter** | v3 Q-branch, v4 built-in upconverter | Airspy HF+ and the RSPs cover HF natively; the wizard's step 4 becomes "not applicable" rather than a recommendation |
| **Gain is one number in tenths of a dB** | Single tuner gain stage | RSP has LNA state + IF gain; Airspy has several stages. "Cap the gain at 29.6 dB" stops being expressible |
| **Bias-T is a boolean** | On or off, one voltage | Varies by device, and some have none |
| **librtlsdr is the driver** | One library, one API | Each family brings its own SDK, and some are not open |

★ **The first row is the load-bearing one.** Everything in this brief — pools, one user per radio,
per-radio isolation by process — follows from RTL's 2.4 MHz window. A wideband SDR does not merely
add a driver; it invalidates the premise that made the architecture necessary. That is a design
conversation to have deliberately when the time comes, not a porting exercise.

### RTL variants are NOT a special case — but their descriptors differ

Nooelec NESDR (SMArt v5 and friends), generic RTL2832U sticks and the RTL-SDR Blog units are all the
same silicon and the same driver. The only thing that varies is the USB descriptor the model
detection reads (§5.2): `NooElec` / `NESDR SMArt v5` rather than `RTLSDRBlog` / `Blog V4`.

So model detection must be a small TABLE of known descriptor strings mapped to capabilities, not a
match on "Blog". And anything unrecognised falls through to the safe path already specced: no
assumption, safe defaults, and a plain question to the user (§5.2, §5.1).

## 6. Admin and lockdown

The two-level model already agreed (foundations amendment) applies per radio:

- **Access PIN** — optional, per radio. Different radios may have different audiences.
- **Admin password** — global. It means "this is my server", and it gates: enabling/disabling
  radios, ports, ceilings, hardware policy, and the update settings.
- **Hardware policy per control, per radio** — `open` / `admin` / `locked` for bias-T, gain, direct
  sampling, plus gain min/max (foundations §5.4). Bias-T especially: it puts DC on the feed, and a
  locked control must mean locked *to the listeners*, who are the people holding the PIN.
- **The host machine is never challenged** (loopback exemption, already shipped).

## 7. ★ What this does NOT solve, and must be said plainly

Isolation is **across** radios. **Within** one radio, listeners still share the tuner — because one
dongle has one LO. Two people on Radio 1 cannot listen to different frequencies; that is the control
token's job (foundations §5), not something more processes can fix.

Likewise **within** one radio the FFT rate and bandwidth are engine-wide, so one listener's throttle
does affect the others on *that* radio. Whether per-client spectrum rates are worth building is a
foundations question and should be answered there, not assumed here.

Stuart's requirement — *"user 1's reduction must not affect user 2"* — is fully met for users on
DIFFERENT radios, which is the model the multi-client brief already chose.

## 8. Acceptance criteria (draft)

1. Two dongles → two child processes → 48001 and 48002, each independently tunable, with the
   landing page at 48000 listing both and their live state.
2. **Isolation proved:** set Radio 1 to Quarter fps and a 5 kHz bandwidth cap while a client on
   Radio 2 runs full rate; Radio 2's measured fps and data rate are unchanged.
3. Kill Radio 1's process; Radio 2's listener notices nothing, and the hub restarts Radio 1.
4. Unplug a dongle → that radio reports offline, the others are unaffected, replug recovers.
5. Both radios appear separately in the phone/watch picker over Bonjour with no client changes.
6. Admin gates what it should: a non-admin cannot change ports, ceilings, or a `locked` bias-T; the
   host machine is never asked for a PIN.
7. **Identity:** with UNIQUE serials, moving a dongle between USB sockets keeps its settings. With
   COLLIDING serials (the stock case), settings follow the socket and the GUI has said so. In
   neither case does a listener reach a different receiver than the one they chose.
8. **Safety:** a dongle that matches no configured radio starts with direct sampling OFF and bias-T
   OFF, and never inherits another radio's settings.
9. Measure and record RAM per radio process on a Pi before advertising a maximum radio count.
