# BRIEF — VibeSDR (and Jr) against a multi-radio VibeServer V3

**Status:** specified 2026-08-10, not started. The web-client scroll half is done (see §5).

## The problem, stated exactly

A V3 server is a **front door** that owns no radio. Every radio lives behind `/r/<id>/…`.
`UberSDRClient` builds every URL as `baseUrl + "/ws/user-spectrum"`, so against a front door it
asks for a radio the door does not have and gets a **503**, which as a WebSocket handshake
surfaces as a bare **1006 with nothing in it** — indistinguishable from "server down".

Measured on the live demo, 2026-08-10:

| path | bare | `/r/8c02e830/…` |
|---|---|---|
| `/ws/user-spectrum` | **503** | 200 |
| `/connection` | — | 200 |
| `/vibeserver/hardware` | 200 | 200 |

★★ **This is NOT "make it connect".** Pointing `baseUrl` at `/r/<id>` would connect, and that is a
useful diagnostic, but it is not the fix: **the app has no way to CHOOSE a radio, and no way into
the admin or setup pages at all** (Stuart, 2026-08-10). Those are the actual gaps.

## 1. A radio splash screen, on connecting to a multi-radio server

Mirror the web client's landing page:
- every radio listed, **with its restrictions** — the directory already publishes `coverage`,
  `allowed`, `restricted`, `listeners`, `maxUsers`, `waiting`, `freeInSec`, so a picker can show
  what a radio is and whether it is free **without opening it**.
- ★★★ **IT MUST SCROLL.** Especially on small screens and in landscape. This is not a nicety: a
  list that cannot scroll makes the radios below the fold *unreachable*, and the user concludes
  the server only has two radios. The web client had exactly this bug (§5).
- **Use `id`, not `serial`** — serials are kept out of URLs. The door still accepts a serial for
  old links.
- ★ Single-radio servers must behave exactly as they do today. A list of one is noise.

## 2. The admin box, beneath the radios

On the same splash, under the radio list:
- activate **admin override**
- open the **admin page**
- open the **setup page**

★ The admin ticket already crosses processes, so the fan-out needs no new server work.

## 3. Password prompts once you are in a radio

- The **radio Hardware tab** needs the password prompt it has in Simple mode (i.e. what the
  current Mac/Android server does today).
- A **second admin password entry in the servers chip**, so a user can unlock admin settings
  quickly without going back out to the splash.

## 4. Admin + setup pages open in a WebView

- with a **"back to SDR" button at the top**, like the UberSDR pages already do.
- ★★ They must **reflow and scale to small screens**. They were written for a desktop browser.
- ★ A WebView keeps them automatically in step with the server, which matters while the server
  pages are still moving. Rendering them natively would mean two implementations drifting.

## 5. DONE — the web client's own landing page did not scroll

`#splash` was `position:fixed; inset:0` with `justify-content:center` and **no overflow rule**, so
content taller than the viewport was clipped with no way to reach it. Fixed:
- `overflow-y:auto`, plus `justify-content: safe center` — a centred flex column whose content
  overflows pushes the first item off the TOP of the scroll container, and nothing above
  `scrollTop 0` can be scrolled to. `safe` falls back to flex-start exactly when it would overflow.
- `#splashSpectro` and `#splash::after` moved from `absolute` to `fixed`: an absolutely positioned
  child of a scroll container **scrolls with the content**, so the band picture and its dimming
  gradient would have slid up the screen behind the cards.

## Order of record

[[next_bring_mac_android_and_apps_to_v3]] says **servers first, clients second** — "clients
written against a server that behaves differently on three platforms grow three code paths for one
feature." The macOS and Android servers are still V2-shaped (no front door, no multi-radio, no
admin page). Decide deliberately whether this brief jumps that queue.

## ★ Do not forget the tour copy

Per AGENTS.md: adding a splash and moving admin entry points means `sdrTour` in `SDRScreen.tsx`,
`pickerTour` in `InstancePickerScreen.tsx` and `AboutOverlay` may now misdescribe where things
are. Grep for the OLD home and the NAME of anything that moves.
