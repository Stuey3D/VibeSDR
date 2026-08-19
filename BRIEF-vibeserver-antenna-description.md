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
   `antenna` values across `g_serverConfig.radios` — not from a curated list and not from
   anything remembered per browser. Three radios on one mast is the common case and it is the
   case that makes typing it three times annoying.

★ So it is a text box with suggestions, not a select: typing a new one must always be possible,
  because the second antenna on a machine is exactly when the list stops being right. An
  `<input list=…>` + `<datalist>` is the whole control.

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
