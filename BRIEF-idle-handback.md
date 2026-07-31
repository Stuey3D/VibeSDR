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

---

# ★★★ SOLVED FROM THE UBERSDR SOURCE — we ping the WRONG SOCKET

Stuart: *"check the UberSDR source code it may give us the answers."* It did.
`madpsy/ka9q_ubersdr` is public; `static/idle-detector.js` is the whole mechanism.

## How UberSDR actually works
1. The page fetches **`session_timeout`** from `POST /connection`.
2. The server counts that down. **`{"type":"ping"}` resets it.**
3. The browser sends that ping **ON USER ACTIVITY** — mouse, key, touch, scroll, wheel — at most
   once per 10 s, and again when the user returns after 30 s away.
4. ★★★ **It sends the ping to BOTH WEBSOCKETS — audio AND spectrum:**
```js
if (window.ws …)                       window.ws.send(JSON.stringify({type:'ping'}));            // audio
if (window.spectrumDisplay?.ws …)      window.spectrumDisplay.ws.send(JSON.stringify({type:'ping'})); // spectrum
```
5. At `session_timeout − 30 s` it shows the confirmation dialog, with 30 s to answer. **The stream
   keeps running throughout** — which is exactly what Stuart observed.

## ★★★ THE BUG
`UberSDRClient` pings **`this.spectrumWs` only**, on a 5-second interval. We never ping the audio
socket. So the session timer never sees us and drops us at `session_timeout`, with no warning,
while our spectrum pings sail past it.

★ It is the same shape as [[jr_vibeserver_release_pass]] — *"a field parsed off the WRONG MESSAGE is
SILENT"*. Here it is a keepalive sent down the wrong socket, and the symptom appeared five minutes
and one screen away from the cause.

## ★★★ MEASURED AGAINST WESSEX, 2026-07-31 — the limits arrive AT CONNECT and we discard them
Stuart: *"on UberSDR can we determine upon connection if a server requires a confirmation of life
every x mins? As we fire up the server we could warn the user."* ★★ **Yes — and we already make the
request.** One `POST /connection` to `wessex.zapto.org`:

```json
{"client_ip":"…","allowed":false,"reason":"Invalid or missing user_session_id",
 "session_timeout":240, "max_session_time":14400, "bypassed":false,
 "allowed_iq_modes":["iq48","iq96"],
 "daily_time_used_secs":0, "daily_time_remaining_secs":-1}
```

★★★ **`UberSDRClient.ts:838` throws all of it away:**
```ts
const json = await resp.json() as { allowed: boolean; reason?: string };
```
So this needs **no new request, no protocol work and no server change** — only to stop discarding
the response we already have.

### What is genuinely new here
- ★★★ **`session_timeout: 240`** — the IDLE limit, in SECONDS. **Not read anywhere today.** This is
  the one dropping people on WESSEX. Their `idle-detector.js` warns at `timeout − 30s` and allows 30s
  to answer, so the real sequence is **210 s idle → dialog → drop at 240 s**. Stuart's "about 5
  minutes" was a good estimate; now it is read from the server instead of guessed.
- ★★ **`daily_time_used_secs` / `daily_time_remaining_secs`** — a DAILY QUOTA, the Kiwi-style
  mechanism we had assumed UberSDR lacked. `-1` = unlimited for this IP.
- ★★ **It all arrives AT CONNECT**, not in a later warning message — which is what makes Stuart's
  idea work: state the terms before the user settles in, rather than only reacting when a clock is
  nearly up.

### ★ NOT new — `max_session_time: 14400` is ALREADY DISPLAYED
That is the four-hour cap, and it is the **240-minute countdown** in Stuart's screenshot. It reaches
us via `sessionSecsLeft` on `session_warning` messages (`UberSDRClient.ts:1570`), not from
`/connection`. ★ Its known bug is the overlap with the station name — see
`BRIEF-server-identity-header.md`, already on the 10.0.1 list. Do not "discover" this twice.

