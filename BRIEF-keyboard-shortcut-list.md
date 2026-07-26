# BRIEF: The keyboard shortcut reference — content, audited from the code

**Project:** VibeSDR
**Author:** Stuart Carr (Stuey3D) with Claude, 2026-07-26
**Status:** CONTENT SETTLED, popup not built.

A **button in the main menu** opens a **popup with a scrollable list**. Not a settings page and
not another panel to navigate — a reference you open, read and dismiss.

★ Ordered by **WHERE it applies**, not alphabetically. The question being answered is *"how does
this work here"*, not *"what does K do"*.

★★ **WRITTEN AS PROSE, NOT A KEY TABLE (Stuart).** A table of single keys makes the reader
assemble the mental model themselves; a sentence hands it to them. *"Menus are navigated with the
up and down arrows, while left and right adjust sliders"* teaches the shape of the thing —
`↑↓ = move` does not. The same reason the drums feel like drums: describe the behaviour, not the
mechanism.

★ Every claim below was **read out of the source**, not recalled. The audit that produced it found
a shortcut that had been committed but never actually shipped.

---

## SERVER LIST

The up and down arrows move through everything on the page — the custom URL box at the top, your
favourites and custom servers, and the directories below them. Enter connects to the highlighted
server, or opens a section that is collapsed, or drops you into the URL box to type an address.
Backspace steps back out: it leaves a directory you have opened, or collapses the group you are
standing in.

With a server highlighted, **F** favourites it, **D** sets or clears it as your default, and **E**
edits it if it is one of your own — directory entries come from the directory, so they are not
ours to change. **S** cycles the sort order.

## WATERFALL SCREEN

Left and right tune, up and down zoom. A tap moves one step; hold the key and it sweeps the band,
speeding up the longer you hold it, exactly as the tuner keys do under a thumb.

Enter opens the frequency box. **D** opens the demodulator, **S** the tuning step, **A** audio,
**M** the menu and **C** chat. Escape is the way back from anything: it closes whatever is open,
brings the controls back if you have hidden them, and — when there is nothing left to close —
opens the servers menu.

### Typing a frequency

Type the number and press Enter. **T** and **B** switch between the Tune and Bookmarks tabs, and
**H**, **K** and **M** choose Hz, kHz or MHz. On an OpenWebRX server, **P** opens the profile
picker; backspace leaves it without switching, which matters because changing profile retunes the
receiver for everyone else listening to it.

In the Bookmarks tab, up and down move through the results — including while you are still typing
in the search box, so you can filter and then step straight down into what you found.

### Menus, panels and dropdowns

Menus are navigated with the up and down arrows, while left and right adjust anything that holds a
value — a slider, or the squelch bar. Enter presses the highlighted control. Backspace goes up a
level: out of a sub-panel, or out of a dropdown list without changing what was selected. Escape
closes the menu completely.

Hold an arrow to keep moving. If you walk away, a menu opened by keyboard closes itself after ten
seconds, flashing once first so you can see why.

## DECODER BOXES

On the HF decoders the waterfall is still the point, so the box leaves the arrows alone until you
ask for them. Press Tab and it takes the keyboard, announcing itself with a flash. Press it again
for the controls in its header, and once more to hand the keyboard back.

While it has the keyboard, up and down move through the list or scroll the output, left and right
move along the header controls, and Space presses whichever you last moved to. Escape or backspace
hand the keyboard straight back, and so does ten seconds of not touching it — tune and zoom are
what you want back.

## DAB & ADS-B

Here the box is the screen rather than something on top of it: the VFO is locked to the multiplex,
so there is nothing to tune. The arrows belong to the box from the moment it appears, and it keeps
them — up and down move through the stations, or scroll the aircraft table.

Space selects a station. On ADS-B there is nothing to select, so the arrows simply scroll. Tab
moves between the list and the header controls rather than leaving, and inside SPEED FIX the arrows
choose a preset, Space sets it, and backspace closes it having changed nothing.

## FM-DX

Left and right tune a step at a time and hold to keep going; up and down zoom the dial. Because
FM-DX is one radio shared by everyone connected, holding an arrow retunes it **once when you
stop**, not once per step.

Enter opens the frequency box, **D** the demodulator options, **S** the tuning step, **C** chat and
**R** your recordings. Escape closes whatever is open, or takes you back to the server list. The
shared-tuner notice you meet on connecting will go with Enter, Space or Escape — whichever you
reach for.

## RECORDINGS

Up and down select, Space plays and pauses, and backspace deletes — or forward-delete, since a Mac
keyboard has no such key. Escape closes the list. Sharing is not on the keyboard: it hands over to
the system share sheet, which is not ours to drive.

## MAPS

Escape brings you back to the receiver.

## ★ COMPATIBILITY MODE

Every VibeSDR shortcut switches off while a receiver's own web page is showing. It is not our page,
so we cannot know what a key would mean there — use the touchscreen, or a trackpad on a Mac. The
app says so on screen if you press a key.

---

## Notes for building it

- ★ The popup is itself keyboard-reachable: arrows scroll it, Escape closes it, and it should say
  so at the top.
- The per-surface hints stay. They teach ONE surface at the point of use; this is the reference for
  afterwards. Neither replaces the other.
- ★ Keep it honest as things change. A stale shortcut list is worse than none, because a user who
  tries something listed and gets nothing concludes the keyboard is broken rather than the
  documentation.
