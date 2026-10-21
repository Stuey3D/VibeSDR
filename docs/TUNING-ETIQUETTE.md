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

A grant lasts for the visit (kept across a reconnect within the user-number hold, 2 minutes).

## Enforcement — the server owns all of it
Per the shared-dial contract the server holds the lock state, the ranks and the timers, and states the
result in the existing `dial` message, per listener:

```json
{"type":"dial", …, "askFirst":true, "tuneLocked":true, "askState":"open", "askSecs":12}
```
`askFirst` = the lock is in force on this radio now; `tuneLocked` = this listener may not tune;
`askState` = `""` | `"open"` (asked, waiting) | `"no"` (told no); `askSecs` = seconds left in that state.
The fields are **absent** when the switch is off — so a new client on an old server dims nothing.
The housekeeping tick (2 s) resolves the timers and re-sends `dial` to anybody whose view changed.

**The server refuses** a locked listener's `tune`, `mode`, `bandwidth` and `dab` frames — at the WebSocket
reader, i.e. only for frames a client sent. Internal entries (the landing, the DAB resume for an arriving
listener, the idle resume) call `handleControl` directly and are never gated: **recovery is not a user
action**, and it is not refused either. A refused frame that would change nothing (a re-assert of where the
radio already is) is dropped silently; a real one gets the room's `config` back (so the client re-adopts)
and, at most every 10 s, a `notice` with `why` = "Ask in chat before tuning — …", which every client since
the refusal pill already shows. **Old clients** therefore simply do not tune and are told why.

Not gated: zoom/pan (a view, not what anybody hears), gain-class controls (unchanged — open on a shared
dial, as before; noted as a follow-up), chat itself, `state`.

## Clients
- **Web** (`web/client`): `tuneLocked` dims the tuning controls; the SpectrumClient refuses to send a gated
  frame while locked (one rule for every path — drum, keys, spectrum click, bookmarks, pad), flashes the
  chat key and the banner, and asks the server for `state` so the readout snaps back.
- **App** (`src/`, phones, iPad, Mac, Android, tvOS): the same gate in `VibeServerWsClient`; the drums dim
  and a touch flashes the chat key and the banner.
- **Jr** (watch, direct): refuses to send a tune while locked and says why. **Buddy** tunes through the
  phone, whose gate covers it.

## Why not lock everyone until they ask?
The longest-connected listener (or the admin) is the one being protected; locking them too would make the
first person on a quiet server ask an empty room. "Nobody above you is here" is the whole rule.
