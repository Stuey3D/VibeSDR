# BRIEF — the website, rebuilt around the ecosystem

**Status:** agreed with Stuart 2026-08-25, not started. The apps ship first.
**Origin:** *"we need a real redesign of the website… ours looks identical [to the others] and as
users are pointing out its a hallmark of AI website design and looks lazy."*

---

## What is actually wrong

Not the colours. Compared with echosdr.com, foxsdr.com and radiosilencesdr.com, what people are
recognising is the **skeleton**, and ours has every bone of it:

| The tell | Ours today |
|---|---|
| Centred hero: headline + subtitle + two CTAs | yes |
| Feature blocks in rounded cards | 11 sections, radii 14/16/18/20px |
| Soft gradients + backdrop blur | 4 gradients, 1 backdrop-filter |
| Airy, symmetrical, everything centred | yes |
| Em-dashes throughout the copy | yes |

★★★ **AND THE DEEPER PROBLEM: THE SITE IGNORES THE PRODUCT'S OWN IDENTITY.** The app has a strong,
unusual look — phosphor on black, Nixie One numerals, instrument panels, hairline ticks, real
dials. The directory page has more character than the marketing site, because it is built like an
instrument rather than a landing page. **The website looks like every SDR site because it looks
like a website. The app looks like nothing else out there.** The fix is not "make it less AI"; it
is *stop having two design languages*.

---

## Positioning

Stuart: *"the app needs to be first and foremost but we need to highlight we are not a simple app
like the others and due to us controlling the DSP we are a full eco system."*

Echo and Radio Silence are **viewers for other people's receivers** — Kiwi, OpenWebRX, WebSDR.
They do not own a demodulator, so what a listener hears is whatever somebody else's server decided
to send. **We write the DSP**, and every other claim is a consequence of that one fact:

> Because we write the demodulator, the same signal path runs everywhere: on your phone, on the
> server you host, on your wrist.

That is what buys Advanced RDS on a watch, VibeAGC on a £30 dongle, and a server that runs on a
phone. State it in that order — the features are consequences, not a list.

★★ **IT IS "AND", NOT "OR".** VibeSDR is also a first-class client for Kiwi, OpenWebRX, WebSDR,
FM-DX, SpyServer and RTL-TCP. The pitch is not a walled garden: *everything they do, plus the whole
chain is ours.*

★★ **THE WATCH CLOSES THE ARGUMENT.** A standalone SDR client on a 1" screen cannot be bolted on;
it exists *because* the DSP is ours. If anyone doubts the ecosystem claim, that is what settles it.

### New, and saying so

Stuart: *"we are a new network trying to establish ourselves so we need to position it that way."*
This governs more than the strip's wording.

★★★ **DO NOT WRITE COPY THAT IMPLIES A NETWORK THAT DOES NOT EXIST YET.** "Receivers all over the
world" would be a lie a visitor disproves in one click, and the click is the directory — which is
on the same site. A product caught overstating its size on its own evidence loses the reader for
everything else it says, including the things that ARE true and are remarkable.

★★ **AND BEING EARLY IS GENUINELY ATTRACTIVE TO THE PEOPLE WE WANT.** The audience for "plug an SDR
into a spare phone" is exactly the audience that likes being third rather than three-thousandth.
Say it plainly — *a new network, run by the people listening to it* — and the small number stops
being an apology and becomes the reason to join.

★ The rule for every claim on the page: **describe the CAPABILITY at full strength and the SCALE
honestly.** "500 kHz to 1.766 GHz, from a phone on a windowsill" is true today and impressive
today. "The world's receivers" is neither.

### Simple yet powerful — the ethos, not a bullet

Stuart: *"the whole ethos of the Vibe ecosystem has been designed around making it as easy as
possible."* The ecosystem claim RISKS SOUNDING LIKE COMPLEXITY — servers, watches, decoders, DSP —
which is exactly how a competitor would read it. The counter is to state every powerful thing as
the easy thing it actually is:

> **Plug an SDR into a spare phone. It's a receiver anyone can listen to.**

No config file, no Docker, no reverse proxy, no port forwarding. The reader supplies the
astonishment themselves. Not "server-side DSP with adaptive AGC" but *"it sorts the gain out
itself"*. Not "distributed multi-client architecture" but *"share it by sending a link"*.
**Describe the moment, not the machinery.**

