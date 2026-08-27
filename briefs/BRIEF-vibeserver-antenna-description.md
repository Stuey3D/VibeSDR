# An antenna box per radio — so the landing page sets reception expectations

Stuart, 2026-08-19:

> I think that we should add a simple antenna description box for the radio so that on the
> landing screen a user can see what they are working with. I have left the RTL fully unlocked
> so it can go all the way to 1.7GHz but it doesnt mean the antenna will perform well all the
> way up that high, it may pull in stronger signals. In the case of my OWRX box i have an
> antenna rated to 300MHz which is pulling in a decent amount of aircraft on ADSB.
>
> **"it sets reception expectations then"**

## The problem it solves

A receiver publishes what the TUNER can do. Nobody publishes what the ANTENNA can do, and the
antenna is what decides whether tuning there is worth the visitor's time. The Pi's RTL is
deliberately unlocked to 1.7 GHz; that range is real and the radio will honestly tune it. The
antenna will not follow it up there, so a visitor who tunes to 1.2 GHz, hears nothing, and
leaves has learned the wrong thing — they conclude the RECEIVER is broken, or that we are
overstating what it does.

★★ This is the same failure as a tour card that misdirects (AGENTS.md): the visitor blames the
   feature, not the description. Here there IS no description at all, so they blame the radio.

★★ And it cuts the other way, which is the better half: a 300 MHz-rated antenna pulling in a
   lot of ADS-B is a REASON TO VISIT, and today there is nowhere to say so. The interesting
   fact about a receiver is often the aerial, not the dongle.

## Shape

A free-text string per radio — `RadioConfig::antenna`, beside `label` — set in the setup page
and shown on the landing page next to the radio it belongs to.

★★★ NOT a dropdown of our making. We do not know what people put up, and a fixed list would be
    wrong the day it shipped. It is the owner's sentence about their own aerial.

★ Free text, so it needs the same escaping as `label` and the same length cap, and it is
  published to strangers — see the repo personal-data rule. "A longwire in the loft" is fine;
  a postcode is not, and the field's helper text should say what it is for.

## The dropdown Stuart actually asked for

> a box to type the antenna in per radio, with a dropdown box after youve filled it out once so
> that if like the pi is setup now the 3 radios share the same you can quickly select it on the
> next 2 radios

★★ The list is built from WHAT THIS SERVER HAS ALREADY BEEN TOLD — the distinct non-empty
   `antenna` values across the other radios — not from a curated list and not from anything
   remembered per browser. Three radios on one mast is the common case and it is the case that
   makes typing it three times annoying.

★ So it is a text box with suggestions, not a select: typing a new one must always be possible,
  because the second antenna on a machine is exactly when the list stops being right. An
  `<input list=…>` + `<datalist>` is the whole control.

### ★★★ THE PRECEDENT ALREADY EXISTS — "Copy from another radio"

`vibe_setup_page.h:1032` already ships exactly this affordance for the band allow/block lists:
`bandCopyRow` / `bandCopyFrom`, built from
`list.map((r, i) => ({r, i})).filter(x => x.i !== curRadio)`, and hidden entirely when there is
only one radio. **Reuse that enumeration.** Its own comment also records the rule that keeps it
safe — it copies *"the lists only ... and sidesteps carrying a per-device calibration (or a
bias-T) onto hardware it does not suit"*. An antenna string is safe to copy by that test; it
describes what is BOLTED TO THE MACHINE, not how a particular tuner is set up.

★★ Where the antenna SHOULD diverge from that control: the band row asks you to pick a SOURCE
   RADIO, because a band list is compound and only makes sense wholesale. An antenna is ONE
   SHORT STRING, so picking the VALUE is the shorter path — "quickly select it on the next 2
   radios" (Stuart) is one interaction with a datalist and two with a copy-from row. Same idea,
   one fewer step, and it still allows typing something new.

### ★★★ IT MUST READ THE UNSAVED CONFIG, NOT THE SAVED ONE

`stashRadio()` (`vibe_setup_page.h:1546`) does `Object.assign(list[curRadio], collectRadio())` on
every tab switch, so the whole machine's config — **including edits not yet saved** — lives in the
page's `cfg.radios[]`.

★★ That is precisely what makes Stuart's flow work: type the antenna on radio 1, switch to radio
   2, and it is already offered — with no save in between. Built against the SAVED config instead,
   the suggestion list would be empty until you saved each radio, and the feature would silently
   require a save-per-radio round trip that nobody asked for. Read `radioList()`, not the server.

## Where it has to appear — the grep list applies

Anything that shows a radio to a listener needs to learn the field, or it will be a setting
that exists and is invisible on two of the three clients:

- `web/client/index.html` + `src/main.ts` — the radio picker cards (`showSplashRadios()`) and
  the single-radio splash.
