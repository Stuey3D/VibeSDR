# BRIEF: the idle hand-back disconnected an ACTIVE decode

**Status:** not started. 10.0.1, alongside the decoder-box work. Stuart, 2026-07-30.

> *"I never saw our app limits for idle before — I was just connected to my UberSDR and it handed
> the connection back after 30 mins. I was decoding SSTV the whole time."*

★ **It is OURS, not the server's.** `SDRScreen.tsx`:
`const IDLE_RELEASE_MS = 30 * 60_000;  // no interaction at all for half an hour`
Not to be confused with the SERVER's session limit (the "YOUR TURN ENDS IN 29:18" countdown), which
a client cannot change.

---

## 1. ★★★ THE BUG: decoding is not counted as being there
The hand-back watches **interaction** — `markInteract()`, driven by touches — plus a `lastViewerRef`
for *"a watch or an open analyser"*. **A running decoder counts as neither.** So a receiver
decoding SSTV for half an hour with nobody touching the screen looks exactly like a phone in a
pocket, and the slot is handed back mid-picture.

★★ **This is wrong regardless of any setting, and should be fixed first.** Decoding IS use — it is
arguably the strongest evidence of use there is, because the user is waiting on a result that takes
minutes to arrive.

**Fix:** treat active decoder output as viewer activity — refresh `lastViewerRef` when a decoder
produces something (an SSTV/WEFAX line, an FT8 or NAVTEX line, an ADS-B update). Cheap, and it uses
the mechanism already there for the analyser.
★ Do NOT simply exempt "a decoder is selected" — a decoder left on a dead frequency produces
nothing, and that phone in a pocket is exactly the case this feature exists for. **Output, not
intent.**

---

## 2. ★★ THE FEATURE: a per-server override
> *"I know that my UberSDR is cool with no limits so I could disable the 30 minute idle disconnect,
> but for other servers I leave it on."*

Right shape: the setting belongs to the **server**, not the app, because the answer genuinely
differs per receiver — you own some of them.

- Store it on the **favourite / server record**, so it travels with iCloud sync like the rest.
- **Default ON, always.** ★★★ Read `memory/third_party_receiver_etiquette.md` before touching this:
  the hand-back exists because *"our Kiwi keepalive runs at 1 Hz for ever, which DEFEATS the
  server's own 'are you still there' kick. An app left running in a pocket held a public receiver's
  only slot until the process died. That is the exact discourtesy that gets third-party clients
  blocked."* A default of OFF would undo the reason it was written.
- ★ Consider offering the override only where it is defensible — your own VibeServer, or a server
  saved as a custom favourite — rather than on every public receiver in the directory. At minimum,
  word it so it is obvious the limit is a courtesy to the owner, not a restriction on the user.
- The 60-second "still here?" warning should stay whatever the setting: silently dropping a session
  is worse than asking.

---

## Order
Fix (1) first — it is a bug, it is small, and it removes most of the reason anyone would want (2).
Then (2), which is a genuine convenience for people running their own receivers.

---

## ★★ RELATED: the session countdown exists on ONE backend
Checked 2026-07-30. `sessionSecsLeft` / `onSessionWarning` live **only in `UberSDRClient`** — which
covers UberSDR AND VibeServer (same protocol). **Kiwi, OWRX, FM-DX and SpyServer show nothing.**