---

## Page shape

1. **Hero — the app.** Live spectrogram background (below). One line, App Store / Play. The
   product, not the philosophy.
2. **The on-air bar** — real numbers from the directory: *"4 radios on 2 servers · 500 kHz –
   1.766 GHz"*. Nobody else can copy it, because nobody else has the network.
3. **One line that reframes everything**, under the fold:
   *"Most SDR apps are a window onto somebody else's receiver. VibeSDR is the receiver too."*
4. **Three doors, equal weight:** **Listen** (any receiver, yours or the world's) · **Host** (turn a
   spare phone into one) · **Wear** (a real receiver on your wrist, not a remote).
5. **The call to arms** — now earned:
   > **Got a spare Android phone?** Plug in an SDR and in a few minutes it's a VibeServer anyone
   > can listen to — Advanced RDS and the decoders included. We run one on a rugged Android
   > handset to prove the point: if it can do it, your old phone can.

   ★ Beside it, `website/assets/shots/xcover-server-live.png` — the REAL screen, captured from the
   XCover **while a listener was actually connected** (Stuart connected so the shot would not say
   "Waiting for a client…"). Measured, not claimed:

   | | |
   |---|---|
   | Waterfall | **5 KB/s** |
   | Audio | **8 KB/s** |
   | Sample rate | 1.2 MS/s |
   | CPU | **91% of 1 core (of 8)** — idle it sits at 12% |

   ★★ **13 KB/s TOTAL IS THE HEADLINE, NOT THE CPU.** "Sharing your radio costs about as much
   bandwidth as a podcast" is the claim that lands, and it is measured. The CPU figure is honest
   but wants framing: 91% of ONE core of EIGHT is roughly a tenth of the phone, and saying "one
   core of eight" is more truthful than a percentage of the whole.

   ★ **IP ADDRESSES REDACTED, FRIENDLY URL KEPT** (Stuart's call — the hostname is in the public
   directory anyway). The listener's PUBLIC IP was on screen, which is a real person's connection
   and must not go on a marketing page or into a public repo. Substituted text is Menlo rather than
   the app's Nixie One, so a re-shoot is worth it if this becomes prominent.

   ★★★ **AND ITS COMPANION — THE OTHER HALF OF THE ARGUMENT.** Stuart also took the receiving
   end: the Mac app listening to that phone, with Advanced RDS fully decoded — station name,
   RadioText, the AF list, MPX S/N 29 dB, constellation and MPX plots, group share. **The two
   shots side by side ARE the pitch**: a rugged Android handset on the left serving 13 KB/s, and
   on the right a full DX-grade analyser reading the signal it sent. No competitor can show the
   second picture, because they do not own the demodulator that produced it.
   `website/assets/shots/mac-advanced-rds.webp` — 2440px, 549 KB (the original was an 8 MB
   Retina PNG; the XCover shot is 60 KB beside it). Checked for personal data before committing:
   no addresses of any kind in frame.

### The tunnel — say this, it is a genuine differentiator

Stuart: *"we make it clear that the connection is handled via the Cloudflare tunnel and no home or
personal DDNS is required and home IP is not shared."* Verified against
`BRIEF-vibeserver-tunnel-directory.md`, which already states it: **"The home address is never
published at all. Not the IP, not a DDNS name."**

What can be claimed, all of it true:

- **No port forwarding.** cloudflared is an ordinary OUTBOUND process — nothing is opened on the
  router. It therefore works behind **CGNAT**, where port forwarding is not merely awkward but
  impossible, and that is a large share of mobile and modern broadband users.
- **No DDNS, no dynamic address to chase.** The hostname is stable and handed out for you.
- **Your home IP is never published.** Listeners connect to Cloudflare; they never see, and never
  connect to, the machine in your house.
- **Real HTTPS with a real certificate** — which a port-forwarded home server CANNOT have, because
  no CA issues for a bare address. So its listeners get "Not Secure" and its admin password
  crosses in clear. ★★ **The tunnel is the more secure option for the listener, not a compromise
  for the host.** That is the opposite of what a reader will assume, so say it plainly.

