# BRIEF — a public VibeServer directory on VibeSDR.net

**Status:** design, 2026-07-28. Nothing built, nothing deployed.
**Origin:** Stuart — *"would it be possible to make a directory on VibeSDR.net and have
online VibeServers reporting to it."*

---

## ★★★ We are not inventing this. sdr.hu has done it for a decade.

Checked before designing, at Stuart's insistence (*"just need to make sure we are not
reinventing the wheel"*). The established protocol, from the OpenWebRX wiki:

> OpenWebRX sends a POST to `sdr.hu/update` to say it wants listing, **repeated every 15
> minutes**. The sdr.hu server then gets the receiver's information via an **HTTP GET to
> `/status`** on the receiver itself.

**Push to announce, pull to verify, 15 minutes.** ★ Stuart arrived at precisely this
independently, down to the interval. KiwiSDR states the same rule: *"Your Kiwi will not
be listed unless kiwisdr.com can connect and poll its status periodically."*

**Copy outright:**
- **A receiver key**, issued by the directory, that authenticates the operator.
  Receiverbook warns anyone holding it can take over the listing — so it IS the identity.
- ★ **Store keys as an ARRAY.** OpenWebRX does, so one receiver can list on several
  directories at once. Costs nothing now, impossible to retrofit gracefully.
- We already have their `/status`: **`/vibeserver.json`**, and richer — it carries
  `busy`, `limitMin` and `freeInSec`, which neither protocol has.

**★★ The ONE genuinely novel part: temporary shares.** Kiwi and OpenWebRX receivers are
permanent loft installations, so nothing in either protocol handles *"Hans is lending me
his laptop for two hours"*. That is the only place we should be inventing.

★ KiwiSDR also offers a **proxy** for people who cannot port forward at all (the CGNAT
answer). Real problem, real solution, but it needs infrastructure we would be paying for.
Explicitly out of scope for v1.

---

## Proving the address — Stuart's actual concern

> *"my concern was about making sure the DDNS is accurate and reported correctly"*

**Never trust the typed address. Prove it, and re-prove the cheap half continuously.**

Two facts are free in a Worker on every ping:

1. **What the hostname resolves to** — via DNS-over-HTTPS (`cloudflare-dns.com/dns-query`),
   an ordinary HTTPS fetch on Cloudflare's own resolver. Workers cannot do raw DNS.
2. **Where the ping came from** — `CF-Connecting-IP` on the request.

★★★ **If those two agree, the DDNS is accurate**: the hostname points at the machine
that is talking to us, right now. No probe, no cost, checked every single ping.

| resolves == source IP | last inbound check | Verdict |
|---|---|---|
| yes | passed | Listed. Nothing to do. |
| **no** | passed | **DDNS stale or wrong.** Hide, keep the entry, re-verify. |
| yes | never / failed | Address right, nothing can get IN — port forward or ISP. |

★★ **"Reported correctly" in the strong sense** — that the address reaches THIS server and
not merely *a* server — needs the server to prove it holds the key. This catches the nasty
case: a typo'd hostname that happens to belong to somebody else's working receiver.
DNS-vs-source-IP catches most of it; only this catches all of it.

★★★ **DO NOT ECHO THE KEY — the probe runs over PLAIN HTTP.** The inbound connection is to
the user's own server on their own port, so an echoed key (which IS the identity) would
cross the wire in the clear on every verification, and anyone on the path could take over
the listing. Challenge-response instead, the same scheme used three times already in this
project (PIN, admin override, watch pairing):

```
GET /vibeserver.json?dirNonce=<nonce>
→ { …, "dirProof": "<HMAC-SHA256(key, nonce)>" }
```

The key never leaves the server; a listener gets a single-use hash worth nothing.

**The accuracy chain:** claim an address → prove the hostname points at you (DNS vs source
IP) → prove the address reaches you (key echo over connect-back) → re-check the cheap half
on every ping. The expensive half runs only at registration and when the cheap half
disagrees.

### Why not a self-check on the server

Stuart's first instinct. On his network it works — **his router hairpins**, proven with
`stuey3d.freemyip.com:8073` reached from a LAN machine. It still cannot be the authority:

- **Not sufficient** — if the ISP blocks inbound at its edge, packets never reach the
  edge; they turn round inside the router. Self-check passes, public still locked out.
- **Not necessary** — many routers do not hairpin, so a correctly forwarded server would
  fail its own test.

★ Keep it anyway, as instant UX: press *List my server* and know in a second that you
typo'd the hostname, rather than waiting on a probe cycle. Just never let it decide.

---

## Registration and liveness

```
POST /api/directory/register  { url, name, location, grid, radio, contact, ttlMin? }
   → 200 { id, key, pingSec }        key = the identity, kept secret
POST /api/directory/ping      { id, key }        every 15 min
   → 200 { listed, reason? }
POST /api/directory/delist    { id, key }
GET  /api/directory                              the public list; apps fetch this
GET  /directory                                  the human page
```

