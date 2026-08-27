# The network dial — a directory organised by frequency, not by radio

Stuart, 2026-08-22: *"This method puts the frequency first not the radio."*

## The idea

Along the top of the directory, an interactive tuning dial in the vintage style of the FM-DX
screen, with the band plan above it. It is not decoration and it is not a second view of the list —
**it is the filter**.

- **Zoomable**, like the dial it is modelled on.
- **Click a band** in the plan to select it; click more to widen the selection.
- **Click or drag the dial itself** to set a lower and upper frequency by hand.
- **The dial only spans what the network can actually hear.** With only the Moto listed, the dial
  is the FM broadcast band and nothing else. Add the demo Pi and it becomes 500 kHz – 1.7 GHz. Lift
  the Airspy's AM/FM restriction and it extends down to 1 kHz. Remove a server and it shrinks
  again.
- **Green where something free covers it. Red where every radio covering it is in use.** Click a
  red stretch to see which radio owns it and join the queue.
- **Then the same bar per country**, so "who can hear this, near here" is one glance.

## Why this is the right shape

The band-chip filter I built first asks "which radio do you want?" and then makes you work out
whether it can hear the thing you came for. This asks the question the listener actually has: *I
want 1467 kHz — who has it, and is it free?* Every other SDR directory is a list of receivers. This
is a map of the spectrum with receivers attached to it.

★★ It also kills the chip wall on its own. 28 named bands and one wideband RTL covers 26 of them;
as chips that is a block of noise, but as a dial it is simply a long green bar.

## What is already true

- Every server publishes `ranges` — what a listener may **actually tune**, already narrowed by the
  owner's allow/block lists and by a locked window (`vsTunableRanges`, shipped 2026-08-22).
- `/vibeserver/radios` publishes `allowed` per radio on a multi-radio front door.
- The directory passes `radios` through whole, so new per-radio fields need no Worker change.

## What is MISSING, and must be built first

★★★ **OCCUPANCY IS PER SERVER, NOT PER RADIO.** The whole red/green idea rests on knowing that
*the radio covering 500 kHz* is busy — but the directory records `listeners` and `maxListeners`
for the SERVER. On a front door holding three receivers that is meaningless for this purpose: the
Pi can be "2 of 10" while the only radio that reaches HF is the one occupied.
→ Publish `listeners` and `maxListeners` per radio.

★★★ AND THE FRONT DOOR DOES NOT KNOW THIS EITHER. `/vibeserver/radios` publishes `users` per
radio, which reads like occupancy and is not — it is `--users`, the CONFIGURED cap
(`main.cpp:1556`, from `r.users = c.users`). Each radio is a separate PROCESS; the door holds the
config and hands off connections, so nobody is counting live listeners per receiver in the place
that publishes the list. That is the real work in step 1, and it is why step 1 is step 1.
★ The phone in simple mode is the easy half: it has exactly one radio, so the server's own counts
ARE that radio's, and it can say so truthfully today.
★★ Until a multi-radio server can answer, its ranges must contribute COVERAGE but not a free/busy
verdict — drawn as covered-but-unknown rather than guessed green. Inventing an occupancy the
server never reported is precisely the mistake already written up in
`client_infers_server_decisions`: Jr ended sessions itself on a countdown only the server could
judge.

★★ **THE PAGE HAS NO BAND PLAN, DELIBERATELY.** Every band name on it today comes from a server,
so the page cannot invent bands for radios nobody told it about. But a dial has to draw the band
plan whether or not a server happens to mention a band — the plan is a property of the SPECTRUM,
not of any receiver.
→ The plan is generated from `vibe_bands.h` into a JSON the page loads, at deploy time, with a
check that fails if the two disagree. ONE definition, region-aware, no hand-kept copy. A second
band table is exactly how the landing page and the directory would come to disagree about what a
server offers.

★ **A NAMED BAND IS NOT A COVERED BAND.** `bandLabel` names a range only when the range *matches*
a band — it was written to describe a RESTRICTION ("locked to FM broadcast"). Capability is a
different question: an unrestricted RTL publishes one range, 500 kHz – 1766 MHz, which matches no
band, so it currently produces no band names at all and is invisible to a band filter despite
hearing everything. The dial reads `ranges` directly and never needs the names for filtering —
names are labels on the ruler, not the filter itself.

## The axis is NOT to scale, and that is the point

Stuart, 2026-08-22: *"gaps in between coverage dont need to be to scale — so for instance say my
Airspy was the only one available it would show AM Broadcast, a small gap saying no coverage, and
then FM broadcast."*

★★★ THE DIAL SPENDS ITS WIDTH ON WHAT YOU CAN ACTUALLY LISTEN TO. A true-to-scale ruler gives the
dead space between bands the same pixels as the bands themselves — with an Airspy limited to AM and
FM, the 86 MHz of nothing between them would be 98% of the dial and the two things you came for
would be slivers at either end. An uncovered stretch becomes a fixed narrow gap, marked as no
coverage, however many megahertz it really spans.

