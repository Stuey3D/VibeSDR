# Must ship in the NEXT released version

Written 2026-08-18, while 10.3.1 (build 140) was in review. These are changes that
exist in the tree but are NOT in the build users will get, so they are invisible
until something deliberately carries them forward.

## 1. The in-app release note for 10.3.1 is WRONG in the shipped build
`src/components/AboutOverlay.tsx` — the V10.3.1 entry in build 140 blames the
local-address silence on the session-registration bug. That bug was real and is
fixed, but it is **not** what was silencing UberSDR over a LAN address: the audio
connection could not reach a plain `ws://` on a private address at all, and the
cure was the transport fallback (`d2dd42d9`).

The corrected note is already in the tree. It must reach a RELEASED build, or the
app keeps explaining its own most visible fault with the wrong reason.

★ This is the AGENTS.md rule about copy that tells a user where something is,
  applied to copy that tells them WHY: verify the whole sentence, not the part
  you came to fix. The first note was written before the cause was known and read
  as settled fact.

## 2. A guard in the phone's delta decoder (already in the tree, NOT urgent)
`src/services/UberSDRClient.ts` — the phone sizes `bins` from the config's `binCount`,
which is exactly why UberSDR's delta-only encoder never broke it. Jr's port of the
same decoder omitted that line and went black (2026-08-18). The added guard also
sizes the array if a delta arrives BEFORE a config — the one ordering the phone
does not currently defend against.

★ Deliberately NOT shipped as its own release: nothing is broken. Stuart,
  2026-08-18: "if the phone doesnt need the fix then keep it as it is."

## 3. The in-app notice endpoint
See the `app_notice_endpoint` memory. Agreed 2026-08-18: `/api/notice` on the
Worker that already serves the website, version-gated and fail-silent, so a known
fault can be announced to users instead of one GitHub issue being the whole
channel.

## 4. Jr: "spectrum lost" flashes on a server you have not visited before
Reported 2026-08-18 after build 42 shipped to TestFlight. On a FRESH server there is
no saved view, so the server starts at its own default centre and Jr asks once to move
it to the VFO — one legitimate round trip. The overlay reads the gap as a stopped
spectrum and flashes "Lost the server" for a second before the first row paints.

★★ The delay is correct behaviour; the WARNING is the bug. ContentView already draws
   this distinction for reconnects — "a recovery IN PROGRESS is not a failure, and must
   not be drawn as one" — and simply does not extend it to a first subscribe. Suppress
   the overlay while a subscription is in flight (a sendView within the last ~3 s with
   no frame yet).

★ Stuart: "not the end of the world as it seems to connect a few seconds later" — so it
  waits for 1.3.2 rather than churning a build while 1.3.1 is in review.

## 5. The "already on another radio" refusal takes too long to appear
Stuart, 2026-08-18: connected on the Mac, then tried the RSP1B from the web client —
correctly refused by the one-radio-per-IP rule (`occHeldElsewhere`), "but the warning
took ages to come up".

The server decides this INSTANTLY, at the handshake, and names the radio you are
already on. Whatever the client does with that answer, it is not showing it promptly —
so a refusal that should read as "you are already listening on the Mac" reads as a
broken radio for several seconds. Check the refusal path end to end: the preflight
answer, the WS close reason, and what the picker/landing page does while waiting.

★ Worth doing BEFORE the soft-limit work, which adds more refusal states (waiting,
  borrowed time, evicted-for-a-waiter). A refusal nobody sees quickly is the fault
  that made tonight feel like an outage.

### DONE 2026-08-19 — it was never SLOW, it was never SENT

The client was not being slow with the answer; it never received one. `sendWs()`
only QUEUES — `outboxOpen()` runs at the top of `acceptWs`, so a writer thread owns
the socket from there on — and `closeAfterFlush()` clears `open_` and shuts the fd
before that thread runs, so the writer drops the frame. The client got a bare 1006
with no message and waited for something else to time out.

★★★ Three sites, all breaking a rule written twelve lines under `outboxOpen`:
    "EVERY exit from this function must call outboxClose(sock), the refusals
    included." Fixed and each measured against local radios, before and after:

- **`elsewhere`** — the reported one. Never arrived; now at 5 ms, naming the radio.
- **`evicted`** — worse. This line has now been fixed TWICE for the same symptom
  (`close()` → `closeAfterFlush()` cured an abort but not the loss). Takeover
  depends on the displaced client being told why, or it retries — the reconnect war
  `vs_takeover` was added to stop. That flag treated the symptom.
- **`session_expired`** — fires most: every session reaching the limit on the public
  server. Listeners were retrying into the cooldown and meeting "PLEASE WAIT"
  having never been told their turn was over.

★★ STILL OPEN, deliberately: the close code is 1006 (no WS close frame). Every
   client treats the message as terminal, so this is cosmetic — but it is why the
   symptom was so hard to read from the client side.

## 8. The admin table hid a backgrounded listener (DONE 2026-08-19)

Found from Stuart's report of the Pi showing one listener, no rows, and 67 kbit/s
instead of 240. Backgrounding closes the SPECTRUM socket and keeps audio playing;
`adminSessionsJson` was keyed on the spectrum socket, so the listener vanished from
the one view whose job is saying who is on. `specListenerCountLocked()` learned this
on 2026-08-17 and its comment says "two definitions of in use that disagreed" —
there were THREE. New `spectrum` field renders as "audio only" so the cheap row
explains itself.

★ Also fixed a rate that read `2147483647k`, seen live while testing: `soleLastBytes`
  lives on the Impl, not the session, so the first poll after a new occupant did an
  unsigned subtraction of the previous occupant's total.

## 6. The front door serves the WRONG PAGE first, then swaps
Stuart, 2026-08-19: *"on slower connections it looks like it is the old single user
single radio (the simple mode) splash screen which then gives way to the main landing
page."*

So the front door (port 48000, multi-radio) serves the single-radio receiver page, and
the client only switches to the radio picker once JavaScript has fetched the radio list.
Fast connection: a flash. Slow connection: several seconds of the WRONG PRODUCT, and
slow connections are exactly who arrives from a blog post on a phone.

★★ THE SERVER ALREADY KNOWS. `/vibeserver.json` answers `"frontDoor": true` at request
   time — so the handler can serve the picker page directly instead of shipping the
   receiver page and letting the browser discover it was wrong. First paint should be
   the truth, not a guess corrected later.

★ Probably also the CLS blemish in Cloudflare's Web Vitals — the only "needs
  improvement" in an otherwise green report (LCP 96% good, 675 ms page load). One swap
  of the whole page is exactly what Cumulative Layout Shift measures.

★★★ Worth doing BEFORE the RTL-SDR Blog post lands: it is the first thing every new
    visitor sees, and the last post reset the site's baseline permanently.

### DONE 2026-08-19 — and the note above got the mechanism WRONG

There is no second page and never was. The front door serves the **same single bundle** a
radio does; what looked like one page giving way to another was the splash drawing its
receiver controls (CONNECT, PIN, the listener count) and `showSplashRadios()` hiding them
once `/vibeserver/radios` had answered. "Serve the picker page directly" was not a thing
that could be done — there is nothing else to serve.

★★ So the cure is the same idea one level down: the door **stamps the page it serves**.
   `GET /` on a front-door process now injects
   `<script>document.documentElement.setAttribute('data-frontdoor','1')</script>` straight
   after the leading `<meta charset>`, and `html[data-frontdoor]` in `index.html` hides
   exactly the elements the JS hides. First paint is the truth; no fetch can beat markup.

- `android/app/src/main/cpp/local_sdr_shim.cpp` — the `GET /` handler; a lazily-built
  patched COPY, never the shared string `vibeWebPage()` hands out by reference, and only
  built inside the front-door branch so a radio pays nothing for a page it never sends.
- `web/client/index.html` — the `html[data-frontdoor]` rules.
- `web/client/src/main.ts` — `isFrontDoor` now SEEDS from the stamp instead of starting
  false. `showSplashRadios()` still sets it from the directory: that stays the authority,
  and it is the only source an older, unstamped server has.

★ Verified against a real front door (`VIBESERVER_CONFIG` on a throwaway config, port
  48133): the stamp lands immediately after the charset meta and the body is 754,473 bytes
  — the bundle's 754,397 plus the 76-byte script, so nothing else moved.

★★★ The two lists — the CSS selectors and the `hide()` calls in `showSplashRadios()` — must
    stay in step. Two places that hide the same five elements is exactly the shape that
    drifts, so a control added to one belongs in the other in the same edit.

