# Several radios, one server

**Decided 2026-08-07 with Stuart.** Three radios (RSP1B, RTL-SDR Blog V4, Airspy HF+) on one Pi.
The RSP in multi-user mode; the other two one-listener-per-radio. All three listed on one landing
page, all three configured from one setup page.

Stuart's framing, and it is the design, not a description:

> Effectively setting up multiple separate vibeservers that happened to be grouped together on one
> setup page and one landing page.

---

## The shape: ONE PROCESS PER RADIO

★★★ **Not one process serving three radios.** `LocalSdrShim` is a hard singleton (`static
LocalSdrShim inst`, 47 `instance()` call sites, one `Impl* p`), and **the wire protocol carries no
radio id at all** — every socket, every control message and every listener table assumes "the"
radio. Making one process serve N radios means threading an id through all of that, and the
singleton is the smallest part of it.

Process-per-radio gives the product Stuart described with none of that, and more besides:

- genuinely independent DSP, listener limits and multi/single-user mode per radio
- a **separate crash domain** — one radio wedging cannot take the other two off the air
- per-radio release-when-idle, which is the whole point for a shared machine
- the existing code runs unchanged; what is new is *around* it, not *inside* it

The costs are real and small: a port per radio, one copy of the station/EiBi lists per process
(~1 MB plus 9,360 entries — nothing on this Pi), and a landing page that aggregates.

## Identity: THE USB SERIAL

Settings follow the **physical radio**, never the enumeration order. Plug a fourth dongle in and
nothing silently inherits another radio's configuration — including its locked frequency range,
which is the one that would put a receiver somewhere its owner never agreed to.

`findOurDevice()` already matches RTL dongles by serial for exactly this reason.

## Configuration: ONE FILE, A `radios` ARRAY

Shared settings stated once; per-radio settings per entry. One file to back up, and the admin
password cannot drift between radios.

```json
{
  "adminPassword": "…", "pin": "", "rxPlace": "Northampton", "rxGrid": "IO92nh",
  "radios": [
    { "serial": "240513CA60", "driver": "sdrplay",  "enabled": true,  "port": 48000,
      "users": 30, "lockedCentre": 6800000, "configured": true },
    { "serial": "00000001",   "driver": "rtlsdr",   "enabled": true,  "port": 48001,
      "users": 1, "configured": true },
    { "serial": "…",          "driver": "airspyhf", "enabled": false, "port": 48002,
      "users": 1, "configured": false }
  ]
}
```

★ `enabled` is the TUI's toggle. `configured` is whether its setup tab was saved.
**Both must be true to serve** — see "two gates" below.

---

## The TUI

Lists every detected radio, **all toggled on by default**, space toggles one off. Then the existing
password and PIN steps, once, shared by all of them.

```
  Radios found — space to toggle, enter to continue

   [x] SDRplay RSP1B            240513CA60
   [x] RTL-SDR Blog V4          00000001
   [ ] Airspy HF+               … (not served)
```

## The setup page

**One tab per radio, and only for radios enabled in the TUI.** Each tab is the whole settings page
as it stands today for a single radio. At the bottom of each tab: **Save radio settings**.

Below the tabs, in its own distinct footer: **Save and reboot server** — what Save does today.

## ★★ TWO GATES, AND THEY ARE DIFFERENT

A radio is served only if it is **enabled in the TUI** *and* **configured in its own tab**.
Stuart, 2026-08-07: *"Any radio that you have not configured in its tab does not get served"* and
*"not selected in the TUI it wont serve"*.

★ These are not the same gate and must not be collapsed into one. Un-ticking a radio in the TUI is
"I do not want this radio served"; never opening its tab is "I have not said what it should do
yet". A half-configured receiver going live on a default frequency is exactly what the second gate
exists to prevent — and the first must win over the second, so un-ticking a fully configured radio
takes it off the air without discarding its settings.

## The landing page

One page, at the primary port, listing every served radio with live state — listener count, the
range it covers, whether it is free or in use. Clicking one opens that radio's receiver on its own
port. One address to hand out.

★ It must read the other instances' state, not guess it. Each process already publishes
`/vibeserver.json`; the landing page aggregates those.

## ★★★ ONE FORWARDED PORT

Stuart, 2026-08-07. Non-negotiable for public servers: asking an owner to forward three ports is a
barrier that grows with every radio added. **48000 is the only port that leaves the machine.**
48001+ bind to **loopback only**, so they are not reachable from outside at all — which is also
strictly safer than three forwarded ports.

Two ways, and the second is the one to build:

1. **Reverse-proxy inside the primary.** Simple and portable, but then every listener's audio and
   waterfall flows through one process. ★★ This codebase has been bitten TWICE by that exact
   shape — the blocking broadcast under a global mutex, and one slow listener freezing all twenty.
   Viable only with the per-connection outbox model the DSP fan-out already uses, and only if
   MEASURED at 30 listeners rather than assumed.
2. **Hand the socket over (`SCM_RIGHTS`).** ✅ chosen. The primary accepts, reads far enough to know
   which radio the request is for, then passes the FD to that radio's process together with the
   bytes it already consumed. From then on the listener talks straight to the radio process: no
   proxy in the path, no shared bottleneck, and the primary goes back to idle.
   ★ It removes the failure mode instead of mitigating it, and it preserves the separate crash
     domain — a wedged radio process cannot stall the front door.
   ★ The fiddly part is replaying the buffered request prefix so the receiving process sees an
     intact stream. That is the piece to write a test for first.

---

## Order of work

1. **`--radio N`** — select any driver from the flat list. ✅ done 2026-08-07; until then `--device`
   reached only the dongle and discovery was a preference chain (Airspy → RSP → RTL), so with three
   radios plugged in *which one you got was a lottery*. It moved Stuart's own demo off the RSP.
2. Config schema: `radios[]`, serial identity, migration from today's single-radio file.
   ★ An existing install must come out of this with its current radio configured and enabled —
   the "not configured, so not served" gate must never silently take a working receiver off air.
3. TUI radio list with the space toggle.
4. systemd template unit `vibeserver@<serial>.service`, and a supervisor that starts one per
   enabled+configured radio.
5. Setup page tabs, per-tab save, footer save-and-reboot.
6. Landing page aggregation.

## Related work landing at the same time

- **Release the radio when idle** (for sharing an SDR with OpenWebRX) — core built and hammer-tested
  on the RTL 2026-08-07: 8 releases, 7 reacquires, 4,392 control messages, and `rtl_test` proved to
  fail while held and succeed while released. Still to do: the RSP and Airspy paths, and the setup
  page toggle. Off by default. See `LocalSdrShim::releaseRadio`.
- The **device lock** that made the above possible — RTL control calls now take the same recursive
  `modeMtx` the RSP and Airspy setters always used. This is why `reopenDevice()` could never be
  called before, and it is groundwork for anything that opens and closes devices at runtime.