★★ WITHIN A COVERED STRETCH THE AXIS IS LINEAR, AND **ZOOM** IS WHAT MAKES SMALL BANDS REACHABLE.
I argued first for a logarithmic axis, on the grounds that one unrestricted RTL covers 500 kHz –
1766 MHz with no gap at all, so medium wave would be 1/1600th of the bar — invisible and
unclickable. Stuart: *"but that is where the bar can be zoomed in and out."* That is the better
answer and it was in the design from the first sentence: this is a DIAL, and a dial zooms. A log
axis would have bought reachability at the cost of the one property a tuning dial must have — that
equal distances are equal frequency steps, so dragging across it behaves the way every radio the
visitor has ever used behaves.

★ WHICH LEAVES ONE RESIDUAL, AND THE BAND PLAN ALREADY SOLVES IT: at full zoom-out a narrow band
is too small to hit with a mouse. The plan along the top is the click target — click "AM (medium
wave) broadcast" and the dial zooms to it. So the labels are not only labels; they are the
navigation, which is also what makes the region dropdown matter.

★★ TICK LABELS DO THE HONEST WORK. Because the axis is neither linear nor continuous, the numbers
along it are not decoration — they are the only thing telling a visitor that the gap they are
looking at is 86 MHz wide and the band beside it is 1 MHz. A non-linear axis without labels is a
lie; with them it is a schematic, which is what this should be.

## Landing on it

Stuart: *"when landing on the directory for the first time you would see a bar zoomed out showing
500 kHz – 1.7 GHz, then zoom it in and out."*

Fully zoomed out is the default, and "fully zoomed out" means **the network's own extent** — not a
fixed 500 kHz – 1.7 GHz written into the page. That figure is simply what the estate happens to
reach today; with only the Moto listed the whole dial is the FM broadcast band, and the moment
somebody lists an HF+ with no restriction it runs down to 1 kHz. The first thing a visitor sees is
therefore an honest statement of what is on offer right now, at a glance, before they have clicked
anything.

★★★ AND A REFRESH MUST NOT YANK THE DIAL OUT FROM UNDER SOMEBODY. The page reloads every minute,
so the extent can change while a visitor is zoomed into 40 m: a server leaves, or one arrives that
reaches further, and a naive implementation would rescale the axis under their hands mid-drag. The
zoom window belongs to the VIEWER and survives a refresh; only the parts of it that no longer
exist are re-drawn as uncovered. It is clamped only when the window ends up entirely outside what
anybody covers, and then it says so rather than silently jumping somewhere else.
★ Same rule the list already follows: a refresh must not shut a card somebody opened.

## The band plan is chosen, not assumed

Stuart, 2026-08-22: *"for the bandplan we have a dropdown to choose region the bandplan should
show."*

The plan differs by ITU region and the differences are not trivia — medium wave ends at 1606.5 kHz
in Region 1 and 1705 kHz in Region 2, and FM broadcast is 87.5–108 MHz here and 76–90 MHz in
Japan. A dial labelled with the wrong plan tells a visitor a band ends where it does not.

★★★ THE REGION CHANGES THE LABELS, NEVER THE COVERAGE. A receiver's `ranges` are frequencies in
Hz and mean the same thing everywhere on earth; the plan is only how we NAME stretches of them and
where a band-click lands. Switching region must therefore never change which servers are listed,
what the dial spans, or what is green — only the words above it. If a region switch ever appears
to add or remove a server, something has been computed from a label that should have been computed
from a number.

★★ SO THE GENERATED PLAN CARRIES ALL THREE REGIONS, not just the one the build machine sits in.
`vibe_bands.h` already keeps a common table plus per-region tables and takes a region argument —
the generator emits every region and the page picks one.

★ DEFAULT AND MEMORY. Region 1 by default, remembered per visitor. Deliberately NOT guessed from
the browser's locale or timezone: a guess that is wrong relabels the spectrum for somebody who
never asked, and the tell — "medium wave stops in the wrong place" — is far too subtle to notice.
An explicit dropdown that starts somewhere sensible beats a clever guess nobody can see being
made.

## Converters, and radios we have not met

Stuart: *"some users may be using HAM it Down which brings satcomms down to the RTL tunable range,
plus we need to prepare for any future radios we will support."*

★★ THE GOOD NEWS IS THE WIRE FORMAT IS ALREADY RIGHT. `ranges` is an arbitrary set of Hz pairs —
not "driver plus limits" — so a receiver behind an upconverter or a satcom downconverter publishes
different NUMBERS and the dial needs no change at all. Nothing in the directory, the union
computation or the axis knows or cares what hardware produced a range. That is worth stating
because it is the one part that would have been expensive to get wrong.

