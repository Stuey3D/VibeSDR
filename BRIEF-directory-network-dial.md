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

★★★ AND THE SAME ARGUMENT KILLS A LINEAR SCALE *INSIDE* COVERAGE. Compressing the gaps is not
enough on its own: one unrestricted RTL covers 500 kHz – 1766 MHz with no gap at all, and on a
linear axis the entire medium wave band — the whole of AM broadcast — is 1/1600th of the bar.
Invisible, unclickable, and exactly the band an AM DXer came for.
→ **Logarithmic within each covered stretch.** Every decade gets equal width, so MW, HF and FM are
all reachable with a mouse, and the mapping stays monotonic and exact so click-to-frequency is
still a real frequency and not an approximation. This is how a spectrum overview is normally drawn,
and it is the only choice that survives both a narrow HF+ and a wideband dongle on the same page.

★★ TICK LABELS DO THE HONEST WORK. Because the axis is neither linear nor continuous, the numbers
along it are not decoration — they are the only thing telling a visitor that the gap they are
looking at is 86 MHz wide and the band beside it is 1 MHz. A non-linear axis without labels is a
lie; with them it is a schematic, which is what this should be.

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