★★ **THE OTHER DIRECTION IS FINE — JUST DO NOT CLAIM IT.** The host DOES see a listener's IP: the
server prints it, and session limits, cooldowns and bans are keyed on it. Stuart: *"listeners
already know their IPs are being logged by servers as they all do it, that is how they manage block
lists."* That is the norm across every SDR receiver on the internet and needs no apology or
disclaimer — so **do not soften the host-side claim on account of it.**
★ The only rule: say "your home address stays private", never "nobody sees anybody". One is true
and is the selling point; the other would be a privacy claim that quietly overreaches, which is
worse than making none.

### "Nobody is left out" — and how to say it without lying

Stuart wants: *"Old or new, low end or high end, small or big screen, nobody is left out with
VibeSDR"* — and flagged the problem himself: *"we dont currently have a windows app yet even though
it is planned."* He is right to flag it, and the fix is not to weaken the line.

**The web client already covers Windows.** A Windows user is not waiting for anything in order to
LISTEN: they open a browser and get the full client — scaling, Advanced RDS, decoders — which the
recording now proves rather than asserts. So:

> **Old or new. Low end or high end. Big screen or small. Nobody is left out.**
>
> Apps for iOS, Android, Mac and Apple Watch. Everywhere else — Windows, Linux, a smart TV — the
> built-in web client, with nothing to install.

★★ **THAT TURNS THE GAP INTO A FEATURE.** Windows users are not on a waiting list; they are already
served, and by something that is not a cut-down version. Said this way the line is TRUE, and it is
stronger than the version that needed the caveat.

★★ **AND IT IS NOT "A PI" ANY MORE — SAY LINUX.** Stuart: *"its not limited to Pi anymore as we
support X86 linux now."* apt carries **arm64 AND amd64**, so any ordinary Intel/AMD Linux box is a
host — an old laptop, a NUC, a home server. "A Raspberry Pi" undersells it and, worse, reads as a
hobby-only product.
★ **BUT SAY WHAT HAS ACTUALLY BEEN TESTED.** Stuart, unprompted: *"tested on debian and ubuntu
though so not sure what mint or steam os will do… or other linux distros."* So: **Debian and Ubuntu
tested; other distributions likely but unverified.** That is a normal, credible thing to print, and
it is the difference between a claim and a promise — a Mint user who fails after being told it
works is a bug report AND a disappointed reader.

★★★ **THE LIMIT IS HOSTING, NOT LISTENING — AND THAT IS THE ONE TO KEEP HONEST.** VibeServer runs
on macOS, Linux, a Pi and Android; **not Windows**. So "anyone can listen" is unconditional, while
"anyone can host" is not yet. Do not let the call-to-arms copy blur the two: *"got a spare Android
phone or a Mac or a Pi"* is accurate and still sounds generous. A Windows owner told they can host,
who then cannot, is the one reader we lose permanently.

### ★★★ THE THREE ORIGINAL CLIPS STAY — THE NEW ONE IS AN ADDITION

`assets/phone-demo.mp4`, `assets/watch-demo.mp4` and `assets/scaling-demo.mp4` (the APP scaling on
the Mac) are keepers and must survive the redesign. Stuart, unprompted and rightly: *"we still want
the original phone and apple watch clips and the original app scaling on the mac."* Nothing in this
brief replaces them — `web-client-live.mp4` is a FOURTH clip that proves a different claim.

★★ **AND THE PAIR IS THE POINT.** `scaling-demo.mp4` shows the APP scaling; `web-client-live.mp4`
shows the BROWSER doing the same. Shown together they say something neither says alone: it is not
that the app is well built, it is that *every* way in behaves this way. The watch clip closes it at
the other end of the range. Do not let a tidier layout thin this set to one "hero video" — the
breadth IS the argument.

### Assets to re-shoot — the current ones misrepresent the product

