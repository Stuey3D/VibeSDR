# Ask before tuning — the tuning etiquette lock

**Stuart, 2026-10-21** (Discord: Mad Radio DXer / M7EIW, NickB G4IRX). Owners of shared-tuner servers
are fed up with people joining and tuning without asking, despite the "SHARED TUNER · ASK TO TUNE"
banner above the frequency. This is the opt-in lock that makes the ask happen.

## What the owner switches on
- **"Ask before tuning"**, a per-radio switch on the setup page directly after *Maximum listeners*
  (and in the phone app's own server setup, after its listener count). **Default OFF.** Stored in the
  radio's entry in `config.json` as `askBeforeTuning`, beside `sessionLimitSoft`, so it survives restarts.
- It only does anything on a **shared dial** (unlocked centre, room for more than one listener) with
  **more than one listener connected**. A lone listener, a locked-centre radio (independent VFOs) or the
  switch off = everything exactly as today.

## Who may tune
The server keeps a **rank order** of the sessions present:
1. the signed-in **admin** (never locked, always outranks everybody);
2. everybody else, **longest-connected first** — by the turn start the session limit already keeps
   (`sockSince`, keyed on the browser/address, so a reload does not jump the queue);
3. but a listener whose **session time limit has run out** drops to the **bottom** (still ordered by
   connection time among themselves).

A listener may tune when **nobody ranked above them is here**, or when they have been **granted** the dial.
Everyone else is **locked**: their tuning controls are dimmed, and touching one flashes the chat key and
the shared-tuner banner reads **"ASK IN CHAT BEFORE TUNING"**.

## Asking, and the answers
The ask is the existing canned **"Can I tune?"** (`ask_tune`) — or **"Anyone using this?"**
(`anyone_using`). Reused rather than duplicated as a new "OK if I tune?" line: an old client already has
these keys, so somebody on last month's app can still ask and be let in.

Replies are **canned ids with a meaning**, never parsed text:

| Meaning | Ids |
|---|---|
| **YES** | `go_ahead` "Go ahead, tune" · `yes_go_ahead` · `all_yours` · `decode_done` |
| **NO** | `please_hold` · `yes_hold` · `mid_decode` · `decoding_10min` · **new** `listening_few_min` "I'm listening to this one — a few more minutes, please?" |

`listening_few_min` is the gentler replacement-in-spirit for the requested "Do you mind, I am listening here":
it says why and asks for a little time, and it is the line that the five-minute limit below honours.

A reply counts only from somebody **ranked above the asker**. While the ask is open, the **highest-ranked**
reply so far stands (the admin's always wins):
- **YES** → unlocked at once.
- **No answer in 15 s** (`kAskWindowS`) → unlocked.
- **NO** → stays locked, for at most **5 minutes** (`kNoHoldS` in `vibe_tune_etiquette.h`, one constant to
  change). Then the asker is **unlocked automatically** — they do not have to ask again. Asking again would
  hand the same listener another "no", for ever: the limit exists so nobody can monopolise the server, and
  an automatic end is the only form of it that cannot be talked around. During a NO the banner counts down.
- A standing NO also ends early if its sayer leaves, or is no longer ranked above the asker (their time
  limit ran out), or someone ranked above the sayer says YES.

### ★★ How long a grant lasts (Stuart, 2026-10-22)
*"If actively tuning they stay up but if left on a station for a few mins they lock again."* A grant lasts
while it is being **used**: it **lapses after 3 minutes** (`kGrantIdleS`) with no **accepted** receiver
change from that session. Any frame in the locked list below (tune, mode, passband, DAB, gain, the shared
DSP …) that the server **accepts** restarts the clock. These do **not**: a refused frame, a re-assert of
where the radio already is, zoom / pan / squelch / the waterfall rate / chat (free — they do not move the
shared dial), and the server's own landing and resume (recovery is not the person). On a lapse the listener
is a **newcomer again** — ask, then YES / 15 s silence / NO ≤ 5 minutes, exactly as before. If nobody ranked
above them is here when it lapses, nothing locks (they are top anyway). The clock belongs to the grant, so a
reconnect within the 2-minute user-number hold keeps it — and it keeps running through the drop.

| Timer | Length | Constant (`vibe_tune_etiquette.h`) |
|---|---|---|
| Ask with no answer → let in | 15 s | `kAskWindowS` |
| A NO holds at most | 5 minutes | `kNoHoldS` |
| A grant with no accepted receiver change → locked again | 3 minutes | `kGrantIdleS` |
| Grant (and its idle clock) kept across a reconnect | 2 minutes | `kGoneForgetS` |

`VIBE_ASK_GRANT_IDLE_S` (1–3600) overrides `kGrantIdleS` — **for tests only** (the live suite runs it at
10 s); never set on a real server.

## Enforcement — the server owns all of it
Per the shared-dial contract the server holds the lock state, the ranks and the timers, and states the
result in the existing `dial` message, per listener:

```json
{"type":"dial", …, "askFirst":true, "tuneLocked":true, "askState":"open", "askSecs":12}
```
`askFirst` = the lock is in force on this radio now; `tuneLocked` = this listener may not tune;
`askState` = `""` | `"open"` (asked, waiting) | `"no"` (told no); `askSecs` = seconds left in that state.
`askGrantSecs` (from 2026-10-22, optional) = this listener holds a grant that matters (they would be locked
without it) and it lapses in this many seconds **if they change nothing more**; every accepted change resets it
to the full 3 minutes, and `dial` is NOT re-sent per tune, so a client that ever shows it counts it down
locally and resets it on its own accepted change. Absent = no grant / unknown. No client reads it yet. When
a grant lapses the listener simply gets `tuneLocked: true` with no `askState` — the newcomer's view.
The fields are **absent** when the switch is off — so a new client on an old server dims nothing.
The housekeeping tick (2 s) resolves the timers (the grant's idle lapse included) and re-sends `dial` to anybody whose view changed.

**The server refuses** a locked listener's `tune`, `mode`, `bandwidth`, `dab` and `dab_service` frames — at the WebSocket
reader, i.e. only for frames a client sent. Internal entries (the landing, the DAB resume for an arriving
listener, the idle resume) call `handleControl` directly and are never gated: **recovery is not a user
action**, and it is not refused either. A refused frame that would change nothing (a re-assert of where the
radio already is) is dropped silently; a real one gets the room's `config` back (so the client re-adopts)
and, at most every 10 s, a `notice` with `why` = "Ask in chat before tuning — …", which every client since
the refusal pill already shows. **Old clients** therefore simply do not tune and are told why.

### ★★ "Lock gain too" (Stuart, 2026-10-21)
On a shared dial there is ONE front end and ONE pipeline, so every receiver setting changes what everybody
hears. These are refused as well (one list, three readers: `kGated` in `local_sdr_shim.cpp askRefuses`,
`ASK_SHARED_SETTINGS` in `src/services/dialChat.ts`, `UberClient.askSharedSettings` on Jr — checked equal by
`scripts/test_ask_before_tuning.ts`):

| Locked while not let in | Message |
|---|---|
| RTL gain + VibeAGC (`gain`), RTL hardware AGC (`agc`) | `gain`, `agc` |
| SDRplay IF gain reduction / LNA state / IF + RF AGC / notches / HDR …, gain-API restart | `rsp_control`, `rsp_agc_restart` |
| Airspy HF+ attenuator / preamp / AGC | `ahf_control` |
| Airspy R2/Mini LNA / mixer / VGA / modes, HackRF LNA / VGA | `airspy_control`, `hackrf_control` |
| Antenna port | `antenna` |
| Tuner IF filter, sample rate, direct sampling | `tunerbw`, `sampleRate`, `directSampling` |
| Shared DSP: NR, notch, de-emphasis, stereo, NFM voice, weak-signal NR, both blankers, CEQ, IMS, auto bandwidth | `nr`, `notch`, `deemph`, `stereo`, `nfmvoice`, `wsp`, `nb`, `nbx`, `ceq`, `ims`, `autobw` |

**Stay free** (checked per handler): zoom / pan / reset (a view — "that shouldn't move the tuning or audio"),
squelch (`sessionSquelch`, per session), the waterfall rate (`fftRate` per client, `set_rate`), Advanced RDS
(`rdsx`, a per-listener subscription), DAB scopes / resync (per socket), ADC stats, raw IQ out and uncompressed
audio (per session; raw IQ is off on a shared dial anyway), chat, `state`, ping, admin unlock. Bias-T, ppm,
automatic direct sampling, the DAB rate boost and raw-IQ policy are behind the admin password already, and an
admin is never locked. Volume, waterfall contrast and the rest of the look are client-side and never sent.

The clients drop a locked setting **silently** (a page restoring its preferences on connect is not a person
reaching); the touch shields on the dimmed controls are what nudge. A server `notice` carrying `ask` is
likewise dropped by new clients — it is for clients that predate the lock.

Not gated: zoom/pan, chat itself, `state`.

## Clients
- **Web** (`web/client`): `tuneLocked` dims the tuning controls; the SpectrumClient refuses to send a gated
  frame while locked (one rule for every path — drum, keys, spectrum click, bookmarks, pad), flashes the
  chat key and the banner, and asks the server for `state` so the readout snaps back.
- **App** (`src/`, phones, iPad, Mac, Android, tvOS): the same gate in `VibeServerWsClient`; the drums dim
  and a touch flashes the chat key and the banner.
  The hardware panel (LocalHardwarePanel) is dimmed and shielded as a whole, with an ASK BEFORE TUNING card
  under the password card (which stays live); the audio sheet dims the shared DSP rows and gates their handlers.
- **Web**, receiver settings: the gain row, each driver's row, RTL AGC, IF filter, sample rate, direct sampling,
  antenna and the shared DSP buttons are dimmed and shielded (`ASK_SHIELD_SEL`).
- **Jr** (watch, direct): refuses to send a tune while locked and says why; its Radio page (gain, AGC, RSP / HF+
  rows, de-emphasis, the FM row) is dimmed and inert with a line saying why, and the socket drops the list.
  ★ Its admin password card is drawn ABOVE the dimmed region while locked and stays live (Stuart: "Jr needs to
  be able to sign in as admin too") — an admin is never locked, and the server sends the dial state the moment
  a sign-in is granted, so the lock clears at once rather than at the next 2 s tick.
  **Buddy** tunes through the phone, whose gate covers it; it exposes no receiver settings.

## Tests
- `vibeserver/test-tune-etiquette.cpp` — the rule on a synthetic clock (ranks, windows, conflicts, lapses, the
  grant's 3-minute idle lapse: 179 s granted, 181 s locked, a tune at 170 s resets, reconnect keeps the clock).
- `scripts/test_ask_before_tuning.ts` — the clients' reader, banner widths, and that app + web gate every send.
- `scripts/test-ask-before-tuning.mjs` — a real vibeserver (`--ask-before-tuning`, fake dongle, silent): refusal at
  the socket, the old-client notice, YES / NO / 15 s on the wire, and the idle lapse (shortened to 10 s with the
  test-only `VIBE_ASK_GRANT_IDLE_S`): tuning keeps it up, zooming alone does not.

## Why not lock everyone until they ask?
The longest-connected listener (or the admin) is the one being protected; locking them too would make the
first person on a quiet server ask an empty room. "Nobody above you is here" is the whole rule.