## 7. Jr
1.3.1 carries the `.waiting` connect deadline, the IPv4 escalation and the
`stop()` serialisation. Awaiting Stuart's test before submission.

## Converter support (up/down-converters) — 2026-09-01, IN THE TREE, NOT ON AIR
Asked for by Sebastian Schmidt by email: a Ham It Up (125 MHz) in front of an
rtl_tcp receiver. Built in two independent halves that share only the idea.

**Client** (`src/services/converter.ts`, `ConverterBackend.ts`, the CONVERTER
section in `MenuSheet`): local USB, rtl_tcp and SpyServer only — gated on
`isLocal && !isVibeServer`. **Server** (`RadioConfig::converterOffsetHz`,
`tuneHw()`, the setup page): the owner sets it once and the server publishes true
RF, so no client learns a converter exists.

★★★ SHIPPING IN 10.5 — Stuart, 2026-09-02: "the converter dialogue needs to ship in this build."
So this is no longer a "before anyone is told it exists" item; it is a RELEASE GATE. The server
half is already on air in 4.1.55 (all three servers), including the setup page's model
quick-picks. The client half ships with the store build.
▶ AND IT SHIPS UNLABELLED. Stuart, 2026-09-02, on marking it beta/experimental: "we include it
and then see if we end up with issues being raised then make changes if needed."
★ No "Beta" chip: the App Store's beta rule is aimed at beta APPS rather than beta features, so a
  label was unlikely to be rejected — but it reads as "we are not sure this works", which serves
  the one person who owns a Ham It Up worst of all. The feature defaults to None and is gated to
  local/rtl_tcp/SpyServer, so nobody meets it by accident, and the existing copy is already honest
  without hedging.
▶ THE BENCH TEST IS STILL UNRUN and needs no converter hardware — LO 1000 as a DOWN-converter,
  tune 1100 MHz, and broadcast FM should appear from the dongle at 100 MHz. It stays worth doing
  at the first idle moment (it needs the test radio's allow-list cleared, or the range check
  refuses 1100 MHz for the wrong reason). Shipping first is a DECISION, not an oversight.

★★★ NOTHING HAS BEEN TESTED ON A RADIO. Typecheck, `scripts/test-converter.ts`
(47 checks) and `vibeserver/test-converter.cpp` (20 checks) only. The bench test
that needs no converter, from BRIEF-converter-support.md §8: set the LO to
1000 MHz, tune the VFO to 1100 MHz, and broadcast FM must come out of the dongle
at 100 MHz. Do that before anyone is told the feature exists.

★★ NOT BUILT, deliberately, and the reasons are recorded in `converter.ts` so
that nobody adds half of it: **inversion** (high-side LNBs) needs the IQ stream
conjugated in the DSP or every decoder fails, and **LNB presets** would put
display frequencies above 2^32 across a native bridge that is `uint32_t`
throughout. No preset ships that needs either.

★ `vibeserver` CMake `project(... VERSION …)` is NOT bumped — do that before any
apt release or the install is a silent no-op.

★ Reply to Sebastian: he owns the one thing that cannot be tested here.

## Buddy — where 229 left it, 2026-09-02
FIXED AND CONFIRMED ON DEVICE: the watch spectrum handoff (it lived inside an
AppState *transition* listener, which a headless launch never fires); the Servers
button waking the phone (its own rate limiter ate the tap, and `backToPicker()`
was a second silent door); two AVAudioEngines running at once whenever Buddy was
on the wrist; UberSDR's crown demanding an arm it does not need.
**UberSDR cold-boots from Buddy perfectly.**

★★★ STILL BROKEN, VIBESERVER PATH ONLY: the pure black screen on opening the
phone app, and needing several attempts to connect. Six theories died on this in
one day — see the memory note `buddy_vibeserver_black_screen_open` for what is
RULED OUT, so nobody re-proposes them.

▶▶ DO THIS FIRST, BEFORE ANY MORE THEORISING: our own NSLog is NOT readable off
the device, and the log archive is a ~20 s buffer — so every piece of
instrumentation added on 2026-09-01 was unreadable. The app's `Documents/` folder
IS readable (`devicectl device info files --domain-type appDataContainer`), so
write boot breadcrumbs there and read them back. One reproduce then answers it.
