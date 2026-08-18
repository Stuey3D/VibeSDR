# BRIEF — soft session limits (share only when someone is waiting)

**Stuart, 2026-08-18.** A session limit exists to share a scarce radio. On the public Pi it is
currently rationing something nobody is queuing for: three days of connection logs show **not one
listener refused for a busy radio**, while the only genuine repeat visitor — a US address, 14
visits — kept stopping at 24, 25, 25, 29 and 30 minutes and coming back. The limit is costing the
most engaged user and protecting no one.

## The setting

Under the existing per-slot time limit (`limitMin`, e.g. 30), one more choice:

| Mode | Behaviour |
|---|---|
| **Hard** | Disconnect at the limit, always. Today's behaviour, unchanged, and the default for an existing install. |
| **Soft** | The limit is a GUARANTEE, not a deadline. You keep the radio past it — until somebody else wants it. |

★ Hard stays the default on upgrade. An owner who chose 30 minutes chose it under today's meaning,
and a setting must not change what it does underneath them.

## What "soft" means precisely

- **Before the limit** — the session is EXCLUSIVE. A newcomer waits (the queue already exists:
  `reservedFor` / `reservedUntil` / `waiting`). Nobody can take it, and that is the promise.
- **After the limit** — the session continues on borrowed time. The moment `waiting > 0`, the
  incumbent is given **60 seconds' notice** and then evicted; the waiter's reservation is what
  guarantees they actually get the slot rather than losing it to whoever reconnects fastest.
- **Admin** — unchanged, and says so in the copy: an owner may take the radio at any time.

★★ THE NOTICE MATTERS AS MUCH AS THE RULE. Being cut off mid-station with no warning is what the
   hard limit already does badly; a soft limit that does the same is not an improvement. 60 s is
   long enough to note the frequency and finish a sentence of speech.

## The wire

`/vibeserver.json` must advertise the mode, or clients cannot describe it honestly:

    "limitMin": 30,
    "limitMode": "hard" | "soft",     ← new; absent = hard, so old servers read correctly
    "sessionSecsLeft": …              ← existing
    "exclusiveSecsLeft": …            ← new: time left of the GUARANTEE (soft only)

★ A client that does not know `limitMode` must keep working — it shows the countdown it always
  did. Absent means hard.

## What the client shows

- **On connect to a soft server**, once: "This receiver is shared. You have it to yourself for
  30 minutes — after that you keep it until somebody else wants it. The owner can take it back at
  any time." A pill, not a modal; it must not stand between a user and the radio.
- **The countdown stays**, but tagged for what it is: EXCLUSIVE while inside the guarantee, then
  something like "borrowed" once past it. The number means two different things either side of
  the limit and must not silently change meaning.
- **On eviction**: the existing `evicted` terminal reason, with a line saying someone else is
  waiting — not an error, and not something to auto-retry into.

## Where the copy lives (AGENTS.md's grep list)
`SDRScreen`'s tour, `AboutOverlay`, the web client's landing page, Jr's tutorial tips, and
`website/index.html` all describe how sharing works. A new sharing MODE means every one of those
sentences needs checking, not just the one being edited.

## Not in scope
Queue positions beyond what exists, per-user quotas, and any notion of "priority" listeners. The
whole value here is one rule an owner can explain in a sentence.