### ★★★ Three traps in the field semantics
1. **It is PER-IP, not per-server.** Their code: *"session timeout disabled (0) — idle detection
   DISABLED for this IP"*, and the daily counters are per-client. **Read it fresh on every
   connection; never cache across servers or assume it is constant.** A receiver may be generous to a
   known user and strict with a stranger.
2. **`0` means NO TIMEOUT — a valid value, not a missing one.** Their code uses nullish coalescing
   precisely to avoid treating 0 as absent, and defaults to **300** only when the field is genuinely
   missing. ★ Getting this backwards would put a countdown on a server that has none.
3. **The policy is returned even when `allowed` is false.** The probe above was REJECTED for a
   malformed session id and still received the full picture. ★ So the "server is busy" screen could
   tell someone what the limits are and how long they would get, instead of only turning them away.

### ★★★ SEQUENCING: DO NOT SHIP THE WARNING BEFORE THE PING FIX
We currently ping the spectrum socket only, on a blind 5-second timer. Displaying *"this receiver
disconnects you after 4 minutes of inactivity"* while that is still true would be **advertising our
own bug as the server's rule** — and it would fire for people who ARE actively listening. Fix the
ping first; then the statement is true.

### ★★ WORDING — say what resets it, without teaching people to defeat it
Stuart: *"the warning should say how to keep the connection alive — 'this server has an idle
disconnect time of 4 minutes; to maintain connection, interact with the receiver'."* ★ Right
instinct: a warning that does not say what to do about it is just anxiety. Two cautions:

1. ★★ **"Interact" is awkward for RADIO specifically.** On a web page you generate mouse movement and
   scrolling incidentally just by being present — which is why their idle detector works. On a phone
   you tune a station, put it in your pocket and LISTEN. **The most engaged user produces the least
   interaction.** An instruction to keep touching things describes the opposite of how the app is
   used.
2. ★★★ **Do NOT coach the user to defeat it.** [[third_party_receiver_etiquette]] is explicit that
   our Kiwi keepalive *"runs at 1 Hz for ever, which DEFEATS the server's own 'are you still there'
   kick"*, and that this is what gets third-party clients blocked. "Here is how to stay connected
   indefinitely" is the user-facing form of the same discourtesy. Explaining the rule is fine;
   framing it as a workaround is not.

**Proposed copy** — states the limit, says why, says what resets it, and promises the question:
> **WESSEX releases your slot after 4 minutes with no activity, so others can listen.**
> Using the controls resets the timer, and you'll be asked before the session ends.

★★ The last clause is only TRUTHFUL once the probe is handled (see "we silently drop unknown
message types"). Today the user gets no question at all, which is exactly why this reads as a fault
rather than a policy.

★ Take the number from the field, never hardcode it — 240 is that operator's choice, not an UberSDR
constant. UK English throughout ([[feedback_uk_english]]).

**Display rules:**
- ★★ **Once per server, not every connect.** A notice that appears every time becomes one people
  dismiss without reading.
- ★ **Only when the limit is short enough to matter.** A four-hour cap needs no warning; four
  minutes does. (Threshold to pick — 15 minutes is a reasonable starting line.)
- ★ Once the ping is right, a live countdown in the existing session-timer slot probably says it
  better than any warning text.

## ★★ THE FIX IS NOT "ALSO PING THE AUDIO SOCKET"
That alone would keep every session alive for ever, because our ping is UNCONDITIONAL — a 5-second
timer that runs whether anyone is there or not. That is precisely the discourtesy already on file:
our Kiwi keepalive *"runs at 1 Hz for ever, which DEFEATS the server's own 'are you still there'
kick"* ([[third_party_receiver_etiquette]]).

**Copy their model instead:**
- Ping **both** sockets.
- Ping **on activity**, throttled to ~10 s — not on a blind timer.
- Read `session_timeout` from `/connection` and show the countdown from it, so the user sees the
  clock the web client shows.
- ★ Then our own 30-minute hand-back becomes redundant ON UBERSDR — the server's own timeout does
  the job properly, which is the outcome the etiquette note actually wants.
