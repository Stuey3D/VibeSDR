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
