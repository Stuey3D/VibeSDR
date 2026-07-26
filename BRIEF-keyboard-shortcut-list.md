# BRIEF: The keyboard shortcut reference — content, audited from the code

**Project:** VibeSDR
**Author:** Stuart Carr (Stuey3D) with Claude, 2026-07-26
**Status:** CONTENT SETTLED, popup not built.

A **button in the main menu** opens a **popup with a scrollable list**. Not a settings page and
not another panel to navigate — a reference you open, read and dismiss.

★ Ordered by **WHERE a key works**, not alphabetically. The question being answered is *"what can
I do from here"*, not *"what does K do"*. Stuart's order: Instance Picker → Waterfall screen and
decoder boxes → DAB & ADS-B → FM-DX.

★ Every entry below was **read out of the source**, not recalled — the audit that produced it
found a shortcut that had been committed but never actually shipped (Esc on the maps).

---

## SERVER LIST

| Key | Does |
|---|---|
| `↑` `↓` | Move through servers, directories and the chooser above them |
| `⏎` | Connect · on a section header, expand or collapse it · on the custom URL box, type in it |
| `⌫` | Leave a directory · otherwise collapse the group you are in |
| `F` | Favourite the highlighted server |
| `D` | Set or clear it as the default |
| `E` | Edit a custom server *(directory entries are not ours to edit)* |
| `S` | Cycle the sort order |

## WATERFALL SCREEN

| Key | Does |
|---|---|
| `←` `→` | Tune — tap steps, hold sweeps |
| `↑` `↓` | Zoom in and out |
| `⏎` | Frequency entry |
| `D` | Demodulator · `S` step rate · `A` audio · `M` menu · `C` chat |
| `Esc` | Close what is open · restore hidden controls · otherwise open the servers menu |

### Frequency entry

| Key | Does |
|---|---|
| `⏎` | Tune the typed frequency |
| `T` `B` | Tune tab · Bookmarks tab |
| `H` `K` `M` | Hz · kHz · MHz |
| `P` | Open the OWRX profile picker *(⌫ leaves it without switching)* |
| `↑` `↓` | Move through bookmark results — works while typing in the search box |

### Menus, panels and dropdowns

| Key | Does |
|---|---|
| `↑` `↓` `←` `→` | Move. On a slider or the squelch bar, `←` `→` change the value |
| `⏎` | Press the highlighted control |
| `⌫` | Step out one level — leave a dropdown WITHOUT changing it |
| `Esc` | Close the panel |
| `D` | In the demodulator card, opens the digital/decoders list |

★ Hold any arrow to repeat. Menus close themselves after 10 s of no keys, with a flash first.

## DECODER BOXES

| Key | Does |
|---|---|
| `Tab` | Take the keyboard · again for the header controls · again to hand it back |
| `↑` `↓` | Move through the list, or scroll the output |
| `←` `→` | Move along the header controls |
| `Space` | Press the highlighted control |
| `Esc` `⌫` | Hand the keyboard back |

★ On an HF waterfall the box only BORROWS the arrows, and returns them after 10 s idle — tune and
zoom are the primary controls there.

## DAB & ADS-B

| Key | Does |
|---|---|
| `↑` `↓` | Move through the stations, or scroll the aircraft table |
| `Space` | Select the station *(nothing to select on ADS-B — it scrolls)* |
| `Tab` | Switch between the list and the header controls |
| `←` `→` | Move along the header controls · in SPEED FIX, choose a preset |
| `⌫` | Close SPEED FIX without changing it |

★ Here the box OWNS the arrows and keeps them: the VFO is locked to the multiplex, so there is
nothing to tune and no timeout.

## FM-DX

| Key | Does |
|---|---|
| `←` `→` | Tune — one step per press, hold to repeat |
| `↑` `↓` | Zoom the dial |
| `⏎` | Frequency entry · `D` demodulator · `S` step · `C` chat · `R` recordings |
| `Esc` | Close what is open · otherwise back to the server list |
| `⏎` `Space` `Esc` | Dismiss the shared-tuner notice |

★ Tuning a shared tuner commits once when you STOP, not once per repeat.

## RECORDINGS

| Key | Does |
|---|---|
| `↑` `↓` | Select · `Space` play or pause · `⌫` or `Del` delete · `Esc` close |

★ Share is deliberately absent — it hands off to the system share sheet, which is not ours to
drive. The same is true of Share on the frequency card.

## MAPS

| Key | Does |
|---|---|
| `Esc` | Back to the receiver |

## ★ COMPATIBILITY MODE

Every VibeSDR shortcut is switched off while a receiver's own web page is showing — it is not our
page, so we cannot know what a key means there. Use the touchscreen, or a trackpad on a Mac.

---

## Notes for building it

- ★ The popup is itself keyboard-reachable: arrows scroll it, `Esc` closes it, and it should say
  so at the top.
- The per-surface hints stay. They teach ONE surface at the point of use; this is the reference
  for afterwards. Neither replaces the other.
- ★ Keep it honest as things change. This list was audited out of the source once; a stale
  shortcut list is worse than none, because a user who tries a listed key and gets nothing
  concludes the keyboard is broken rather than the documentation.