- ✅ **`web-client-2026-08.webp`** replaces the July shot (below). Heart on 96.6, Advanced RDS
  open — and it happens to show the same day's work live: `MULTIPATH 6.0% · slight · IMS standing
  by · CEQ has it`, with CEQ reporting its own before→after.
- ★★ **`assets/web-client.jpg` is from 25 JULY and no longer looks like the web client.** Since
  then it has gained the Advanced RDS analyser, the IF filter picker, the AGC/IF status chip, the
  band-plan bar and the station logos. Stuart: *"we need a new screenshot for the web client too as
  it no longer looks like it did on the site."* A stale screenshot is not a cosmetic problem — it
  sells a weaker product than the one that exists.
- ★★★ **AND A SCREEN RECORDING OF THE WEB CLIENT, MOVING.** Stuart: *"a second screen recording
  showing the web client behaving dynamically like the app is another amazing selling point."* He
  is right, and it is the single most persuasive asset available: the whole doubt about a
  browser-based receiver is that it will be a static, laggy toy. A waterfall scrolling, a dial
  spinning and RDS filling in — in a BROWSER, with nothing installed — refutes that in two seconds
  and cannot be refuted back. It also proves the "no app required at the other end" claim the
  caption already makes and the still image cannot.
  ✅ **SHOT** — `web-client-live.mp4` (16.5 s, silent, 3.4 MB, poster beside it). Tuned to Heart on
  96.6 with Advanced RDS open, so the waterfall scrolls and the constellation and MPX plots move.
  ★ Trimmed at 16.5 s: the original ran on to the operator opening Control Centre to stop the
  recording, which showed the Wi-Fi panel and was a weaker ending anyway.

  ★★★ **AND IT SHOWS SOMETHING BETTER THAN WE ASKED FOR — THE WINDOW BEING RESIZED.** Stuart:
  *"the web client also scaling perfectly like the app does… so we can say you dont even need the
  app to listen to VibeServers as the built in web client scales from a tiny phone up to a desktop
  monitor — everyone regardless of device is equal in the VibeSDR ecosystem."*

  That last clause is the best framing of the ecosystem produced so far and should probably be ON
  the page, close to those words. It reframes "mobile-first · scalable" from a specification into a
  PROMISE, and the recording proves it in one gesture rather than asserting it in a bullet. It also
  removes the reader's obvious objection — "another app to install" — before they raise it: there
  is a way in with nothing installed at all, and it is not a cut-down one.

### The version label rots — stop hand-writing it

`website/index.html` carried **v3.1.18 while 4.1.1 was shipping**, and the comment beside it
already recorded the SAME failure from 2026-08-16. A warning that a hand-maintained version will
rot is not a fix for it — it did not prevent the second time and will not prevent the third.
The apt repo publishes the current version, and the GitHub API names the latest release; either can
fill the label AND the download href at load. Patched to 4.1.1 for now; do it properly here.

### The network dial as the top strip — Stuart's idea, and the best structural one yet

*"I think this dial along the top strip where the current ON AIR strip is now, to advertise the
VibeServer Network and have it follow the scroll like the on air does now… doesnt need to be as
thick, just a single bar, but make it follow the same design style. So that bar is the background
with a little red VFO and the text over it talks about the VibeServer network and is a link to go
to the page."*

★★★ **THIS IS THE "STOP HAVING TWO DESIGN LANGUAGES" FIX, MADE CONCRETE.** It puts the instrument
at the top of the marketing site instead of describing it further down — and it is live data, so
it is the one banner on the page that cannot be mistaken for decoration. It also advertises the
network in the one place every visitor looks.

**What it is:** a single thin bar — the coverage blocks (green free / red no-slots / blue temporary)
across the network's range, a small red VFO needle, and the wording over the top as a link to
`vibeserver.vibesdr.net`. No numerals, no ticks, no interaction: at strip height they would be
noise, and the DIRECTORY page is where the dial is a control.

**How:**
- The renderer exists in `directory/public/index.html` (`dialSegments`, `.dTick*`, `.dNum`,
  `.dBand`, the free/busy/temp/unknown classes). Reduce, do not rewrite — and the palette must be
  the SAME variables, or the two pages will drift apart, which is the fault this whole brief is
  about.
- The sticky treatment is already written: `#onAirBar { position: sticky; top: 0; z-index: 60 }`.
- ▶ **DATA IS THE ONE REAL PROBLEM.** vibesdr.net and the directory are SEPARATE Workers, so the
  site needs `/api/directory` cross-origin — either CORS on that endpoint or a tiny proxy on the
  site's own Worker. Decide this first; everything else is presentation.
- ▶ **AND DECIDE WHAT HAPPENS TO "ON AIR".** It advertises Stuart's own receivers; the new bar
  advertises the network, which INCLUDES them. Two sticky strips would be one too many. Probably
  the network bar replaces it and absorbs the on-air state (a live receiver of his is just a green
  block), but that is his call, not an assumption to make quietly.

