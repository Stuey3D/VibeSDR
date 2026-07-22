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