**Registration** → resolve, compare to source IP, connect back, check the key echo. Listed
in seconds; appearing is never gated on a sweep.

**Ping** → DNS-vs-source-IP each time. Full connect-back only at registration, on IP
change, and once a day as a staggered spot-check (a stable IP proves the ROUTE is
unchanged, not that the FORWARD is — a router reboot can drop a UPnP mapping silently).

**Leaving cleanly** → explicit delist on stop-sharing / quit. Covers most real cases,
because someone lending a radio for an evening deliberately stops.

### ★★ Expiry, not detection

**The server declares its own TTL and refreshes it.** The list query is
`WHERE expires_at > now()`, evaluated at READ time — so a phone that goes flat, gets killed
by Android, or leaves wifi **vanishes on its own**: no probe, no cron, no write. A dead
server stops *renewing* rather than needing to be *detected*.

★ This is what makes a slow ping interval safe, and it is what makes temporary shares work.

### ★★ The IP-change grace window — ONE HOUR

Stuart: *"my DDNS has cron jobs on multiple machines to update it every 20 mins."* Plus DNS
TTL on top. Anything shorter than an hour delists working servers after every power cut.

★★★ **HIDE, do not show, while the record is stale.** A stale DDNS record does not fail —
**it resolves to the wrong host**, i.e. whoever now holds your old IP on your ISP. Sending
listeners there is worse than showing nothing.

★★ **And usually skip the wait entirely:** the ping already told us the server's TRUE
current IP. Verify inbound against that IP and keep the entry live, listed BY IP, until
DNS catches up — same router, same forward, so it should pass first time. Swap back to the
hostname when they agree. An IP change becomes "briefly listed by IP", not "gone for an
hour". The hour then covers only the real failure: stopped pinging, or pinging but
unreachable.

★ **Bare IP listings** (some operators do this): skip the hostname resolution, but KEEP the
initial inbound probe. An outbound ping says nothing about whether anything can get in.

---

## Two flavours of listing

The only structural difference is whether the entry has an end time.

- **Permanent** — register once, ping in, marked offline when it stops. As simple as
  Receiverbook. The loft Pi, the always-on Mac mini, the Moto on a charger.
- **Temporary** — declares how long it is offered for and expires on its own. Hans's
  evening; a phone in a pocket.

★ Let the sharer say how long: **1 hour · This evening · Until I stop**. The listing then
reads *"Hans — SDRplay RSP1A — available for another 40 minutes"*, which is real
information and the same idea as the per-listener session limit applied to a whole server.
It also stops anyone bookmarking a phone and wondering where it went.

---

## The directory page — a world map

Stuart's design. Leaflet, with **`leaflet.terminator`** for the day/night shadow.

★★ The terminator is NOT decoration for this audience: it IS the greyline. A DXer can see
which receivers are sitting in enhanced propagation right now, which is a reason to click
a server rather than a background texture.

| Marker | Meaning |
|---|---|
| **Green** | Permanent server, free slot |
| **Red** | No slots available |
| **Yellow** | Temporary server, free slot |

★ **Differentiate by SHAPE as well as colour.** Red/green is the most common form of
colour blindness and this legend is entirely red/green/yellow. Filled circle / ring /
diamond costs nothing and makes the map readable to everyone.

- Position from the operator-typed **Maidenhead locator** — a grid square is a square, not
  a house, which is the privacy answer as well as the convenient one.
- Cluster markers where receivers are close.
- Popup: name, radio, tuning range, `busy`, `freeInSec`, and a Listen link.
- ★ The page can only LINK to a server: VibeSDR.net is HTTPS, listed servers are plain
  HTTP, so the browser blocks any fetch. Live status must come from the directory's own
  record. The native apps have no such restriction.

---

## Client side

- Pickers fetch `GET /api/directory` and give VibeServers **top spot**, above OWRX / Kiwi /
  UberSDR. The pickers already rank by source.
- Show `busy` and `freeInSec` — *"free in 4 min"* is the difference between waiting and
  giving up, and both are already in the identity response.
- ★★ **The client decides what is reachable NOW.** Apps have no port restriction and no
  mixed-content problem, so the picker can probe a server the moment you look at it and
  grey it out if it does not answer — whatever the directory last believed. Clean split:
  **the directory decides who is listed; the client decides what is reachable.**

---

## Cost — free, but only with the right store

Stuart's question, and it decides the design: *"that will mean I'd have to pay Cloudflare
wouldn't it?"* **No — if two traps are avoided.**

★★★ **NOT Workers KV.** Free tier is **1,000 writes/day**. A 15-minute ping is **96
writes/day per server** — about ten servers and the budget is gone. This would look
perfectly healthy in testing with one server and fall over the moment it was used.

★★ **Use D1** (Cloudflare SQLite). Free: **100,000 row writes/day**, 5M reads, 5 GB. The
same ping supports ~1,000 servers.