★★★ BUT `vsTunableRanges()` DERIVES FROM `driverCoverage(driver)`, AND THAT TABLE IS A CEILING
TODAY. Confirmed with Stuart, 2026-08-22: converters are not supported anywhere in the tree — no
upconverter, transverter or LO-offset setting exists (`HW_OFFSET_HZ` is DC-spur offset tuning, a
different thing entirely). So a Ham It Up user today would have their real coverage ERASED on the
way out, because `permitted()` intersects with the dongle's native span — and worse, the retune
clamp would refuse frequencies they can genuinely hear, telling them the radio is broken when it
is our table.
→ When converter support lands, `driverCoverage()` becomes a DEFAULT rather than a ceiling, and
the converter's offset has to reach both the published ranges and the clamp. One value, two
readers — and the clamp is the one that fails loudly, so it must not be forgotten.

★★ AND AN UNKNOWN DRIVER MUST NOT MEAN "NO COVERAGE". `driverCoverage()` returns empty for a
driver it does not recognise, which is honest for the label ("we cannot say") but on a dial it
means a brand-new radio SILENTLY DISAPPEARS from the network's extent while working perfectly —
listed, listenable, and invisible in the one place people will look. A radio we have not met must
be able to state its own range, and a server that cannot say must be visible as such rather than
absent.
★ Same rule the frequency search already follows: a server with no published range is counted and
named, never silently dropped.

## The availability computation

Client side, from `ranges` plus per-radio occupancy. A sweep line over every range boundary in the
network gives elementary intervals; for each, count the radios covering it and how many have a
free slot.

- **none covering** → not part of the dial at all (the dial's extent IS the union)
- **≥1 free, at least one of them permanent** → green
- **≥1 free, but every one of them is a temporary share** → blue (Stuart: *"highlight blue if only
  a tempory share covers that part of the spectrum"*). Here now, gone in fifteen minutes — a
  different promise from green, and the listener deserves to know which they are being offered
  before they plan an evening around it. Blue already means "temporary" everywhere else on this
  page, so it costs no new vocabulary.
- **≥1 covering, none free** → red, and clickable: which radio, and the queue

★★ THE PRECEDENCE IS DELIBERATE. "Busy" beats "temporary" because it answers a more urgent
question — can I have it *now* — while blue answers *how long can I keep it*. And a permanent radio
beats a temporary one, so green is never weakened by a share that happens to overlap it: green
means "this will still be here", and it must not be able to mean less than that.

★ A server that never published a range contributes nothing to the dial and must be listed
somewhere visible, not silently dropped — the same rule the frequency search already follows.

## Order of work

1. Per-radio `listeners` / `maxListeners` on the wire. *(Nothing above works without it.)*
2. Band plan generated from `vibe_bands.h`, with a drift check.
3. The union bar for the whole network — extent, green/red, no interaction.
4. Click and drag to filter; the list below reacts.
5. Band plan labels above the dial; click to select bands.
6. Zoom.
7. Per-country bars.
8. The queue, on a red stretch.

★ 3 is the first thing worth looking at, and 1–3 are enough to prove the idea.

## What it actually became (2026-08-23, shipped)

Steps 1–6 are live on vibeserver.vibesdr.net. Where the build departs from the plan above, it is
because Stuart looked at it and said so:

- **The availability is in the NUMERALS, not a bar.** *"the solid green bar is not needed as we
  will colour the numbers instead."* Green/red/blue/dim on the ticker's own figures.
- **The band plan is the app's strip** — solid blocks butted together, coloured by TYPE from
  `BAND_HEX` (ham red, broadcast blue, utility green, CB orange), *"over the frequency ticker"*.
  The type is derived in the generator from the label, so there is one rule and not one per reader.
- **Bands are AND, and per RADIO.** *"if I want say the 40m band and the 20m band I would click
  those and ... only the RTL on the Pi would show as its the only one that can do both bands."*
  The chip filter's OR was right for "show me FM servers" and wrong for a dial you compose a query
  on. The server card then shows only the receivers that answer, and counts the ones it left out.
- **The needle is the control.** Click the glass to place the red VFO; the zoom keys work about it.
  A second click places the upper needle and the pair is the manual range search.
- **Edge plates page the dial** by 80% when there is spectrum beyond the glass — *"kinda like
  tapping the edges of the spectrum on a real SDR"* — and the tune arrows walk it a tenth at a
  time. Dragging works too, but it had to stop selecting the page to do it.
- **The band plan is generated** by `scripts/gen-bandplan.sh` from `vibe_bands.h`, all three ITU
  regions, with `--check` to fail on drift. The region-from-position rule is SAMPLED from the
  header's own `ituRegion()` rather than ported, so the page cannot disagree with the server about
  where a band ends. Filtering to US receivers moves the plan to Region 2 on its own.

### Still to do
- **7. Per-country bars** and **8. the queue on a red stretch** — untouched.
- ★★ **EiBi ON THE DIAL.** Stuart: *"we could even integrate Eibi and stagger the stations like a
  real vintage radio with the stations written on the dial itself, but that we will look at later
  as it may become crowded."* Both sides already carry an EiBi service (`eibi.cpp`, `eibi.ts`), so
  the data is in reach; the open question is density, and it is a LOOK-AT-LATER, not a next step.
- ★ The dial has no per-radio *queue* yet: a red stretch says who owns it, not how long.