★ Kiwi is the gap worth closing: it is the backend most likely to time you out, and it already tells
us — `KiwiAdapter` handles `ip_limit`, and the note there records `ip_limit_mins` verified at 25
minutes a day on a live receiver. Today we only REACT when kicked ("This KiwiSDR keeps ending the
session…"); we could count down to it instead, which is the difference between a receiver that
seems to break and one that tells you where you stand.
★ See also `BRIEF-server-identity-header.md`: the countdown box currently draws over the server's
own name on UberSDR.

### ★★★ EVERY BACKEND WITH A LIMIT SHOULD COUNT DOWN — Stuart, 2026-07-30
| Backend | Limit | State today |
|---|---|---|
| **UberSDR / VibeServer** | per-session seconds | ✅ `sessionSecsLeft` → countdown |
| **KiwiSDR** | ★ daily per-IP (`ip_limit_mins`, 25 min/day verified) and per-session on many receivers | ✗ we only react to the KICK — **the one to do** |
| **FM-DX** | shared tuner; check whether the server sends any per-user clock | ✗ nothing parsed — needs a protocol check |
| **SpyServer** | check | ✗ nothing parsed — needs a protocol check |
| **OpenWebRX** | SLOTS, not a clock — its own code frees "the server slot" on pause | probably nothing to count down |

★★★ **THE RULE: only count down what the SERVER TELLS US.** Never estimate a remaining time from
when we connected, and never assume a default. A countdown that is wrong is worse than no countdown
— it either panics someone with time left or drops them with none. This is
[[feedback_no_inferred_hardware_readouts]] applied to time: if the receiver does not say, show
nothing.
★ Where a server sends a limit but no remaining time, a plain statement ("this receiver allows 25
minutes a day") beats a fabricated clock.

---

## ★★ SEPARATE AND CONFIRMED: an UberSDR LIVENESS PROBE shows as a disconnect
Stuart, 2026-07-30, on WESSEX: *"if I don't tune or do anything for a bit it asks if we are still
there… we put a disconnect message up and to be fair the reconnect button was instant, but it's not
a full disconnect, it was a server checking for life."*

★ **Confirmed theirs, not ours:** it fires at about **5 minutes** (our hand-back is 30), and the
same prompt appears in WESSEX's OWN web UI. So this is an UberSDR server feature we do not speak.

### ★★ THE STREAM KEEPS RUNNING WHILE THE PROBE IS UP
Stuart: *"I'm not sure, but I think the data keeps running whilst that message is up — the
spectrum/audio keeps running."* (His words, hedged; worth confirming, but it fits.)

★★★ **That REFRAMES the bug.** If the server keeps serving while it waits for an answer, then the
probe is ADVISORY and the disconnect only comes later, when nobody replies. So we are NOT
mislabelling a probe as a disconnect — **we are missing the QUESTION entirely and reporting only the
CONSEQUENCE.** The disconnect message was accurate by the time it appeared; the user simply never
got the chance to say "still here".

★ Which makes the fix smaller and more obviously right: surface the question WHEN IT ARRIVES,
while everything is still working, and the drop never happens. Nothing needs to intercept or
suppress the disconnect handling that already exists.
★ To confirm: watch whether frames keep arriving between the probe and the drop — if they do, the
window between the two is exactly how long the user has to answer, and that is what the UI should
show.

★★★ **We silently drop unknown messages.** `UberSDRClient` handles exactly: `pong`, `rds`,
`session_expired`, `cooldown`, `busy`, `evicted`, `session_warning`, `admin`, `rdsx`, `hwinfo`,
`config`. The probe is none of those, so it is ignored, the server gives up, and the drop surfaces
as "disconnected" — a failure message for a question.

**Fix, in order:**
1. ★ **LOG UNKNOWN MESSAGE TYPES.** One line, and it is how this class of thing gets found at all:
   a server adds a message, we ignore it, and the symptom appears somewhere unrelated. That single
   change would have identified this in minutes instead of by inference.
2. Then handle it: present the receiver's question AS a question — "the receiver is asking if you
   are still listening" with a button — not as a disconnect.
3. ★★★ **DO NOT AUTO-ANSWER IT BLINDLY.** That is precisely the discourtesy recorded in
   [[third_party_receiver_etiquette]] and in the hand-back's own comment: our Kiwi keepalive runs at
   1 Hz for ever and DEFEATS the server's own idle kick, so an abandoned app holds a public
   receiver's slot. If we ever answer automatically, answer only on real evidence of presence
   (decoder output, recent interaction, screen on) — the same test §1 above needs.

★ Get the message name from the WESSEX web client (the popup is in its own JS), or from the log in
(1) once it ships.