- the setup page (`vibe_setup_page.h`) — where it is typed, per radio.
- `/vibeserver.json` and `/vibeserver/radios` — it has to reach the clients somehow.
- the apps' pickers: `src/screens/InstancePickerScreen.tsx`, and Jr's.

★ ABSENT MEANS NOT SET, and a radio with no antenna string must render exactly as it does
  today — no empty label, no "unknown". Most servers will never fill this in.

## Open questions

- Does it belong on the picker CARD (visible before you commit) or only inside the receiver?
  The card is where it sets expectations, which is the stated purpose — but the cards are
  already dense.
- Should a range hint travel with it ("good to 300 MHz")? Structured beats prose for the one
  thing a client could ACT on — greying the band edges the aerial cannot reach — but it is a
  second field to get wrong, and Stuart asked for a description, not a spec. Prose first.

---

# Part 2 — a PERMANENT owner message on the landing screen

Stuart, 2026-08-19:

> alongside the antenna description box I think a admin message for the landing screen would be
> useful too. Right now by default the server goes to idle 5fps mode, so for the demo server I
> would put a note on the landing screen advising users that this is normal behaviour and can be
> disbled in the menu.
>
> the landing screen one was more for **permanent messages from the server admin, such as a
> donation link to keep the server running, or rules etc.**

## ★★★ This is NOT the existing notice, and must not reuse it

The server already has `notice` — and it is the wrong vehicle on all three counts:

| | existing `notice` | what this needs |
|---|---|---|
| lifetime | `setNotice(text, **minutes**, err)` — expires | permanent, no expiry |
| where | drawn over the RECEIVER, after you connect | the LANDING screen, before you commit |
| behaviour | dismissible, and stays dismissed | part of the page |

The notice answers "something is happening right now" (the Pi is currently carrying *"Currently
experimenting and developing the server software"*). This answers "what you should know about this
receiver, always". Folding one into the other would mean a donation link that expires, or a
maintenance warning nobody can dismiss.

## What it is for — the owner's standing statement

Three uses, all named by Stuart:

- **Setting expectations about our own behaviour.** The demo drops to 5 fps when idle. That is
  deliberate and reversible in the menu, but to a first-time visitor it looks like a struggling
  server — the *same failure as the antenna*: they blame the receiver for something that is
  working as designed. ★★ Worth noting we keep re-learning this: a thing that is correct but
  unexplained gets read as a fault.
- **Asking for support.** A donation link, because somebody is paying for this.
- **House rules.** Whatever the owner needs to say to strangers.

## Shape

Server-level free text, shown on the landing screen beside the radio list — one statement per
machine, not per radio (the antenna field in Part 1 is the per-radio one).

★★★ **THE LINK IS THE HARD PART.** A donation link is the stated use, so this field WILL contain
    URLs — and it is owner-authored text rendered on a page served to strangers. Raw HTML is out:
    that is stored XSS on every VibeServer in the world, and we ship this to other people's
    machines. Options, in order of preference:

  1. **A separate `linkUrl` + `linkLabel`** beside the text. Nothing to parse, nothing to escape
     beyond attribute quoting, and the client controls exactly how the anchor is rendered.
     Scheme-restricted to `https:` and `http:` at BOTH ends — a `javascript:` URL in an href is
     the whole vulnerability, and validating only in the setup page means the next client to
     render it is the one that ships it.
  2. Autolink bare URLs in escaped text. More forgiving to type, one more thing to get wrong.

  ★ `rel="noopener noreferrer"` and `target="_blank"` on whatever we render.

★ Length cap and the same personal-data warning as the antenna field: this is published, and the
  helper text should say so.

## Where it has to appear — same grep list as Part 1

The landing screen exists in four places and they will drift unless all four learn it at once:
the web client's splash and picker cards, the setup page (where it is typed), `/vibeserver.json`,
and the apps' pickers. ★ ABSENT MEANS NOTHING SHOWN — a server that never sets it must look
exactly as it does today.

## SETTLED — where it is typed

Stuart, 2026-08-19: *"so that would live under the server tab of the setup page"*.

★★★ That decides the SCOPE, not just the location. The setup page's SERVER tab is the one place
    with **no radio behind it** — `curRadio` is -1 there, deliberately (`vibe_setup_page.h`). So
    this is one statement per MACHINE, saved to the server config rather than to a `RadioConfig`,
    and it renders on the front door's picker where the machine is what the visitor is looking at.

★★ It also settles Part 1 by contrast: the antenna belongs to a RADIO, so it is typed on that
   radio's tab and lives in `RadioConfig`. Two fields, two tabs, two scopes — and the SERVER tab
   already exists and is always shown, "even with one radio", so there is no new navigation to
   build for either.

★ A single-radio Simple server has no picker, so the message has to reach that splash too — same
  grep list, and the reason the list matters.
