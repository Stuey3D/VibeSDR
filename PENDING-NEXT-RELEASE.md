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

## 7. Jr
1.3.1 carries the `.waiting` connect deadline, the IPv4 escalation and the
`stop()` serialisation. Awaiting Stuart's test before submission.
