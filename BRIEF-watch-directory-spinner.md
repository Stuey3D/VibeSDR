# BRIEF: the watch directory list spins for ever — no timeout anywhere in the chain

**Status:** not started. **10.0.1.** Diagnosed 2026-07-31 from TestFlight feedback.

## The report
TestFlight, 2026-07-31 11:07 UTC, `AL6Uck3NnfD6ZLrzLOeEktg`. iPhone 12, iOS 26.5, Apple Watch on
**watchOS 26.5**, Wi-Fi, **`zh-Hans-CN` / Asia/Shanghai**, VibeSDR **10.0.0 build 9**.

> 在iOS可以打开，但是在i watch上打开以后，选择SDR后就一直在转圈圈无法进入
> *"It opens on iOS, but after opening it on the Apple Watch, once I select an SDR it just keeps
> spinning and won't go in."*

**Screenshot 1 (watch):** the directory list — Receiverbook, KiwiSDR (spinner), a row reading
"Loading…", FM-DX.
**Screenshot 2 (phone):** completely healthy — 21074 kHz USB, live waterfall, **28 k/s · 22 fps**.

★★ So it is **not** a crash, **not** a launch failure, and **not** the watch app being broken. The
watch is stuck on the DIRECTORY LIST, before any receiver has been chosen. The user's phrase
"选择SDR" means picking from the SDR list, not tuning one.

---

## ★★★ THE BUG: nothing in the chain has a timeout
Buddy does **not** fetch directories itself — **it asks the phone to**:
```swift
// WatchLink.swift:640 — fire and forget. No reply expected, no timer, no retry.
func browse(_ dir: String) { send(["cmd": "browse", "dir": dir]) }

// InstancePickerView.swift:181
// "MIRROR the phone: nil = still waiting for its reply, [] = it couldn't load"
if link.directories[dir.id] == nil { ProgressView(); Text("Loading…") }
```
`nil` means *"no answer yet"* — and **nothing ever changes `nil` to anything else.**

Then on the phone, `src/services/directories.ts` fetches `receiverbook.de`,
`rx.linkfanel.net/kiwisdr_com.js` and `servers.fmdx.org/api/` with **no timeout and no
AbortController**.

★★★ **So if the phone's fetch never returns, the phone never replies, and the watch spins for
ever.** That is not a glitch — it is what the code does. Same outcome if the phone app simply is not
running to receive the message.

## Why this user hits it
Mainland China. All three directory hosts are foreign and are very plausibly blocked or crawling
from there. ★ Receiverbook is additionally a **~400 KB HTML page** we download and parse.
★★ Consistent with everything observed: the phone works because it is connected to a receiver that
IS reachable; any DIRECTORY listing hangs.

★ **The cause is not China-specific** — China is only the reliable trigger. Any slow, captive or
flaky network produces the same infinite spinner, including the Pi's own hotspot
([[BRIEF-rds-logbook]] §1: on that hotspot there is no internet at all).

---

## ★★ THE FIX IS SMALL — THE FAILURE UI ALREADY EXISTS
`InstancePickerView.swift:184` already has exactly the right state and it is currently
**unreachable** in this scenario, because it only renders when the phone replies with an EMPTY list:
```swift
Text("Couldn't load — tap to retry").foregroundColor(.orange)
  .onTapGesture { link.browse(dir.id) }
```

1. **Timeout the phone's directory fetches** — `directories.ts`, ~10–15 s via `AbortController`, so
   the phone ALWAYS replies, even if only to say it failed.
2. **Timeout `browse` on the watch** — ~20 s, setting `[]` so the retry row appears even when the
   phone never answers at all (app not running, watch link down).
3. ★ **Distinguish "the phone isn't reachable" from "the directory failed"** — different causes,
   different user actions. Silence from the phone is not the same as a dead host.

★ Note the detect path already gets this right (`SDRDirectory.swift:74`,
`req.timeoutInterval = 5`) — the directory fetches are the ones that were missed.

---

## ★★ NO REPLY IS POSSIBLE — AND IT DOES NOT MATTER
`"emailAddress": null` in the feedback. TestFlight feedback without an address is anonymous with no
reply channel, so the tester cannot be asked anything.

★★★ **Fortunately the fix does not depend on knowing which half failed.** A blocked directory host
and an unreachable phone produce the IDENTICAL infinite spinner, and **both are fixed by the same
two timeouts.** Ship it blind.

★★ **The second-order lesson.** Had the failure state been reachable, *"Couldn't load — tap to
retry"* would have appeared in their screenshot and told US which half failed. **An error state is
not only courtesy to the user — it is the diagnostic you get back when you cannot ask.** A silent
spinner cost us the one piece of information an anonymous report could have carried.
★ Same family as "LOG UNKNOWN MESSAGE TYPES" in [[BRIEF-idle-handback]]: the cheapest debugging tool
is code that says what went wrong at the moment it goes wrong.

★ For the record, had a reply been possible the question would have been: *does the server list load
on the PHONE itself?* (yes → the watch link; no → the route to the directory hosts). Their
screenshots point to the latter. And a **custom server address** works with no directory at all,
which is the workaround for anyone who reports this.
★ They are on **build 9**; current is **22**. Not believed fixed in 22.

---

## ★ WHILE IN THERE
`http://servers.fmdx.org/api/` is fetched over **HTTP and 301-redirects to HTTPS** (verified
2026-07-31). It works because URLSession/fetch follow redirects, but it is a wasted round trip on a
slow link — and exactly the kind of link where it hurts. Use `https://` directly.
See [[backend_limits_probed]] for the rest of that endpoint's shape.
