# DMR ID → callsign (V12 beta90)

Stuart, 2026-11-21: "no callsigns on this dmr just weird numbers". DMR carries a 24-bit source ID and a destination,
which is a talkgroup or another ID. The numbers are **RadioID.net** registrations. The only callsign DMR itself carries
is the optional **talker alias**, which `vibe_dvsig.h` already decodes, and most radios never send one.

## RESUME HERE

- **Built:** `android/app/src/main/cpp/vibe_dmr_id.h`, a per-ID lookup behind an **owner switch that is OFF by default**
  (`dmrIdLookup`). The DV ID box now shows "GI4SZW (2355254) → TG 2351", on the amateur bands only, and the alias wins.
- **Waiting on:** RadioID.net's approval before an owner of a PUBLIC server should switch it on (see "The terms").
  Stuart, or the project, should ask RadioID. Until then the switch stays off by default and its help says to leave it
  off on a public server.
- **Talkgroup names are not shown.** RadioID publishes no talkgroup list to look up. Its talkgroups appear only inside
  repeater answers. "TG 2351" stays a number.

## The terms (RadioID.net "API and Data Use Policy", radioid.net/api, read 2026-11-21)

Quoted as the page gives them:
- "This API, the static dump files, and any screen-scraped reuse of RadioID data are covered by the same policy."
- "Normal lookup use is allowed."
- "Do not mirror or re-publish RadioID as a public feed, bulk export, reusable database, competing directory, …"
- "Commercial, bulk, mirrored, scraped, or public-facing redistributions require approval."
- "Commercial, bulk, and public-facing integration uses may require approval."
- "Competing directory, search, mapping, or export services are not allowed without written permission."
- "Automated clients should identify themselves with a clear User-Agent and contact address."
- "We may rate-limit, require authentication, revoke access, or block abusive clients at any time."
- "Be gentle. Excessive requests, bulk mirroring, competing uses, or screen-scraping for reuse may be blocked."
- "Limits and authentication enforcement will be introduced separately at a future date."

The full text is at radioid.net/api_use_policy, which is behind a browser check that our tools could not pass. The lines
above are from the API page itself.

**What that means for us:** the brief's plan was for the directory Worker to mirror the user and talkgroup lists daily
and serve lookups by ID. That would be a mirrored, public-facing redistribution, which needs approval, so **no mirror was
built**. Instead each server looks up each new ID once through RadioID's own per-ID API, which is normal lookup use. It
is behind the owner's switch because a public server shows the answer to its listeners, and "public-facing integration
uses may require approval".

## How it works

- **The lookup:** `https://radioid.net/api/dmr/user/?id=N`, one request at a time, at most one every 2 s. Only 7-digit
  user IDs are asked about (not repeaters, all-call or 0). Only `callsign` is read from the answer, and it is
  whitelisted (A–Z 0–9 /, 3–12). The name, city and other fields in the answer are never kept.
- **Bounded memory:** a cache of 1024 answers (the least recently used goes first), a queue of 32 IDs, found callsigns
  asked again after a week, unknown IDs after a day. A failing service is backed off from 1 minute, doubling to an hour,
  and a block page is not an answer. The 270k-entry list is never held in a Pi's RAM.
- **The report:** `dvsig::Listener::status` passes the identities copy, and only on an amateur band, through
  `Lookup::annotate`. Each DMR entry gains `"srcCall"` (and `"dstCall"` for a private call; a talkgroup is never looked
  up). An answer that lands later raises the report's version, so the box redraws. NXDN IDs are a different namespace
  and are not touched.
- **The clients:** `src/services/dvsig.ts` (web + app) and `handheld-ui/src/dvsig.h` (R36S). `dvWho` shows
  "CALL (ID) → TG n", or "CALL (ID) → CALL (ID)" for a private call. A talker alias wins as the name, and the ID then
  stays a number beside it. A looked-up callsign is whitelisted again and never read from a public copy.
- **User-Agent:** the daemon sends `VibeServer/<version> (+https://vibesdr.net)` through curl; Android's VibeHttp now
  sends `VibeServer (+https://vibesdr.net)`. This is the project's web address and never a person's.
- **Settings:** `dmrIdLookup` (ServerConfig, machine-level). On the Setup page it is in the **PSK Reporter** card. On
  Android it is on the remote setup page (D_RADIO: not an Icom, which has no DV ID) and on the ServerModeScreen **PSK
  REPORTER** card.

## Tests

- `vibeserver/test-dmr-id.cpp`: **29/29**, with a fake fetcher whose answer has the shape of a real one captured
  2026-11-21 (personal fields replaced). It covers the parse; off by default; each new ID asked once; the 2 s gap; misses
  kept a day; the backoff; the cache bound (3000 IDs give 1024 kept, newest kept); the queue bound; the annotation (DMR
  only, never a talkgroup, never NXDN).
- `scripts/test_dvsig.ts`: 32/32, including the five new DMR lines.
- One real lookup was made by hand once (ID 2355254, the brief's example) to see the answer's shape. Nothing else
  touched RadioID.
