# BRIEF: the receiver introduces itself, on every backend

**Status:** not started. Post-v10. Written 2026-07-30.

## The observation
What makes an UberSDR session feel finished is not the artwork — it is that the receiver **says who
and where it is**, top-right:

```
STUEY3D - MLA30+ (2.8m Quad Loop/Garden/Facing NW/SE)
Moulton, Northamptonshire, England, UK
```

You know what you are listening to and where it is standing. **Every other backend gives you a
waterfall with no sense of place.** Stuart: *"tempted to add the name from the directory to the top
corner so all servers have the same look and feel as UberSDR."*

★ This matters more than decoration: on a remote SDR the location is the single most useful fact on
screen. It is what makes a signal mean something — a station heard on a loop in Northamptonshire is
a different claim from the same station on a beverage in Norway.

## ★ Confirmed missing on VibeServer too (2026-07-30)
The WEB client shows the server's name top-right; the APP does not, even on VibeServer where we
know the name. Stuart: *"not an urgent fix though as it may mess with the timer clock and I don't
want to break that so close to release."*

★★ **THE TIMER CLOCK SHARES THAT CORNER** — the "YOUR TURN ENDS IN 29:18" session countdown sits
exactly where the name would go. That is the real constraint, not the sourcing: whatever is built
must give way to the countdown, or share the corner deliberately, and it must be tested WITH a
session limit running. Do not treat the corner as empty just because it usually looks it.

## Where the text comes from
We already hold it, which is why this is cheap:
- The **directory entry** (name, location, country) for a listed server.
- The **favourite** for a custom one — though a user-typed favourite may have a name and no location.
- The **server itself** for UberSDR (which is what today's header renders) and Kiwi.

## ★★ The real design problem: DUPLICATION, not sourcing
- **Kiwi** and **FM-DX** already put their own identity on screen. This must **replace** those, not
  sit beside them, or the same name appears twice in different fonts.
- **OWRX** has a name but usually no location — the line has to **degrade gracefully** to one line
  rather than leaving an empty second row or, worse, inventing a location.
- **VibeServer** is the user's OWN radio. "Moulton, Northamptonshire" is noise when you are sitting
  in it; it may want the radio/antenna instead, or nothing.
- ★★★ **Never infer a location we were not given** — [[feedback_no_inferred_hardware_readouts]]
  applies to place as much as to hardware. No geocoding from a hostname, no "probably UK" from a TLD.

## Constraints
- The corner is contested: the servers chip, the link meter and (on small screens) the clock all
  live near it. Check on a 41mm-equivalent phone layout, not just an iPad.
- It must not be tappable-looking unless it does something. If it IS tappable, "show this server's
  details / favourite it" is the obvious action.
- Two lines maximum, truncating on the FIRST line (the name) and never on the location.

## Open question
Does this go on the watch? Jr has no room for two lines of chrome, but a **single** line under the
band label might be the most valuable text on that screen — you can be a long way from the receiver.

Related: `BRIEF-spectrum-backgrounds.md`, `memory/instance_picker_overhaul.md`.