- Workers: 100,000 requests/day free — API and probes both fit.
- ★★ **NOT Durable Objects.** The natural fit for per-server state, and they **require the
  paid plan** — that alone is the £5/month being avoided.
- ★ Rate-limit registration. The free tier is generous, not infinite.
- ★ No VPS either: this project is kept at **no recurring costs**
  ([[monetization_and_backend_roadmap]]).

★ If it ever outgrows free, that means public VibeServers exist in numbers — the desired
outcome, and a £5/month decision made with evidence rather than up front.

---

## Privacy and safety — decide BEFORE the first deploy

### ★★★ Encryption — Stuart's requirement, 2026-07-28

> *"our site is HTTPS so I would like all traffic between VibeServer and vibesdr.net to be
> encrypted. If a user has setup a HTTP ddns then its their choice to make, but we respect
> privacy at all stages"*

- **VibeServer → VibeSDR.net: ALWAYS HTTPS.** Register, ping, delist. No plain-HTTP
  fallback, and no "retry over HTTP if TLS fails" — that is how a fallback becomes the
  path an attacker forces.
- **VibeSDR.net → the user's server: HTTP if that is what they run** — their machine,
  their choice — but ★★ **NOTHING SECRET EVER CROSSES IT.** A nonce out, a hash back.
- ★ **Publish the hostname, NOT the resolved IP.** The directory needs the source IP to do
  its checks; it does not need to print it. Avoids pinning a specific address into a public
  page and a search index.
- ★ Source IP is **verification state, not a record to accumulate**. Keep it no longer than
  the check needs.

★★ **ARCHITECTURAL CONSEQUENCE: the directory client lives in the PLATFORM layer, not the
C++ shim.** `net_shim` is raw sockets with no TLS, and bundling BoringSSL into the shim to
make one HTTPS POST would be a maintenance burden for nothing. Kotlin/OkHttp on Android,
`URLSession` on the Mac — both get the system trust store, certificate validation and TLS
upkeep for free. The shim exposes the state; the platform does the talking.

### Publishing an address at all

★★★ **This publishes a home IP address on a public web page.**

1. **Opt-in only.** Never a default, never inferred from "the server is reachable".
2. **Say it plainly at the point of listing**, not in a help page: *"This publishes your
   server's address publicly so anyone can listen. Your address may be visible in search
   results and to anyone who visits the directory."*
3. **One-press delist**, effective immediately.
4. **Location is an operator-typed grid square.** Never GPS, never derived from IP.
5. **Rate-limit registrations**, and keep a manual removal path for abuse.

★ Cross-reference [[vibeserver_sharing_limits]]: session limits, IP cooldown and the admin
override exist precisely so a listed server is not defenceless. A listing should arguably
REQUIRE a session limit — an unlimited public receiver with one slot is a receiver one
person holds all day.

★ **Moderation is the hidden cost.** sdr.hu's decline is a caution: a public directory is
a moderation commitment, not just an endpoint.

---

## Build order

1. ★★★ **Throwaway Worker first.** Does `connect()` from `cloudflare:sockets` reach an
   arbitrary port, **and does it work on the FREE plan?** Both matter; one Worker settles
   both. Target: Stuart's OpenWebRX box on `stuey3d.freemyip.com:8073`, live and happy to
   answer an HTTP GET. ★ Note `fetch()` CANNOT do this — it is limited to an allowlist
   (80, 8080, 8880, 2052, 2082, 2086, 2095 + TLS equivalents); 8073 and our own 48000 are
   both off it, and asking users to re-forward to 8080 is a bad ask.
2. D1 schema + register/ping/delist + the DNS-vs-source-IP check.
3. The map page.
4. **VibeServer UI — as simple as it gets** (Stuart's design):

   ```
   PUBLIC LISTING
   Address   [ stuey3d.freemyip.com:8073 ]   ✓ reachable

     ( ) List permanently
     ( ) List temporarily
   ```

   Both off = not listed, the safe default, needing no explanation. The two switches are
   mutually exclusive and the ONLY thing that differs behind them is **what happens when
   the pings stop**: permanent keeps the entry and expects it back (a reboot does not lose
   your listing); temporary expires shortly after and does not return on its own.

   ★ Turning a switch off sends an immediate **delist** — it goes now, rather than lapsing.
   ★ The address box's tick is the SELF-CHECK: instant feedback that you typed it right.
     It does not decide the listing (see above), it just separates "I typed it wrong" from
     "something is wrong somewhere".
   ★ No duration picker in v1. "Temporary" already means "expires when I stop".
5. Client pickers: directory source, top spot, live reachability probe.

## Open questions

1. Do **offline permanent** servers stay visible greyed (Receiverbook does) or vanish?
   Stuart has said dead servers should not be listed — worth confirming this includes a
   known-good server that is merely asleep.
2. Should listing require a session limit, and/or a PIN-free public mode?
3. Show a listed server with **no radio attached**, or hide it until one is?
4. Who removes a server that is listed but serving something it should not?
