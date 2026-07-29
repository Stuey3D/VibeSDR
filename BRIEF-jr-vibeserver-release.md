# BRIEF — Jr ↔ VibeServer, the release-blocking set

> **STATUS 2026-07-28, Jr build 57 — VERIFIED ON HARDWARE:** PIN prompt (1),
> port scan (7), session limit + countdown (3), admin takeover BOTH WAYS (4).
> Remaining: mDNS guidance copy (2), Airspy auto-contrast (5), the per-radio
> panels beyond gain (6), and the initialising screen.

From on-air testing 2026-07-28 (Jr 48, Moto G35 VibeServer). Ordered by whether
it stops a tester getting on the air.

## 1. ★★★ PIN NEVER PROMPTS — connect by IP and you cannot get in

**Symptom.** "Checking for PIN" and it never asks. Stuart had to switch the PIN
OFF on the Moto to connect at all.

**Cause.** The PIN sheet is wired to the mDNS path ONLY —
`InstancePickerView.vibePinSheet` fires off `ad.pinRequired`, a field that comes
from the mDNS advert. A favourite (`favRow`) passes `f.pin`, and a typed IP
passes nothing. So with no mDNS there is no `pinRequired`, no sheet, and no way
to enter anything.

**What then happens is worse than nothing.** `resolveVibeAuth` still receives a
nonce (the server issues one regardless), HMACs it with an EMPTY key, and opens
the sockets with a token that must be rejected — then retries twice and reports
"reconnecting". A wrong/absent PIN is indistinguishable from a flaky link.

**Fix.**
- The client must SAY it needs a PIN: `required == true` with no PIN (or a
  rejected one) is a distinct state, not a connection failure.
- The UI must prompt on EVERY path — discovered, favourite, typed IP — with the
  saved PIN pre-filled, and offer to save it.
- ★ Do not retry a known-wrong PIN. The server locks out after too many tries
  (`lockedFor`), so a silent retry loop actively harms the user.

## 2b. ★★ mDNS ALSO dies when the Android host power-saves — cause NOT yet known

2026-07-28, Moto in power saving: `vibesdr-moto-g35.local` dead, **direct IP
connected instantly**. So the process, the foreground service and the listening
TCP socket all survived — only the mDNS answer stopped.

★ THE OBVIOUS EXPLANATION IS WRONG. Android drops multicast to an app in
power-save unless it holds a `WifiManager.MulticastLock` — but VibeServer
ALREADY acquires one on start (`VibeLocalSdrModule.kt:367`, released at :394).
So the lock is held and `.local` still stopped answering. Do not "fix" this by
adding a lock that is already there.

Worth checking next, with evidence rather than theory:
- Is the native responder's thread still alive and its socket still joined to
  224.0.0.251 after a doze transition? A dropped IGMP group membership would
  look exactly like this and would need a re-join on network change.
- Does the responder answer if queried immediately after a wake?
- Is this Doze proper (which the MulticastLock does not prevent) rather than
  WiFi power-save?

★ Either way it strengthens the decision below: tell users to enter an IP.

## 2. ★★ mDNS does not resolve over the Bluetooth link

`vibesdr-moto-g35.local` fails when the watch is on Bluetooth rather than WiFi;
an IP address works over BOTH.

**Decision (Stuart): stop advertising mDNS as a route.** Tell the user to enter
an IP. Saying ".local works" and then having it fail on the exact link a watch
most often uses is worse than not offering it. Discovery still populates the ON
YOUR NETWORK list when it happens to work — this is about the typed field's
guidance and any copy that promises `.local`.

## 3. ★★ Session time limit is invisible on Jr

The server can impose a per-listener limit and Jr shows nothing, so the
disconnection arrives from nowhere.

**Spec (Stuart's, verbatim intent):**
- On connect: state that the owner has set a limit of XX minutes.
- Show the countdown pill above the band label for the first ~10 seconds.
- Then periodic reminders at 20 / 10 / 5 minutes remaining (for a 30-minute
  limit — i.e. reminders at meaningful marks, not a fixed cadence).
- For the LAST 2 MINUTES the pill stays up until disconnection.

★ The pill is the same countdown throughout; what changes is how long it is
shown. A limit you are not told about reads as a bug in our app, not a policy of
the server owner's.

## 4. ★★ Admin override missing on Jr

The server supports an admin password that overrides occupancy. Jr has no way to
use it.

**Spec:**
- Connecting to an OCCUPIED server prompts for the admin password, exactly as
  the UberSDR/Kiwi "already in use" path does today.
- Once connected as a non-admin, the CONTROLS STAY GREYED with an inline admin
  password box beside them, so the promotion happens where the frustration is
  rather than back in a menu.

## 5. ★ Auto contrast wrong on Airspy at full zoom-out

Same defect the phone client had: the spectrum goes enormously tall because the
SDR++-style side lobes dominate the range. Fix belongs with whatever the phone
did — do not re-derive it.

## 6. ★ Radio controls are still RTL-SDR-shaped

Expected — not yet done. Jr shows RTL gain controls whatever the VibeServer
actually has (RTL / Airspy / SDRplay). See [[one_radio_assumption_family]]: this
is the same "written when there was one radio" shape, so fix it by asking the
server what it has rather than by adding a second special case.

## 7. ★★ Port scan stopped finding the server

Worked previously, not on 2026-07-28. `probeVibeServerPort` (SDRDirectory.swift:91)
fires **50 concurrent** probes across 48000–48049 with a **2 s** timeout each.

★ Suspect the LINK, not the logic: this testing was over the Bluetooth relay,
which is high-latency and low-throughput, so fifty simultaneous sockets with a
2 s budget plausibly all time out — while the same code over WiFi succeeds. That
would explain "it worked before" without anything having changed in the code.

Fix direction: batch the probes (say 8 at a time) and raise the timeout to ~5 s
on a constrained link. Slower in the worst case, but a scan that finishes is
worth more than one that is fast and finds nothing.

★ Also note the scan is SKIPPED entirely when the typed address already has a
port (`hasPort`) — correct, but worth remembering when it "doesn't run".

## Not blocking release

- FM-DX dial sync to Jr (schema rework — Jr stores one global
  `vibe.fmdx.stations`, the phone is per-server with timestamps).
- Manual floor/ceiling + hold-menu return — `BRIEF-jr-display-manual-range.md`.

## 8. ~~The session pill is main-screen only~~ — NOT AN ISSUE (Stuart, 07-28)

I flagged that DAB and ADS-B would miss the countdown. They cannot: the session
limit is a VIBESERVER feature, and DAB/ADS-B only exist on OWRX, which has no
per-listener limit. The pill can never be needed on those screens.

★ The TERMINAL screens (time limit reached, taken over) are at the app root
anyway, so they cover every screen regardless — which is right, because those
say why a connection ENDED and that can be seen from anywhere.