**The wording carries a live count.** Stuart: *"right now we have 4 radios on 2 servers so give a
live update of the server count too… hopefully we can recruit a few more into the network."* The
directory already computes all of it — radios, servers and the combined range.

★★★ **AND THE COUNT IS SMALL, WHICH IS A RISK AND AN OPPORTUNITY, SO FRAME IT.** "2 servers" read
flatly can say *nobody uses this*; the same number as an invitation says *there is room, and you
would be early*. Since recruitment is the whole reason the strip exists, it should read as an
invitation and not as a scoreboard:

> **VibeServer Network · 4 radios on 2 servers · 500 kHz – 1.766 GHz — add yours →**

★★ **AND IT MUST NOT LOOK BROKEN AT ZERO OR ONE.** A strip that says "0 radios on 0 servers"
because the fetch failed is worse than no strip: the honest fallback is the wording without the
figures, never a zero. Same rule as everywhere else in this product — "nobody said" and "none" are
different answers.

★ The number growing is its own reward, and it is the one metric on the page that improves by
itself if the pitch works. Worth watching after launch: if it does not move, the call to arms is
not landing, and that is useful to know quickly.

### Craft notes
- **Square the corners.** 14–20px radii with soft borders is the single strongest tell.
  Instrument panels have hairlines and screen-printed labels.
- **Left-align, increase density.** The house style is centred and airy; radio gear is dense,
  panelled and full of numbers.
- **Use Nixie One and the phosphor palette**, so the site and the product are visibly one thing.

---

## The live background: push-and-cache

Stuart's own design, and the bandwidth instinct behind it is right: *"my idea was to feed live
waterfall data from the PI but the fanout would hammer my bandwidth or cost me [my] cloudflare
account. We could put the spectrogram in the background from the Pi as that is live data just
timelapsed."*

**Most of this already exists.** `/vibeserver/spectrogram` serves a `VSPG` binary — 2048 bins per
row, `rows=` to limit it. The landing page already refreshes it every 15 s, so the render path is
written; reuse it rather than inventing one.

★★★ **VISITORS MUST NEVER FETCH IT FROM THE PI.** The Pi already POSTs to `/api/directory/ping`;
add a small rendered image on the same authenticated channel every few minutes. The Worker stores
it and serves it from edge cache. That gives all three things at once:

- **Pi bandwidth is constant** — one upload every few minutes whether 5 or 5,000 people visit
- **Cloudflare cost is trivial** — one ~30 KB image, served from the edge
- **A power cut does not blank the page** — the last good image persists, which was the point of
  caching it (Stuart)

★★ **IT IS THE RSP's WINDOW (2.8–10.8 MHz), NOT THE NETWORK'S.** The spectrogram only builds on a
**locked-frequency** radio, because a readable one needs every row to share a profile — a
free-tuning dial gives 40m for an hour, then FM, then wherever someone left it at 2am. So the hero
background is honestly *"live HF from a receiver in England"*, and the on-air bar carries the
network-wide claim. Two different truths; the page reads better for not blurring them.

★ **SHOW THE REAL SPAN, NEVER A FIXED CLAIM** — as the landing page already does ("BAND ACTIVITY ·
9.8 H"). Stuart's call. A hard-coded "24 hours" would have hidden the cadence bug below
indefinitely.

---

## Open questions
- Does the pushed image go to R2 or KV? (Size and retention decide it.)
- Rendered PNG/WebP on the Pi, or raw rows rendered by the Worker? Rendering on the Pi is simpler
  and keeps the palette identical to the app's.
- Does the hero degrade to a still image on mobile, or is a 30 KB WebP fine everywhere?

## Done on the way (2026-08-25)
- **Every restart ate five hours of spectrogram history.** `spectroTaken` reset to zero while the
  rows were restored, so each start re-ran the 300-row fast fill — five minutes of data in 300 of
  the 1440 slots. This is why Stuart had never seen more than ~13.5 hours. Fixed in 4.1.1-6.
- Directory: numerals unstroked (the "blur"), `claimable` honoured so a soft-limited radio reads
  **FREE · TAKE OVER** instead of FULL, VFO instructions added, lens icon redrawn.
